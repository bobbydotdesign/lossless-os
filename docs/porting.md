# Porting Lossless OS to another player

Lossless OS keeps the app and the hardware apart, so a new player adds its
own hardware part and shares everything else. This is how the pieces fit,
what a new player has to bring, and what the app still has to learn.

## The layers

```
  app/            the app: screens, library, playback, decoders, EQ,
                  themes, playlists, settings, the installer
  lib/            the Bluetooth stack (lbt), codecs (Opus, ...)
  ───────────────────────────────────────────────────────────────────
  app/los.h,      the platform interface: what the app asks of a player
  app/los_sys.h   (files, audio out, volume, battery, USB, backlight,
                  Bluetooth's transport, threads, the player's name, ...)
  ───────────────────────────────────────────────────────────────────
  platform/h2/    the H2's side of that interface: its DAC, screen, keys,
                  power chip, USB, Bluetooth chip
  platform/zephyr/  start-up and files, the same on every Zephyr player
  platform/host/  the Mac simulator: the same interface, pretend hardware
  ───────────────────────────────────────────────────────────────────
  boards/, soc/,  Zephyr's description of the player and its chip
  drivers/, dts/  (boards/hifiwalker/h2, soc/ingenic/x1000)
  boot/           the H2's own bootloader
  tools/          images, the release zip, installers
```

The app never touches a register. The simulator is the proof: it runs the
same app on a Mac with a second implementation of the platform interface.

## What a new player brings

Named after the player: `platform/<player>/` (the H2's is `platform/h2/`)
and `boards/<maker>/<model>/`.

1. **A research note** first, in the style of the H2's
   [h2-hardware.md](h2-hardware.md): the chip, DAC, screen, controls,
   Bluetooth, power, how it starts, and a tested way back to its own
   firmware. Every fact with where it came from.
2. **Zephyr for its chip.** If Zephyr already supports the chip or a close
   relative, a board file is most of it. If not, a SoC port under `soc/`
   with its timer, interrupts and UART, the way `soc/ingenic/x1000` was
   done for the H2.
3. **The platform interface** in `platform/<player>/`:
   - the card (reading, and writing inside `/.lossless` and `/Playlists`)
   - audio out at each file's own rate and depth, the volume in the DAC
   - the screen (a framebuffer the app draws into) and its backlight
   - keys and the wheel, as the app's key events
   - battery, charging and switching off
   - USB: the card for a computer, and the USB DAC
   - Bluetooth's transport: bringing the chip up so it speaks plain HCI;
     `lbt` does the rest
   - the player's name and the way back to its own firmware
4. **Starting and installing.** How Lossless OS gets onto the player and
   starts: from the card where the player allows it (the H2), or a small
   loader where it doesn't. Always with a tested way back.
5. **A release.** `tools/mkrelease.py` makes one zip per player
   (`lossless-os-<version>-<player>.zip`); the site's installer serves it.

## What the app still has to learn

Honest about what isn't ready for a second player yet:

- **Screen size.** Layouts are placed for 320 × 240. A player with another
  screen needs them to scale or adapt; the Y2's 480 × 360 is the first.
  This is the main piece of work in the app for any new player.
- **Touch.** The app is made for a wheel and buttons. A touchscreen player
  needs touch to work everywhere, not just a few screens.
- **Per-player installing.** The installer (`app/install.c`) and the boot
  file (`app/bootfile.c`) follow the H2's way of starting; another player
  plugs its own in.

## Where to start

Run the simulator ([building.md](building.md)): most of the app can be
worked on without a player at all. For hardware, start from the player's
research note and get a log on its screen before anything else.
