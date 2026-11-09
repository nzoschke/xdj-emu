#!/usr/bin/env bash
# Builds the XDJ-RX3 guest pieces into build/rx3/:
#
#   qemu-arm        static aarch64 qemu-arm (linux-user) from qemu/src, the
#                   same patched tree the host QEMU is built from
#   initramfs.cpio.gz
#                   the boot shim: busybox, qemu-arm, the RX3 board module
#                   (build/docker-out, from ./build.sh), /init (guest/rx3/init)
#
# and, with --firmware <decrypted XDJRX3 ISO>, the per-install disks:
#
#   rootfs.ext4     the RX3's own root filesystem (rootfs.cramfs + pdj + gui
#                   + settings) with the board-tool replacements of
#                   guest/rx3/board/
#   board.img       U-Boot environment where the RX3's fw_printenv looks
#
# The firmware is never copied into the repository: it is read from the path
# given and the outputs stay under build/rx3/ (git-ignored).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/build/rx3"
IMAGE=dj-emu-rx3-tools:dev
FIRMWARE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --firmware) FIRMWARE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        *) echo "usage: $0 [--firmware <XDJRX3.iso>]" >&2; exit 2 ;;
    esac
done
[ -d "$REPO/qemu/src" ] || { echo "run ./qemu/build.sh first (it clones qemu/src)" >&2; exit 1; }
mkdir -p "$OUT"

docker build -q -t "$IMAGE" "$REPO/guest/rx3" >/dev/null

run() {
    docker run --rm --network none \
        --mount "type=bind,source=$REPO,target=/src,readonly" \
        --mount "type=bind,source=$OUT,target=/out" \
        ${FIRMWARE:+--mount "type=bind,source=$FIRMWARE,target=/firmware.iso,readonly"} \
        "$IMAGE" bash -euo pipefail -c "$1"
}

if [ ! -x "$OUT/qemu-arm" ]; then
    echo "[rx3] building static qemu-arm (linux-user)..."
    run '
        cp -a /src/qemu/src /tmp/qemu && cd /tmp/qemu && rm -rf build
        PIP_NO_INDEX=1 ./configure --target-list=arm-linux-user --static --disable-system \
            --disable-tools --disable-docs --disable-plugins --disable-werror > /out/qemu-arm-configure.log 2>&1 \
            || { cat /out/qemu-arm-configure.log; exit 1; }
        ninja -C build qemu-arm >/dev/null
        strip -o /out/qemu-arm build/qemu-arm
    '
fi

echo "[rx3] building initramfs..."
run '
    root=/tmp/initramfs; mkdir -p $root/{bin,dev,proc,sys,newroot,usr/libexec}
    cp /bin/busybox $root/bin/busybox
    for applet in sh mount umount mkdir mknod switch_root sleep cat echo ls ln grep insmod ifconfig chroot; do
        ln -s busybox $root/bin/$applet
    done
    cp /out/qemu-arm $root/usr/libexec/qemu-arm
    gcc -static -O2 -Wall -Wextra -o $root/bin/subucom-log /src/guest/rx3/subucom-log.c
    gcc -static -O2 -Wall -Wextra -o $root/bin/rx3-panel /src/guest/rx3/panel-bridge.c
    mkdir -p $root/lib/modules
    for ko in rx3_board rx3fb udev_usb1 virtio_snd; do
        cp /src/build/docker-out/modules/$ko.ko $root/lib/modules/
    done
    install -m 0755 /src/guest/rx3/init $root/init
    (cd $root && find . | cpio -o -H newc --quiet | gzip -9) > /out/initramfs.cpio.gz
'

if [ -n "$FIRMWARE" ]; then
    echo "[rx3] assembling the root filesystem from the firmware..."
    run 'python3 -I /src/guest/rx3/assemble.py /firmware.iso /out'
fi
ls -l "$OUT"
