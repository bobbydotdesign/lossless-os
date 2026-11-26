#!/bin/bash
# AAC (M4A) through app/source.c (our MP4 reader, Android's fixed-point
# decoder) against macOS's decoder (afconvert): the same length (the
# encoder's delay and padding cut the same way, gapless) and nearly the
# same samples; and seeking, sample for sample. Stereo and mono.
# usage: tests/audio/check_aac.sh file.flac... [file.m4a...]   (macOS)
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/audio-check
tests/audio/build.sh
for f in "$@"; do
    if [[ "$f" == *.m4a ]]; then
        cases=("as is")
    else
        cases=("stereo 256k" "mono 96k")
    fi
    for enc in "${cases[@]}"; do
        if [ "$enc" = "as is" ]; then
            cp "$f" $OUT/t.m4a
        elif [ "$enc" = "mono 96k" ]; then
            afconvert -f WAVE -d LEI16 -c 1 --mix "$f" $OUT/mono.wav   # straight to mono AAC fails
            afconvert -f m4af -d aac -b 96000 $OUT/mono.wav $OUT/t.m4a
        else
            afconvert -f m4af -d aac -b 256000 "$f" $OUT/t.m4a
        fi
        afconvert -f WAVE -d LEI16 -c 2 $OUT/t.m4a $OUT/ref.wav    # mono plays on both sides
        $OUT/source_dump $OUT/t.m4a $OUT/ours.raw >/dev/null
        printf '%s, %s: ' "$(basename "$f")" "$enc"
        python3 tests/audio/compare_raw.py $OUT/ref.wav $OUT/ours.raw 60
        $OUT/seek_test $OUT/t.m4a | tail -1
    done
done
