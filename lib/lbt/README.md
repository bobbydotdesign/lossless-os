# lbt: the lossless Bluetooth stack

A small Bluetooth Classic host stack for Lossless OS, written from the
Bluetooth specifications (Apache-2.0). It does only what a music player
needs:

- play to headphones, speakers and cars (A2DP source, SBC)
- take their buttons and share one volume (AVRCP target and controller,
  absolute volume)
- tell a car's screen the song (title, artist, album, number, length,
  position) and give it the cover (AVRCP 1.6 cover art: BIP over OBEX over
  L2CAP in Enhanced Retransmission Mode)
- find, pair with and reconnect to them (inquiry; visible while the player's
  pairing screen is open; Secure Simple Pairing, with a code to confirm on
  both screens when the other side has one; stored link keys)

Later: small data transfers for Locker sync (play history, playlists) over
L2CAP or RFCOMM.

## Licenses

| Piece | Source | License |
|---|---|---|
| lbt (this folder) | written from the specs | Apache-2.0 |
| SBC encoder and decoder | Android's (Broadcom / Open Interface), the `libsbc` module Zephyr ships | Apache-2.0 |
| Chip patch (.hcd) | Cypress patch for this chip family, if it works on the H2 | Cypress binary license; until then the user's own stock firmware |

Code from other Bluetooth stacks isn't copied in. Zephyr is Apache-2.0
like lbt, so its code can be reused with its notices.

Specs: Bluetooth Core 5.3 (Vol 2 Part E HCI, Vol 3 Part A L2CAP, Part B
SDP, Part C GAP, Vol 4 Part A UART transport), GAVDP 1.3, AVDTP 1.3,
A2DP 1.3, AVCTP 1.4, AVRCP 1.6; for cover art IrOBEX 1.5, GOEP 2.0 and
BIP 1.2.

## Layers

```
   app/bluetooth.c (thread, keys, player, volume, screens)
   platform/h2/h2_bt.c (the chip: power, patch, UART) or host/bt.c (simulated)
        |
   source                    a music player's Bluetooth: connect and
        |                    reconnect, audio pacing, buttons, volume
   a2dp ----- avrcp   coverart   gap    profiles
     |          |        |
   avdtp      avctp    obex      sdp    protocols
     \          |        |       /
          l2cap (basic; ERTM for OBEX)   channels over one link
            |
           hci               commands, events, ACL data, credits
            |
        transport            H4 over the UART (or a test harness)
```

Everything runs on one thread (the Bluetooth thread) from one event loop:
bytes from the UART, commands from the UI, and timers. No layer blocks.
Each layer is a plain C file with no platform dependencies, so it builds
and runs in tests on a Mac. The transport and the clock are the only
platform parts (`lbt_platform.h`).

## Tests

`make -C lib/lbt/test` builds and runs host tests:

- unit tests per layer, byte for byte against the spec's packet formats
- a fake controller that plays scripted event sequences
- replays of real traffic recorded on the H2 with AirPods (PacketLogger
  files, see below)
- the whole stack through `source.c` against simulated AirPods
  (`test/sim.c`: a controller with real buffer limits and radio speed,
  headphones that pair, set up audio, press buttons, swipe volume, walk
  out of range, go in the case, and play what arrives through a jitter
  buffer): first sound, pairing from scratch, reconnecting, power cycles,
  and an hour of music with the player stalling now and then.
  `test/test_source <name>` runs one; `LBT_LOG=1` shows the stack's log.

## Finding out what a device does

On the player, `/.lossless/logs/bluetooth.txt` has the stack's log: how
pairing went (what the other side pairs as), which of our services it looks
up (SDP), its first AVRCP commands, whose channels were used. That's
usually enough to see where a new car or speaker stops.

The AirPods session the tests replay was recorded on the H2 as a
PacketLogger file (`test/pklg.py` reads them); Lossless OS doesn't record
yet.

## Milestones

1. HCI and L2CAP, with host tests (done)
2. GAP and security: set-up, inquiry with names, connect, SSP "just works",
   stored keys, legacy PIN (done: `gap.c`)
3. SDP server records for A2DP source and AVRCP (done: `sdp.c`)
4. AVDTP setup, both roles, SBC negotiation and RTP media packets (done:
   `avdtp.c`); PCM to SBC media packets (done: `a2dp.c`, encoder from
   Zephyr's libsbc module)
5. AVCTP and AVRCP: buttons, play status, absolute volume both ways (done:
   `avrcp.c`)
6. First sound on the H2 (done, AirPods Pro 3)
7. The player's logic as a tested layer of its own: reconnecting, idle
   suspend, the AirPods' button quirk (done: `source.c`, `test/sim.c`)
8. Moved into the Lossless OS repository, on Zephyr (done; test 7 on the
   H2 checks it)
9. Pairing keys kept on the card (done); more headphones (done: Powerbeats
   Pro, Bose QuietComfort 35)
10. Speakers and cars (done: a Sonos speaker, a Volvo XC90): both sides
    opening a channel at once (theirs is used), pairing with a code on both
    screens, visible to their searches, the song for their screen, cover
    art (`coverart.c`, with ERTM in `l2cap.c`)
11. Two pairs of headphones at once (after version 1)

The tests use the bytes the AirPods Pro 3 actually sent
(`test/traces/airpods-pro3-session1.md`). The SBC encoder and decoder are
vendored in `sbc/`.
