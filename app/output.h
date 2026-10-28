/*
 * Where the music goes: the player's own output (the DAC, bit-perfect) or
 * Bluetooth headphones. Playback writes here; it never knows which.
 *
 * Bluetooth takes the music as 16-bit stereo at the headphones' rate
 * (resample.c), through a short ring the Bluetooth thread reads in real
 * time.
 *
 * Later, two pairs of headphones at once (listening with a friend).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_OUTPUT_H
#define LOS_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the audio thread (playback.c) ---- */
bool output_start(unsigned rate, unsigned bits, unsigned channels);
size_t output_write(const int32_t *frames, size_t count);   /* taken, never blocks */
uint64_t output_played(void);    /* frames heard since start (any thread) */
void output_pause(bool paused);
void output_stop(void);

/* EQ (eq.h): off is bit-perfect; on, the music goes out at 24 bits
 * through it. Any thread; the audio thread follows at its next write. */
#include "eq.h"
void output_set_eq(bool on, const int gains[EQ_BANDS]);

/* ---- the Bluetooth thread ---- */
/* Headphones are ready for audio at 'rate', or gone (0) */
void output_bluetooth(int rate);
/* 'frames' stereo frames for the headphones; false (and silence) if
 * nothing is playing to them */
bool output_bluetooth_read(int16_t *pcm, int frames);

/* Bluetooth is where the music goes (any thread) */
bool output_is_bluetooth(void);

#endif
