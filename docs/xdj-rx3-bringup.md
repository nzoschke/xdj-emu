# XDJ-RX3 bring-up: handoff

Status as of 2026-11-08, branch `xdj-rx3` (last commit `3b4e9af`). Design
background is in [xdj-rx3.md](xdj-rx3.md).

## Where it stands

- The RX3's own userland boots on dj-emu's AArch64 kernel: busybox `init` →
  `rcS` → `apl_start` → the **unmodified 1.20 player** (`rbp`), all ARM32
  code under `qemu-arm` translation.
- It reaches the home screen (rekordbox splash, both decks, hot cues, Beat FX)
  in the RX3's own RGB565 mode, on the emulated `rx3fb` framebuffer.
- Emulated devices (`guest/modules/rx3_board/`): `subucom_spi{1,2}.0`,
  `subucom_spi_rdy{3,4}.0`, `gpiodrv`, `printkdrv0`, `hidg0`, `paudiog0`
  (`rx3_board.ko`); `/dev/fb0` and `/dev/tsc2007_2-0048` (`rx3fb.ko`).
- Board identity: `/proc/cpuinfo` reports a Cortex-A9, `Hardware : Freescale
  i.MX 6Quad SABRE Smart Device Board`, `Revision : 63712`
  (`/etc/dj-emu/board`, read by qemu-arm, patch 22).
- **Open problem:** the player never opens the panel (`subucom_*`) or touch
  devices, so no input reaches it. Its UI start-up is blocked: the main
  thread waits on the panel message thread, which is only created later in
  start-up. On a real unit nothing posts to the panel that early, so some
  difference in our emulated board triggers it.

## Build and run

```sh
cd ~/src/dj-emu
./qemu/build.sh                    # host QEMU (once; again after patch changes)
./build.sh --artifacts-only        # guest kernel + modules (after module changes)
guest/rx3/build.sh --firmware ~/src/rx3-proto/build-source/extracted/XDJRX3.iso
                                   # qemu-arm, boot shim, root disk; drop --firmware
                                   # to rebuild only qemu-arm/initramfs
rm build/rx3/qemu-arm              # force a qemu-arm rebuild after patch 22 edits
guest/rx3/run-dev.sh > build/rx3/live.log 2>&1 &     # boot (serial log)
build/rx3/RX3View build/rx3/run/rx3fb.shm            # live LCD window
pkill -f build/rx3/run                               # stop
```

Rebuild the viewer with
`xcrun swiftc -O -o build/rx3/RX3View guest/rx3/viewer/RX3View.swift`.
Keep the run directory path short: macOS limits Unix socket paths to 104
bytes. The player is up about 60–90 s after boot (`### Application Start!! ###`
in the log).

## Debugging tools

| Tool | What it does |
|---|---|
| `guest/rx3/rx3sh.py build/rx3/run/shell.sock '<cmd>'` | Runs a shell command inside the running RX3 root (`rx3.shell=1`, on by default in `run-dev.sh`). Use it for `ps`, `/proc/<pid>/task/*/{comm,wchan}`, `/proc/asound`, `/tmp`, `dmesg`. |
| `guest/rx3/dev-strace-rbp.sh exit` | Wraps `/root/pdj/rbp` in the assembled root: next boot prints the player's stderr and last syscalls when it exits. |
| `guest/rx3/dev-strace-rbp.sh live` | Same, but reports after 90 s while it keeps running: each thread's last syscall, device opens, the main thread's last 60 syscalls. |
| `RX3_DFILTER=<pc>+4 guest/rx3/dev-strace-rbp.sh cpulog` | QEMU's own register log at given guest PCs (shows callers through R14). |
| `guest/rx3/dev-strace-rbp.sh gdb` + `RX3_KCMD_EXTRA=rx3.gdb=1 guest/rx3/run-dev.sh` | Player waits for gdb on host port 1234. gdb 13 asserts in qemu-user's multithreaded stub on breakpoints, so this was of limited use. |
| Serial log notices | `rx3_board: <proc> opened <dev>`, `... timer on/off`, `rx3fb: ... opened the touch panel`, and `subucom spiN tx` frame dumps (`rx3.log=1`). |
| `guest/rx3/rx3fb2png.py` | Decodes `rx3fb.shm` to a PNG. |

The trace wrappers change the root disk; run `guest/rx3/build.sh --firmware …`
again for a clean one.

## How each step so far was found

Every fix came from the same loop: boot, see where the player stops, give it
the device or board state a real RX3 would have, boot again.

1. **Exec format error at `switch_root`:** the boot shim unmounted
   `binfmt_misc`, which dropped the qemu-arm registration. Fix: keep it
   mounted and move `/proc`, `/sys`, `/dev` into the new root.
2. **Crash after `gpio` errors:** no `/dev/gpiodrv`. Fix: emulate it with the
   ABI from Pioneer's GPL driver (`arch/arm/mach-mx6/gpiodrv.c`).
3. **A thread spinning on `select()` with `EINVAL`:** qemu-user did not
   normalise `tv_usec >= 1s`. Fixed in patch 22.
4. **NULL dereference after `SIOCGIFHWADDR eth0`:** no network device. Fix:
   virtio-net.
5. **Crash after opening `/dev/hidg0`:** the USB-B gadget nodes were missing.
   Fix: emulate them as "no host attached".
6. **DirectFB failing on `/dev/galcore`:** no Vivante GPU. Fix:
   `/usr/etc/directfbrc` with `no-hardware`, `disable-module=gal` (the path
   comes from the library's strings).
7. **Crash in the GUI layer, line 676:** the player needs an RGB565 layer,
   which virtio-gpu cannot offer. Fix: `rx3fb`, an ivshmem-backed fbdev.
8. **Database thread blocked opening `/tmp/req_LocalDBServer`:** loopback was
   down. Pioneer's own initramfs (`initramfs/init` in the GPL package) runs
   `ifconfig lo up`; the boot shim now does the same.
9. **Board identity:** the player checks `/proc/cpuinfo`'s `Revision` for the
   RX3 board value. qemu-arm's default CPU omitted those lines; it now runs a
   Cortex-A9 and reports the board from `/etc/dj-emu/board`.

The prototype's binary patch list
(`~/src/rx3-proto/build-source/tools/patch-rbp/PATCHES.md`) turned out to be a
good map of missing devices: most of its patches were working around exactly
the hardware the emulation now provides.

## Ruled out (2026-11-08, later)

Boot-to-boot comparisons that did **not** get the player to open the panel:

- **Sound:** no card at all (`RX3_KCMD_EXTRA=snd-dummy.enable=0`) behaves
  exactly like the dummy card, and the player never opens
  `/dev/snd/controlC*` before it blocks, so sound init is not the trigger.
  (RX3-shaped cards are still needed for audio, and qemu-arm cannot yet
  translate ALSA PCM ioctls.)
- **Every file the player looks for and does not find** (the `live` wrapper
  now lists them). Added, each now accepted: `/proc/udev_usb1`, `udev_usb2`
  and friends (cdj3k-emu's `udev_usb1.ko`, now loaded by the shim); the LCD
  backlight `pwm-backlight.1` and `/sys/class/paudiog/paudiog0/connect`
  (`rx3_board.ko`; `CONFIG_BACKLIGHT_CLASS_DEVICE=y`); factory and user touch
  calibration in `/root/settings` (values from the rbpi4b project). The only
  remaining miss is `/root/settings/XdjSettings.dat`, normal on a first boot.

## Leads for the open problem

Each is a difference between our board and a real RX3 that could make the
player raise something during start-up:

- **What gets posted to the panel early.** The block is a message posted to
  the panel peer from the UI message layer (`uif::IMsgHandler::postMessageInternal`
  was the caller, from the `cpulog` wrapper mode) before the panel thread
  exists. Finding which UI object posts it, and why on this board, is the most
  direct route.
- **Panel answers.** No panel chip ever answers. cdj3k-emu's experience
  (`docs/subucom.md`) is that the player needs a valid idle frame (header,
  power-on bit, CRC) before it engages; the RX3's frame format is still
  unknown.
- **GPIO inputs.** All `gpiodrv` reads return 0. The board file
  (`arch/arm/mach-mx6/board-mx6q_sabresd.c`) names the RX3 pins; check
  whether any input the player reads should idle high.
- **Settings and USB state.** `/root/settings` starts empty (from
  `settings.tar.gz`); no USB media events are emulated yet (cdj3k-emu's
  `udev_usb1.ko` is the model to follow).

Useful sources: Pioneer's XDJ-RX3 GPL package (kernel drivers, board file,
initramfs scripts), extracted at
`/private/tmp/claude-501/-Users-noah/c6d40c95-f15d-476f-8cb9-8651029fd9ab/scratchpad/rx3-gpl/`.
That scratchpad is temporary: if it is gone, download the package again from
pioneerdj.com's open-source page (the two `pioneerdj_xdj_rx3.tar.bz2.*` ZIPs
listed in `~/src/rx3-toolkit/app/firmware/key_source.json`). The firmware's
own scripts are in the assembled root under `/etc/rc.d/` and `/root/pdj/`.
