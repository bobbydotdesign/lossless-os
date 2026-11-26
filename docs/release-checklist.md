# Before publishing

## Safety: no player left unable to start, always a way back

What's known (2026-11-25): the chip's USB boot mode is in ROM (Menu held
while plugging in), so a player can always be started over USB: the
rescue kit uses it. The H2 had five board revisions; ours
is hw4 (AXP2101, original firmware V1.9 to V2.3 on the H2, V2.2 to 2.3 on
the Eros Q, V3.4 to 3.6 on the Surfans F20). Older boards should refuse our
`update.upt` (its `name=erosq_2024` is checked against the player; reported,
not tested). Within hw4 there are two flash chips (Winbond W25N01GV, ours;
XTX XT26G01C on some V2.3 units) and a later screen like the hw1/hw2 one.
Another bootloader that didn't know the XTX chip left V2.3 players stuck.

Kept simple on purpose (a free hobby project): no installer app, the docs
on GitHub do the work.

- [x] **Battery before writing flash:** the bootloader writes only with USB
      power in or the battery at 30% or more, and otherwise says so and
      changes nothing.
- [x] **The XTX flash chip:** the flash waits cover both chips' datasheet
      maximums (page read 350 us, program 1.7 ms), and the writer knows
      both IDs. Untested on an XTX player: install.md asks the first one to
      report.
- [x] **Rescue kit** in every release (tools/h2-rescue.sh, docs/rescue.md):
      writes HiFi Walker's factory bootloader, or ours, over USB boot from
      a Mac or Linux computer.
- [x] **Tested on the H2:** the rescue kit with HiFi Walker's V2.3, a
      first install through the original recovery, the low-battery refusal
      (2026-11-26, a V2.3 player with the Winbond chip).
- [x] **Only what's checked:** 0.9 is for the H2 on V2.3 only (README,
      install.md, the release's READ ME and notes).
- [ ] **Later, to widen it:** check HiFi Walker's V2.2 file holds a
      bootloader (UBOOT.BIN), and the Eros Q's and F20's own files, then
      list those versions in install.md.

## Docs

- [x] Who it's for, by the original firmware's version, per brand.
- [x] What it changes and the risks, plainly, before the steps.
- [x] Battery, a data cable. FAT32 and exFAT cards both work (the
      bootloader reads exFAT since 2026-11-25), so no formatting.
- [x] Uninstall with HiFi Walker's download page and the V2.3 file's
      SHA-256.
- [x] Rescue (docs/rescue.md), linked from the README, install.md, the
      release's READ ME and the bootloader's "Install failed" screen.
- [x] Everything points to GitHub, not the Lossless site.

## The repository

- [ ] Publish from a fresh repository made from a clean tree, not this
      history: early commits contain simulator screenshots with album art
      and a Bluetooth trace's device addresses.
- [ ] No pairing keys, logs or recordings: `lib/lbt/test/traces/*.pklg`
      stays out (gitignored); `bt-run*` logs live outside this repository.
- [ ] Licences: LICENSE (Apache-2.0), NOTICE, `lib/lbt/sbc/LICENSE`,
      `assets/fonts/OFL-*.txt`. Check the Noto subset against the OFL's
      rules for modified fonts (it keeps no reserved name).
- [ ] The Bluetooth firmware patch is not included: the player copies its
      own from its original firmware (`app/stockfw.c`), and install.md
      says where to get one by hand.
- [ ] README: hardware list, install and the way back are current.
- [ ] `docs/h2-hardware.md` sources are cited for every fact.
- [ ] Simulator and all tests build and pass on a clean checkout.
- [ ] Our own USB vendor and product IDs (pid.codes gives them to open
      source hardware and firmware) in `platform/h2/h2_usb.c`, in place of
      pid.codes' test ID 1209:0001, which is for testing only.
