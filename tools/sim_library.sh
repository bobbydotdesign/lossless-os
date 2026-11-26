#!/bin/sh
# Make a library for the Mac simulator in the layout the player's card uses
# (Artist/Album/), from a folder of "Artist - Album" folders, such as the
# Lossless desktop app's ~/Music/Lossless. Files are linked, not copied.
#
#   tools/sim_library.sh [~/Music/Lossless] [build/sim-music]
#
# SPDX-License-Identifier: Apache-2.0
set -e
SRC=${1:-$HOME/Music/Lossless}
OUT=${2:-$(cd "$(dirname "$0")/.." && pwd)/build/sim-music}
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$SRC"
for d in */; do
    d=${d%/}
    artist=${d%% - *}
    album=${d#* - }
    mkdir -p "$OUT/$artist/$album"
    for f in "$d"/*; do
        ln -s "$SRC/$f" "$OUT/$artist/$album/$(basename "$f")"
    done
done
echo "$OUT: $(ls "$OUT" | wc -l | tr -d ' ') artists"
