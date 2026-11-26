#!/usr/bin/env python3
"""Make a copy of pset.bin with power-chip writes patched in.
   pset.py out.bin REG=MASK:VALUE ...   (hex), e.g. 91=01:01"""
import sys
d = bytearray(open('pset.bin', 'rb').read())
i = d.find(b'PWRS')
ws = [tuple(int(x, 16) for x in a.replace('=', ':').split(':')) for a in sys.argv[2:]]
d[i + 4:i + 8] = len(ws).to_bytes(4, 'little')
for k, (r, m, v) in enumerate(ws):
    d[i + 8 + 3 * k:i + 11 + 3 * k] = bytes((r, m, v))
open(sys.argv[1], 'wb').write(d)
