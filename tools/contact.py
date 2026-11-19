"""Tile frame dumps (saves/frame_*.bmp) into one PNG contact sheet.

Usage: python tools/contact.py out.png [dir] [columns] [scale-down]
"""
import glob, os, struct, sys, zlib


def read_bmp(p):
    b = open(p, 'rb').read()
    off = struct.unpack('<I', b[10:14])[0]
    w, h = struct.unpack('<ii', b[18:26])
    bpp = struct.unpack('<H', b[28:30])[0]
    top_down = h < 0
    h = abs(h)
    stride = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(h):
        r = b[off + y * stride: off + y * stride + w * bpp // 8]
        rows.append(r)
    if not top_down:
        rows.reverse()
    return w, h, bpp // 8, rows


def main():
    out = sys.argv[1]
    d = sys.argv[2] if len(sys.argv) > 2 else 'saves'
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    sx = int(sys.argv[4]) if len(sys.argv) > 4 else 4    # horizontal decimation (768 wide)
    sy = sx // 2 or 1                                     # 240 lines: half the vertical decimation
    files = sorted(glob.glob(os.path.join(d, 'frame_*.bmp')))
    if not files:
        sys.exit('no frames')
    w, h, bp, _ = read_bmp(files[0])
    tw, th = w // sx, h // sy
    rows_n = (len(files) + cols - 1) // cols
    W, H = cols * (tw + 2), rows_n * (th + 2)
    canvas = [bytearray(W * 3) for _ in range(H)]
    for k, f in enumerate(files):
        fw, fh, bp, rows = read_bmp(f)
        ox, oy = (k % cols) * (tw + 2), (k // cols) * (th + 2)
        for y in range(th):
            src = rows[y * sy]
            dst = canvas[oy + y]
            for x in range(tw):
                p = x * sx * bp
                dst[(ox + x) * 3:(ox + x) * 3 + 3] = bytes((src[p + 2], src[p + 1], src[p]))
    raw = b''.join(b'\0' + bytes(r) for r in canvas)

    def chunk(t, dd):
        c = struct.pack('>I', len(dd)) + t + dd
        return c + struct.pack('>I', zlib.crc32(t + dd) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)
    print(out, len(files), 'frames')


if __name__ == '__main__':
    main()
