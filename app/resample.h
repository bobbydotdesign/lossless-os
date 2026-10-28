/*
 * Sample rate conversion for Bluetooth: any rate in, 16-bit stereo out at
 * the rate the headphones agreed to (44.1 or 48 kHz). The wired output
 * never uses this: it plays files at their own rate, bit-perfect.
 *
 * A polyphase windowed-sinc filter (Kaiser window): flat to about 20 kHz,
 * images and aliases down by about 80 dB. The same rate in and out passes
 * the samples through, only reduced to 16 bits (with TPDF dither).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_RESAMPLE_H
#define LOS_RESAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RESAMPLE_MAX_TAPS 320   /* per phase; enough for 192 kHz down to 44.1 */

typedef struct {
    unsigned in_rate, out_rate;
    unsigned up, down;          /* out/in = up/down, reduced */
    unsigned taps;              /* per phase */
    unsigned phase;             /* where the next output falls, 0..up-1 */
    int shift;                  /* input bits to 24 */
    int32_t *coef;              /* up * taps, Q20, phase-major */
    int32_t hist[2][RESAMPLE_MAX_TAPS * 2];   /* last inputs per channel, twice over */
    unsigned hist_pos;
    unsigned channels;
    uint32_t dither;
} resampler;

/* malloc_fn: where the filter table comes from. False if the rates or
 * channels aren't supported (1 or 2 channels, 8 to 384 kHz). */
bool resample_init(resampler *r, unsigned in_rate, unsigned bits, unsigned channels,
                   unsigned out_rate, void *(*malloc_fn)(size_t));
void resample_free(resampler *r, void (*free_fn)(void *));

/* Most output frames 'in' input frames can make */
size_t resample_max_out(const resampler *r, size_t in);

/* Convert 'in' frames (interleaved int32 holding 'bits'-bit samples) to
 * stereo int16 frames in 'out'; returns the frames written. 'out' must hold
 * resample_max_out(in) frames. */
size_t resample_run(resampler *r, const int32_t *in, size_t frames, int16_t *out);

#endif
