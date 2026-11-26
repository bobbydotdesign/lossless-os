#!/bin/bash
# Opus through app/source.c (our Ogg reader, libopus in fixed point)
# against ffmpeg with libopus (floating point): the same length, and
# nearly the same samples (fixed and floating point round differently;
# both are as close to the original as each other).
# usage: tests/audio/check_opus.sh file.flac...   (macOS, ffmpeg with libopus)
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/audio-check
mkdir -p $OUT
tests/audio/build.sh
for f in "$@"; do
    for kbps in 64 160; do
        ffmpeg -v error -y -i "$f" -map 0:a -ac 2 -c:a libopus -b:a ${kbps}k $OUT/t.opus
        ffmpeg -v error -y -c:a libopus -i $OUT/t.opus -f s16le -ac 2 $OUT/ref.raw
        $OUT/source_dump $OUT/t.opus $OUT/ours.raw >/dev/null
        python3 - $OUT/ref.raw $OUT/ours.raw "$f" $kbps <<'PY'
import sys, array, math
# Fixed and floating point round differently: mostly far below the
# codec's own error, now and then more. Judge by the typical 20 ms frame.
a = array.array('h', open(sys.argv[1], 'rb').read())
b = array.array('h', open(sys.argv[2], 'rb').read())
fr, snrs = 960 * 2, []
for k in range(0, min(len(a), len(b)) - fr, fr * 5):
    s = sum(x * x for x in a[k:k + fr])
    if s > 1e6:
        e = sum((x - y) ** 2 for x, y in zip(a[k:k + fr], b[k:k + fr]))
        snrs.append(10 * math.log10(s / max(e, 1)))
snrs.sort()
med = snrs[len(snrs) // 2] if snrs else 99
ok = len(a) == len(b) and med > 50
print(('ok  ' if ok else 'BAD ') + '%s kbps: %d vs %d samples, typically %.0f dB apart' % (sys.argv[4], len(a), len(b), med), sys.argv[3])
PY
    done
done
