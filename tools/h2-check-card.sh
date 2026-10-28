#!/bin/bash
# After the H2 has written to its card (test 4c): is the file system clean,
# and did anything outside /.lossless change? Compares with the file list
# taken before (backup/card-before-4c-*/files.txt).
#
#   tools/h2-check-card.sh backup/card-before-4c-2026-10-27-1628
#
# SPDX-License-Identifier: Apache-2.0
set -e
BEFORE=$1/files.txt
H2=/Volumes/H2
[ -f "$BEFORE" ] || { echo "no $BEFORE"; exit 1; }
[ -d "$H2" ] || { echo "H2 not mounted"; exit 1; }
NOW=$(mktemp)
(cd "$H2" && find . -type f ! -name '._*' ! -path './.Spotlight-V100/*' ! -path './.fseventsd/*' \
    ! -path './.Trashes/*' -exec stat -f '%z %N' {} \; 2>/dev/null | sort -k2) > "$NOW"
echo "== outside /.lossless (should be only the boot file and uImage, from installs):"
diff <(grep -v ' ./.lossless/' "$BEFORE") <(grep -v ' ./.lossless/' "$NOW") || true
echo "== /.lossless now:"
grep ' ./.lossless/' "$NOW" | grep -v '/fonts/'
echo "== file system:"
diskutil verifyVolume "$H2" 2>&1 | grep -E "exit code|Warning|corrupt|error" || true
rm -f "$NOW"
