"""rx3fb2png.py <rx3fb.shm> <out.png>: decode the RX3 LCD (guest/modules/rx3_board/rx3fb.c)."""
import mmap, struct, sys, zlib
f = open(sys.argv[1], "rb"); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
magic, ver, gen, w, h, bpp, stride, yoff, pix = struct.unpack_from("<9I", m, 0)
ro, rl, go, gl, bo, bl = struct.unpack_from("<6B", m, 36)
assert magic == 0x46335852, hex(magic)
base = pix + yoff * stride
def ch(v, o, l): x = (v >> o) & ((1 << l) - 1); return (x * 255) // ((1 << l) - 1) if l else 0
rows = bytearray()
for y in range(h):
    rows.append(0); r0 = base + y * stride
    for x in range(w):
        v = struct.unpack_from("<H" if bpp == 16 else "<I", m, r0 + x * (bpp // 8))[0]
        rows += bytes((ch(v, ro, rl), ch(v, go, gl), ch(v, bo, bl)))
def c(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + c(b"IDAT", zlib.compress(bytes(rows), 6)) + c(b"IEND", b""))
print(f"gen={gen} {w}x{h} bpp={bpp} stride={stride} yoffset={yoff} rgb=({ro},{rl})({go},{gl})({bo},{bl})")
