#!/usr/bin/env python3
"""RX3 1.20 main-panel RX framing recovered from rbp's Sub/Eup validators.

This covers spi1/spi2, not the separate JogCom ready channels. Payload defaults
are diagnostic zeros, not a calibrated physical panel state. Firmware is never
modified. Run with --help to generate a record for /dev/rx3_subucom.
"""
import argparse
import struct
import sys

# logical length, channel index, device read length, covered bytes
FORMATS = {"eup": (100, 0, 100, 96), "sub": (28, 1, 104, 24)}

# key id -> (absolute byte offset for deck 1, bit); deck 2 strides 4/16.
# Extracted by executing stock checkDeckData; see panel_oracle.py.
KEY_BITS = {
    "sub": {
        0x4117: (6, 0),
        0x4118: (6, 1),
        0x4119: (6, 2),
        0x411A: (6, 3),
        0x411B: (6, 4),
        0x411C: (6, 5),
        0x411D: (6, 6),
        0x411E: (6, 7),
        0x4116: (7, 0),
        0x4115: (7, 1),
        0x4114: (7, 2),
        0x4113: (7, 3),
        0x4101: (7, 6),
        0x4102: (7, 7),
    },
    "eup": {
        0x4306: (16, 0),
        0x4110: (17, 0),
        0x4121: (17, 2),
        0x410B: (17, 3),
        0x411F: (17, 4),
        0x4120: (17, 5),
        0x4214: (17, 6),
        0x4215: (17, 7),
        0x4103: (18, 0),
        0x410F: (18, 1),
        0x4108: (18, 2),
        0x4107: (18, 3),
        0x4111: (18, 4),
        0x4112: (18, 5),
        0x4104: (18, 6),
        0x410E: (18, 7),
        0x410D: (19, 0),
        0x410C: (19, 1),
        0x4125: (19, 2),
        0x4124: (19, 3),
        0x4322: (19, 4),
        0x4323: (19, 5),
    },
}


def with_key(kind, payload, key, deck, pressed):
    """Change one known button bit, preserving the rest of the panel state."""
    if deck not in (1, 2):
        raise ValueError("deck must be 1 or 2")
    payload = bytearray(payload)
    if len(payload) != FORMATS[kind][3]:
        raise ValueError("wrong covered payload length")
    offset, bit = KEY_BITS[kind][key]
    offset += (deck - 1) * (16 if kind == "eup" else 4)
    if pressed:
        payload[offset] |= 1 << bit
    else:
        payload[offset] &= ~(1 << bit)
    return bytes(payload)


def crc16(data):
    """Reflected 0x1021, init/xorout ffff (CRC-16/X-25)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc ^ 0xFFFF


def checksum(payload):
    """PanelCommData reverses each covered word before calling CRC16."""
    if len(payload) % 4:
        raise ValueError("CRC payload must contain whole 32-bit words")
    return crc16(b"".join(payload[i:i + 4][::-1] for i in range(0, len(payload), 4)))


def encode(kind, payload=None):
    size, _, _, covered = FORMATS[kind]
    if payload is None:
        payload = bytes(covered)
    if len(payload) != covered:
        raise ValueError(f"{kind} requires {covered} covered bytes")
    return bytes(payload) + struct.pack("<HH", 0, checksum(payload))


def valid(kind, frame):
    size, _, _, covered = FORMATS[kind]
    return len(frame) >= size and struct.unpack_from("<H", frame, size - 2)[0] == checksum(frame[:covered])


def link_record(kind, frame, seq=0):
    size, chan, read_size, _ = FORMATS[kind]
    if len(frame) != size or not valid(kind, frame):
        raise ValueError("expected one logical frame with valid CRC")
    wire = frame.ljust(read_size, b"\0")
    return struct.pack("<BBHI", chan, 0, len(wire), seq) + wire


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=FORMATS)
    parser.add_argument("--payload-hex", help="covered bytes, before CRC word; defaults to diagnostic zeros")
    parser.add_argument("--record", action="store_true", help="emit binary host-link record instead of frame hex")
    parser.add_argument("--key", type=lambda s: int(s, 0), help="firmware key id, e.g. 0x410b for EUP QUANTIZE")
    parser.add_argument("--deck", type=int, choices=(1, 2), default=1)
    parser.add_argument("--pressed", action="store_true", help="set the key bit; default clears it")
    args = parser.parse_args()
    payload = bytes.fromhex(args.payload_hex) if args.payload_hex else None
    if args.key is not None:
        if args.key not in KEY_BITS[args.kind]:
            parser.error("key id is not mapped for this panel")
        payload = with_key(args.kind, payload or bytes(FORMATS[args.kind][3]), args.key, args.deck, args.pressed)
    frame = encode(args.kind, payload)
    if args.record:
        sys.stdout.buffer.write(link_record(args.kind, frame))
    else:
        print(frame.hex())


if __name__ == "__main__":
    main()
