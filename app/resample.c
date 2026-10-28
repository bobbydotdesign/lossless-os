/*
 * Polyphase sample rate conversion (see resample.h).
 *
 * Output n falls at input time n * down / up. Zero-stuffing the input by
 * 'up' and low-pass filtering at up * in_rate, then keeping every 'down'th
 * sample, gives y = sum_k h[r + k * up] * x[i - k], with i = n * down / up
 * and r = n * down % up: one short filter per phase r.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>
#include "resample.h"

#define COEF_BITS   20
/* Flat to 20 kHz; anything that would fold back below 20 kHz is down 80 dB.
 * The cut-off sits at the lower Nyquist rate, with the transition
 * (20 to 24.1 kHz at 44.1 kHz out) folding only above 20 kHz. */
#define TAPS_AT_1   64      /* taps per phase when not reducing the rate */
#define KAISER_BETA 8.0     /* about 80 dB down in the stop band */
#define PASS        1.0     /* cut-off, of the lower Nyquist rate */
#define PI          3.14159265358979323846

/* The last table made: the player has no FPU, so making one takes a
 * moment; the next song at the same rates reuses it */
static struct { int32_t *coef; unsigned up, down, taps; } made;

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 40; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

bool resample_init(resampler *r, unsigned in_rate, unsigned bits, unsigned channels,
                   unsigned out_rate, void *(*malloc_fn)(size_t))
{
    memset(r, 0, sizeof(*r));
    if (channels < 1 || channels > 2 || bits < 8 || bits > 32 ||
        in_rate < 8000 || in_rate > 384000 || out_rate < 8000 || out_rate > 96000)
        return false;
    unsigned g = gcd(in_rate, out_rate);
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->up = out_rate / g;
    r->down = in_rate / g;
    r->channels = channels;
    r->shift = (int)bits - 24;
    r->dither = 0x2545f491u;

    if (r->up == r->down) {
        r->taps = 1;
    } else {
        double reduce = r->up < r->down ? (double)r->down / r->up : 1.0;
        unsigned taps = (unsigned)ceil(TAPS_AT_1 * reduce);
        r->taps = taps > RESAMPLE_MAX_TAPS ? RESAMPLE_MAX_TAPS : taps;
    }
    r->coef = malloc_fn(sizeof(int32_t) * r->up * r->taps);
    if (!r->coef)
        return false;

    if (r->taps == 1) {
        r->coef[0] = 1 << COEF_BITS;
        made.coef = NULL;
        return true;
    }
    if (made.coef == r->coef && made.up == r->up && made.down == r->down && made.taps == r->taps)
        return true;

    /* the prototype runs at up * in_rate; cut off just under the lower
     * of the two Nyquist rates */
    unsigned len = r->up * r->taps;
    double fc = PASS * 0.5 / (r->up > r->down ? r->up : r->down);   /* cycles per sample */
    double mid = (len - 1) / 2.0, i0b = bessel_i0(KAISER_BETA);
    for (unsigned p = 0; p < r->up; p++) {
        double sum = 0;
        double h[RESAMPLE_MAX_TAPS];
        for (unsigned k = 0; k < r->taps; k++) {
            double t = (double)(p + k * r->up) - mid;
            double s = t == 0 ? 2 * fc : sin(2 * PI * fc * t) / (PI * t);
            double w = t / mid;
            w = bessel_i0(KAISER_BETA * sqrt(fmax(0, 1 - w * w))) / i0b;
            h[k] = s * w;
            sum += h[k];
        }
        (void)sum;
        /* each phase is a filter of its own: scale all of them together so
         * the average phase passes DC at 1 */
        for (unsigned k = 0; k < r->taps; k++)
            r->coef[p * r->taps + (r->taps - 1 - k)] = (int32_t)lrint(h[k] * r->up * (1 << COEF_BITS));
    }
    made.coef = r->coef;
    made.up = r->up;
    made.down = r->down;
    made.taps = r->taps;
    return true;
}

void resample_free(resampler *r, void (*free_fn)(void *))
{
    if (r->coef == made.coef)
        made.coef = NULL;     /* the memory may come back holding anything */
    if (r->coef)
        free_fn(r->coef);
    r->coef = NULL;
}

size_t resample_max_out(const resampler *r, size_t in)
{
    return (in * r->up + r->down - 1) / r->down + 1;
}

static inline int32_t to24(const resampler *r, int32_t s)
{
    return r->shift >= 0 ? s >> r->shift : (int32_t)((uint32_t)s << -r->shift);
}

/* 24-bit to 16 with triangular dither (none when nothing is lost) */
static inline int16_t to16(resampler *r, int64_t v, bool dither)
{
    if (dither) {
        r->dither = r->dither * 1664525u + 1013904223u;
        int32_t d = (int32_t)(r->dither >> 24) - (int32_t)((r->dither >> 16) & 0xff);
        v += d;
    }
    v = (v + 128) >> 8;
    if (v > 32767)
        v = 32767;
    if (v < -32768)
        v = -32768;
    return (int16_t)v;
}

size_t resample_run(resampler *r, const int32_t *in, size_t frames, int16_t *out)
{
    size_t made = 0;
    unsigned taps = r->taps, ch = r->channels;
    bool dither = r->shift > -8 || r->up != r->down;   /* 16-bit in, same rate: exact */

    for (size_t i = 0; i < frames; i++) {
        unsigned p = r->hist_pos;
        int32_t a = to24(r, in[i * ch]);
        int32_t b = ch == 2 ? to24(r, in[i * ch + 1]) : a;
        r->hist[0][p] = r->hist[0][p + taps] = a;
        r->hist[1][p] = r->hist[1][p + taps] = b;
        r->hist_pos = p + 1 == taps ? 0 : p + 1;

        /* the newest 'taps' inputs, oldest first */
        const int32_t *w0 = &r->hist[0][p + 1], *w1 = &r->hist[1][p + 1];
        while (r->phase < r->up) {
            const int32_t *c = &r->coef[r->phase * taps];
            int64_t s0 = 0, s1 = 0;
            for (unsigned k = 0; k < taps; k++) {
                s0 += (int64_t)c[k] * w0[k];
                s1 += (int64_t)c[k] * w1[k];
            }
            out[made * 2] = to16(r, s0 >> COEF_BITS, dither);
            out[made * 2 + 1] = to16(r, s1 >> COEF_BITS, dither);
            made++;
            r->phase += r->down;
        }
        r->phase -= r->up;
    }
    return made;
}
