#!/usr/bin/env bash
# Bring-up only: wraps /root/pdj/rbp in build/rx3/rootfs.ext4 so the next
# run-dev.sh boot runs the player under qemu-arm's syscall trace and prints
# the last syscalls to the console when it exits. Re-run guest/rx3/build.sh
# --firmware ... to get a clean root back.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cat > "$REPO/build/rx3/rbp-wrapper" <<'WRAP'
#!/bin/sh
QEMU_LOG=strace QEMU_LOG_FILENAME=/tmp/rbp.strace /root/pdj/rbp.bin "$@" 2>/tmp/rbp.err
rc=$?
{
    echo "dj-emu rx3: rbp exited $rc; stderr:"
    tail -n 60 /tmp/rbp.err
    echo "dj-emu rx3: devices and framebuffer:"
    grep -E 'open\("/dev|open\(".*directfb|FBIO|ioctl\(' /tmp/rbp.strace | grep -v 'SNDRV\|0x80045300' | tail -n 60
    echo "dj-emu rx3: last syscalls:"
    grep -v '_newselect\|clock_gettime\|gettimeofday\|futex\|nanosleep' /tmp/rbp.strace | tail -n 30
} > /dev/console
WRAP
docker run --rm --network none --mount "type=bind,source=$REPO/build/rx3,target=/out" \
    dj-emu-rx3-tools:dev bash -euo pipefail -c '
        debugfs -w /out/rootfs.ext4 -R "stat /root/pdj/rbp.bin" 2>/dev/null | grep -q Inode \
            || debugfs -w /out/rootfs.ext4 -R "ln /root/pdj/rbp /root/pdj/rbp.bin" >/dev/null 2>&1
        debugfs -w /out/rootfs.ext4 -f - >/dev/null 2>&1 <<CMDS
unlink /root/pdj/rbp
write /out/rbp-wrapper /root/pdj/rbp
sif /root/pdj/rbp mode 0100755
sif /root/pdj/rbp.bin links_count 1
CMDS
        debugfs /out/rootfs.ext4 -R "ls -l /root/pdj" 2>/dev/null | grep -E " rbp"'
