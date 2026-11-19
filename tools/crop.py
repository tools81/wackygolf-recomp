"""Crop the same region from several BMPs and place them side by side in one PNG.

Usage: python tools/crop.py out.png x y w h in1.bmp [in2.bmp ...]
"""
import struct, sys, zlib


def read(p):
    b = open(p, 'rb').read()
    off = struct.unpack('<I', b[10:14])[0]
    w, h = struct.unpack('<ii', b[18:26])
    top = h < 0
    h = abs(h)
    rows = [b[off + y * w * 4: off + (y + 1) * w * 4] for y in range(h)]
    if not top:
        rows.reverse()
    return w, h, rows


def main():
    out = sys.argv[1]
    x0, y0, cw, ch = map(int, sys.argv[2:6])
    ims = [read(p) for p in sys.argv[6:]]
    W = len(ims) * (cw + 4)
    canvas = [bytearray(W * 3) for _ in range(ch)]
    for k, (w, h, rows) in enumerate(ims):
        for y in range(ch):
            src = rows[y0 + y]
            for x in range(cw):
                p = (x0 + x) * 4
                o = (k * (cw + 4) + x) * 3
                canvas[y][o:o + 3] = bytes((src[p + 2], src[p + 1], src[p]))
    raw = b''.join(b'\0' + bytes(r) for r in canvas)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, ch, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)


if __name__ == '__main__':
    main()
