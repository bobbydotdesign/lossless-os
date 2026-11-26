# Players

Lossless OS is one app for several music players. The app (screens,
library, playback, Bluetooth, themes, playlists) is shared; each player
brings the part that talks to its own hardware ([porting.md](porting.md)
says how that fits together).

| Player | Inside | Screen and controls | Status |
|---|---|---|---|
| HiFi Walker H2 (also sold as the AIGO Eros Q and the Surfans F20) | Ingenic X1000, ESS ES9018K2M DAC | 320 × 240, scroll wheel and buttons | **Supported** on V2.3: this is the player Lossless OS is built and tested on. Other versions and the Eros Q and F20 once their way back is checked ([install.md](install.md)) |
| Innioasis Y2 | MediaTek MT6582, Cirrus CS43131 DAC | 480 × 360, scroll wheel and buttons | **Next**: researched, port planned |
| HiBy R1 | Ingenic X1600E (the X1000's newer sibling), Cirrus CS43131 DAC | 480 × 800 portrait touchscreen, five buttons, no wheel | **Planned**: researched; close to the H2 inside, needs a touch layout |
| Fosi Audio DAP Neo | Rockchip RKnanoD (two small Cortex-M3 cores, about 1 MB of memory), Cirrus CS43198 DAC | 240 × 320 portrait, scroll wheel and buttons | **Later**: researched; a smaller edition, as the app needs far more memory today |

## HiFi Walker H2

Everything in the README works here. Install and the way back:
[install.md](install.md). How the hardware works, with where each fact
came from: [h2-hardware.md](h2-hardware.md).

## Innioasis Y2

A MediaTek MT6582 (four ARM Cortex-A7 cores, about 1 GB of memory) with a
Cirrus CS43131 DAC, a 480 × 360 screen, a scroll wheel read by a small
microcontroller, and Bluetooth built into the chip.

What's different from the H2:

- **A new chip for Zephyr.** ARM instead of MIPS: Zephyr already knows the
  Cortex-A7 core; the MediaTek parts (timer, card, audio, screen, USB,
  power) are new drivers.
- **It can't start from the card.** The chip only boots from its internal
  storage, so running Lossless OS means writing a small loader to one of
  its partitions, with a backup and a tested way back to the original
  firmware first.
- **A bigger screen.** The app's layouts are made for 320 × 240 today;
  480 × 360 is the first step towards layouts that fit any screen.
- **Hi-res needs checking.** The DAC goes to 384 kHz / 32-bit; whether
  the chip's audio block feeds it more than 16-bit at 44.1 and 48 kHz is
  still to be found out.

## HiBy R1

An Ingenic X1600E, the X1000's newer sibling (the same kind of core, 64 MB
of memory), the same AXP2101 power chip as the H2, a Cirrus CS43131 DAC
and a 480 × 800 portrait touchscreen. Its original firmware comes from
the same maker as the H2's, and its updates aren't signed.

What's different from the H2:

- **Close inside.** Much of the H2's chip support and its power and
  Bluetooth code are a starting point rather than new work. Newer R1s use
  a different Wi-Fi and Bluetooth chip, which would need its own way in.
- **Touch, and no wheel.** Every screen needs a portrait layout made for
  touch. That's most of the work, and it's design as much as code.
- **How it starts.** Its updates carry the kernel but never the
  bootloader, so Lossless OS would take a kernel's place. What the
  bootloader checks, and the surest way back, get worked out first,
  read-only, before anything is written.
- **Hi-res needs checking.** The DAC goes to 384 kHz / 32-bit; how much
  the chip's audio block sends it is still to be found out.

## Fosi Audio DAP Neo

A Rockchip RKnanoD (two small Cortex-M3 cores with about 1 MB of memory on
the chip and none beside it), a Cirrus CS43198 DAC, a 240 × 320 portrait
screen, a scroll wheel, and a Bluetooth module that runs its own stack.
It starts an unsigned image from a file copy, and a recovery mode in the
chip's ROM can always restore it.

What's different from the H2:

- **Much less memory.** The app uses around 14 MB today (screens, covers,
  fonts, the library); the Neo has about 1 MB in all. It's a fit for a
  smaller edition of Lossless OS that shares its design, settings, library
  format and decoders, rather than a straight port.
- **A portrait screen**, so its own layouts.
- **Bluetooth in the module.** It's driven with simple commands rather
  than through the app's own Bluetooth stack (unless it also speaks plain
  HCI, which is still to be found out).

## Want to help?

Own one of these, or a player with similar insides? Issues with what you
know about it (photos of the board, the firmware version, how it updates)
are very welcome. Read [porting.md](porting.md) for how a port is put
together.
