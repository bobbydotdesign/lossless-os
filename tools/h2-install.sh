#!/bin/sh
# Put a Lossless OS build on the H2's card (mounted at /Volumes/H2), where
# our bootloader starts it at power-on: /.lossless/boot/lossless.bin (the
# one there kept as lossless.bin.old, as the player's own installer does).
#
#   tools/h2-install.sh out/lossless.bin
#   tools/h2-install.sh out/lossless.bin --card-files
#
# --card-files also puts what the app reads on the card: the fonts in
# /.lossless/fonts, and the checksum test file (check.bin and check.txt
# from the build's folder, if there) in /.lossless/test. It prints the
# card's format and music folder too.
#
# SPDX-License-Identifier: Apache-2.0
set -e
IMG=${1:?usage: h2-install.sh out/lossless.bin [--card-files]}
H2=/Volumes/H2
OS=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$H2" ] || { echo "H2 not mounted"; exit 1; }
head -c 4 "$IMG" | grep -q LOSB || { echo "$IMG isn't a boot file (tools/mklosboot.py)"; exit 1; }

if [ "$2" = "--card-files" ]; then
    diskutil info "$H2" | grep -E "File System Personality|Disk Size|Volume Free Space" || true
    echo "Music: $(find "$H2/Music" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ') artists," \
         "$(find "$H2/Music" -mindepth 2 -maxdepth 2 -type d 2>/dev/null | wc -l | tr -d ' ') albums"
    mkdir -p "$H2/.lossless/fonts"
    cp "$OS"/assets/fonts/*.ttf "$OS"/assets/fonts/*.otf "$OS"/assets/fonts/OFL-*.txt "$H2/.lossless/fonts/"
    DIR=$(dirname "$IMG")
    if [ -f "$DIR/check.bin" ]; then
        mkdir -p "$H2/.lossless/test"
        cp "$DIR/check.bin" "$DIR/check.txt" "$H2/.lossless/test/"
    fi
    find "$H2/.lossless" -name '._*' -delete
    sync
    echo "ok fonts"
fi

B=$H2/.lossless/boot/lossless.bin
mkdir -p "$H2/.lossless/boot"
if [ -f "$B" ]; then rm -f "$B.old"; mv "$B" "$B.old"; fi
cp "$IMG" "$B"
rm -f "$H2/.lossless/boot/._"*
sync
cmp "$IMG" "$B" && echo "ok $B ($(wc -c < "$IMG" | tr -d ' ') bytes)"
diskutil eject "$H2"
