#!/bin/bash
# Checks writing over Lossless OS's USB mode (macOS): copies a folder of
# test files (one big, many small) to the card, unmounts and mounts it
# again so they're read back from the player and not the Mac's cache,
# compares every file, checks the file system, then deletes the folder.
# usage: tests/h2-usb/write_test.sh [/Volumes/H2]
# SPDX-License-Identifier: Apache-2.0
set -e
VOL=${1:-/Volumes/H2}
[ -w "$VOL" ] || { echo "$VOL isn't mounted writable"; exit 1; }
DEV=$(diskutil info "$VOL" | awk '/Device Node/ {print $3}')
SRC=$(mktemp -d)
DIR="$VOL/lossless-usb-test"
trap 'rm -rf "$SRC"' EXIT

# the test files: 100 MB, 64 of 1 to 600 KB, 200 of under 4 KB
dd if=/dev/urandom of="$SRC/big.bin" bs=1m count=100 2>/dev/null
for i in $(seq 1 64); do dd if=/dev/urandom of="$SRC/mid$i.bin" bs=1k count=$((i * 9 + 1)) 2>/dev/null; done
for i in $(seq 1 200); do head -c $((i * 17 + 3)) /dev/urandom > "$SRC/small$i.bin"; done
(cd "$SRC" && shasum -a 256 *.bin) > "$SRC/../usb-test.sha256"

echo "writing $(du -sh "$SRC" | cut -f1) to $DIR"
rm -rf "$DIR"
start=$(date +%s)
cp -R "$SRC" "$DIR"
sync
echo "written in $(( $(date +%s) - start )) s"

echo "unmounting and mounting again ($DEV)"
diskutil unmount "$VOL" >/dev/null
diskutil mount "$DEV" >/dev/null
sleep 1
start=$(date +%s)
(cd "$DIR" && shasum -a 256 -c "$SRC/../usb-test.sha256" --quiet) && echo "read back: every file identical ($(( $(date +%s) - start )) s)"
rm -f "$SRC/../usb-test.sha256"

echo "checking the file system"
diskutil unmount "$VOL" >/dev/null
diskutil verifyVolume "$DEV" | tail -2
diskutil mount "$DEV" >/dev/null
sleep 1
rm -rf "$DIR"
sync
echo "test folder deleted"
