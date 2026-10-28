"""Convert a binary PPM (P6) to a PNG at 2x, with no libraries.

  python3 ppm2png.py in.ppm out.png

SPDX-License-Identifier: Apache-2.0
"""
import struct
import sys
import zlib


def main(src, dst, scale=2):
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    rows = []
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        wide = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(w))
        rows += [b"\0" + wide] * scale

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
