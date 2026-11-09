#!/usr/bin/env python3
"""Persistent RX3 panel state and bring-up CLI over the dedicated panel socket.

  panel_control.py build/rx3/app/panel.sock source
  panel_control.py build/rx3/app/panel.sock load1
  panel_control.py build/rx3/app/panel.sock play1

Each CLI call persists its covered EUP/SUB state next to the socket. Native UI
clients should keep one Panel object and change one control at a time.
"""
import argparse
import json
import socket
import struct
import time
from pathlib import Path
from panel_frames import encode, link_record, with_key

GLOBAL_KEYS = {
    "source": (92, 7), "browse": (92, 6), "tag": (92, 5),
    "playlist": (92, 4), "search": (92, 3), "menu": (92, 2),
    "back": (94, 0), "info": (92, 4), "push": (93, 4),
    "load1": (93, 6), "load2": (93, 5), "beatfx": (91, 2),
}


class Panel:
    def __init__(self):
        self.eup = bytearray(96)
        self.sub = bytearray(24)
        # Analog center/level defaults, separate from all-zero diagnostic RX.
        for base in (4, 20):
            struct.pack_into("<HH", self.eup, base, 0x1FF8, 0)
            struct.pack_into("<H", self.eup, base + 6, 0x1FF8)
        for base in (36, 52):
            for offset in (0, 2, 4, 6, 8):
                struct.pack_into("<H", self.eup, base + offset, 512)
        struct.pack_into("<H", self.eup, 74, 768)  # master level
        struct.pack_into("<H", self.eup, 80, 512)  # headphones
        struct.pack_into("<H", self.eup, 82, 512)  # cue/master mix
        for base in (12, 16):
            struct.pack_into("<H", self.sub, base + 2, 1023)  # channel faders
        struct.pack_into("<H", self.sub, 22, 512)  # crossfader center

    def records(self):
        return [link_record("eup", encode("eup", self.eup)),
                link_record("sub", encode("sub", self.sub))]

    def send(self, sock):
        for rec in self.records():
            sock.sendall(rec)

    def button(self, name, down):
        if name in GLOBAL_KEYS:
            offset, bit = GLOBAL_KEYS[name]
            self.eup[offset] = (self.eup[offset] | (1 << bit)) if down else (self.eup[offset] & ~(1 << bit))
        else:
            deck = int(name[-1])
            key = {"play": 0x4101, "cue": 0x4102, "quantize": 0x410B,
                   "sync": 0x4112, "hotcue": 0x4113, "loop": 0x4114}[name[:-1]]
            kind = "sub" if key in (0x4101, 0x4102, 0x4113, 0x4114) else "eup"
            setattr(self, kind, bytearray(with_key(kind, getattr(self, kind), key, deck, down)))

    def browse(self, delta):
        value = struct.unpack_from("<H", self.eup, 84)[0]
        struct.pack_into("<H", self.eup, 84, (value + delta) & 65535)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket")
    parser.add_argument("control", help="source/browse/back/push/load1/play1/cue1/quantize1/... or turn")
    parser.add_argument("delta", nargs="?", type=int, default=1)
    args = parser.parse_args()
    state = Path(args.socket).with_suffix(".state.json")
    panel = Panel()
    if state.exists():
        saved = json.loads(state.read_text())
        for kind, size in (("eup", 96), ("sub", 24)):
            payload = bytes.fromhex(saved[kind])
            if len(payload) != size:
                raise ValueError("invalid saved panel state")
            setattr(panel, kind, bytearray(payload))
    with socket.socket(socket.AF_UNIX) as sock:
        sock.connect(args.socket)
        if args.control == "turn":
            panel.browse(args.delta)
            panel.send(sock)
        else:
            panel.button(args.control, True)
            panel.send(sock)
            time.sleep(0.12)
            panel.button(args.control, False)
            panel.send(sock)
    state.write_text(json.dumps({"eup": panel.eup.hex(), "sub": panel.sub.hex()}))


if __name__ == "__main__":
    main()
