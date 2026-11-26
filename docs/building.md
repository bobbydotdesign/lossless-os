# Building Lossless OS

## For the H2

Setup, once: Zephyr 4.4.2 in `../zephyr-ws` (only the LVGL and FatFs
modules) and the Zephyr SDK with the MIPS toolchain in `~/zephyr-sdk-1.0.1`:

```bash
brew install python@3.12 ninja dtc
git clone --depth 1 --branch v4.4.2 https://github.com/zephyrproject-rtos/zephyr ../zephyr-ws/zephyr
python3.12 -m venv ../zephyr-ws/.venv && . ../zephyr-ws/.venv/bin/activate
pip install west -r ../zephyr-ws/zephyr/scripts/requirements-base.txt
cd ../zephyr-ws && west init -l zephyr && west config manifest.project-filter -- '-.*,+lvgl,+fatfs' && west update --narrow -o=--depth=1
```

SDK: unpack `zephyr-sdk-1.0.1_macos-aarch64_minimal` and
`toolchain_gnu_macos-aarch64_mips-zephyr-elf` (sdk-ng releases) into
`~/zephyr-sdk-1.0.1/gnu/`, then run its `setup.sh -c`.

Build:

```bash
. ../zephyr-ws/.venv/bin/activate && export ZEPHYR_BASE=$PWD/../zephyr-ws/zephyr
west build -b hifiwalker_h2 . -d build/h2
python3 tools/mklosboot.py build/h2/zephyr/zephyr.elf build/h2/zephyr/zephyr.bin out/lossless.bin
```

`out/lossless.bin` is the boot file our bootloader starts: a 64-byte
header (`LOSB`, version 1, load address, entry point, size, CRC-32, OS
version), then the image (`tools/mklosboot.py`).

Our bootloader (`boot/stage1` and `boot/stage2`), with the same SDK. It
also needs xorriso (`brew install xorriso`):

```bash
make -C boot        # out/uboot.bin (the flash image) and out/update.upt
```

`out/update.upt` is the update file the original firmware's recovery
installs into the flash's bootloader partition (`tools/mkbootimage.py`,
`tools/mkupdate.py`). How it starts Lossless OS: docs/h2-hardware.md,
"Starting from our bootloader", and the comments in `boot/`.

## Putting a build on your player

Our bootloader must be in the player first: install a release once, as
anyone would ([install.md](install.md)). Then, on a Mac, with the H2
connected as a drive (at `/Volumes/H2`):

```bash
tools/h2-install.sh out/lossless.bin                # what starts at power-on
tools/h2-install.sh out/lossless.bin --card-files   # also the fonts and test files
```

It copies the build to `/.lossless/boot/lossless.bin`, keeps the one there
as `lossless.bin.old`, and ejects the card. To go back, put
`lossless.bin.old` back in its place.

Or make a release ("Releases" below) and install it as anyone would.

Holding **Back** while switching on starts without writing to the card;
holding **Menu** shows a check screen.

## Releases

A version tag (`v1.0`) makes GitHub build the release
(`.github/workflows/release.yml`): the zip and `manifest.json` on a GitHub
release. The version is `LOS_VERSION` in `app/version.h`;
`tools/mklosboot.py` puts it in the boot file header's OS version field,
so the installer can tell versions apart.

A release by hand, after the build above and `make -C boot`:

```bash
python3 tools/mkrelease.py 1.0
```

In `out/release`: `lossless-os-1.0-h2.zip` (`update.upt` and
`Lossless/`: `lossless.bin`, `fonts/`, `READ ME.txt`, `SHA256SUMS`, laid
out like the card), `manifest.json` (each file's size and SHA-256, for the
site's installer) and `NOTES.md` (the release page's text).
