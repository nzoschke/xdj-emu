#!/usr/bin/env bash
# Bring-up only: wraps /root/pdj/rbp in build/rx3/rootfs.ext4 so the next
# run-dev.sh boot runs the player under qemu-arm's syscall trace and prints
# the last syscalls to the console when it exits. Re-run guest/rx3/build.sh
# --firmware ... to get a clean root back.
set -euo pipefail
MODE="${1:-exit}"   # exit: report when rbp exits; live: report after 90 s;
                    # gdb: run rbp under qemu-arm's gdb stub on guest port 1234
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "$MODE" = cpulog ]; then
# Registers at every entry to the guest PCs in $RX3_DFILTER (e.g. 0x3664ec+4),
# from QEMU's own log; printed after 90 s (R14 gives the callers).
cat > "$REPO/build/rx3/rbp-wrapper" <<WRAP
#!/bin/sh
QEMU_LOG=cpu,nochain QEMU_DFILTER=${RX3_DFILTER:?set RX3_DFILTER} QEMU_LOG_FILENAME=/tmp/cpu.log /root/pdj/rbp.bin "\$@" &
(
    sleep 90
    {
        echo "dj-emu rx3: cpu log for ${RX3_DFILTER}: entries \$(grep -c '^R00' /tmp/cpu.log)"
        grep -o 'R14=[0-9a-f]*' /tmp/cpu.log | sort | uniq -c | sort -rn | head -20
    } > /dev/console
) &
wait
WRAP
elif [ "$MODE" = gdb ]; then
cat > "$REPO/build/rx3/rbp-wrapper" <<'WRAP'
#!/bin/sh
echo "dj-emu rx3: rbp waiting for gdb on port 1234" > /dev/console
QEMU_GDB=1234 exec /root/pdj/rbp.bin "$@"
WRAP
elif [ "$MODE" = live ]; then
cat > "$REPO/build/rx3/rbp-wrapper" <<'WRAP'
#!/bin/sh
QEMU_LOG=strace QEMU_LOG_FILENAME=/tmp/rbp.strace /root/pdj/rbp.bin "$@" 2>/tmp/rbp.err &
main=$!
(
    sleep 90
    {
        echo "dj-emu rx3: rbp after 90 s; stderr:"
        tail -n 20 /tmp/rbp.err
        echo "dj-emu rx3: last syscall of each thread:"
        awk '$1 ~ /^[0-9]+$/ { last[$1] = $0 } END { for (t in last) print last[t] }' /tmp/rbp.strace | cut -c1-200
        echo "dj-emu rx3: device opens:"
        grep -E 'open\("/dev' /tmp/rbp.strace | cut -c1-160 | sort | uniq -c | sort -rn | head -40
        echo "dj-emu rx3: paths that failed to open or stat (no library search):"
        grep -E '(open|stat64|access|opendir|readlink)\("[^"]*".*= -1 errno=[0-9]+' /tmp/rbp.strace \
            | grep -v -E '"/lib/|"/usr/lib/|/tls/|/v7l/|/neon/|/vfp/|ld.so' \
            | sed -r 's/^[0-9]+ //; s/,0x[0-9a-f]+//g' | cut -c1-120 | sort | uniq -c | sort -rn | head -40
        echo "dj-emu rx3: main thread, last 60 syscalls:"
        grep "^$main " /tmp/rbp.strace | tail -n 60 | cut -c1-150
        echo "dj-emu rx3: gpio activity (ioctl 0xf1, seeks, polls, selects):"
        grep -E 'f10[1-4]|_llseek|lseek|^[0-9]+ poll|_newselect' /tmp/rbp.strace | sed -r 's/^[0-9]+ //' | cut -c1-120 | sort | uniq -c | sort -rn | head -40
    } > /dev/console
) &
wait
WRAP
else
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
fi
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
