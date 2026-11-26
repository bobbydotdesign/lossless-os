#!/usr/bin/env python3
"""Our bootloader as the image for the player's flash (uboot.bin): what
the original firmware's recovery writes to its bootloader partition
(flash 0 to 0x80000) from an update file (tools/mkupdate.py).

  mkbootimage.py stage1-flash.bin stage2.bin out.bin

Layout:
  0x00000  the boot ROM's header (X1000 Programming Manual 27.3 and 27.8,
           Table 27-11): 06 05 04 03 02, 55 AA 55 00 (SPI NAND), the
           copied code's crc7, pages per block / 32, bytes per page / 1024,
           then the length the ROM copies (from offset 0, little-endian);
           zeros to 0x800. The ROM checks the crc7 and won't start a first
           stage whose crc7 is wrong (it falls back to USB boot). The
           manual doesn't define it; found from two images that boot (the
           factory's stores 0x00, another bootloader's 0x31): CRC-7 with
           the polynomial x^7 + x^3 + 1 (0x09), starting from 0, bits
           most significant first, over the bytes from 0x800 up to the
           length, stored as is.
  0x00800  the first stage (boot/stage1, linked at 0xf4001800, at most
           10 KB: the ROM copies at most 12 KB including the header)
  0x04000  the second stage: a 64-byte header ("LOS2", version 1, load
           address, entry, size, CRC-32 of the image), then the image
           (boot/stage2, linked at 0x81e00000)
  0x20000  a copy of all of the above, in the next block: the ROM retries
           the next block if the first stage doesn't load, and the first
           stage tries the second copy of the second stage

The flash is a Winbond W25N01GV: 2048-byte pages, 64 pages a block.

SPDX-License-Identifier: Apache-2.0
"""
import struct
import sys
import zlib

PAGE = 2048
PAGES_PER_BLOCK = 64
BLOCK = PAGE * PAGES_PER_BLOCK
STAGE1_AT = 0x800
STAGE1_MAX = 10 * 1024
STAGE2_AT = 0x4000
STAGE2_LOAD = 0x81E00000
STAGE2_ENTRY = 0xA1E00000      # the uncached alias: it sets up the caches first
PARTITION = 0x80000


def crc7(data):
    """CRC-7, polynomial x^7 + x^3 + 1, from 0, most significant bit first"""
    c = 0
    for b in data:
        for i in range(7, -1, -1):
            bit = (b >> i & 1) ^ (c >> 6 & 1)
            c = (c << 1) & 0x7F
            if bit:
                c ^= 0x09
    return c


def block_image(stage1, stage2):
    length = STAGE1_AT + len(stage1)
    length = (length + 511) & ~511                 # the manual asks for 512-byte steps
    copied = stage1 + bytes(length - STAGE1_AT - len(stage1))
    header = bytes([6, 5, 4, 3, 2, 0x55, 0xAA, 0x55, 0x00, crc7(copied),
                    PAGES_PER_BLOCK // 32, PAGE // 1024]) + struct.pack("<I", length)
    img = bytearray(header + bytes(STAGE1_AT - len(header)))
    img += stage1
    img += bytes(STAGE2_AT - len(img))
    s2 = struct.pack("<4sIIIII", b"LOS2", 1, STAGE2_LOAD, STAGE2_ENTRY, len(stage2),
                     zlib.crc32(stage2) & 0xFFFFFFFF)
    img += s2 + bytes(64 - len(s2)) + stage2
    return bytes(img), length


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stage1 = open(sys.argv[1], "rb").read()
    stage2 = open(sys.argv[2], "rb").read()
    if len(stage1) > STAGE1_MAX:
        sys.exit(f"mkbootimage: the first stage is {len(stage1)} bytes; at most {STAGE1_MAX}")
    block, length = block_image(stage1, stage2)
    if len(block) > BLOCK:
        sys.exit(f"mkbootimage: one copy is {len(block)} bytes; a block holds {BLOCK}")
    image = block + bytes(BLOCK - len(block)) + block
    if len(image) > PARTITION:
        sys.exit("mkbootimage: too big for the bootloader partition")
    open(sys.argv[3], "wb").write(image)
    print(f"{sys.argv[3]}: {len(image)} bytes; first stage {len(stage1)} bytes "
          f"(ROM copies {length}, crc7 0x{image[9]:02x}), second stage {len(stage2)} bytes "
          f"(crc {zlib.crc32(stage2) & 0xFFFFFFFF:08x}), copies at 0 and 0x{BLOCK:x}")


if __name__ == "__main__":
    main()
