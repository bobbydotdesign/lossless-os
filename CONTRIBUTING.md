# Contributing

Thanks for wanting to help. A few things keep Lossless OS what it is.

## Code from others

Lossless OS is Apache-2.0, so code from others has to fit: Apache-2.0,
MIT, BSD or zlib, with its licence and notice (add it to NOTICE). No GPL
code, and nothing from players' original firmware. How the hardware works
goes into `docs/h2-hardware.md` (or a new file for a new player) as facts
(addresses, bits, values, order, timing) with their source, and drivers are
written from those notes.

## Sound

The audio path stays bit-perfect unless the listener asks otherwise: no
resampling, dither or volume scaling on the way to the DAC. Anything that
changes samples (EQ, Bluetooth's resampling) is off by default or only
where it can't be avoided, and says so on screen.

## Code

- C11, small files that do one thing; comments say why, in plain words.
- `app/` knows nothing about Zephyr or the Mac: what it needs comes through
  `app/los.h` and `app/los_sys.h`.
- New behaviour gets a test where it can: host tests run on a Mac
  (`make -C lib/lbt/test`, `tests/audio/`), and the simulator can run
  scripts and take screenshots (`los-sim --script`).
- Run AddressSanitizer on the simulator for anything touching memory
  (`cmake -DCMAKE_C_FLAGS=-fsanitize=address ...`).

## Hardware

Anything that writes a player's flash needs a tested way back first, and
says so in the pull request. Nothing in Lossless OS writes the flash: our
bootloader goes in and comes off through the original firmware's recovery.
Writes to the card stay inside `/.lossless/`. The installer
(app/install.c) goes through `bootfile_path_ok` (app/bootfile.c), which
allows only the boot file (`/.lossless/boot/lossless.bin`, with `.new` or
`.old`) and `/update.upt`, the one file outside `/.lossless/` it may
remove. Changes there keep the rules in docs/install.md and run
`tests/install/run.sh`.

## Licence

By contributing you agree your work is licensed under Apache-2.0, as the
rest of the project.
