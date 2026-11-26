# Roadmap to version 1

Version 1: a player you can hand to someone. Each step is tested on the
H2 before the next.

## Done

- Starts at power-on, straight into the app (no menu), our own screen
  driver; key lock and a lock screen that dims and sleeps
- Library from the card (albums with covers, artists, songs), Now Playing,
  repeat and shuffle, scrubbing with the wheel
- Bit-perfect FLAC / WAV / AIFF to the ES9018K2M DAC, volume in the DAC
- Bluetooth with lbt, our own stack: headphones (AirPods Pro, Powerbeats
  Pro, Bose QuietComfort 35), a speaker (Sonos) and a car (Volvo XC90).
  Pairing with a code on screen where the other side has one, several
  remembered, buttons, one volume, the song's details on a car's screen,
  covers for cars that ask over Bluetooth (AVRCP 1.6 cover art)
- Clock, date and time setting; settings and pairings saved to the card
  (only ever inside `/.lossless`)
- Playlists on the player: Favourites (hold Play on Now Playing), Add to
  playlist, Play next, Remove from playlist, Delete playlist (hold Play on
  a song or a playlist for its panel); plain `.m3u8` files in `/Playlists`
- EQ: off by default (bit-perfect); when on, applied at 24 bits with
  headroom, and the format chip says so; presets you can adjust and reset,
  three custom EQs
- USB from Lossless OS: the player as a drive on a computer (Transfer
  files), with an Ask / Transfer / DAC / Charge setting and a way to choose
  again from Settings
- USB DAC: the player as a computer's sound card (USB Audio Class 2,
  asynchronous, 44.1 to 384 kHz, 16 / 24 / 32-bit, bit-perfect, volume in
  the DAC). Tested with macOS at every rate and depth, switching between
  them (32-bit reaches the DAC as its top 24 bits: the X1000's I2S carries
  24)

Done in the simulator, being tested on the H2:

- Resume where you left off after switching off
- Battery level and charging on screen; switching off safely when it's low
- Screen timeout on every screen (dim, then off), brightness
- Settings: clock 12 / 24 hours, what the wheel does on Now Playing
  (scrub or volume), sleep timer, switching off when idle (5 to 60 minutes,
  or never), About (version, card, battery)
- Playlists (M3U on the card) and a library cache for big libraries
- Gapless playback (a change of sample rate still has a short gap: the
  DAC is set again)
- ALAC (Apple Lossless, `.m4a`), our own decoder
- MP3 (Android's decoder), Ogg Vorbis (Tremor), Opus (libopus): fixed
  point (the build has no hardware floating point); MP3 seeks are exact
  for one bit rate, within about half a second for VBR
- AAC from Apple and others (`.m4a`; Android's decoder, plain AAC),
  gapless and seeking exactly; whatever can't be added is listed, with why

## Version 1 (left)

1. USB DAC tested with Windows and Linux
2. Published: a fresh public repository, our own USB IDs (pid.codes)

## After version 1

- Listen with a friend: two pairs of Bluetooth headphones (or speakers)
  at once (two links in lbt). The jack and headphones together was tried
  and taken out: their delays never quite matched.
- Our own bootloader in flash (writes flash: only with a tested way back)
- More players: the Innioasis Y2 and the HiBy R1, then a smaller edition
  for the Fosi Audio DAP Neo ([players.md](players.md)); layouts that fit
  any screen size come first ([porting.md](porting.md))
- Lossless Locker sync over USB
