#!/bin/sh
# Make Lossless OS what the H2 starts at power-on (no menu), without writing
# the flash: the bootloader starts a boot file from the card (in a hidden
# folder, ending .erosq; docs/h2-hardware.md, "The boot file"), so that file
# becomes Lossless OS (tools/mkbootfile.py). The file that was there is
# kept, on the card beside it as <name>.original and on the Mac in backup/.
#
#   tools/h2-install-default.sh out/lossless.erosq out/uImage
#
# Back to what was there: tools/h2-restore.sh. The recovery menu (hold
# Vol+ while switching on) still has USB mode, Linux (/uImage, also Lossless
# OS) and Shutdown; Play + Power still starts the stock firmware.
#
# SPDX-License-Identifier: Apache-2.0
set -e
BOOT=$1
IMG=$2
H2=/Volumes/H2
OS=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$BOOT" ] && [ -f "$IMG" ] || { echo "usage: $0 lossless.erosq uImage"; exit 1; }
[ -d "$H2" ] || { echo "H2 not mounted (USB mode, at $H2)"; exit 1; }

ours() {   # is this file Lossless OS (our LOAD address, 0x80ffff00)?
    [ "$(dd if="$1" bs=1 skip=28 count=4 2>/dev/null | xxd -p)" = "00ffff80" ]
}

# the boot file the bootloader starts: the .erosq in a hidden folder
CUR=$(find "$H2" -mindepth 2 -maxdepth 2 -path "$H2/.*" -name '*.erosq' -type f \
      ! -path "$H2/.lossless/*" ! -path "$H2/.Trashes/*" 2>/dev/null | head -1)
[ -n "$CUR" ] || { echo "no boot file (a .erosq in a hidden folder): is the bootloader installed?"; exit 1; }
SAVED=$(ls "$CUR".* 2>/dev/null | head -1)
if ! ours "$CUR" && [ -z "$SAVED" ]; then
    STAMP=$(date +%Y-%m-%d-%H%M)
    mkdir -p "$OS/../backup/boot-$STAMP"
    cp "$CUR" "$OS/../backup/boot-$STAMP/"
    cp "$CUR" "$CUR.original"
    echo "kept what was there: backup/boot-$STAMP and $CUR.original"
fi

# what Lossless OS reads, in its own folder
mkdir -p "$H2/.lossless/fonts" "$H2/.lossless/bluetooth"
cp "$OS"/assets/fonts/*.ttf "$OS"/assets/fonts/*.otf "$OS"/assets/fonts/OFL-*.txt "$H2/.lossless/fonts/"
if [ ! -f "$H2/.lossless/bluetooth/BCM4343A1.hcd" ]; then
    # the Bluetooth chip's patch, from wherever the card has it (not ours to ship)
    HCD=$(find "$H2" -maxdepth 4 -name BCM4343A1.hcd -type f ! -path "$H2/.lossless/*" 2>/dev/null | head -1)
    [ -n "$HCD" ] && cp "$HCD" "$H2/.lossless/bluetooth/"
fi

cp "$BOOT" "$CUR"
cp "$IMG" "$H2/uImage"
[ -f "$H2/linux_cmdline.txt" ] || printf 'lossless\n' > "$H2/linux_cmdline.txt"
find "$H2/.lossless" "$(dirname "$CUR")" -maxdepth 1 -name '._*' -delete
rm -f "$H2/._uImage"
sync
cmp "$BOOT" "$CUR" && cmp "$IMG" "$H2/uImage" && echo "ok: Lossless OS starts at power-on"
diskutil eject "$H2"
