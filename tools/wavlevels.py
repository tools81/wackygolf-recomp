"""Print per-second RMS levels of a 16-bit WAV (quick audio sanity check)."""
import array, math, struct, sys
b = open(sys.argv[1], 'rb').read()
i = 12
while i < len(b):
    cid, sz = b[i:i + 4], struct.unpack('<I', b[i + 4:i + 8])[0]
    if cid == b'fmt ':
        fmt = struct.unpack('<HHIIHH', b[i + 8:i + 24])
    if cid == b'data':
        data = b[i + 8:i + 8 + sz]
        break
    i += 8 + sz
a = array.array('h', data)
sec = fmt[2] * fmt[1]
print(' '.join('%d' % math.sqrt(sum(x * x for x in a[s * sec:(s + 1) * sec:7]) / max(1, len(a[s * sec:(s + 1) * sec:7])))
               for s in range(len(a) // sec)))
