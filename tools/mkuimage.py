#!/usr/bin/env python3
"""Wrap a Zephyr build for the H2's bootloader ("Linux" item, /uImage).

Legacy U-Boot image header, uncompressed. The load address and entry point
come from the ELF: the image starts at its first loadable byte and Zephyr's
__start is the entry.

  python3 tools/mkuimage.py build/h2-first-light/zephyr/zephyr.elf out/uImage

SPDX-License-Identifier: Apache-2.0
"""
import struct
import sys
import time
import zlib

MAGIC = 0x27051956
OS_LINUX, ARCH_MIPS, TYPE_KERNEL, COMP_NONE = 5, 5, 2, 0


def elf_layout(path):
    """Entry point and the address of the first loaded segment."""
    with open(path, "rb") as f:
        e = f.read()
    if e[:4] != b"\x7fELF" or e[4] != 1 or e[5] != 1:
        sys.exit("not a 32-bit little-endian ELF: " + path)
    entry, phoff = struct.unpack_from("<II", e, 24)
    phentsize, phnum = struct.unpack_from("<HH", e, 42)
    loads = []
    for i in range(phnum):
        p_type, _, _, p_paddr, p_filesz = struct.unpack_from("<IIIII", e, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            loads.append(p_paddr)
    return entry, min(loads)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkuimage.py zephyr.elf zephyr.bin uImage")
    elf, binary, out = sys.argv[1:]
    entry, load = elf_layout(elf)
    with open(binary, "rb") as f:
        data = f.read()
    name = b"Lossless OS".ljust(32, b"\0")
    fields = [MAGIC, 0, int(time.time()), len(data), load, entry, zlib.crc32(data)]
    tail = struct.pack(">BBBB32s", OS_LINUX, ARCH_MIPS, TYPE_KERNEL, COMP_NONE, name)
    header = struct.pack(">7I", *fields) + tail
    fields[1] = zlib.crc32(header)
    header = struct.pack(">7I", *fields) + tail
    with open(out, "wb") as f:
        f.write(header + data)
    print("%s: %d bytes, load 0x%08x, entry 0x%08x" % (out, len(data), load, entry))


if __name__ == "__main__":
    main()
