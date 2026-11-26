#!/bin/sh
# jpegenc against two reference decoders: Pillow (libjpeg) and macOS's own
# (sips). The picture must decode, at 200 x 200, close to what went in.
#
# SPDX-License-Identifier: Apache-2.0
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
cc -O2 -Wall -Wextra -Iapp tests/image/jpegenc_test.c app/jpegenc.c -o build/jt
build/jt build/jt.jpg build/jt.rgb
sips -g pixelWidth -g pixelHeight build/jt.jpg | grep -q "pixelWidth: 200"
/usr/bin/python3 - <<'PY'
from PIL import Image
im = Image.open("build/jt.jpg")
assert im.size == (200, 200) and im.mode == "RGB", (im.size, im.mode)
got = im.tobytes()
want = open("build/jt.rgb", "rb").read()
err = sum(abs(a - b) for a, b in zip(got, want)) / len(want)
worst = max(abs(a - b) for a, b in zip(got, want))
print("mean error %.2f, worst %d (of 255)" % (err, worst))
assert err < 3.0, "too far from the picture"
PY
echo "jpegenc: decodes, close to the original"
