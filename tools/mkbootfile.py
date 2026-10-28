#!/usr/bin/env python3
"""Lossless OS as the file the H2's bootloader starts by default.

The H2's bootloader starts a boot file from the card at power-on (in a
hidden folder, ending .erosq; the recovery menu is only shown while Vol+ is
held). That file is:

  checksum   4 bytes, big-endian: 116 (the player's model number) plus the
             sum of every byte of the image
  model      4 bytes, "eros"
  image      copied to the address in its header's LOAD entry, then run
             from that address

The header is "BEGINHDR", then 4-byte labels each followed by a 4-byte
little-endian value, then "ENDH"; it must start within the first 128 bytes.

Our image is Zephyr's, which starts at its load address but is entered
further in, so 256 bytes go in front of it: a jump to the entry and the
header, loaded 256 bytes below Zephyr so Zephyr lands where it was linked.

Facts: docs/h2-hardware.md, "The boot file".

  python3 tools/mkbootfile.py zephyr.elf zephyr.bin lossless.erosq

SPDX-License-Identifier: Apache-2.0
"""
import struct
import sys

from mkuimage import elf_layout

MODEL_NUMBER = 116
MODEL_NAME = b"eros"
PREFIX = 256


def search_header(img, label):
    """As the bootloader reads it (its search_header)."""
    i = next((i for i in range(8, min(len(img), 128) - 7, 4) if img[i:i + 8] == b"BEGINHDR"), None)
    if i is None:
        return 0
    i += 8
    while i < min(len(img), i + 256) - 7:
        if img[i:i + 4] == b"ENDH":
            return 0
        if img[i:i + 4] == label:
            return struct.unpack_from("<I", img, i + 4)[0]
        i += 8
    return 0


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkbootfile.py zephyr.elf zephyr.bin lossless.erosq")
    elf, binary, out = sys.argv[1:]
    entry, load = elf_layout(elf)
    if (entry ^ load) & 0xf0000000:
        sys.exit("entry and load address in different 256 MB regions")
    with open(binary, "rb") as f:
        data = f.read()

    start = load - PREFIX
    jump = 0x08000000 | ((entry & 0x0fffffff) >> 2)      # j entry
    prefix = struct.pack("<II", jump, 0)                  # (nop in the delay slot)
    prefix += b"BEGINHDR" + b"LOAD" + struct.pack("<I", start) + b"ENDH"
    prefix = prefix.ljust(PREFIX, b"\0")
    img = prefix + data

    if search_header(img, b"LOAD") != start:
        sys.exit("header check failed")
    checksum = (MODEL_NUMBER + sum(img)) & 0xffffffff
    with open(out, "wb") as f:
        f.write(struct.pack(">I", checksum) + MODEL_NAME + img)
    print("%s: %d bytes, loaded at 0x%08x, Zephyr at 0x%08x, entry 0x%08x"
          % (out, len(img) + 8, start, load, entry))


if __name__ == "__main__":
    main()
