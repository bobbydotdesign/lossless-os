#!/bin/sh
# The installer end to end in the simulator, on a pretend card: the first
# start from the release (installs, removes our update.upt once its
# bootloader is in the flash, never the factory's), a swap cut short (finished, or the version before put back),
# updates (only a newer version, kept as .old), low battery, and three
# failed starts. Needs build/host/los-sim, out/lossless.bin and
# out/update.upt with out/uboot.bin (a build).
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
SIM=./build/host/los-sim
[ -x "$SIM" ] && [ -f out/lossless.bin ] && [ -f out/update.upt ] && [ -f out/uboot.bin ] || { echo "needs build/host/los-sim and out/lossless.bin (see docs/building.md)"; exit 1; }
T=$(cd "$(mktemp -d)" && pwd -P)
C=$T/card
B=$C/.lossless/boot/lossless.bin
P=$C/Lossless/lossless.bin
mkdir -p "$C/Lossless/fonts" "$T/shots"
cp out/lossless.bin "$P"
cp assets/fonts/*.ttf "$C/Lossless/fonts/"
python3 - "$T" <<'PY'
import os, struct, sys
t = sys.argv[1]
# a flash holding another bootloader than the update file's
u = bytearray(open("out/uboot.bin", "rb").read()); u[0x4100] ^= 1
open(os.path.join(t, "other-flash.bin"), "wb").write(u)
open(os.path.join(t, "their.upt"), "wb").write(bytes(0x8001) + b"CD001" + os.urandom(300000))
PY
printf 'wait 1500\nkey enter\nwait 1500\n' > "$T/update.txt"
printf 'wait 1500\n' > "$T/start.txt"
# run SCRIPT [BATTERY%]: env on the command itself (an assignment before a
# shell function would stay set for the rest of the script)
# The pretend flash's bootloader partition: $FLASH (the update file's own
# bootloader unless a test says otherwise)
FLASH=out/uboot.bin
run() { env LOS_NO_AUDIO=1 LOS_BATTERY="${2:-76}" LOS_BOOT_FLASH="$FLASH" "$SIM" --script "$T/$1.txt" --name "$1" --shots "$T/shots" --music "$C" >/dev/null 2>&1; }
fails=0
check() { if eval "$1"; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }
# pkg VERSION DST: our boot file with another version, and another build
pkg() { python3 - out/lossless.bin "$1" "$2" <<'PY'
import struct, sys, zlib
d = bytearray(open(sys.argv[1], "rb").read())
maj, mnr = (int(x) for x in sys.argv[2].split("."))
d[24:28] = struct.pack("<I", maj << 16 | mnr << 8)
d[-1] ^= 0x5a                     # another build, too
d[20:24] = struct.pack("<I", zlib.crc32(bytes(d[64:])) & 0xffffffff)
open(sys.argv[3], "wb").write(d)
PY
}

# the first start, from the release (the bootloader started /Lossless);
# the flash holds another bootloader (a Vol+ start turned down for low
# battery, say): our update.upt stays until its own is in
cp out/update.upt "$C/update.upt"
FLASH=$T/other-flash.bin; run start; FLASH=out/uboot.bin
check "cmp -s $B out/lossless.bin && [ ! -e $B.old ]" "first start: installs itself into /.lossless/boot"
check "cmp -s $C/update.upt out/update.upt" "another bootloader in the flash: our update.upt kept"
FLASH=$T/nowhere.bin; run start; FLASH=out/uboot.bin
check "cmp -s $C/update.upt out/update.upt" "the flash can't be read: our update.upt kept"
run start
check "[ ! -e $C/update.upt ]" "its bootloader in the flash: our update.upt removed"
check "cmp -s $P out/lossless.bin" "the release itself is never written"
cp "$T/their.upt" "$C/update.upt"; run start
check "cmp -s $C/update.upt $T/their.upt" "someone else's update.upt: kept"
rm -f "$C/update.upt"
run start
check "cmp -s $B out/lossless.bin" "restart: nothing changes"

# updates: only a newer version is offered (a Lossless folder left from an
# earlier install must never go back over what runs)
pkg 0.9 "$P"; run update
check "cmp -s $B out/lossless.bin" "same version, another build: no update"
pkg 0.1 "$P"; run update
check "cmp -s $B out/lossless.bin" "older version: no update"
pkg 99.0 "$P"; run update 10
check "cmp -s $B out/lossless.bin" "newer version, low battery: nothing changes"
run update
check "cmp -s $B $P && cmp -s $B.old out/lossless.bin" "newer version: updated, the version before kept"

# a swap cut short
cp "$B" "$B.new"; rm -f "$B"; run start
check "cmp -s $B $P && [ ! -e $B.new ]" "swap cut short, .new there: finished"
rm -f "$B"; run start
check "cmp -s $B out/lossless.bin" "swap cut short, no .new: the version before back"

# three failed starts: the version before back
pkg 99.0 "$P"; run update
printf '3\n' > "$C/.lossless/state/starts.txt"; run start
check "cmp -s $B out/lossless.bin" "three failed starts: the version before back"

# a damaged release on a card without an installed boot file: nothing
rm -rf "$C/.lossless/boot"; cp out/lossless.bin "$P"
python3 -c "import sys;d=bytearray(open(sys.argv[1],'rb').read());d[5000]^=0xff;open(sys.argv[1],'wb').write(d)" "$P"
run start
check "[ ! -e $B ]" "damaged release: nothing installed"

[ "$fails" -eq 0 ] && rm -rf "$T" || echo "kept: $T"
[ "$fails" -eq 0 ] && echo "all installer tests passed"
exit "$fails"
