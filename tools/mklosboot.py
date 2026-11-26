#!/usr/bin/env python3
"""Lossless OS as the boot file our own bootloader starts:
/.lossless/boot/lossless.bin on the card.

  mklosboot.py zephyr.elf zephyr.bin out.bin

The file is a 64-byte header, then the image (zephyr.bin), all
little-endian:

  0   "LOSB"          magic
  4   1               header version
  8   load address    where the image goes (its first byte)
  12  entry point     where to start it
  16  image size      bytes after the header
  20  image CRC-32    the usual CRC-32 (zlib.crc32) of the image
  24  OS version      major << 16 | minor << 8 | patch, 0 if unknown
  28  0 ...           reserved, to 64 bytes

The second stage (boot/stage2) refuses a file whose magic, version, size,
addresses or CRC don't check out.

SPDX-License-Identifier: Apache-2.0
"""
import os
import re
import struct
import sys
import zlib

HEADER = 64
MAGIC = b"LOSB"


def elf_info(elf):
    """(load address of the first loadable segment, entry point), read from
    the 32-bit little-endian ELF file itself"""
    d = open(elf, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        sys.exit(f"mklosboot: {elf} isn't a 32-bit little-endian ELF file")
    entry, phoff = struct.unpack_from("<II", d, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
    for i in range(phnum):
        p_type, _off, _vaddr, paddr = struct.unpack_from("<IIII", d, phoff + i * phentsize)
        if p_type == 1:   # PT_LOAD
            return paddr, entry
    sys.exit(f"mklosboot: no loadable segment in {elf}")


def os_version():
    """LOS_VERSION from app/version.h, packed: "0.9" -> 0x000900"""
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        with open(os.path.join(here, "..", "app", "version.h")) as f:
            m = re.search(r'LOS_VERSION\s+"(\d+)\.(\d+)(?:\.(\d+))?"', f.read())
    except OSError:
        return 0
    if not m:
        return 0
    return int(m.group(1)) << 16 | int(m.group(2)) << 8 | int(m.group(3) or 0)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf, binfile, out = sys.argv[1:]
    load, entry = elf_info(elf)
    image = open(binfile, "rb").read()
    header = struct.pack("<4sIIIIII", MAGIC, 1, load, entry, len(image),
                         zlib.crc32(image) & 0xffffffff, os_version())
    header += bytes(HEADER - len(header))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "wb").write(header + image)
    print(f"{out}: {len(image)} bytes, load 0x{load:08x}, entry 0x{entry:08x}, "
          f"crc 0x{zlib.crc32(image) & 0xffffffff:08x}")


if __name__ == "__main__":
    main()
