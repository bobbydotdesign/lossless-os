#!/usr/bin/env python3
"""Make the card read check for test 4: check.bin (4 MB of fixed random
bytes, an odd size so the last block is partial) and check.txt ("<size>
<crc32 hex>"). The H2 reads check.bin and compares.

  python3 tools/make_check_file.py out/h2-test4

SPDX-License-Identifier: Apache-2.0
"""
import os
import random
import sys
import zlib

out = sys.argv[1] if len(sys.argv) > 1 else "out/h2-test4"
os.makedirs(out, exist_ok=True)
r = random.Random(4)
data = bytes(r.getrandbits(8) for _ in range(4 * 1024 * 1024 + 1234))
open(os.path.join(out, "check.bin"), "wb").write(data)
open(os.path.join(out, "check.txt"), "w").write("%d %08x\n" % (len(data), zlib.crc32(data)))
print("%s: %d bytes, crc32 %08x" % (out, len(data), zlib.crc32(data)))
