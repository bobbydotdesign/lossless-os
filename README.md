# Lossless OS

An open-source operating system for portable music players, made for
people who care how their music sounds. It runs straight on the player's
chip on [Zephyr](https://zephyrproject.org): no Linux underneath, one small
image that starts in a moment and leaves the memory for music.

- **Bit-perfect.** Files play at their own rate and depth, straight to the
  DAC: no resampling, no dither, no software volume (volume is the DAC's
  own). FLAC, ALAC, WAV and AIFF with decoders of our own, checked
  sample-for-sample against reference decoders; MP3, Ogg Vorbis and Opus
  with open-source fixed-point decoders. Gapless.
- **Bluetooth, our own stack.** `lbt` (lib/lbt) plays to headphones,
  speakers and cars over A2DP (SBC) with their buttons and volume, the
  song's title, artist, album and position, and its cover (AVRCP 1.6 with
  cover art over OBEX). It pairs (with a code on screen where the other
  side has one), reconnects and remembers. Tested with AirPods Pro,
  Powerbeats Pro, Bose QuietComfort 35, a Sonos speaker (over its
  Bluetooth) and a Volvo XC90 (paired from the player, with a code on both
  screens; the song's title, artist and album on its screen).
- **A calm interface.** An iPod-like library (albums with covers, artists,
  songs), Now Playing, a lock screen, in Geist Mono with a dot-matrix touch.
- **Yours.** No account, no network, nothing phoning home. Your music is
  folders on a card. Apache-2.0.

Written from scratch: no code from other firmware (see [NOTICE](NOTICE)
and "Clean room" below).

## Status

Early, but in daily use on one player. Supported hardware:

| Player | Chip | State |
|---|---|---|
| HiFi Walker H2 (hardware revision 4), also sold as AIGO Eros Q / Surfans F20 | Ingenic X1000, ES9018K2M DAC, CYW43438 Bluetooth | Starts at power-on; screen, buttons and wheel, card (reads and saves), sound, Bluetooth, clock, battery |

Works: library from the card with embedded covers, cached for big
libraries; FLAC / ALAC / WAV / AIFF up to 32-bit / 384 kHz, and MP3 /
Ogg Vorbis / Opus, gapless;
playlists (M3U); seeking (the wheel on Now Playing scrubs); repeat and
shuffle; resume after switching off; volume in the DAC with a volume bar;
a ten-band EQ with its curve, presets you can adjust and reset, and three
custom EQs (off is bit-perfect); Bluetooth headphones, speakers and cars
(several remembered, switch, forget); key lock and a lock screen; screen
dimming and brightness; battery level; sleep timer; date and time;
settings and pairings saved to the card; USB: Transfer files (the card as
a drive) or Use as DAC (the player as the computer's sound card, 44.1 to
384 kHz, bit-perfect at 16 and 24-bit; a 32-bit stream reaches the DAC as
its top 24 bits, as 32-bit files do, since the X1000's I2S carries 24;
tested with macOS).

Not yet: AAC, two
pairs of headphones at once, other players. The plan is in
[docs/roadmap.md](docs/roadmap.md).

## Try it without a player

The same app runs in a Mac simulator (SDL2), including Bluetooth against
simulated headphones:

```bash
brew install cmake ninja sdl2 pkg-config
cmake -S platform/host -B build/host -G Ninja && ninja -C build/host
./build/host/los-sim --music ~/Music      # arrow keys, Return, Esc, Home, L, + / -
```

Music can be in any folders on the card (tags win; `Music/Artist/Album/` is
tidy but not needed) as FLAC, ALAC (`.m4a`), WAV, AIFF, MP3, Ogg Vorbis or
Opus, with covers embedded or as `cover.jpg`; playlists are `.m3u` / `.m3u8`
files. [docs/card-layout.md](docs/card-layout.md) has the details, and the
`/.lossless/player.json` the player leaves for computers. `tools/sim_library.sh` lays out a library the
way the player's card has it.

## Build for the H2

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
python3 tools/mkbootfile.py build/h2/zephyr/zephyr.elf build/h2/zephyr/zephyr.bin out/lossless.erosq
python3 tools/mkuimage.py  build/h2/zephyr/zephyr.elf build/h2/zephyr/zephyr.bin out/uImage
```

## Install on an H2

The H2 needs an open-source bootloader in its flash that starts a boot
file from the card (install one first, following its own instructions;
docs/h2-hardware.md, "The boot file", says what it has to do). Lossless OS
doesn't touch the flash: it replaces the file that bootloader starts at
power-on, and keeps that file so it can be put back.

1. Hold **Vol+** while switching on, choose **USB mode**, connect to a Mac.
2. `tools/h2-install-default.sh out/lossless.erosq out/uImage`

Then a normal power-on starts Lossless OS. To go back:
`tools/h2-restore.sh` (from USB mode). **Play + Power** still starts
the player's original firmware, which stays untouched. Holding **Back**
while switching on starts without writing to the card; holding **Menu**
shows a check screen.

Bluetooth needs the chip's firmware patch from the player's original
firmware on the card, in `/.lossless/bluetooth/BCM4343A1.hcd` (it is not
ours to ship; the installer copies it there if the card has it elsewhere).

## Layout

| Path | What it is |
|---|---|
| `app/` | The app and its audio side: screens, library, playback, FLAC / ALAC / WAV / AIFF, output to DAC or Bluetooth, resampling for Bluetooth, settings. No platform code |
| `app/los.h`, `app/los_sys.h` | What a platform provides to the app |
| `lib/lbt/` | lbt, the Bluetooth stack (HCI to A2DP and AVRCP), with host tests and simulated headphones |
| `platform/host/` | The Mac simulator |
| `platform/zephyr/` | Zephyr start-up and services for the app |
| `platform/h2/` | The H2: screen, buttons, power, sound, Bluetooth chip, clock, backlight |
| `drivers/` | Zephyr drivers: the X1000's SD controller, the screen |
| `soc/`, `boards/`, `dts/` | The X1000 chip and the H2 board for Zephyr |
| `tools/` | Image tools, installers, generators |
| `tests/` | Audio checks against reference decoders, H2 bring-up tests |
| `docs/` | Hardware facts with sources, the card layout, the roadmap |

## Tests

```bash
make -C lib/lbt/test                         # Bluetooth stack, simulated headphones
cc -O2 -Iapp tests/audio/resample_test.c app/resample.c -lm -o build/rt && build/rt
cc -O2 -Iapp tests/audio/seek_test.c app/source.c app/flac.c -o build/st && build/st your/*.flac
tests/audio/check_flac.sh your/*.flac        # every sample against macOS's decoder
tests/image/check_jpegenc.sh                 # the cover encoder against two JPEG decoders
```

## Clean room

Other firmware for these players, and Linux, is GPL; Lossless OS is
Apache-2.0. We read such code only for hardware facts (register addresses, pins, command sequences), write those
facts down in [docs/h2-hardware.md](docs/h2-hardware.md) with their source,
and write our code from the facts. Please keep it that way in
contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0 ([LICENSE](LICENSE)). Third-party parts and their
licences: [NOTICE](NOTICE).
