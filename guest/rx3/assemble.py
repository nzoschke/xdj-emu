#!/usr/bin/env python3
"""Assemble the XDJ-RX3 root filesystem from a decrypted firmware ISO.

usage: assemble.py <XDJRX3.iso> <outdir>

Runs inside guest/rx3/Dockerfile (bsdtar, fsck.cramfs, mke2fs). Produces:

  rootfs.ext4  images/rootfs.cramfs, plus images/pdj.tar.gz in /root/pdj,
               images/gui.tar.gz and the ISO's gui/ in /root/gui,
               images/settings.tar.gz in /root/settings, plus the board-tool
               replacements in guest/rx3/board/ (originals kept as *.orig)
  board.img    a U-Boot environment at 0xC0000 (size 0x2000, CRC32 first),
               where the RX3's fw_printenv reads it in SD mode; the values
               start from the firmware's own images/fw_env.txt
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ENV_OFFSET = 0xC0000
ENV_SIZE = 0x2000
BOARD_IMG_SIZE = 1 << 20
ROOTFS_SIZE = "768M"
BOARD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board")

# Overrides on top of images/fw_env.txt. aplstart=on lets apl_start launch the
# player; rootfs=mmc with BOOTMODE=MMC (board/bootmod) skips the NAND (UBI)
# mounts, so /root/gui and /root/settings come from this image.
ENV_OVERRIDES = {
    "aplstart": "on",
    "rootfs": "mmc",
    "dhcp_server": "0",
    "serial_num": "DJEMU0000001",
    "no_reset_usbbhub": "1",
}


def run(*cmd):
    subprocess.run(cmd, check=True)


def environment(fw_env_txt):
    env = {}
    with open(fw_env_txt, encoding="latin-1") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                env[k] = v
    env.update(ENV_OVERRIDES)
    data = b"".join(f"{k}={v}".encode("latin-1") + b"\0" for k, v in env.items()) + b"\0"
    assert len(data) <= ENV_SIZE - 4, "U-Boot environment too large"
    data = data.ljust(ENV_SIZE - 4, b"\0")
    return struct.pack("<I", zlib.crc32(data)) + data


def main():
    iso, out = sys.argv[1], sys.argv[2]
    work = tempfile.mkdtemp(prefix="rx3-")
    isodir = os.path.join(work, "iso")
    root = os.path.join(work, "root")
    os.makedirs(isodir)
    run("bsdtar", "-xf", iso, "-C", isodir)
    images = os.path.join(isodir, "images")

    # Base root: the firmware's cramfs, extracted with ownership and modes.
    run("fsck.cramfs", f"--extract={root}", os.path.join(images, "rootfs.cramfs"))

    pdj_parent = os.path.join(root, "root")
    os.makedirs(pdj_parent, exist_ok=True)
    run("tar", "-xzf", os.path.join(images, "pdj.tar.gz"), "-C", pdj_parent)
    for name in ("gui", "settings"):
        dst = os.path.join(root, "root", name)
        os.makedirs(dst, exist_ok=True)
        run("tar", "-xzf", os.path.join(images, f"{name}.tar.gz"), "-C", dst)
    if os.path.isdir(os.path.join(isodir, "gui")):
        run("cp", "-a", os.path.join(isodir, "gui") + "/.", os.path.join(root, "root", "gui"))
    for rev in ("app.rev", "system.rev", "release.txt"):
        src = os.path.join(images, rev)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(root, "root", "pdj", rev) if rev == "app.rev" else os.path.join(root, "etc", rev))

    # Board tools that read SoC registers through /dev/mem on the real unit.
    for tool in sorted(os.listdir(BOARD)):
        dst = os.path.join(root, "usr", "bin", tool)
        if os.path.exists(dst) and not os.path.exists(dst + ".orig"):
            os.rename(dst, dst + ".orig")
        shutil.copy(os.path.join(BOARD, tool), dst)
        os.chmod(dst, 0o755)

    rootfs = os.path.join(out, "rootfs.ext4")
    if os.path.exists(rootfs):
        os.remove(rootfs)
    run("mke2fs", "-q", "-t", "ext4", "-L", "rx3root", "-E", "root_owner=0:0", "-d", root, rootfs, ROOTFS_SIZE)

    with open(os.path.join(out, "board.img"), "wb") as f:
        f.truncate(BOARD_IMG_SIZE)
        f.seek(ENV_OFFSET)
        f.write(environment(os.path.join(images, "fw_env.txt")))
    shutil.rmtree(work)
    print("assembled rootfs.ext4 and board.img")


if __name__ == "__main__":
    main()
