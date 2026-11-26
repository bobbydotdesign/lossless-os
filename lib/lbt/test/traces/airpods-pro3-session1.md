# AirPods Pro 3 session 1 (recorded on the H2, 2026-10-27)

`airpods-pro3-session1.pklg`: 116 s, 7,560 packets, recorded on the H2
with the player's earlier Bluetooth. Already paired (stored link key, blanked in this file), auto-reconnect at boot.
ACL packets were cut to 96 bytes when recorded. Summary from `pklg.py`.

What lbt has to do, in the order it happened:

1. **Connect** (we page them). Connection Complete, handle 0x000c, AirPods
   address (left out here).
2. **SDP, us asking them**: one search for their services, then disconnect.
3. **Security**: they ask nothing; we send Authentication Requested, the
   controller asks for the link key (Link Key Request), we reply with the
   stored key, then Set Connection Encryption. Encryption on 0.16 s later.
4. **L2CAP info**: both sides ask for extended features (type 2) and fixed
   channels (type 3). AirPods: features 0x00000280, fixed channels 0x0400010000000040.
5. **SDP, them asking us**: AirPods open SDP to us while we open AVDTP, and
   send nine service searches / attribute requests (looking for our A2DP
   source and AVRCP records). Their SDP MTU: 128 and 256.
6. **AVDTP signaling** (we open it; they answer pending, then success).
   Their MTU 1023.
   - DISCOVER: three sink endpoints, seid 1, 2, 3.
   - GET_ALL_CAPABILITIES for each:
     - seid 1: SBC, `3f ff 02 35` (all rates and modes, all blocks and
       subbands, both allocations, bitpool 2 to 53), delay reporting
     - seid 2: AAC, `c0 01 8c 83 e8 00` (MPEG-2 AAC LC, 44.1 and 48 kHz,
       stereo, VBR, up to 256 kbps)
     - seid 3: vendor codec, Apple (company 0x004c), `00 00 01 80 00 80 00 8c 00 83 e8 00`
   - SET_CONFIGURATION to seid 1: SBC `21 15 02 35` (44.1 kHz, joint
     stereo, 16 blocks, 8 subbands, loudness, bitpool 2 to 53)
   - OPEN, then a second AVDTP channel for audio (MTU 1023), then START
     (accepted after 1.9 s).
7. **AVCTP / AVRCP** (we open it right after OPEN).
   - Both sides ask for capabilities (GetCapabilities, pdu 0x10).
     AirPods support events 0x0d (volume changed); we offered 0x01 and 0x02.
   - We register for volume notifications (event 0x0d); they register for
     playback status (event 0x01).
   - Absolute volume: we set 95/127 on connect; they report every change in
     steps of 8 (each report arrives twice, ~10 ms apart: changed, then the
     re-registered interim); our changes go out as SetAbsoluteVolume and
     they confirm each.
   - Buttons (pass-through press and release, we accept each):
     play 0x44 x5, pause 0x46 x3, forward 0x4b, backward 0x4c.
8. **Audio**: 4,835 media packets, 4.7 MB, RTP with SBC frames.
9. **Disconnect**: we close the audio channel, then signaling, then the link
   goes (reason 0x16, local host).

Notes for lbt:

- Answer L2CAP "pending" correctly: they use it for AVDTP and AVCTP.
- They open SDP towards us during setup, so the SDP server must be up
  before we connect.
- AAC is on offer: a better-sounding option than SBC once we have an
  encoder.
