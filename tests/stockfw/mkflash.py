#!/usr/bin/env python3
"""A flash image of an original firmware's system partition, for
tests/stockfw: the UBIFS image out of a HiFi Walker update (.upt), put in
UBI as a player's flash holds it, with what a used flash has besides.

    mkflash.py UPDATE.upt OUT_DIR

Writes OUT_DIR/system.ubifs (the image as it is) and three flash images
of 64 MB in 128 KB blocks:
  flash.bin        logical blocks in shuffled flash blocks, two of them
                   also in older, stale copies with wrong data; erased
                   blocks; a bad block (all zeros); UBI's own volume
  flash-nomst.bin  the same with both master nodes broken: no index
  flash-empty.bin  all erased
SPDX-License-Identifier: Apache-2.0
"""
import os, random, struct, sys, zlib

BLOCK, BLOCKS, VID, DATA = 131072, 512, 2048, 4096
LEB = BLOCK - DATA


def crc_raw(b):
    return ~zlib.crc32(b) & 0xffffffff


def ubifs_from_upt(path):
    iso = open(path, "rb").read()
    if iso[0x8000:0x8006] != b"\x01CD001":
        sys.exit("not an update file")
    root = iso[0x8000 + 156:0x8000 + 190]
    at, size = struct.unpack_from("<I", root, 2)[0] * 2048, struct.unpack_from("<I", root, 10)[0]
    for off in range(0, size, 2048):
        sec = iso[at + off:at + off + 2048]
        r = 0
        while r < 2048 and sec[r]:
            nl = sec[r + 32]
            if sec[r + 33:r + 33 + nl].split(b";")[0].upper() == b"SYSTEM.UBI":
                s = struct.unpack_from("<I", sec, r + 2)[0] * 2048
                return iso[s:s + struct.unpack_from("<I", sec, r + 10)[0]]
            r += sec[r]
    sys.exit("no SYSTEM.UBI in the update file")


def ec_header(ec):
    h = struct.pack(">IB3xQIII32x", 0x55424923, 1, ec, VID, DATA, 0x1234)
    return h + struct.pack(">I", crc_raw(h))


def vid_header(vol, lnum, sqnum):
    h = struct.pack(">IBBBBII4xIIII4xQ12x", 0x55424921, 1, 1, 0, 0, vol, lnum, 0, 0, 0, 0, sqnum)
    return h + struct.pack(">I", crc_raw(h))


def block(vol, lnum, sqnum, data):
    b = bytearray(b"\xff" * BLOCK)
    b[0:64] = ec_header(3)
    if vol is not None:
        b[VID:VID + 64] = vid_header(vol, lnum, sqnum)
        b[DATA:DATA + len(data)] = data
    return bytes(b)


def main():
    ubifs, out = ubifs_from_upt(sys.argv[1]), sys.argv[2]
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "system.ubifs"), "wb").write(ubifs)
    lebs = [ubifs[i:i + LEB] for i in range(0, len(ubifs), LEB)]
    rnd = random.Random(1)

    def flash(lebs):
        content = []
        sq = 1000
        for lnum, data in enumerate(lebs):
            content.append(block(0, lnum, sq, data))
            sq += 1
        # older copies, a lower sequence number and garbage: the newest wins
        for lnum in (1, 30):
            content.append(block(0, lnum, 5, bytes(rnd.getrandbits(8) for _ in range(256)) * (LEB // 256)))
        content.append(block(0x7fffefff, 0, 1, b"\x00" * 1024))     # UBI's own volume
        content.append(block(0x7fffefff, 1, 2, b"\x00" * 1024))
        content.append(b"\x00" * BLOCK)                            # bad
        while len(content) < BLOCKS - 8:
            content.append(block(None, 0, 0, b""))                 # free: erase counter only
        while len(content) < BLOCKS:
            content.append(b"\xff" * BLOCK)                        # erased
        rnd.shuffle(content)
        return b"".join(content)

    open(os.path.join(out, "flash.bin"), "wb").write(flash(lebs))
    broken = list(lebs)
    for lnum in (1, 2):
        b = bytearray(broken[lnum])
        for off in range(0, LEB, 2048):
            if b[off:off + 4] == struct.pack("<I", 0x06101831) and b[off + 20] == 7:
                b[off + 100] ^= 0xff                               # its CRC no longer matches
        broken[lnum] = bytes(b)
    open(os.path.join(out, "flash-nomst.bin"), "wb").write(flash(broken))
    open(os.path.join(out, "flash-empty.bin"), "wb").write(b"\xff" * BLOCK * BLOCKS)


main()
