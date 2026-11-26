#!/bin/bash
# MP3 through app/source.c (our frame reader, Android's fixed-point
# decoder) against ffmpeg (floating point): the same length (LAME's delay
# and padding cut the same way) and nearly the same samples.
# usage: tests/audio/check_mp3.sh file.flac...   (macOS, ffmpeg with libmp3lame)
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/audio-check
tests/audio/build.sh
for f in "$@"; do
    for enc in "-b:a 320k" "-q:a 2" "-ac 1 -ar 22050 -b:a 64k"; do
        ffmpeg -v error -y -i "$f" -map 0:a $enc -c:a libmp3lame $OUT/t.mp3
        ffmpeg -v error -y -i $OUT/t.mp3 -f s16le $OUT/ref.raw
        $OUT/source_dump $OUT/t.mp3 $OUT/ours.raw >/dev/null
        python3 - $OUT/ref.raw $OUT/ours.raw "$enc" <<'PY'
import sys, array, math
a = array.array('h', open(sys.argv[1], 'rb').read())
b = array.array('h', open(sys.argv[2], 'rb').read())
n = min(len(a), len(b))
s = sum(a[i] * a[i] for i in range(0, n, 7)) or 1
e = sum((a[i] - b[i]) ** 2 for i in range(0, n, 7)) or 1
snr = 10 * math.log10(s / e)
ok = len(a) == len(b) and snr > 60
print(('ok  ' if ok else 'BAD ') + '%s: %d vs %d samples, %.0f dB apart' % (sys.argv[3], len(a), len(b), snr))
PY
    done
done
