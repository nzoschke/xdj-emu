# xdj-emu

Experimental emulation of the **Pioneer DJ XDJ-RX3**, running its unmodified
firmware in QEMU with a native hardware-shaped window.

This project builds on [nsaintot/cdj3k-emu](https://github.com/nsaintot/cdj3k-emu).
Its Git history, CDJ implementations, build infrastructure, attribution, and
licenses are preserved. See [the upstream README](README.upstream.md) for the
inherited CDJ features; those descriptions do not imply RX3 feature parity.

## Current status

RX3 support is a development bring-up, verified on Apple Silicon macOS:

- Stock firmware boots through its own init scripts and reaches the main UI.
- The native RX3 hardware window shows the real firmware LCD and sends panel
  buttons, browse encoder, mixer controls, and touch input.
- Main-panel framing, CRCs, and 72 deck-button bits are checked against the
  actual firmware validators/decoders.
- Read-only USB folder browsing works, including files on a connected physical
  drive exposed through a virtual FAT view.

**Not yet a fully working RX3:** continuous audio playback still enters XRUN
recovery; jog rotation and jog LCDs are unfinished; some drawn controls are
placeholders, and firmware LED/meter feedback is not connected. Track playback
and complete rekordbox library recognition are not established. The RX3 window
currently attaches to a separately launched guest, outside the normal CDJ
model-selection/install flow.

No Pioneer firmware, decryption keys, music, or generated disk images are
included. Supply your own firmware and key. RX3's updater format differs from
the CDJ format; see the firmware notes below.

## Development setup

You need Rust, Docker, and the host build dependencies described in the
[upstream build instructions](README.upstream.md). RX3's development launcher
currently uses macOS HVF and an AArch64 guest. From the repository root:

```sh
# Build patched host QEMU, the kernel, and board modules.
./qemu/build.sh
./build.sh --artifacts-only

# Supply a locally decrypted RX3 firmware ISO.
guest/rx3/build.sh --firmware /path/to/XDJRX3.iso

# Boot a UI QA guest with an optional read-only music folder.
RX3_AUDIO=0 RX3_USB_DIR=/path/to/music \
  guest/rx3/run-dev.sh "$PWD/build/rx3/app"

# In another terminal, build and open the native RX3 window.
guest/rx3/open-window.sh build/rx3/app
```

The main LCD is touchable. SOURCE selects USB1; click the browse encoder to
enter a source/folder, and scroll or drag it to move through entries. All build
outputs stay under ignored directories. `RX3_AUDIO=1` enables the experimental
audio path for investigation; use `0` for UI QA.

The current media bridge has a 504 MiB virtual FAT capacity, even when the
physical source drive is larger. Do not point it at an arbitrary large music
library. The [bring-up guide](docs/xdj-rx3-bringup.md) documents the physical-drive
view, mount/event ABI, diagnostics, and remaining limitations.

## Source structure

| Area | Purpose |
|---|---|
| `crates/cdj3k-emu-firmware/src/rx3.rs` | RX3 updater decryption and ISO validation |
| `guest/rx3/` | Filesystem assembly, boot shim, media setup, panel bridge, and developer tools |
| `guest/modules/rx3_board/` | Pioneer devices, GPIO, framebuffer, touch, and USB gadget stubs |
| `qemu/patches/22-*`, `23-*` | ARM32 userland compatibility and device/ALSA ioctl translation |
| `crates/cdj3k-emu-panel/src/rx3.rs` | Native EUP/SUB panel frame encoding |
| `guest/rx3/panel_oracle.py` | Real firmware validator and button-decoder verification |
| `app/cdj3k-emu/src/rx3/` | Vector hardware rendering around the live LCD |
| `guest/modules/virtio_snd/`, `guest/kernel-patches/05-*` | Experimental RX3 audio formats and capture timing |

Existing `cdj3k-emu` crate and executable names are retained to keep the RX3
changes focused and upstream updates manageable.

## Research and verification

- [Architecture and firmware format](docs/xdj-rx3.md)
- [Boot findings, reproduction, and current verification](docs/xdj-rx3-bringup.md)
- [Recovered panel protocol and firmware oracle](docs/xdj-rx3-panel.md)

Panel verification can run without hardware, using your own extracted stock
RX3 1.20 `rbp` and Python's `unicorn` / `pyelftools` packages:

```sh
cargo test -p cdj3k-emu-panel rx3
python3 guest/rx3/panel_oracle.py /path/to/rbp
```

Inherited release packaging is manual during RX3 bring-up; this repository does
not currently offer a supported RX3 binary release.

## Attribution and licenses

Based on [nsaintot/cdj3k-emu](https://github.com/nsaintot/cdj3k-emu), with RX3
support developed in this repository. Preserve [NOTICE](NOTICE),
[LICENSE-MIT](LICENSE-MIT), [LICENSE-APACHE](LICENSE-APACHE), and the license
headers/notices applicable to individual components, including QEMU and kernel
code. This project is not affiliated with or endorsed by Pioneer DJ or
AlphaTheta.
