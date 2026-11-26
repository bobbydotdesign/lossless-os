#!/bin/sh
# Write a bootloader to the H2 over the chip's USB boot mode, for a player
# that doesn't start any more (or to put the factory bootloader back
# without the card). Mac or Linux. docs/rescue.md has the steps.
#
#   h2-rescue.sh FILE
#
# FILE: HiFi Walker's firmware update (an .upt: the factory bootloader is
# taken out of it), our update.upt, or a bootloader image (.bin).
#
# The USB boot mode is in the chip's ROM, so it always works: switch the
# player off, hold Menu and plug it into the computer. This script starts
# our first stage in the player's memory (it sets up and tests the main
# memory), loads the image, and starts our writer, which checks the image,
# writes only the bootloader partition (flash 0 to 0x80000), reads it all
# back and shows the result on the player's screen. Nothing else in the
# flash is touched.
#
# From the rescue zip, everything it needs sits beside it (stage1.bin,
# writer.bin, h2-usb). From the source tree, it builds them (the Zephyr
# SDK for the first two, libusb for h2-usb).
#
# SPDX-License-Identifier: Apache-2.0
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FILE=$1
[ -n "$FILE" ] && [ -f "$FILE" ] || { echo "usage: $0 FILE (.upt or .bin)"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "this needs python3 (Mac: xcode-select --install)"; exit 1; }

# macOS has no timeout command: the same with Python
if ! command -v timeout >/dev/null 2>&1; then
    timeout() {
        python3 -I -c 'import subprocess, sys
try:
    sys.exit(subprocess.run(sys.argv[2:], timeout=float(sys.argv[1])).returncode)
except subprocess.TimeoutExpired:
    sys.exit(124)' "$@"
    }
fi

if [ -f "$HERE/stage1.bin" ]; then              # the rescue zip
    STAGE1=$HERE/stage1.bin WRITER=$HERE/writer.bin U=$HERE/h2-usb
    [ -x "$U" ] || make -s -C "$HERE" h2-usb >/dev/null
else                                            # the source tree
    OS=$(cd "$HERE/.." && pwd)
    make -s -C "$OS/boot/stage1" stage1.bin >/dev/null
    make -s -C "$OS/boot/stage2" flash.bin >/dev/null
    STAGE1=$OS/boot/stage1/stage1.bin WRITER=$OS/boot/stage2/flash.bin U=$OS/tools/h2-usb/h2-usb
    [ -x "$U" ] || make -s -C "$OS/tools/h2-usb" >/dev/null
fi
[ -x "$U" ] || { echo "h2-usb didn't build: it needs libusb (Mac: brew install libusb pkg-config)"; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 -I - "$FILE" "$T/load.bin" <<'PY'
import struct, sys, zlib

data = open(sys.argv[1], "rb").read()

def from_iso(iso):
    """UBOOT.BIN from an ISO 9660 image (the .upt files are ISO images)."""
    if iso[0x8000:0x8006] != b"\x01CD001":
        return None
    root = iso[0x8000 + 156:0x8000 + 190]
    at, size = struct.unpack_from("<I", root, 2)[0] * 2048, struct.unpack_from("<I", root, 10)[0]
    off = 0
    while off < size:
        sec = iso[at + off:at + off + 2048]
        r = 0
        while r < 2048 and sec[r]:
            rl, nl = sec[r], sec[r + 32]
            name = sec[r + 33:r + 33 + nl].decode("ascii", "replace").upper().split(";")[0]
            if name == "UBOOT.BIN":
                start = struct.unpack_from("<I", sec, r + 2)[0] * 2048
                return iso[start:start + struct.unpack_from("<I", sec, r + 10)[0]]
            r += rl
        off += 2048
    return b""

img = from_iso(data)
if img is None:
    img = data                       # a bootloader image as it is
elif not img:
    sys.exit("there's no bootloader (UBOOT.BIN) in that update file")
if img[:8] != bytes([6, 5, 4, 3, 2, 0x55, 0xAA, 0x55]):
    sys.exit("that isn't a bootloader image for this player")
if not 0x1000 <= len(img) <= 0x80000:
    sys.exit("the image's size is wrong for the bootloader partition")
ours = img[0x4000:0x4004] == b"LOS2"
open(sys.argv[2], "wb").write(struct.pack("<4sIII", b"LOSW", len(img), zlib.crc32(img) & 0xFFFFFFFF, 0) + img)
print("%s: %d bytes" % ("Lossless OS's bootloader" if ours else "the factory bootloader", len(img)))
PY

echo "Waiting for the player: switch it off, hold Menu and plug it in (the blue light comes on)."
until timeout 8 "$U" info >/dev/null 2>&1; do sleep 2; done
sleep 2
# the first stage: main memory set up and tested, or stop (writing from
# broken memory would write garbage)
timeout 60 "$U" run1 "$STAGE1" 0xf4001800 2>&1 | grep -E "Handed|No answer" || true
rm -f "$T/status.bin"
timeout 30 "$U" read 0xf4002e00 256 "$T/status.bin" >/dev/null 2>&1 || true
python3 -I - "$T/status.bin" <<'PY'
import struct, sys
try:
    m, step, err, n = struct.unpack("<4I", open(sys.argv[1], "rb").read()[:16])
except (OSError, struct.error):
    sys.exit("No answer from the player. Unplug it, wait for the light to go out, and start again.")
if step != 4 or err:
    sys.exit("The player's memory didn't start (step %d, error 0x%x). Nothing was written. "
             "Unplug it and start again." % (step, err))
print("Memory set up and tested.")
PY
timeout 60 "$U" load "$T/load.bin" 0xa1000000 2>&1 | grep -E "Sent|h2-usb:" || true
timeout 30 "$U" run2 "$WRITER" 0xa1e00000 2>&1 | grep -E "Started|h2-usb:" || true
echo "Writing. In under a minute the player's screen says \"Bootloader written and checked\"."
echo "Then hold Power until the screen goes off, unplug it, and switch it on."
