/*
 * Checks app/resample.c: a tone comes out at the right pitch and level with
 * little else, tones above the new Nyquist rate are gone, and 16-bit
 * audio at the same rate passes through exactly.
 *
 *   cc -O2 -I app tests/audio/resample_test.c app/resample.c -lm && ./a.out
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "resample.h"

static int failures;

/* Level of 'hz' in the output (dB re full scale) and of everything else */
static void measure(const int16_t *y, size_t n, double rate, double hz, double *tone_db, double *rest_db)
{
    double sc = 0, cc = 0, ss = 0, sc2 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s = sin(2 * M_PI * hz * i / rate), c = cos(2 * M_PI * hz * i / rate);
        sc += y[i * 2] * s; cc += y[i * 2] * c; ss += s * s; sc2 += c * c;
    }
    double a = sc / ss, b = cc / sc2, rest = 0;
    for (size_t i = 0; i < n; i++) {
        double fit = a * sin(2 * M_PI * hz * i / rate) + b * cos(2 * M_PI * hz * i / rate);
        double e = y[i * 2] - fit;
        rest += e * e;
        s2 += y[i * 2] * (double)y[i * 2];
    }
    (void)s2;
    *tone_db = 20 * log10(sqrt(a * a + b * b) / 32768);
    *rest_db = 10 * log10(rest / n / (32768.0 * 32768.0 / 2));
}

static void tone(unsigned in_rate, unsigned bits, double hz, double amp, unsigned out_rate,
                 double want_db, double max_rest_db)
{
    resampler r;
    if (!resample_init(&r, in_rate, bits, 2, out_rate, malloc)) {
        printf("FAIL init %u -> %u\n", in_rate, out_rate);
        failures++;
        return;
    }
    size_t in_n = in_rate / 2;          /* half a second */
    int32_t *x = malloc(in_n * 2 * sizeof(int32_t));
    int16_t *y = malloc(resample_max_out(&r, in_n) * 2 * sizeof(int16_t));
    double scale = ldexp(1, (int)bits - 1);
    for (size_t i = 0; i < in_n; i++)
        x[i * 2] = x[i * 2 + 1] = (int32_t)lrint(amp * scale * sin(2 * M_PI * hz * i / in_rate));
    /* in uneven pieces, as the player feeds it */
    size_t made = 0;
    for (size_t at = 0, piece = 1; at < in_n; at += piece, piece = piece * 7 % 1000 + 1) {
        if (at + piece > in_n)
            piece = in_n - at;
        made += resample_run(&r, x + at * 2, piece, y + made * 2);
    }
    size_t skip = out_rate / 50;        /* the filter's start */
    double t, rest;
    measure(y + skip * 2, made - skip, out_rate, hz, &t, &rest);
    size_t want_n = (size_t)((double)in_n * out_rate / in_rate);
    /* want_db NAN: a tone the output can't hold; only the rest counts */
    bool ok = (isnan(want_db) || fabs(t - want_db) < 0.25) && rest < max_rest_db && llabs((long long)made - (long long)want_n) <= 2;
    printf("%s %6u/%2u -> %u, %5.0f Hz: tone %6.2f dB (want %.2f), rest %6.1f dB, %zu frames\n",
           ok ? "ok  " : "FAIL", in_rate, bits, out_rate, hz, t, want_db, rest, made);
    if (!ok)
        failures++;
    resample_free(&r, free);
    free(x);
    free(y);
}

static void exact_16(void)
{
    resampler r;
    resample_init(&r, 44100, 16, 2, 44100, malloc);
    int32_t x[2000];
    int16_t y[1100 * 2];
    for (int i = 0; i < 2000; i++)
        x[i] = (int32_t)((i * 7919) % 65536) - 32768;
    size_t n = resample_run(&r, x, 1000, y);
    int bad = 0;
    for (size_t i = 0; i < n * 2; i++)
        bad += y[i] != x[i];
    printf("%s 44100/16 -> 44100: %zu frames, %d differ\n", n == 1000 && !bad ? "ok  " : "FAIL", n, bad);
    failures += n != 1000 || bad;
    resample_free(&r, free);
}

int main(void)
{
    exact_16();
    double half = 20 * log10(0.5);
    /* everyday rates: the tone at its level, the rest below -85 dB */
    tone(44100, 24, 1000, 0.5, 44100, half, -90);   /* 16-bit dither: about -93 */
    tone(48000, 24, 1000, 0.5, 44100, half, -85);
    tone(48000, 16, 1000, 0.5, 44100, half, -85);
    tone(88200, 24, 1000, 0.5, 44100, half, -85);
    tone(96000, 24, 1000, 0.5, 44100, half, -85);
    tone(176400, 24, 1000, 0.5, 44100, half, -85);
    tone(192000, 24, 1000, 0.5, 44100, half, -85);
    tone(44100, 24, 1000, 0.5, 48000, half, -85);
    tone(96000, 24, 1000, 0.5, 48000, half, -85);
    /* high in the pass band: still there */
    tone(96000, 24, 18000, 0.5, 44100, half, -80);
    tone(48000, 24, 19000, 0.5, 44100, half, -80);
    /* above the new Nyquist rate (22.05 kHz): gone */
    tone(96000, 24, 30000, 0.5, 44100, NAN, -80);
    tone(96000, 24, 26000, 0.5, 44100, NAN, -78);   /* would land at 18.1 kHz */
    tone(88200, 24, 25000, 0.5, 44100, NAN, -78);   /* would land at 19.1 kHz */
    tone(192000, 24, 60000, 0.5, 44100, NAN, -80);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
