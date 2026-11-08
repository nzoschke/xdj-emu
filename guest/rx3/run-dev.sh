#!/usr/bin/env bash
# Boots the XDJ-RX3 guest by hand, outside the app, for bring-up. Uses the
# host QEMU (qemu/install), the guest kernel (build/Image) and the pieces
# guest/rx3/build.sh puts in build/rx3/. The LCD is RUN/rx3fb.shm (see
# guest/modules/rx3_board/rx3fb.c); the serial console goes to stdout.
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
rm -f "$RUN/rx3fb.shm"

exec "$QEMU" \
    -machine virt,gic-version=3 -accel hvf -cpu host -smp 4 -m 2G \
    -kernel "$REPO/build/Image" -initrd "$RX3/initramfs.cpio.gz" \
    -append "console=ttyAMA0,115200 rdinit=/init loglevel=5 virtio_blk.emmc_index=0 rx3.log=1 rx3.shell=1 ${RX3_KCMD_EXTRA:-} fbcon=map:9" \
    -drive if=none,id=root,file="$RUN/rootfs.ext4",format=raw -device virtio-blk-device,drive=root \
    -drive if=none,id=board,file="$RUN/board.img",format=raw -device virtio-blk-device,drive=board \
    -object memory-backend-file,id=rx3fb,mem-path="$RUN/rx3fb.shm",size=16M,share=on \
    -device ivshmem-plain,memdev=rx3fb,master=on \
    -display none \
    -qmp unix:"$RUN/qmp.sock",server=on,wait=off \
    -netdev user,id=link0,restrict=on,hostfwd=tcp:127.0.0.1:${RX3_GDB_PORT:-1234}-:1234 -device virtio-net-device,netdev=link0 \
    -device virtio-serial-device \
    -chardev socket,id=shell,path="$RUN/shell.sock",server=on,wait=off \
    -device virtconsole,chardev=shell \
    -serial stdio -monitor none -no-reboot
