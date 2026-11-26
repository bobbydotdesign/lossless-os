/*
 * EQ: ten bands an octave apart (31 Hz to 16 kHz), off by default. Off
 * means bit-perfect: the samples aren't touched. On: second-order peaking
 * filters (RBJ "Audio EQ Cookbook", an octave wide) in fixed point on
 * 24-bit samples, their gains set so together they land on the sliders at
 * each band's frequency, with the level turned down by the curve's highest
 * point so nothing clips.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_EQ_H
#define LOS_EQ_H

#include <stdbool.h>
#include <stdint.h>

#define EQ_BANDS 10
#define EQ_MAX_DB 12

extern const unsigned eq_band_hz[EQ_BANDS];

typedef struct {
    int gain_db[EQ_BANDS];            /* the sliders */
    bool on[EQ_BANDS];                /* the filters that do something */
    unsigned rate, channels;
    int pre;                          /* level, Q28: down by the curve's highest point */
    float headroom_db;                /* that, in dB (0 when nothing is boosted) */
    int32_t b0[EQ_BANDS], b1[EQ_BANDS], b2[EQ_BANDS], a1[EQ_BANDS], a2[EQ_BANDS];  /* Q28 */
    int32_t x1[EQ_BANDS][2], x2[EQ_BANDS][2], y1[EQ_BANDS][2], y2[EQ_BANDS][2];
    int64_t err[EQ_BANDS][2];         /* rounding carried to the next sample */
    bool flat;                        /* all bands at 0 dB: nothing to do */
} eq_state;

/* Filters for these gains at this rate (clears the history) */
void eq_setup(eq_state *e, const int gain_db[EQ_BANDS], unsigned rate, unsigned channels);

/* frames x channels samples, 'bits' wide in, 24 bits out (in place is fine) */
void eq_run(eq_state *e, const int32_t *in, int32_t *out, unsigned frames, unsigned bits);

/* Each filter's own gain (dB) so that together they make these slider
 * gains at the bands' frequencies */
void eq_design(const int gain_db[EQ_BANDS], unsigned rate, double out[EQ_BANDS]);

/* What these gains do, in dB, at each of n frequencies (for drawing the
 * curve; the level turned down by the largest boost isn't included) */
void eq_response(const int gain_db[EQ_BANDS], unsigned rate, const float *hz, int n, float *db);

/* How far the level is turned down for these gains, in dB (0 if nothing
 * is boosted): the curve's highest point */
float eq_headroom_db(const int gain_db[EQ_BANDS], unsigned rate);

#endif
