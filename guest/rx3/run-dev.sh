#!/usr/bin/env bash
# Boots the XDJ-RX3 guest by hand, outside the app, for bring-up. Uses the
# host QEMU (qemu/install), the guest kernel (build/Image) and the pieces
# guest/rx3/build.sh puts in build/rx3/. The display goes to the same
# shared-memory file the app reads; the serial console goes to stdout.
#
#   guest/rx3/run-dev.sh [RUN_DIR]     (default: build/rx3/run; keep it short,
#                                       macOS caps socket paths at 104 bytes)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
RX3="$REPO/build/rx3"
RUN="${1:-$RX3/run}"
mkdir -p "$RUN"
QEMU="$REPO/qemu/install/bin/qemu-system-aarch64"
# A scratch copy, so a bring-up run never dirties the assembled root.
cp -c "$RX3/rootfs.ext4" "$RUN/rootfs.ext4" 2>/dev/null || cp "$RX3/rootfs.ext4" "$RUN/rootfs.ext4"
cp "$RX3/board.img" "$RUN/board.img"

exec "$QEMU" \
    -machine virt,gic-version=3 -accel hvf -cpu host -smp 4 -m 2G \
    -kernel "$REPO/build/Image" -initrd "$RX3/initramfs.cpio.gz" \
    -append "console=ttyAMA0,115200 rdinit=/init loglevel=5 virtio_blk.emmc_index=0 rx3.log=1 virtio_gpu.modeset=1 video=Virtual-1:1280x800-32@60" \
    -drive if=none,id=root,file="$RUN/rootfs.ext4",format=raw -device virtio-blk-device,drive=root \
    -drive if=none,id=board,file="$RUN/board.img",format=raw -device virtio-blk-device,drive=board \
    -device virtio-gpu-device,xres=1280,yres=800,max_outputs=1 \
    -display shm,path="$RUN/main.shm" \
    -qmp unix:"$RUN/qmp.sock",server=on,wait=off \
    -nic none -serial stdio -monitor none -no-reboot
