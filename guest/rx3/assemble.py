#!/usr/bin/env python3
"""Assemble the XDJ-RX3 root filesystem from a decrypted firmware ISO.

usage: assemble.py <XDJRX3.iso> <outdir>

Runs inside guest/rx3/Dockerfile (bsdtar, fsck.cramfs, mke2fs). Produces:

  rootfs.ext4  images/rootfs.cramfs, plus images/pdj.tar.gz in /root/pdj,
               images/gui.tar.gz and the ISO's gui/ in /root/gui,
               images/settings.tar.gz plus the factory touch calibration in
               /root/settings, plus the board-tool
               replacements in guest/rx3/board/ (originals kept as *.orig)
               a /usr/etc/directfbrc for software rendering and
               /etc/dj-emu/board, the board identity qemu-arm reports
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

# DirectFB configuration for the emulated board (the firmware ships none).
# The unit renders through its Vivante GC2000 (gfxdriver "gal", /dev/galcore),
# which is not emulated: DirectFB draws in software into the same fbdev. Its
# evdev/PS2 input drivers are off because the RX3's controls arrive through
# subucom and its touch controller, not evdev.
DIRECTFBRC = """\
no-hardware
disable-module=gal
disable-module=keyboard
disable-module=linux_input
disable-module=ps2mouse
"""

# The board as its kernel names it in /proc/cpuinfo (qemu-arm reads this file;
# qemu/patches/22-linux-user-xdj-rx3.patch). The player checks Revision: an
# i.MX6 Quad (0x63xxx) with board bits 0x700, IMX_BOARD_REV_8, which Pioneer's
# board file and the player's SystemStats::BOARD_REV_RX3 both use for the RX3.
BOARD_ID = """\
Cpu=cortex-a9
Hardware=Freescale i.MX 6Quad SABRE Smart Device Board
Revision=63712
Serial=0000000000000000
"""

# Touch-panel calibration, which the factory writes to the settings partition
# (/root/settings/TouchCalib_{Factory,User}.dat): six integers mapping panel
# readings to LCD pixels. These are the values the rbpi4b project found for a
# panel that reports pixels, as rx3fb's tsc2007 device does.
TOUCH_CALIB = "0\n0\n320\n200\n1280\n800\n"

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
    for name in ("TouchCalib_Factory.dat", "TouchCalib_User.dat"):
        with open(os.path.join(root, "root", "settings", name), "w") as f:
            f.write(TOUCH_CALIB)
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

    os.makedirs(os.path.join(root, "etc", "dj-emu"), exist_ok=True)
    with open(os.path.join(root, "etc", "dj-emu", "board"), "w") as f:
        f.write(BOARD_ID)

    # This DirectFB build reads /usr/etc/directfbrc (its sysconfdir).
    os.makedirs(os.path.join(root, "usr", "etc"), exist_ok=True)
    with open(os.path.join(root, "usr", "etc", "directfbrc"), "w") as f:
        f.write(DIRECTFBRC)

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
