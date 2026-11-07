"""PGM (P5) -> PNG, optionally downscaled by an integer factor (box filter)."""
import struct, sys, zlib

src, dst = sys.argv[1], sys.argv[2]
f = int(sys.argv[3]) if len(sys.argv) > 3 else 1
data = open(src, 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
if f > 1:
    W, H = w // f, h // f
    out = bytearray(W * H)
    for y in range(H):
        rows = [px[(y * f + j) * w:(y * f + j) * w + W * f] for j in range(f)]
        for x in range(W):
            s = 0
            for row in rows:
                s += sum(row[x * f:x * f + f])
            out[y * W + x] = s // (f * f)
    w, h, px = W, H, bytes(out)
raw = b''.join(b'\0' + px[y * w:(y + 1) * w] for y in range(h))
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
