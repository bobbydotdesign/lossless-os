#!/bin/sh
# Decode FLAC files with our decoder and with macOS (afconvert) and compare
# every sample. usage: tests/audio/check_flac.sh file.flac...   (macOS only)
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/flac-check
mkdir -p $OUT
cc -std=c11 -O2 -Wall -Wextra -Iapp tests/audio/flac_dump.c app/flac.c -o $OUT/flac_dump
pass=0; fail=0
for f in "$@"; do
    info=$($OUT/flac_dump "$f" $OUT/ours.raw) || { echo "FAIL decode: $f"; fail=$((fail+1)); continue; }
    bits=$(echo "$info" | awk '{print $3}')
    afconvert -f WAVE -d LEI$bits "$f" $OUT/ref.wav
    if python3 - $OUT/ref.wav $OUT/ours.raw <<'PY'
import sys
w = open(sys.argv[1], 'rb').read()
i = 12
while w[i:i+4] != b'data':
    i += 8 + int.from_bytes(w[i+4:i+8], 'little')
n = int.from_bytes(w[i+4:i+8], 'little')
ref = w[i+8:i+8+n]
ours = open(sys.argv[2], 'rb').read()
sys.exit(0 if ref == ours else 1)
PY
    then pass=$((pass+1)); else echo "DIFFERENT: $f ($info)"; fail=$((fail+1)); fi
done
echo "$pass identical, $fail different"
