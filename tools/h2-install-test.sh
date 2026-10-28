#!/bin/sh
# Put a Lossless OS test image on the H2's card (mounted at /Volumes/H2),
# where the bootloader's recovery menu item "Linux" loads it from.
#
#   tools/h2-install-test.sh out/h2-test1/uImage
#   tools/h2-install-test.sh out/h2-test4/uImage --card-files
#
# --card-files also puts what the app reads on the card: the fonts in
# /.lossless/fonts, and the checksum test file (check.bin and check.txt
# from the image's folder) in /.lossless/test. It prints the card's format
# and music folder too.
#
# SPDX-License-Identifier: Apache-2.0
set -e
IMG=${1:?usage: h2-install-test.sh path/to/uImage [--card-files]}
H2=/Volumes/H2
OS=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$H2" ] || { echo "H2 not mounted"; exit 1; }

if [ "$2" = "--card-files" ]; then
    diskutil info "$H2" | grep -E "File System Personality|Disk Size|Volume Free Space" || true
    echo "Music: $(find "$H2/Music" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ') artists," \
         "$(find "$H2/Music" -mindepth 2 -maxdepth 2 -type d 2>/dev/null | wc -l | tr -d ' ') albums"
    mkdir -p "$H2/.lossless/fonts" "$H2/.lossless/test"
    cp "$OS"/assets/fonts/*.ttf "$OS"/assets/fonts/*.otf "$OS"/assets/fonts/OFL-*.txt "$H2/.lossless/fonts/"
    DIR=$(dirname "$IMG")
    cp "$DIR/check.bin" "$DIR/check.txt" "$H2/.lossless/test/"
    find "$H2/.lossless" -name '._*' -delete
    sync
    cmp "$DIR/check.bin" "$H2/.lossless/test/check.bin" && echo "ok fonts and test file"
fi

cp "$IMG" "$H2/uImage"
# the loader refuses to start without this file; we ignore its contents
[ -f "$H2/linux_cmdline.txt" ] || printf 'lossless\n' > "$H2/linux_cmdline.txt"
rm -f "$H2/._uImage" "$H2/._linux_cmdline.txt"
sync
cmp "$IMG" "$H2/uImage" && echo "ok uImage ($(wc -c < "$IMG" | tr -d ' ') bytes)"
diskutil eject "$H2"
