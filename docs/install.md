# Installing Lossless OS

## Check your player first

Lossless OS is made for one version of the board inside these players
(hardware revision 4). The original firmware's version tells you which
board you have: **System Settings > About The Player > Version**.

| Player | Version |
|---|---|
| HiFi Walker H2 | V2.3 |

That's the version Lossless OS is tested on, the way back to the original
firmware included. For now, don't install it on anything else:

- **H2 V1.9 to V2.2** is the same board, but going back needs HiFi
  Walker's file for that version, and nobody has checked it yet.
- **AIGO Eros Q and Surfans F20** are the same player under other names
  (Eros Q V2.2 to V2.3, F20 V3.4 to V3.6), but going back needs their own
  maker's file, and nobody has checked those yet.
- **Older versions** are an older board. The release's `update.upt` names
  this board (`erosq_2024`), and the original firmware's updater should
  refuse it on an older one, but don't count on that.

Also:

- **The card as it is.** FAT32 and exFAT both work, so there's no need
  to format it, and your music stays where it is.
- **Battery at 30% or more**, or the player plugged in.
- **A cable that carries data**, not one for charging only.

## Before you start: what could go wrong

Lossless OS has been tested on one H2 (V2.3). It's a hobby project, early,
and free, with no warranty (see [LICENSE](../LICENSE)). The risks, plainly:

- **Installing replaces the player's bootloader**, the small program that
  starts first. If that write is cut short, or the player is a version
  nobody has tried, it may not start. The chip has a USB rescue mode built
  into it that nothing can erase, and [rescue.md](rescue.md) uses it to put
  the original bootloader back. That needs a Mac or Linux computer and a
  few Terminal commands.
- **Some V2.3 players have a different flash chip.** Lossless OS reads and
  writes both kinds, but only the Winbond one has been tested on a player.
  If yours is the other, you'd be the first: please say how it went in an
  issue.
- **Some newer players have a different screen.** If the screen stays
  blank or scrambled after installing, the way back (below) works without
  it.

Your music is never touched, and the original firmware stays in the
player: going back is one file and one button.

## What it changes, and what it never does

Lossless OS comes with its own bootloader: the small program the player
runs first at power-on. It lives in the player's flash, in the bootloader
partition, in place of the factory one. The first time, the original
firmware's own recovery puts it there, from an update file on the card,
the same way the original firmware installs its own updates. After that,
our bootloader writes bootloader updates, and the factory bootloader when
you uninstall, itself, and only with USB power in or the battery at 30% or
more (otherwise it says so and changes nothing). Nothing in Lossless OS
itself writes the flash.

At power-on our bootloader:

- **Vol+** held (or the original firmware asked for its recovery), with an
  `update.upt` on the card: takes the bootloader out of it, checks it
  (the boot ROM's header and checksum; for ours, its own checksum too),
  writes it, reads it back, shows the result and restarts. Ours is an
  update; the factory bootloader, from HiFi Walker's own firmware update,
  is how Lossless OS comes off. With no such file: starts the original
  firmware's recovery.
- Otherwise: starts Lossless OS from the card.
- If Lossless OS isn't on the card, or its file doesn't check out: shows
  the lossless mark and "Lossless OS isn't on the card. Copy it to the card
  again from your computer." On battery the player switches off after 30
  seconds; with USB power in, it stays on.

The original firmware stays in the flash, but our bootloader doesn't start
it. To use the original firmware again, uninstall (below).

The recovery installs whatever `update.upt` is on the card. Only leave one
there that you mean to install.

## Install

1. Download `lossless-os-<version>-h2.zip` from the
   [releases](https://github.com/bobbydotdesign/lossless-os/releases).
2. Plug the player into your computer with its USB cable. The card shows
   up as a drive called H2.
3. Copy everything in the zip (`update.upt` and the `Lossless` folder) to
   the top level of the card. Eject H2 and unplug.
4. Switch the player off. Hold **Vol+** while switching it on, until the
   update starts. Let it finish: the player restarts into Lossless OS.

The first start installs Lossless OS from the `Lossless` folder into the
card's hidden `.lossless` folder, without asking. It also deletes
`update.upt`: it has done its job, and left there, Vol+ would only install
it again. The `Lossless` folder can be deleted after.

### What the player checks

- The boot file is whole: its header and CRC-32 check out. Our bootloader
  checks the same before it starts one.
- It writes the new one as `lossless.bin.new`, reads it back and compares
  it, and checks it again. Then it keeps the one there as
  `lossless.bin.old` and renames the new one in.
- If the power goes in between, the next start finishes the job, or puts
  the one before back.
- It deletes `update.upt` only if it's ours: an ISO 9660 image holding our
  bootloader. Anyone else's update file, such as the original firmware's,
  stays where it is.

## Updates

1. Copy the new release's `Lossless` folder to the top level of the card,
   in place of the old one.
2. Switch on as usual. Lossless OS asks **Update to Lossless OS
   ...?** Press **Play** (or **Back** for not now).

It needs at least 30% battery, or USB power. It's only offered for a newer
version than the one running, so an older `Lossless` folder left on the
card never goes back over it. The new version starts next time; the one
before stays as `lossless.bin.old`.

## Uninstall

**Settings > About > Uninstall Lossless OS** shows the steps. Nothing changes on
the player there. To go back to the original firmware:

1. Download `H2-Firmware-V2.3.upt` from
   [HiFi Walker's download page](https://hifiwalker.com/pages/download)
   (61,779,968 bytes, SHA-256
   `289394f23dbea6943d512fa8cf08b4a2361227def515f397d14b7d301d3a1d20`).
   It holds the factory bootloader.
2. Copy it to the top level of the card as `update.upt`.
3. Charge the player to 30% or more, or plug it in.
4. Switch the player off. Hold **Vol+** while switching it on. The screen
   says "Installing the bootloader...", then "The original bootloader is
   back. Restarting...".

The player then starts the original firmware, as it came. Your music stays
on the card. The original firmware's own updater works again too, with
the same file, if you ever want it.

Left on the card: the hidden `.lossless` folder (Lossless OS itself,
settings, pairings, the library list) and the `Lossless` folder if you
kept it. Delete them from a computer.

### If Lossless OS won't start

- If it fails to start three times in a row, it puts the version before
  back by itself, says so, and switches off.
- If the player says Lossless OS isn't on the card, copy the release's
  `Lossless` folder to the card again. Our bootloader starts
  `/Lossless/lossless.bin` when the installed one is missing or doesn't
  check out, and Lossless OS installs itself from it again.
- **Vol+** at power-on with no `update.upt` on the card starts the
  original firmware's recovery.
- If the player shows nothing at all, or a light but no picture, see
  [rescue.md](rescue.md).

## What's on the card

| Where | What |
|---|---|
| `/update.upt` | Our bootloader's update file, from the release. Lossless OS deletes it once the bootloader in it is the one in the flash. |
| `/Lossless/` | The release: `lossless.bin`, `fonts/`, `READ ME.txt`, `SHA256SUMS`. Can be deleted after the first start. |
| `/.lossless/boot/lossless.bin` | Lossless OS, as our bootloader starts it. `lossless.bin.old` is the version before; `lossless.bin.new` is there only while writing. |
| `/.lossless/` | Everything else Lossless OS keeps: settings, logs, fonts, Bluetooth ([card-layout.md](card-layout.md)). |

Outside `/.lossless`, Lossless OS only ever writes playlists in
`/Playlists` (the ones you make on the player, and changes to those
there) and removes `/update.upt` (only ours). Everything else it writes is
inside `/.lossless`.

## Bluetooth

The Bluetooth chip (Infineon CYW43438) needs a small patch loaded at every
start. Every H2 already has one, in its original firmware, which stays on
the player. At its first start Lossless OS finds the player's own patch
there and copies it to `/.lossless/bluetooth/BCM4343A1.hcd`. It only reads
the original firmware, and you don't need to do anything.

If that didn't work, the Bluetooth screen says "No patch for the chip",
and `/.lossless/logs/install.txt` says why. You can then put a patch there
yourself: download
[this file from Infineon](https://raw.githubusercontent.com/Infineon/bluetooth-freertos/e0d13d8e1f49f5522043133856fa268d1a279b32/firmware/COMPONENT_43438/COMPONENT_AW-CU427-P/COMPONENT_HCI-UART/BCM4343A1_001.002.009.0153.0000_Generic_UART_26MHz_wlbga_eLG_lite_AnyCloud.hcd) (35,290 bytes,
SHA-256
`bfa4d94fa82e44ce4e449b3781350986595f6075f16b63bf51693d431e221011`),
which means accepting
[Infineon's licence](https://github.com/Infineon/bluetooth-freertos/blob/994df9e0a319449a24854f696634f94dbdd37c8e/LICENSE.txt),
and save it on the card as `/.lossless/bluetooth/BCM4343A1.hcd` (the
folder starts with a dot, so it's hidden: on a Mac, Cmd+Shift+. shows
it). Switch the player off and on again. A patch you put there is kept.

Without a patch, everything else works.

## Logs

The player writes what it did to `/.lossless/logs/install.txt`.
