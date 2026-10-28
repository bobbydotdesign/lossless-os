#!/bin/sh
# What was there before back as what the H2 starts at power-on (undoes
# h2-install-default.sh): the copy kept beside the boot file.
# SPDX-License-Identifier: Apache-2.0
set -e
H2=/Volumes/H2
[ -d "$H2" ] || { echo "H2 not mounted (USB mode, at $H2)"; exit 1; }
CUR=$(find "$H2" -mindepth 2 -maxdepth 2 -path "$H2/.*" -name '*.erosq' -type f \
      ! -path "$H2/.lossless/*" ! -path "$H2/.Trashes/*" 2>/dev/null | head -1)
SAVED=$(ls "$CUR".* 2>/dev/null | head -1)
[ -n "$CUR" ] && [ -n "$SAVED" ] || { echo "no saved boot file on the card"; exit 1; }
cp "$SAVED" "$CUR"
sync
cmp "$SAVED" "$CUR" && echo "ok: what was there before starts at power-on"
diskutil eject "$H2"
