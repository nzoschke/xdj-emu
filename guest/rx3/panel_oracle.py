#!/usr/bin/env python3
"""Execute stock RX3 1.20 validators and deck-key decoders in Unicorn.

Usage: python panel_oracle.py /path/to/stock/rbp [--key-map]
Dependencies: unicorn, pyelftools (install in a local analysis venv).

Only accepts the analyzed ELF hash. Allocation and libc calls are modeled;
key sinks and readiness notifications are captured. During validator tests,
field consumers are skipped: live device tests are required separately. During
key mapping, the real deck decoder executes against a minimal history object.
No firmware code or validator checks are patched, and no device is contacted.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_C1_C0_2,
    UC_ARM_REG_FPEXC, UC_CPU_ARM_CORTEX_A9,
)
from panel_frames import crc16, encode, KEY_BITS

STOCK_SHA256 = "3f0a1a9c4d107fcb856eb59bd77b39139b239b111f431d40d11b5c7e66dffddb"
FIELD_CONSUMERS = {0x37EECC, 0x37EBF8, 0x37FB04, 0x37E570, 0x37EB10, 0x37E4AC}
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
STOP, KEY_SINK = 0x100FE000, 0x100FF000


class Harness:
    def __init__(self, path):
        if hashlib.sha256(Path(path).read_bytes()).hexdigest() != STOCK_SHA256:
            raise ValueError("ELF is not the analyzed stock RX3 1.20 player")
        self.u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        with open(path, "rb") as source:
            elf = ELFFile(source)
            segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
            end = max(s["p_vaddr"] + s["p_memsz"] for s in segments)
            self.u.mem_map(0, (end + 4095) // 4096 * 4096)
            for segment in segments:
                self.u.mem_write(segment["p_vaddr"], segment.data())
        self.u.mem_map(0x10000000, 0x1000000)
        self.heap = 0x10100000
        self.u.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
        self.u.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.u.hook_add(UC_HOOK_CODE, self.hook)
        self.skip_fields = True
        self.keys, self.ready = [], []

    def wr(self, addr, value):
        self.u.mem_write(addr, struct.pack("<I", value))

    def rd(self, addr):
        return struct.unpack("<I", self.u.mem_read(addr, 4))[0]

    def hook(self, uc, addr, size, _):
        if addr < 0xE000:
            raise RuntimeError(f"unexpected target {addr:#x}")
        r = [uc.reg_read(reg) for reg in REGS]
        if self.skip_fields and addr in FIELD_CONSUMERS:
            value = 0
        elif addr in (0xE328, 0xEAA8):  # operator new / new[]
            value = self.heap
            self.heap += (r[0] + 15) // 16 * 16
        elif addr == 0xE970:  # memset
            uc.mem_write(r[0], bytes([r[1] & 255]) * r[2])
            value = r[0]
        elif addr == 0xF300:  # memcpy
            uc.mem_write(r[0], bytes(uc.mem_read(r[1], r[2])))
            value = r[0]
        elif addr == 0xECD0:  # operator delete
            value = 0
        elif addr == 0x37C8D8:  # IKeyManager::notify1stKeyHandled
            self.ready.append(r[1])
            value = 0
        elif addr == KEY_SINK:
            self.keys.append(tuple(r[1:]))  # id, event type, channel
            value = 0
        elif addr in (0xE6F4, 0xF5AC):
            raise RuntimeError("firmware entered a C++ exception path")
        else:
            return
        uc.reg_write(UC_ARM_REG_R0, value)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def call(self, addr, *args):
        for reg, value in zip(REGS, args):
            self.u.reg_write(reg, value)
        self.u.reg_write(UC_ARM_REG_SP, 0x10FFF000)
        self.u.reg_write(UC_ARM_REG_LR, STOP)
        try:
            self.u.emu_start(addr, STOP, count=1000000)
        except Exception as exc:
            raise RuntimeError(f"firmware stopped at {self.u.reg_read(UC_ARM_REG_PC):#x}") from exc
        if self.u.reg_read(UC_ARM_REG_PC) != STOP:
            raise RuntimeError("instruction limit reached")
        return self.u.reg_read(UC_ARM_REG_R0)

    def setup(self, kind):
        self.obj, self.buf = 0x10000000, 0x10001000
        self.u.mem_write(self.obj, bytes(0x8000))
        outer, inner = (0x4E0968, 0x4DD118) if kind == "eup" else (0x4E09A8, 0x4DD170)
        self.wr(self.obj, outer)
        self.wr(self.obj + 0x30, inner)
        self.wr(self.obj + 0x3C, 0x00020001)  # overwrite enabled, history size 2
        self.wr(self.obj + 0x40, 0xFFFF)  # no previous frame
        self.wr(self.obj + 0x48, 0x10002000)  # two 12-byte PanelRxData records
        self.wr(self.obj + 0x4C, 0x10003000)  # receiver
        self.wr(self.obj + 0x50, 0x10004000)  # static panel info
        for offset in (0, 12):
            self.wr(0x10002000 + offset, inner)
        self.wr(0x10003064, 0x10005000)  # receiver key manager
        self.wr(0x10005000, 0x10006000)
        self.wr(0x10006008, KEY_SINK)  # virtual key notification
        self.first = 0x365494 if kind == "eup" else 0x365870
        self.normal = 0x3655F4 if kind == "eup" else 0x3659D0

    def frame(self, frame, first=False):
        self.u.mem_write(self.buf, bytes(frame))
        return self.call(self.first if first else self.normal, self.obj, self.buf, len(frame))

    def state(self):
        return self.rd(self.obj + 64) & 65535, self.rd(self.obj + 68), self.u.mem_read(self.obj + 84, 1)[0]


def verify(harness):
    h = harness
    h.u.mem_write(0x10008000, b"123456789")
    assert h.call(0x4186C, 0x10008000, 9) == crc16(b"123456789") == 0x906E
    for kind, size in (("eup", 96), ("sub", 24)):
        # Nonuniform payloads exercise word reversal, not just the CRC polynomial.
        for payload in (bytes(size), bytes(range(size)), bytes((i * 73 + 19) & 255 for i in range(size))):
            h.setup(kind)
            frame = encode(kind, payload)
            bad = bytearray(frame)
            bad[-1] ^= 1
            assert h.frame(bad, first=True) == 5 and h.state() == (65535, 0, 0)
            assert h.frame(frame, first=True) in range(5) and h.state() == (0, 1, 0)
            h.frame(frame)
            assert h.state() == (1, 2, 1)
            before = h.state()
            h.frame(bad)
            assert h.state() == before
            h.frame(frame)
            assert h.state() == (0, 3, 1)
        print(f"{kind}: stock first/normal validators accept 3 payloads; bad CRCs leave history unchanged")
    assert h.ready == [1, 1, 1, 2, 2, 2]


def map_keys(harness):
    h = harness
    h.skip_fields = False
    output = []
    for kind, offsets, function in (
        ("sub", range(4, 12), 0x37E570),
        ("eup", list(range(16, 20)) + list(range(32, 36)), 0x37EECC),
    ):
        for offset in offsets:
            for bit in range(8):
                # This is a jog direction flag, which enters the rotary history
                # path; the minimal fixture models key delivery only.
                if kind == "eup" and offset in (16, 32) and bit == 1:
                    continue
                h.setup(kind)
                h.wr(h.obj + 0x34, h.buf)
                h.wr(h.obj + 0x40, 0)
                h.wr(0x10002004, 0x10007000)
                h.u.mem_write(h.buf + offset, bytes([1 << bit]))
                h.keys = []
                for deck in (1, 2):
                    h.call(function, h.obj, deck)
                press = h.keys[:]
                # Reverse the edge to verify release semantics independently.
                h.u.mem_write(h.buf + offset, b"\0")
                h.u.mem_write(0x10007000 + offset, bytes([1 << bit]))
                h.keys = []
                for deck in (1, 2):
                    h.call(function, h.obj, deck)
                assert h.keys == [(key, 2, deck) for key, event, deck in press]
                assert all(event == 0 for key, event, deck in press)
                for key, event, deck in press:
                    rel_offset, expected_bit = KEY_BITS[kind][key]
                    stride = 16 if kind == "eup" else 4
                    assert (offset, bit) == (rel_offset + (deck - 1) * stride, expected_bit)
                if press:
                    output.append({"kind": kind, "offset": offset, "bit": bit,
                                   "events": [{"id": f"0x{key:04x}", "channel": deck,
                                               "press_type": event, "release_type": 2}
                                              for key, event, deck in press]})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rbp")
    parser.add_argument("--key-map", action="store_true", help="emit deck key bit map as JSON")
    args = parser.parse_args()
    harness = Harness(args.rbp)
    if args.key_map:
        print(json.dumps(map_keys(harness), indent=2))
    else:
        verify(harness)
        mappings = map_keys(harness)
        print(f"{len(mappings)} deck button bits match encoder map; high=press type 0, low=release type 2")


if __name__ == "__main__":
    main()
