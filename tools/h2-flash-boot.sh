#!/bin/sh
# Write our bootloader to the H2's flash over the chip's USB boot mode
# (hold Menu while plugging in): for developers. The same as
# tools/h2-rescue.sh, with out/uboot.bin by default.
#
#   tools/h2-flash-boot.sh [out/uboot.bin]
#
# SPDX-License-Identifier: Apache-2.0
set -e
OS=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:-$OS/out/uboot.bin}
[ -f "$IMG" ] || { echo "no $IMG (make -C boot)"; exit 1; }
exec "$OS/tools/h2-rescue.sh" "$IMG"
