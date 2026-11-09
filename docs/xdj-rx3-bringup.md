# XDJ-RX3 bring-up: handoff

Status as of 2026-11-08, branch `xdj-rx3` (baseline `b75a09a`, plus the
USB overcurrent GPIO fix described below). Design
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
- **Startup deadlock resolved:** USB overcurrent inputs GPIO 126 and 204
  were incorrectly low. They now idle high in `rx3_board`; the unmodified
  player opens all four panel channels and the touch device. `UiMain`,
  `JogCom:1`, `JogCom:2`, and `TouchPanel` threads reach their event loops.
- **Panel RX recovered:** EUP/SUB frame lengths, CRC, initialization, and 72
  deck button bits are established. Diagnostic frames are accepted by stock
  rbp; QUANTIZE press/release works independently on both decks. See
  [panel protocol recovery](xdj-rx3-panel.md) for tools and evidence.
- **Remaining:** a persistent panel state source, analog/jog recovery, and
  touch delivery. An INFO touch tap still produced no visible change after
  panel initialization.

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
bytes. The fixed clean boot opened the panel and touch devices at guest uptime
3.72 s. The trace wrappers wait 90 s before reporting; that delay is not a
boot requirement. `### Application Start!! ###` marks the application launch.

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

The firmware mounts `/tmp` on a 512 KiB tmpfs. A full syscall trace fills it
quickly (observed at 496 KiB), truncating evidence and potentially changing
application behavior. For extended traces, use a path on the root disk, such
as `/root/rbp.strace`, or explicitly enlarge the diagnostic tmpfs. A clean
boot without tracing used 52 KiB.

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

## Reverse-engineered startup blocker (resolved)

Analyzed stock 1.20 `rbp`, SHA-256
`3f0a1a9c4d107fcb856eb59bd77b39139b239b111f431d40d11b5c7e66dffddb`.
Addresses below are ARM virtual addresses. Key instruction words were checked
against the extracted ELF's PT_LOAD segments, rather than relying solely on
an older disassembly. Fresh guest `/root/pdj/rbp` MD5 matched the extracted
stock file: `4f2efcfc0c9e3f539289f863acfddcc6`.

The blocked player's main stack was read through `/proc/3843/mem` using the
bring-up shell. Its firmware mappings start at their ARM virtual addresses;
this must be checked in `/proc/<pid>/maps` before repeating the technique.
These are stack return addresses corroborated by call-site disassembly,
not a debugger-generated unwind:

| Return address | Caller |
|---|---|
| `0x0104cc` | `main`, after calling `UiObjectManager::init` |
| `0x31bb84` | `UiObjectManager::init`, constructing `UsbStorageManager` |
| `0x322a3c` | `UsbStorageManager` constructor, calling `GpioManager` |
| `0x028dc0` | `GpioManager` constructor, synchronous initial GPIO callback |
| `0x32407c` | `UsbStorageManager::notify_over_current(false)` |
| `0x2cf64c` | `IMsgHandler::postMessageInternal`, calling panel peer |
| `0x36653c` | `PanelComPeerLinux::postMessage`, sleeping on readiness |

The GPIO constructor reads one byte and immediately invokes its callback at
`0x028dbc`, before installing the IRQ or starting its GPIO thread. The USB
constructor selects GPIO 126 for channel 1 and 204 for channel 2 at
`0x322970–0x322974`. `handleGpioMessage` at `0x3240ec` checks those same pairs.
In `notify_over_current`, `0x324058–0x32405c` returns immediately for a high
input when there is no outstanding fault; low enters the fault notification.

The GPL board header `arch/arm/mach-mx6/board-mx6q_sabresd.h:117–120`
labels GPIO7_12 (204) as USB-A 1 OCS and GPIO4_30 (126) as USB-A 2 OCS.
Those physical port labels are opposite the firmware object's channel numbers;
keep both inputs high rather than assuming the channel-to-port names match.

The notification queues a message, then `postMessage` waits at
`0x366528–0x366544` for object byte `+0x88` to become nonzero. The panel
thread sets that byte at `0x366da0`, after its epoll setup. Normal startup
opens the panel and touch devices in `UiObjectManager::startUp` at
`0x31ddbc–0x31dde0`, after UI construction. Thus a false overcurrent during
construction creates a circular startup dependency. No panel response can
resolve this particular pre-open wait.

Fix: initialize only `gpio_val[126]` and `[204]` to 1. No rbp patch or
message-thread bypass is needed.

Validation: rebuilt guest artifacts and assembled a fresh root from the ISO,
then booted into `build/rx3/re` without a trace wrapper. Evidence is in
`build/rx3/gpio-boot.log`, `gpio-validation.txt`, and `gpio-home.png`
(local, ignored build outputs). All four channel FDs and the touch FD were
present. Dmesg reports:

| Channel | Timer interval | RX bytes |
|---|---:|---:|
| spi1 | 2 ms | 100 |
| spi2 | 2 ms | 104 |
| rdy3 | 1 ms | 24 |
| rdy4 | 1 ms | 24 |

Both ready channels emitted changing 148-byte TX frames. The home-screen
capture is still RGB565, 1280×800, stride 2560. This establishes progress
past startup into device event loops, not functional panel input or audio.

## Next work, with acceptance criteria

1. **Implement persistent panel state.** Framing and 72 deck button bits are
   recovered and verified; see [the protocol note](xdj-rx3-panel.md). Maintain
   complete EUP/SUB control state, recompute CRC, and deliver press/release
   changes through `/dev/rx3_subucom`. Diagnostic zeros are accepted but are
   not calibrated analog defaults. Ready-channel JogCom RX remains separate.
2. **Verify touch end to end.** The shared-memory touch slot and device are
   wired, but an INFO tap at LCD (1230,24), held 150 ms, left the screenshot
   unchanged. Trace `TouchPanel::commRxDataProc` (`0x2d734c`) and its dispatch
   gates; determine whether panel readiness or calibration prevents delivery.
   Acceptance: a known visible UI control reacts to press and release.
3. **Extend input coverage.** QUANTIZE works on both decks, and the firmware
   oracle verifies 72 button bits. Exercise SUB buttons live, recover mixer
   selectors and analog calibration, then the ready-channel jog protocol.
4. **USB media and audio.** Media event nodes already exist as of `b75a09a`;
   exercise insertion/removal with real filesystem state. Recover ALSA card
   names, order, channels, and rates before implementing audio endpoints.
   Acceptance: browse/load a track, then advance playback and capture PCM.
5. **App integration.** Once input works, port provisioning and model/UI
   support. The current standalone boot already establishes the execution
   model; it does not establish usable audio performance.

Useful next disassembly anchors, all in stock 1.20:

| Address | Routine / finding |
|---|---|
| `0x365494`, `0x3655f4` | Eup `check1stData` / `checkData` |
| `0x365870`, `0x3659d0` | Sub `check1stData` / `checkData` |
| `0x2e2e30` | `PanelCommData::calulateCheckCrc`: reverses bytes within each 32-bit word before calling CRC16; use its virtual CRC address to determine coverage |
| `0x04186c` | `common_crc16::crc`: table-driven reflected update, initial `0xffff`, final complement; polynomial `0x8408` confirmed by executing stock code |
| `0x3645b8`, `0x3645c0` | logical Sub/Eup format sizes 28 / 100 bytes; do not equate these automatically with device transfer sizes |
| `0x37e4ac`, `0x37e570`, `0x37eb10` | Sub other/deck/mixer checks |
| `0x37ebf8`, `0x37eecc`, `0x37fb04` | Eup mixer/deck/other checks |

The framing and first-frame acceptance sequence below these anchors have
since been recovered and validated; [the protocol note](xdj-rx3-panel.md)
records the CRC offsets, startup behavior, button map, and live evidence.

Useful sources: Pioneer's XDJ-RX3 GPL package (kernel drivers, board file,
initramfs scripts), extracted at
the locally extracted `rx3-gpl/` source tree (not included).
That scratchpad is temporary: if it is gone, download the package again from
pioneerdj.com's open-source page (the two `pioneerdj_xdj_rx3.tar.bz2.*` ZIPs
listed in `~/src/rx3-toolkit/app/firmware/key_source.json`). The firmware's
own scripts are in the assembled root under `/etc/rc.d/` and `/root/pdj/`.

## Native hardware window and media QA (2026-11-08)

The native app now has a dedicated RX3 mode rather than using a CDJ codec:
`cdj3k-emu --rx3 <run-directory>`. `guest/rx3/open-window.sh <run-directory>`
builds a development macOS bundle with the required QEMU dylib. The window
attaches to an already running `run-dev.sh` guest. It does not yet provision or
own that guest's lifecycle. A per-run file lock prevents multiple windows from
silently queuing connections to QEMU's single-client panel socket.

The first vector hardware rendition uses a 1982 × 1280 reference canvas for
both decks, the mixer, the raised LCD, navigation, USB bank and microphone
section. Its proportions follow the RX3 manufacturer top-view photo (reference
only, not bundled as an asset). The real RGB565 firmware image is placed in the
LCD glass; the same scaled rectangle maps touch coordinates. Jog faces remain
blank because their separate ready-channel display protocol is unfinished.
Some utility knobs/selectors are visual placeholders, and lamp/meter feedback
has not been wired to the firmware's panel TX stream. This is not a finished
hardware emulation.

Two native-window bugs found during manual QA:

- Button pulses must survive repeated egui layout passes and the panel sample
  interval. Releases now happen in `App::logic` after at least 120 ms.
- `rx3fb.shm` generation is a **metadata** sequence, not a pixel/frame counter.
  DirectFB can draw through mmap without a pan ioctl. Caching pixels solely by
  generation froze the display and made accepted clicks appear ineffective.
  The viewer now samples pixels at 30 Hz, skips unchanged pixel buffers and
  reopens the framebuffer if the guest replaces its backing file.

Computer-use verification in the native hardware window: clicking SOURCE
opens the firmware SOURCE page with USB1 and its capacity; BROWSE switches the
screen; pressing the browse encoder opens USB1's FOLDER view and lists the
self-generated `Verification.wav`. Track loading/playback is not yet verified.
The current UI QA guest intentionally runs with audio disabled.

Example development UI QA launch:

```sh
RX3_AUDIO=0 RX3_USB_DIR="$PWD/build/rx3/media" \
  guest/rx3/run-dev.sh "$PWD/build/rx3/app"
# In another terminal:
guest/rx3/open-window.sh build/rx3/app
```

The host media directory is exposed through read-only QEMU VVFAT. RX3 derives
`/dev/sda1` from `/media/usb1/sda1` before asking blkid about the filesystem;
therefore the virtio partition needs the `/dev/sda1 -> /dev/vdc1` alias. Mount
through that name and notify `/proc/udev_usb1` with exactly
`mount /media/usb1/sda1` (no newline). Adding filesystem/protection suffixes
breaks this firmware's whole-path parser. A native panel client holds the
panel socket, so do not simultaneously use the Python panel CLI.

## Audio work still in progress

qemu-user lacked ARM32 ALSA ioctl translation. Patch 23 adds explicit
translation for PCM/control commands, native-long structures, transfer pointers,
and the old mmap-status fallback through SYNC_PTR. Native ALSA boundary sentinels
must be widened as well as their structure fields. A standalone ARM32 `aplay`
probe successfully played a one-second 48 kHz S32 stream through virtio sound.
That probe does not establish RX3 application playback.

RX3's fixed device identities are mapped by `board/asound.conf` to three virtio
outputs and four dummy capture substreams. All captures use Dummy device 0:
Dummy device 1 forces non-interleaved access, while this RX3 audio loop calls
`snd_pcm_readi` regardless. The opt-in `snd-dummy.model=rx3` kernel profile
provides 64-frame, 44.1/48/96 kHz S24/S32 stereo periods, preserving the larger
period used for other models. The virtio output driver now accepts low-24-bit
S24_LE slots and shifts them into S32 before transport. Using an ALSA `plug`
output exposed non-interleaved access that this firmware loop could not use;
the RX3 output aliases now name hardware PCM directly.

The RX3 also always reads `/dev/paudiog0` via the proprietary `'B'/0x51` ioctl.
Pioneer's published `f_paudio.h` defines a native-long `usb_xferi`; disconnected
reads return a silent four-channel block and result 0. The bridge and board
module implement that behavior rather than returning ENOTTY and terminating
rbp's audio thread.

With `RX3_AUDIO=1`, the stock player opens the devices and reaches
`audioDeviceAboutToStart()` at 44.1 kHz with 64-frame buffers. Its linked PCM
streams still enter XRUN recovery; successful continuous application audio is
**not** established. Jog rotation/LCD recovery, PCM clock/recovery behavior,
complete control/LED feedback, writable media and normal app lifecycle remain
required before calling this a fully working RX3 app.

### Physical DEADQA media QA

The attached exFAT volume `/Volumes/DEADQA` is currently exposed read-only on
USB1 through `build/rx3/media-deadqa`. This host directory contains symlinks to
the volume's non-hidden top-level entries, avoiding macOS's inaccessible
`.Spotlight-V100` directory while reading the music from the actual drive.
`RX3_USB_LABEL=DEADQA` labels the virtual FAT volume; `RX3_KEEP_DISKS=1` keeps
the running guest's existing root/board disks on a media-switch reboot.
The virtual FAT capacity is 504 MiB, not the physical USB drive's 62.9 GB;
this particular content set fits. This is a file-backed view, not raw exFAT
passthrough. QMP confirms USB1 is read-only and the guest mounts it `ro`.

Host and guest `PIONEER/rekordbox/export.pdb` MD5 both equal
`213381a869cb9959358b83086cdae6d7`. Native-window QA confirms SOURCE → USB1 →
browse encoder opens FOLDER and displays the drive's Contents/artist folders.
The firmware currently reports zero indexed songs/playlists, so rekordbox
library recognition is not established; folder access is verified. No files
were written to the physical drive.
