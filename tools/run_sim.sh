#!/bin/sh
# Run a simulator script and turn its screenshots into 2x PNGs.
# usage: tools/run_sim.sh sim/tour.txt tour
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/.."
rm -f sim/shots/"$2"-*.bmp sim/shots/"$2"-*.png
./build/host/los-sim --script "$1" --name "$2" >/dev/null
for f in sim/shots/"$2"-*.bmp; do
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
    sips -z 480 640 "${f%.bmp}.png" >/dev/null
    rm "$f"
done
ls sim/shots/"$2"-*.png
