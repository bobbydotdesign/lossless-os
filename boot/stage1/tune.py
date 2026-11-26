#!/usr/bin/env python3
"""A copy of tune.bin trying one lane-1 gate setting.
   tune.py out.bin MASK VALUE [ADDR]   (hex; ADDR: another register, left set)"""
import sys
d = bytearray(open('tune.bin', 'rb').read())
i = d.find(b'TUNE')
d[i + 4:i + 8] = int(sys.argv[2], 16).to_bytes(4, 'little')
d[i + 8:i + 12] = int(sys.argv[3], 16).to_bytes(4, 'little')
d[i + 12:i + 16] = (1).to_bytes(4, 'little')
d[i + 16:i + 20] = (int(sys.argv[4], 16) if len(sys.argv) > 4 else 0).to_bytes(4, 'little')
open(sys.argv[1], 'wb').write(d)
