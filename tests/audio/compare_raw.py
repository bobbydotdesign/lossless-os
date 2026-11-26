"""Compare two 16-bit stereo recordings: a WAV (any header) and raw samples.
Prints both lengths and how far apart they are (signal to difference, dB).
usage: python3 compare_raw.py reference.wav ours.raw [min_db]
SPDX-License-Identifier: Apache-2.0
"""
import sys, array, math, struct

def wav_samples(path):
    d = open(path, 'rb').read()
    at = 12
    while at + 8 <= len(d):
        cid, size = d[at:at + 4], struct.unpack('<I', d[at + 4:at + 8])[0]
        if cid == b'data':
            return array.array('h', d[at + 8:at + 8 + size])
        at += 8 + size + (size & 1)
    raise SystemExit('no data chunk')

a = wav_samples(sys.argv[1])
b = array.array('h', open(sys.argv[2], 'rb').read())
n = min(len(a), len(b))
s = sum(a[i] * a[i] for i in range(0, n, 3)) or 1
e = sum((a[i] - b[i]) ** 2 for i in range(0, n, 3)) or 1
db = 10 * math.log10(s / e)
need = float(sys.argv[3]) if len(sys.argv) > 3 else 60
ok = len(a) == len(b) and db > need
print(('ok  ' if ok else 'BAD ') + '%d vs %d frames, %.1f dB apart' % (len(a) // 2, len(b) // 2, db))
sys.exit(0 if ok else 1)
