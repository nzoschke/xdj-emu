# XDJ-RX3 support (design)

> Status: design + first building blocks. The RX3 is emulated the same way as
> the CDJ models: its own firmware userland boots on a stock kernel, and the
> player talks to emulated copies of its own hardware, driven by the
> on-screen device. There is no MIDI-controller path.

## Why the RX3 is different

| | CDJ-3000 / 3000X / 1500X | XDJ-RX3 |
|---|---|---|
| SoC | RK3399 / RK3566, AArch64 | i.MX6 Quad, ARMv7 (soft-float, glibc 2.13) |
| On Apple silicon | runs natively under HVF | **cannot**: Apple cores have no AArch32 |
| Firmware container | LUKS1 `.UPD` | AES-256-CBC per 512-byte sector `.UPD` (see below) |
| Panel link | one `subucom_spi` channel | two `subucom_spi` channels + two `subucom_spi_rdy` lines |
| Screens | main LCD + jog LCD (DRM) | one 1280×800 touchscreen (fbdev, `tsc2007` touch) |

## Execution model

A stock AArch64 kernel runs under HVF (as for the CDJs). The RX3's whole
userland (busybox `init` → `/etc/rc.d/rcS` → `rc.local` →
`/root/pdj/apl_start` → `rbp`) runs as ARM32 code through `qemu-arm`
user-mode translation, registered with `binfmt_misc` using the `F` flag so it
works inside the firmware root.

Full-system ARMv7 emulation (TCG, an i.MX6 board model) was rejected: it runs
at roughly a quarter of real time, which audio cannot tolerate.

Feasibility was checked on 2026-11-08: the 1.20 player boots to its home
screen in an AArch64 HVF guest under translation, and its framebuffer reaches
the host through this project's `-display shm` path unchanged.

## Firmware

`crates/cdj3k-emu-firmware/src/rx3.rs`:

* key = first line of the player's `aes256.key`, cut to 31 bytes, NUL-padded
  to 32;
* drop the 16-byte trailer; each 512-byte sector `n` is AES-256-CBC with
  IV = `n` (u32 LE) ‖ 12 zero bytes;
* the result is ISO 9660 (`CD001` at byte 32769). It holds
  `images/rootfs.cramfs` (LE zlib cramfs), `images/pdj.tar.gz` (`rbp`,
  scripts), `images/gui.tar.gz`, `images/uImage`, and `SUB.mot` / `EUP.mot`
  (sub-CPU firmware, not run).

Still to port: a cramfs reader and root-filesystem assembly at provision time.

## Hardware the player opens

From the strings of the 1.20 `rbp`:

| node | real driver (GPL tree, `linux-3.0.101`) | emulation plan |
|---|---|---|
| `/dev/subucom_spi1.0`, `/dev/subucom_spi2.0` | `arch/arm/mach-mx6/subucom_spi.c` | `subucom_virt` RX3 mode, same ABI |
| `/dev/subucom_spi_rdy3.0`, `/dev/subucom_spi_rdy4.0` | `arch/arm/mach-mx6/subucom_spi_rdy.c` | `subucom_virt` RX3 mode |
| `/dev/tsc2007_2-0048` | `drivers/input/touchscreen/tsc2007.c` (Pioneer char-dev variant) | small module fed from host touch |
| `/dev/fb0` | mxc fb + Vivante DirectFB | virtio-gpu fbdev → `main.shm` |
| `/dev/gpiodrv`, `/dev/printkdrv0`, `/dev/mem` | Pioneer misc drivers | stubs |
| `/dev/hidg0`, `/dev/paudiog0` | USB gadget (PC link) | out of scope for now |
| ALSA | i.MX SSI/ESAI + codecs | dj-emu `virtio_snd` |

### subucom ABI (from `subucom_spi.h`, `subucom_spi_rdy.h`)

* `read()` returns the latest RX frame; a kernel timer polls the bus at the
  interval userland sets.
* `ioctl(SUBUCOM_IOC_MESSAGE(N))`, magic `'p'` nr 0: an array of
  `struct subucom_ioc_transfer { u8 multi_id, _reserved, part_id, part_num;
  u32 bytes; u64 tx_buf; }` queues TX data (multiplexed, partitioned).
* `RD/WR_TIMER_STATUS` (nr 1, u8), `RD/WR_TIMER_INTERVAL` (nr 2, u32),
  `RD/WR_BITS_PER_WORD` (nr 3, u16), `RD/WR_RX_BYTES` (nr 4, u32);
  `spi_rdy` adds `RD/WR_TX_BUF_STAT` (nr 6, u8) and `RD/WR_RDY_COUNT` (nr 7, u32).

### qemu-arm and these ioctls

qemu-user forwards only ioctls it knows. The scalar ones above can be added to
its ioctl table. `SUBUCOM_IOC_MESSAGE` carries a guest pointer inside the
struct (like spidev's `SPI_IOC_MESSAGE`, which upstream does not support), so
it needs a custom handler in `linux-user` that copies `tx_buf` from guest
memory. The struct uses fixed-width types, so the layout is identical on
ARM32 and AArch64; only the pointer needs translating. This is a patch in
`qemu/patches/` for the guest-side `qemu-arm` build.

## Open work, in order

1. `xdjrx3` model and argv branch; boot the RX3 userland in the app window.
2. Provisioning in Rust: cramfs, root assembly, `qemu-arm` + modules overlay.
3. `subucom_virt` RX3 mode + qemu-arm ioctl patch; log real traffic.
4. RX3 frame codec (`MisoCodec`/`MosiCodec`), from `rbp` + traffic logs.
5. Touch, audio, USB media events.
6. RX3 slate: two decks + mixer; two-deck `PanelState`.

## References (read, not copied)

* rbpi4b / rx3-ddj-sb3 (MIT): proved `rbp` runs under qemu-arm on an AArch64
  HVF guest. Its controller (MIDI) path is deliberately not used here.
* Pioneer XDJ-RX3 GPL source package (pioneerdj.com): driver ABIs above.
