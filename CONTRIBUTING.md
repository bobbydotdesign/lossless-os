# Contributing

Thanks for wanting to help. A few things keep Lossless OS what it is.

## Clean room

No code from GPL projects (other firmware for these players, Linux,
U-Boot) or from players' original firmware. Reading them to learn how the
hardware works is fine: write the facts (addresses, bits, values, order,
timing) into `docs/h2-hardware.md` (or a new file for a new player) with a
"Source:" line, then write the code from those facts. Code from Apache-2.0, MIT,
BSD or zlib projects can be used with its licence and notice (add it to
NOTICE).

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
says so in the pull request. Writes to the card stay inside `/.lossless/`.

## Licence

By contributing you agree your work is licensed under Apache-2.0, as the
rest of the project.
