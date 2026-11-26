#!/usr/bin/env python3
"""A release of Lossless OS for the H2, from out/lossless.bin (tools/mklosboot.py)
and out/update.upt (make -C boot).

  python3 tools/mkrelease.py 1.0

Makes, in out/release:
  lossless-os-1.0-h2.zip   laid out like the card: copy its contents to the
                           card's top level (docs/install.md)
      update.upt           our bootloader: the player's own recovery installs
                           it when switched on holding Vol+ (Lossless OS
                           deletes it once that bootloader is in)
      Lossless/            what the bootloader starts the first time; Lossless
                           OS installs itself from here (and it can be
                           deleted after)
          lossless.bin     Lossless OS
          fonts/           the fonts, copied into /.lossless/fonts
          READ ME.txt      the steps, in short
          SHA256SUMS
  manifest.json            each file in the zip with its size and SHA-256
  NOTES.md                 the release page's text

The only file in a release that writes the player's flash is update.upt,
and only ours: it is checked to be our bootloader (tools/mkupdate.py) for
this player model. Nothing else that looks like a bootloader image ships.

SPDX-License-Identifier: Apache-2.0
"""
import hashlib
import json
import os
import shutil
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
FORBIDDEN = ("bootloader", "-boot.bin", "uboot", ".upt")   # flash images (update.upt is checked apart)

READ_ME = """Lossless OS {version} for the HiFi Walker H2

Check first. Your player's original firmware shows its version under
System Settings > About The Player > Version. This release is for the
HiFi Walker H2 with V2.3 only: anything else, not yet (the install guide
says why). The battery must be at 30% or more (or the player plugged in).
The card can stay as it is: FAT32 or exFAT.

1. Plug the player into your computer with its USB cable. The card shows
   up as a drive called H2.
2. Copy everything in this zip (update.upt and the Lossless folder) to the
   top level of the card. Eject H2 and unplug.
3. Switch the player off. Hold Vol+ while switching it on, until the update
   starts. Let it finish: the player restarts into Lossless OS.

update.upt installs Lossless OS's own bootloader, through the player's own
recovery. Lossless OS deletes it once that bootloader is in; the Lossless
folder can be deleted too.

To go back to the original firmware: copy HiFi Walker's H2-Firmware-V2.3.upt
to the card as update.upt, then switch on holding Vol+.

Install guide, the way back, and rescue for a player that won't start:
{repo}/blob/main/docs/install.md
"""


RESCUE_READ_ME = """Lossless OS rescue kit, for a HiFi Walker H2 that won't start

Nothing here goes on the card. It runs on a Mac or Linux computer and
talks to the player's chip over USB, in the chip's own USB boot mode,
which is built into the chip and always works.

1. Get the bootloader to write: HiFi Walker's firmware update for your
   version (the .upt file) puts the original back. Lossless OS's own
   update.upt works too.
2. Mac: install Apple's command line tools and libusb
   (xcode-select --install; brew install libusb pkg-config).
   Linux: a C compiler and libusb-1.0-0-dev. Run it with sudo, or allow
   USB device a108:1000.
3. Switch the player off. In Terminal, in this folder:
       ./h2-rescue.sh path/to/the-update.upt
4. When it asks, hold Menu on the player and plug it into the computer.
   The blue light comes on. In under a minute the screen says
   "Bootloader written and checked".
5. Hold Power until the screen goes off, unplug, and switch it on.

Steps and help: {repo}/blob/main/docs/rescue.md
"""


def boot_file_ok(path):
    """Our boot file: LOSB header, version 1, the image's size and CRC-32"""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 64 or d[:4] != b"LOSB" or struct.unpack_from("<I", d, 4)[0] != 1:
        return False
    size, crc = struct.unpack_from("<II", d, 16)
    return size == len(d) - 64 and zlib.crc32(d[64:]) & 0xffffffff == crc


def update_ok(path):
    """Our update file: an ISO 9660 image for erosq_2024 holding our
    bootloader (the second stage's LOS2 header)"""
    with open(path, "rb") as f:
        d = f.read()
    mark = b"LOS2" + struct.pack("<II", 1, 0x81E00000)
    return (len(d) < 2 * 1024 * 1024 and d[0x8001:0x8006] == b"CD001" and mark in d
            and b"erosq_2024" in d)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: mkrelease.py VERSION")
    version = sys.argv[1]
    repo = os.environ.get("LOS_REPO", "https://github.com/bobbydotdesign/lossless-os")
    out = os.path.join(ROOT, "out")
    boot, upt = os.path.join(out, "lossless.bin"), os.path.join(out, "update.upt")
    if not os.path.exists(boot) or not boot_file_ok(boot):
        sys.exit("out/lossless.bin is missing or fails its check (tools/mklosboot.py)")
    if not os.path.exists(upt) or not update_ok(upt):
        sys.exit("out/update.upt is missing or isn't our bootloader (make -C boot)")

    fonts = os.path.join(ROOT, "assets", "fonts")
    files = {                                   # path in the release: bytes
        "update.upt": open(upt, "rb").read(),
        "Lossless/lossless.bin": open(boot, "rb").read(),
        "Lossless/READ ME.txt": READ_ME.format(version=version, repo=repo).encode(),
    }
    for name in sorted(os.listdir(fonts)):
        if name.endswith((".ttf", ".otf", ".txt")) and not name.startswith("."):
            files["Lossless/fonts/" + name] = open(os.path.join(fonts, name), "rb").read()
    sums = "".join("%s  %s\n" % (hashlib.sha256(d).hexdigest(), p) for p, d in sorted(files.items()))
    files["Lossless/SHA256SUMS"] = sums.encode()

    for p in files:
        low = p.lower()
        if p != "update.upt" and any(f in low for f in FORBIDDEN) or "/." in "/" + p:
            sys.exit("refusing to ship %s" % p)

    rel = os.path.join(out, "release")
    shutil.rmtree(rel, ignore_errors=True)
    os.makedirs(rel)
    zpath = os.path.join(rel, "lossless-os-%s-h2.zip" % version)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p, d in sorted(files.items()):
            info = zipfile.ZipInfo(p, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, d)
    with open(os.path.join(rel, "NOTES.md"), "w") as f:
        f.write("Lossless OS %s for the HiFi Walker H2 with firmware V2.3.\n\n"
                "Download the zip and follow [the install guide](%s/blob/main/docs/install.md): "
                "check your player's firmware version first. It installs Lossless OS's own "
                "open-source bootloader through the player's own recovery (Vol+); HiFi Walker's "
                "firmware update puts the player back. The rescue zip is for a player that "
                "won't start ([docs/rescue.md](%s/blob/main/docs/rescue.md)).\n\n"
                "Use at your own risk: it replaces the player's bootloader. Read the install "
                "guide before you start.\n"
                % (version, repo, repo))

    # each file in the zip, with its size and SHA-256
    manifest = {"version": version, "player": "HiFi Walker H2", "zip": os.path.basename(zpath),
                "files": [{"path": p, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()}
                          for p, d in sorted(files.items())]}
    with open(os.path.join(rel, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    # the rescue kit: for a player that won't start (docs/rescue.md); runs
    # over USB boot from a Mac or Linux computer, never copied to the card
    kit = os.path.join(out, "rescue")
    rescue = {
        "stage1.bin": os.path.join(kit, "stage1.bin"),
        "writer.bin": os.path.join(kit, "writer.bin"),
        "h2-rescue.sh": os.path.join(HERE, "h2-rescue.sh"),
        "h2-usb.c": os.path.join(HERE, "h2-usb", "h2-usb.c"),
        "Makefile": os.path.join(HERE, "h2-usb", "Makefile"),
    }
    missing = [p for p in rescue.values() if not os.path.exists(p)]
    if missing:
        sys.exit("the rescue kit is missing %s (make -C boot rescue)" % ", ".join(missing))
    rpath = os.path.join(rel, "lossless-os-%s-h2-rescue.zip" % version)
    with zipfile.ZipFile(rpath, "w", zipfile.ZIP_DEFLATED) as z:
        for name, src in sorted(rescue.items()):
            info = zipfile.ZipInfo("lossless-os-rescue/" + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o755 if name.endswith(".sh") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(src, "rb").read())
        info = zipfile.ZipInfo("lossless-os-rescue/READ ME.txt", date_time=(2026, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        z.writestr(info, RESCUE_READ_ME.format(repo=repo))
    print("%s: %d files, and manifest.json; %s" % (zpath, len(files), os.path.basename(rpath)))


if __name__ == "__main__":
    main()
