#!/bin/sh
# Make ALAC, WAV, AIFF and AIFF-C copies of FLAC files with macOS (afconvert),
# read each through app/source.c and compare every sample with macOS's own
# decode. usage: tests/audio/check_sources.sh file.flac...   (macOS only)
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/audio-check
mkdir -p $OUT
tests/audio/build.sh
pass=0; fail=0
for f in "$@"; do
    bits=$(afinfo "$f" | awk '/source bit depth/ {sub(/I/, "", $NF); print $NF}')
    afconvert -f WAVE -d LEI$bits "$f" $OUT/ref.wav
    for kind in flac alac wav aiff aifc; do
        case $kind in
            flac) cp "$f" $OUT/t.flac; t=$OUT/t.flac ;;
            alac) # format flags: 1 = 16-bit, 2 = 20-bit, 3 = 24-bit, 4 = 32-bit source
                  flag=4; [ "$bits" = 16 ] && flag=1; [ "$bits" = 20 ] && flag=2; [ "$bits" = 24 ] && flag=3
                  afconvert -f m4af -d alac/$flag "$f" $OUT/t.m4a; t=$OUT/t.m4a ;;
            wav)  t=$OUT/ref.wav ;;
            aiff) afconvert -f AIFF -d BEI$bits "$f" $OUT/t.aiff; t=$OUT/t.aiff ;;
            aifc) afconvert -f AIFC -d BEI$bits "$f" $OUT/t.aifc; t=$OUT/t.aifc ;;
        esac
        info=$($OUT/source_dump $t $OUT/ours.raw) || { echo "FAIL open $kind: $f"; fail=$((fail+1)); continue; }
        if python3 - $OUT/ref.wav $OUT/ours.raw <<'PY'
import sys
w = open(sys.argv[1], 'rb').read()
i = 12
while w[i:i+4] != b'data':
    i += 8 + int.from_bytes(w[i+4:i+8], 'little')
n = int.from_bytes(w[i+4:i+8], 'little')
sys.exit(0 if w[i+8:i+8+n] == open(sys.argv[2], 'rb').read() else 1)
PY
        then pass=$((pass+1)); else echo "DIFFERENT $kind: $f ($info)"; fail=$((fail+1)); fi
    done
done
echo "$pass identical, $fail different"
