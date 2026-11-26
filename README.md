# Lossless OS

Lossless OS is free, open source firmware for music players. For now it
runs on the HiFi Walker H2 with firmware V2.3. More players are planned
(see [What's next](#whats-next)).

The H2 is a great little DAP: a proper DAC, a physical scroll wheel, an SD
card slot, a battery that lasts, and Bluetooth. However, the software
didn't live up to the rest of the device, so I wrote my own. It's a hobby
project that I'm proud of and want to share.

I'll never charge for it, so feel free to use it for as long as you want.
I'll provide updates periodically if I find other ways to improve it.

<!-- Screenshots go here: home, an album, Now Playing, the lock screen. -->

> [!WARNING]
> **Proceed at your own risk.** Installing Lossless OS replaces your
> player's bootloader, the first thing it runs at power-on. I've tested it
> a lot on my own H2 (V2.3), and I'd give it to a friend without worrying,
> because of the safeguards: it won't write on a low battery, it checks the
> file before writing and the flash after, the original firmware stays on
> the player, and the chip has a rescue mode that can't be erased. Still, I
> can't promise nothing will go wrong on yours, and I can't be responsible
> if it does. Lossless OS isn't made or supported by HiFi Walker, AIGO or
> Surfans, and installing it may void your warranty.

## Try it

First check your player: the original firmware's **System Settings >
About The Player > Version** should say V2.3. That's the version it's
tested on, the way back to the original firmware included. The AIGO Eros
Q and the Surfans F20 are the same player under other names, but their way
back hasn't been checked yet, so for those, wait for a later release.

Then:

1. Download the zip from the
   [releases](https://github.com/bobbydotdesign/lossless-os/releases).
2. Plug the player in and copy what's in the zip to the top of its card.
3. Switch the player on while holding Vol+. It does the rest in a few
   minutes.

[docs/install.md](docs/install.md) has the details, and what could go
wrong, plainly. Read it before you start.

Changed your mind? Copy HiFi Walker's own firmware update to the card as
`update.upt`, switch on holding Vol+, and the player is back the way it
came. Your music stays where it is. If a player ever won't start at all,
the chip has a rescue mode that can't be erased:
[docs/rescue.md](docs/rescue.md).

## What's in it

- Bit-perfect playback. Every file plays at its own sample rate and bit
  depth, straight to the DAC, gapless. The DAC handles the volume itself,
  so nothing touches your samples.
- FLAC, ALAC, WAV and AIFF up to 32-bit / 384 kHz, plus AAC, MP3, Ogg
  Vorbis and Opus.
- A library built from your card, in whatever folders you like, with the
  covers from your files.
- Bluetooth for headphones, speakers and cars, with their buttons, volume,
  song info and covers. I wrote the Bluetooth stack (`lbt`) for this, and I
  use it every day with AirPods Pro, Powerbeats Pro, Bose QC35s, a Sonos
  speaker and my car.
- USB DAC mode, up to 384 kHz.
- A 10-band EQ with presets and three slots of your own. There's also a
  mono switch for one-ear listening, which someone asked for on Reddit.
  Both are off by default, and with them off, playback stays bit-perfect.
- Playlists you make on the player: hold Play to add a song to your
  Favourites, put it in a playlist or play it next. They're saved as
  plain `.m3u8` files, so your computer and other apps can read them. M3U
  playlists from your computer work too.
- Shuffle and repeat, resume, a sleep timer, a key lock, a fast letter
  scroll through long lists, and the wheel for volume or scrubbing.
- Themes. Four come built in: Graphite, the default; Gallery, which puts
  your covers first and tints each screen with the album in front;
  Backlight, an old green LCD; and Classic, a nod to the iPod classic.
  Making your own is one small text file; [docs/themes.md](docs/themes.md)
  shows how.

## Join in

I made this for fun and for the people who love these little players as
much as I do. Help is very welcome.

- Found a bug or have an idea? Open an issue. A photo of the screen helps
  a lot.
- Made a theme you love? Send it in, with a photo of the player wearing it.
- Have a player with similar insides? Ports are welcome.
- Want to write code? [CONTRIBUTING.md](CONTRIBUTING.md) has the how.

## Hacking on it

You don't need a player to start. The whole thing runs in a simulator on a
Mac, pretend Bluetooth headphones included:

```bash
brew install cmake ninja sdl2 pkg-config
git clone https://github.com/zephyrproject-rtos/lvgl ../lvgl && git -C ../lvgl checkout 85aa60d
cmake -S platform/host -B build/host -G Ninja -DLVGL_DIR=$PWD/../lvgl && ninja -C build/host
./build/host/los-sim --music ~/Music      # arrow keys, Return, Esc, Home, L, + / -
```

That LVGL is the same one the player is built with. If you've set up
Zephyr for the player ([docs/building.md](docs/building.md)), leave out
the clone and the `-DLVGL_DIR`: it finds it there.

Point it at any folder of music. `tools/sim_library.sh` lays a library out
the way the player's card has it ([docs/card-layout.md](docs/card-layout.md)).

For the real player, start with [docs/building.md](docs/building.md). How
the H2 works, every register and pin along with where the fact came from,
is in [docs/h2-hardware.md](docs/h2-hardware.md).

Under the hood it runs on [Zephyr](https://zephyrproject.org), right on the
player's chip. There's no Linux underneath, just one small image that
starts in a moment and leaves the memory for music. It has its own
bootloader too.

| Folder | What's there |
|---|---|
| `app/` | Screens, library, playback, decoders, output to the DAC or Bluetooth, settings |
| `lib/lbt/` | The Bluetooth stack, with host tests and simulated headphones |
| `boot/` | The bootloader |
| `platform/` | Each player's hardware side (the H2's in `platform/h2`), Zephyr start-up, and the Mac simulator |
| `drivers/`, `soc/`, `boards/`, `dts/` | The X1000 chip and the H2 board for Zephyr |
| `tools/` | Image tools, the release, installers |
| `tests/` | Audio checks against reference decoders, the installer, bring-up tests |
| `docs/` | Installing, building, the players, porting, hardware notes, the card layout, the roadmap |

Some tests to run:

```bash
make -C lib/lbt/test                         # Bluetooth stack, simulated headphones
tests/audio/check_flac.sh your/*.flac        # every sample against macOS's decoder
tests/audio/check_aac.sh your/*.flac         # AAC against macOS's decoder, and seeking
tests/image/check_jpegenc.sh                 # the cover encoder against two JPEG decoders
tests/install/run.sh                         # the installer's path rules and boot file checks
tests/install/sim.sh                         # installing, a cut-short update, Uninstall, in the simulator
tests/stockfw/run.sh V2.3.upt                # finding the Bluetooth patch in HiFi Walker's update file
```

## What's next

- Two pairs of Bluetooth headphones at once, to listen with a friend.
- HE-AAC files.
- More players: the Innioasis Y2 and the HiBy R1, then a smaller edition
  for the Fosi Audio DAP Neo. [docs/players.md](docs/players.md) has where
  each one is at, and [docs/porting.md](docs/porting.md) shows how a port
  fits together, if you'd like to help.

The full list is in [docs/roadmap.md](docs/roadmap.md).

## License

Apache 2.0 ([LICENSE](LICENSE)). The pieces that come from other projects,
and their licences, are listed in [NOTICE](NOTICE).

## The main quest

Lossless OS is a side quest to
[Lossless Locker](https://losslesslocker.com/), an end-to-end solution for
managing, hosting and streaming the music you own, and syncing it to your
DAP, the H2 included.
If you like Lossless OS, take a peek, and help support the cause:
supporting bands, owning your music, and creating the best experiences for
both.

Have fun with it.
