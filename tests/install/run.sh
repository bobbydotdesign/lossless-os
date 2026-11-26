#!/bin/sh
# The installer's tests: path rules, and real files (ours from out/, a
# broken copy of ours, something else, our update file and a stand-in for
# the factory's).
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
[ -f out/lossless.bin ] || { echo "needs out/lossless.bin (see docs/building.md)"; exit 1; }
T=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -Iapp -o "$T/test_bootfile" tests/install/test_bootfile.c app/bootfile.c
python3 - out/lossless.bin "$T" <<'PY'
import os, struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
t = sys.argv[2]
b = bytearray(d); b[5000] ^= 0xff                     # one byte changed: the CRC no longer matches
open(os.path.join(t, "broken.bin"), "wb").write(b)
open(os.path.join(t, "other.bin"), "wb").write(b"\x00" * 64 + b"\x11" * 4096)
# our update file holds the second stage's header (tools/mkbootimage.py)
upt = bytes(0x8001) + b"CD001" + bytes(0x2000) + b"LOS2" + struct.pack("<IIIII", 1, 0x81e00000, 0xa1e00000, 1000, 0) + bytes(1000)
open(os.path.join(t, "our.upt"), "wb").write(upt)
open(os.path.join(t, "their.upt"), "wb").write(bytes(0x8001) + b"CD001" + os.urandom(300000))
PY
"$T/test_bootfile" out/lossless.bin "$T/broken.bin" "$T/other.bin" "$T/our.upt" "$T/their.upt"
# and the real update file, when it's been built (tools/mkupdate.py)
[ -f out/update.upt ] && "$T/test_bootfile" out/lossless.bin "$T/broken.bin" "$T/other.bin" out/update.upt "$T/their.upt" out/uboot.bin
rm -rf "$T"
