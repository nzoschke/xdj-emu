# XDJ-RX3 panel RX: recovered framing and deck buttons

Recovered from stock 1.20 `rbp` on 2026-11-08. SHA-256:
`3f0a1a9c4d107fcb856eb59bd77b39139b239b111f431d40d11b5c7e66dffddb`.
The encoder is [panel_frames.py](../guest/rx3/panel_frames.py); the executable
firmware oracle is [panel_oracle.py](../guest/rx3/panel_oracle.py).

Scope: the two main SPI panel channels. The 24-byte JogCom RX packets on
`subucom_spi_rdy3.0` / `rdy4.0` are a separate protocol and remain undecoded.

## Confirmed wire format

All offsets here are bytes in the buffer returned by the character device.
Fields consumed as halfwords are little endian.

| | EUP | SUB |
|---|---:|---:|
| Device | `/dev/subucom_spi1.0` | `/dev/subucom_spi2.0` |
| Host bridge channel | 0 | 1 |
| Device read size | 100 | 104 |
| Logical validator size | 100 | 28 |
| CRC-covered range | 0–95 | 0–23 |
| Excluded halfword | 96–97 | 24–25 |
| Stored CRC16 | 98–99 | 26–27 |
| Additional transfer padding | none | 28–103, ignored by this validator |

The CRC is reflected CRC16 with polynomial `0x8408`, initial value `0xffff`,
final XOR `0xffff`; `123456789` gives `0x906e` (CRC-16/X-25). Before CRC,
reverse the bytes **within each four-byte word** of the covered range:
`[a,b,c,d,e,f,g,h]` becomes `[d,c,b,a,h,g,f,e]`. Store the result as a
little-endian halfword at the offset above. The whole last four-byte word is
excluded; its first halfword can be zero.

Evidence: `PanelCommData::calulateCheckCrc` at `0x2e2e30`; CRC16 at `0x4186c`
with polynomial literals at `0x41a60`; EUP/SUB `getCrcAdress` at `0x364838` /
`0x364874`; comparisons at `0x365650` / `0x365a2c`.

No fixed magic/header or sequence progression is checked by these validators.
Bytes 2–3 are copied into the EUP/SUB version slots of `tStaticPanelInfo`
on the first accepted frame; their exact physical encoding is unconfirmed.
Zero is accepted. Do not invent a power-on header based on the CDJ protocol.

Diagnostic zero payloads, with working CRCs:

- EUP: 98 zero bytes followed by `ae cb` (100 bytes total).
- SUB: 26 zero bytes followed by `08 ae` (28 bytes total); pad with 76 zeros
  for the current 104-byte device read.

These are accepted diagnostic states, not a recreation of physical neutral:
zero analog values can move sliders, mixer assignments, and other settings.
A production panel model must preserve all control values while changing keys.

## Initialization and change delivery

`PanelComReciever::handleIncomingPanelComMessage` at `0x36442c` routes device
index 0 to EUP and 1 to SUB. Each checker starts with history index `0xffff`.
`check1stData` validates CRC (returns 5 on failure), records bytes 2–3, detects
EUP startup mode, then seeds history with the **bitwise complement** of the
covered payload. The caller immediately invokes `checkData` on the same frame,
so initial control states generate transitions. This explains the two samples
counted from one injected frame.

`checkData` validates CRC before invoking deck/mixer/other consumers, stores the
new sample in a two-entry history, and calls `IKeyManager::notify1stKeyHandled`
with mask 1 for EUP or mask 2 for SUB, once. The first-handled flag is checker
byte `+0x54`. Index is `u16 +0x40`, sample count is `u32 +0x44`.

Both the kernel emulation and `PanelComPeerLinux::read_data` suppress identical
buffers. No heartbeat is needed to deliver a changed button state in the tested
setup. A held key must stay set in subsequent states until release; send each
press and release as a separate write, separated by enough time for polling
(tested at 100–150 ms).

EUP `detectStartMode` at `0x38131c` reads byte 16/32 bit 2 into two panel flags.
Its special-mode combinations include byte 18 bit 3 with byte 19 bit 2 or 3,
byte 18 bits 3+7, and byte 94 bits 3+4. These produce modes 1/3, 2, and 4;
the caller has test/factory-reset/settings branches. Keep startup diagnostic
frames zero in those positions. SUB `detectStartMode` simply returns 0.

## Recovered deck keys

High bit = press, delivering firmware event type **0**; low = release, type
**2**. Channels are 1 and 2. A key bit is a state, not a pulse counter.
All 72 mappings (36 per deck) were checked by executing the stock decoder on
both rising and falling edges. Names below are supplied where established in
the prototype ABI; hex-only entries retain their firmware identity.

SUB deck base is 4 for deck 1, 8 for deck 2:

| Offset from deck base | Bit | Key ID | Control |
|---|---:|---|---|
| 2 | 0–7 | `0x4117`–`0x411e` | Pads A–H |
| 3 | 0 | `0x4116` | Beat Jump mode |
| 3 | 1 | `0x4115` | Slip Loop mode |
| 3 | 2 | `0x4114` | Auto Loop mode |
| 3 | 3 | `0x4113` | Hot Cue mode |
| 3 | 6 | `0x4101` | PLAY |
| 3 | 7 | `0x4102` | CUE |

EUP deck base is 4 for deck 1, 20 for deck 2:

| Offset from deck base | Bit | Key ID | Control |
|---|---:|---|---|
| 12 | 0 | `0x4306` | Jog touch |
| 13 | 0 | `0x4110` | Slip |
| 13 | 2 | `0x4121` | — |
| 13 | 3 | `0x410b` | Quantize |
| 13 | 4 | `0x411f` | Search forward |
| 13 | 5 | `0x4120` | Search reverse |
| 13 | 6 | `0x4214` | Track forward |
| 13 | 7 | `0x4215` | Track reverse |
| 14 | 0 | `0x4103` | — |
| 14 | 1 | `0x410f` | Reverse |
| 14 | 2 | `0x4108` | Master Tempo |
| 14 | 3 | `0x4107` | Tempo range |
| 14 | 4 | `0x4111` | Master |
| 14 | 5 | `0x4112` | Sync |
| 14 | 6 | `0x4104` | Vinyl |
| 14 | 7 | `0x410e` | Reloop |
| 15 | 0 | `0x410d` | Loop Out |
| 15 | 1 | `0x410c` | Loop In |
| 15 | 2 | `0x4125` | — |
| 15 | 3 | `0x4124` | — |
| 15 | 4 | `0x4322` | — |
| 15 | 5 | `0x4323` | — |

EUP offset 12 bit 1 enters jog rotation processing; it is not a mapped button.
Deck decoders are at `0x37e570` (SUB) and `0x37eecc` (EUP).

Additional layout established from the consumers: SUB mixer blocks start at
12/16; channel fader values are little-endian halfwords at 14/18, producing
key `0x501e`, event type 5. Crossfader at 22 produces `0x6017`. These analog
paths divide by 1023. EUP mixer blocks start at 36/52, with trim at block+2
(`0x5019`) and EQ high at block+0 (`0x501a`), also divided by 1023. Full analog,
selector, mixer-button, and jog calibration remains unfinished.

## Tools and reproduction

The encoder uses only Python's standard library. For example:

```sh
# Logical frame hex: EUP deck 1 QUANTIZE held down.
python3 guest/rx3/panel_frames.py eup --key 0x410b --deck 1 --pressed

# Binary host-link record: SUB deck 2 PLAY held down.
python3 guest/rx3/panel_frames.py sub --key 0x4101 --deck 2 --pressed --record > /tmp/rx3-play.rec
```

`--payload-hex` accepts the covered payload (96/24 bytes) to preserve the rest
of the control state. In Python, `with_key` changes one bit in an existing
payload; `encode` regenerates CRC; `link_record` wraps it for the host bridge.
A host record is `<u8 channel, u8 reserved, u16 length, u32 sequence>` followed
by the padded device buffer. The bridge currently ignores sequence. Write each
record separately inside the guest to `/dev/rx3_subucom`; do not write that
record directly to the player-facing SPI device.

The oracle does not require hardware, Docker, or a running guest:

```sh
python3 -m venv build/rx3/analysis-venv
build/rx3/analysis-venv/bin/pip install unicorn pyelftools
build/rx3/analysis-venv/bin/python guest/rx3/panel_oracle.py /path/to/stock/rbp
# Add --key-map to emit the recovered per-deck bit positions as JSON.
```

It verifies the ELF hash; executes the actual CRC routine and first/normal
validators on three payload patterns per kind; rejects corrupted CRCs; checks
history/readiness transitions; and executes both deck decoders to verify all
72 button mappings and release events. Allocation/libc are modeled and outgoing
key notifications captured. Field consumers are skipped only during validator
tests; the key-map tests execute the real deck consumers. This is a narrow
firmware oracle, not a whole-system emulator or proof of analog/jog behavior.

## Live confirmation

Used the existing guest at `build/rx3/re`, player PID 3838 (stock binary).
`UiObjectManager*` was read from `0x026867c0`, receiver from manager+`0x70`;
EUP checker is receiver+`0x9c`, SUB checker receiver+`0xf4`. These offsets are
specific to this firmware; inspect mappings and pointers before using them.

| Injection | EUP index / samples / first-handled | SUB index / samples / first-handled |
|---|---|---|
| Before RX | 65535 / 0 / 0 | 65535 / 0 / 0 |
| Valid zero diagnostic frame each | 1 / 2 / 1 | 1 / 2 / 1 |
| One bad-CRC frame each | 1 / 2 / 1 | 1 / 2 / 1 |
| Valid frame each again | 0 / 3 / 1 | 0 / 3 / 1 |

EUP QUANTIZE press/release at absolute byte 17 bit 3 visibly toggled deck 1's
QUANTIZE indicator. Byte 33 bit 3 independently toggled deck 2. Repeating both
gestures restored their indicators. The same player process remained alive.
Local captures: `build/rx3/panel-{idle,quantize,quantize-deck2,quantize-restored}.png`.
SUB acceptance is confirmed through its checker state; its individual buttons
were decoded in the oracle but have not all been exercised in the live UI.

A touch INFO tap still had no visible effect after initializing the panel.
Touch delivery needs its own trace. Next implementation work is a persistent
panel state source using this framing, followed by mixer/jog recovery and
media/playback tests; valid main-panel RX and deck button encoding are now
established.
