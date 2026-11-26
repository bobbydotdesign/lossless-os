#!/bin/sh
# The original firmware reader (app/stockfw.c, app/lzo1x.c), against an
# original firmware update from HiFi Walker (not in this repository: their
# download page, docs/install.md):
#
#   tests/stockfw/run.sh H2-Firmware-V2.3.upt
#
# SPDX-License-Identifier: Apache-2.0
set -e
UPT=$1
[ -f "$UPT" ] || { echo "usage: $0 H2-Firmware-V2.3.upt"; exit 1; }
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -Iapp -Iboot/common -o "$T/test" tests/stockfw/test_stockfw.c app/stockfw.c app/lzo1x.c boot/common/inflate.c
python3 -I tests/stockfw/mkflash.py "$UPT" "$T"
# V2.3's patch: /lib/firmware/BCM4343A1_001.002.009.0122.0538.hcd
SHA=9873217acdeaee78bd502a31fa876dc1b120c9d07ca8c16420853bc44ba64972
"$T/test" lzo "$T/system.ubifs"
"$T/test" fuzz "$T/system.ubifs"
"$T/test" patch "$T/flash.bin" $SHA
"$T/test" scan "$T/flash-nomst.bin" $SHA
"$T/test" none "$T/flash-nomst.bin"
"$T/test" none "$T/flash-empty.bin"
echo "stockfw: all passed"
