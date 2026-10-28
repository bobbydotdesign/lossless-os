#!/bin/sh
# Render test 3's screens on the Mac into build/h2-preview/*.png (2x size).
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../../.."
out=build/h2-preview
mkdir -p $out
cc -std=c11 -Wall -Wextra -O1 -Iapp -Iplatform/h2 -Itests/h2-buttons/src \
    tests/h2-buttons/host/preview.c platform/h2/draw.c platform/h2/ui.c \
    tests/h2-buttons/src/page.c -o $out/preview-buttons
(cd $out && ./preview-buttons)
for f in $out/*.ppm; do
    python3 tests/h2-diagnostics/host/ppm2png.py "$f" "${f%.ppm}.png"
    rm "$f"
done
ls $out/buttons-*.png $out/off*.png
