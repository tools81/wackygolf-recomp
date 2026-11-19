"""Find frame dumps showing the clubhouse (teal walls) with a light bottom bar.

Usage: python tools/findbar.py DIR
"""
import glob, os, struct, sys


def read(p):
    b = open(p, 'rb').read()
    off = struct.unpack('<I', b[10:14])[0]
    w, h = struct.unpack('<ii', b[18:26])
    return w, abs(h), b[off:]


def px(data, w, x, y):
    o = (y * w + x) * 4
    return data[o + 2], data[o + 1], data[o]


hits = []
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.bmp'))):
    w, h, d = read(f)
    white = sum(1 for y in range(205, 235, 3) for x in range(0, w, 8) if min(px(d, w, x, y)) > 225)
    total = len(range(205, 235, 3)) * len(range(0, w, 8))
    teal = sum(1 for y in range(60, 120, 6) for x in range(0, w, 16)
               if (lambda c: c[0] < 60 and c[1] > 190 and 140 < c[2] < 200)(px(d, w, x, y)))
    if white > total * 0.6 and teal > 20:
        hits.append(os.path.basename(f))
print(len(hits), 'hits', hits[:10])
