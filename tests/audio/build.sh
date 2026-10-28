#!/bin/bash
# Builds the audio test tools (source_dump, seek_test) with every decoder:
# ours, libopus, Tremor and Android's MP3 decoder (fixed point, as on the players).
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
OUT=build/audio-check
R=$PWD
mkdir -p $OUT/opus $OUT/tremor $OUT/pvmp3
OPUS=$(sed -n "s|^    \${OPUS_DIR}/|$R/lib/opus/|p" lib/opus/sources.cmake)
(cd $OUT/opus && cc -std=c11 -O2 -w -DFIXED_POINT -DOPUS_BUILD -DVAR_ARRAYS -DDISABLE_FLOAT_API \
    -I$R/lib/opus/include -I$R/lib/opus/celt -I$R/lib/opus/silk -I$R/lib/opus/silk/fixed $OPUS -c)
TREMOR=""
for f in block codebook floor0 floor1 info mapping0 mdct registry res012 sharedbook synthesis window; do
    TREMOR="$TREMOR $R/lib/tremor/$f.c"
done
(cd $OUT/tremor && cc -O2 -w -Dmalloc=tremor_malloc -Dcalloc=tremor_calloc -Drealloc=tremor_realloc \
    -Dfree=tremor_free -DHAVE_ALLOCA_H -I$R/lib/tremor -I$R/lib/ogg/include $TREMOR $R/lib/ogg/src/bitwise.c -c)
PVMP3=$(sed -n "s|^    \${PVMP3_DIR}/|$R/lib/pvmp3/|p" lib/pvmp3/sources.cmake)
(cd $OUT/pvmp3 && c++ -std=c++11 -O2 -w -include $R/lib/pvmp3/build_config.h -I$R/lib/pvmp3/include \
    -I$R/lib/pvmp3/src $PVMP3 -c)
APP="app/source.c app/flac.c app/alac.c app/mp4.c app/ogg.c app/opusfile.c app/oggvorbis.c app/mp3file.c"
INC="-Iapp -Ilib/opus/include -Ilib/tremor -Ilib/ogg/include -Ilib/pvmp3/include -Ilib/pvmp3/src"
for t in source_dump seek_test; do
    cc -std=c11 -O2 -Wall -Wextra $INC tests/audio/$t.c $APP $OUT/opus/*.o $OUT/tremor/*.o $OUT/pvmp3/*.o -lm -lc++ -o $OUT/$t
done
