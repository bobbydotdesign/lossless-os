#!/usr/bin/env python3
"""Our bootloader as an update file the original firmware's recovery
installs (switch on holding Vol+ with it on the card as update.upt).

  mkupdate.py uboot.bin update.upt

The file is an ISO 9660 image (volume id CDROM, Rock Ridge names), as the
original firmware's own updates are, holding:
  uboot.bin    the bootloader image (tools/mkbootimage.py)
  update.txt   what to write: one "bootloader" entry, name "uboot", the
               file, its MD5
  version.txt  the player model the update is for: erosq_2024 (hardware
               revision 4: HiFi Walker H2 V1.9 and later)
The recovery writes uboot.bin to the bootloader partition and nothing else.

Needs xorriso (Homebrew: brew install xorriso).

SPDX-License-Identifier: Apache-2.0
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

MODEL = "erosq_2024"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image = open(sys.argv[1], "rb").read()
    out = os.path.abspath(sys.argv[2])
    md5 = hashlib.md5(image).hexdigest()
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "uboot.bin"), "wb").write(image)
        open(os.path.join(d, "update.txt"), "w").write(
            "bootloader={\n"
            "        name=uboot\n"
            "        file_path=autoupdate/uboot.bin\n"
            f"        md5={md5}\n"
            "}\n")
        open(os.path.join(d, "version.txt"), "w").write(
            "version={\n"
            f"        name={MODEL}\n"
            f"        ver={stamp}\n"
            "}\n")
        xorriso = shutil.which("xorriso") or "/opt/homebrew/bin/xorriso"
        if os.path.exists(out):
            os.remove(out)
        r = subprocess.run([xorriso, "-as", "mkisofs", "-quiet", "-V", "CDROM", "-R",
                            "-o", out, d], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"mkupdate: xorriso failed:\n{r.stderr}")
    print(f"{sys.argv[2]}: uboot.bin {len(image)} bytes, md5 {md5}, for {MODEL}")


if __name__ == "__main__":
    main()
