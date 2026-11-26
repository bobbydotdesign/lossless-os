/*
 * Checks app/eq.c: each band does what the design says (eq_response) at
 * its frequency, the level drops by the curve's highest point so nothing
 * clips, flat passes samples through exactly, and the filters stay clean
 * (little besides the tone) even at 192 kHz.
 *
 *   cc -O2 -Iapp tests/audio/eq_test.c app/eq.c -lm -o eq_test && ./eq_test
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eq.h"

static int failures;

/* tone level in dB re the input's level, and the rest in dB re full scale */
static void run(unsigned rate, double hz, const int g[EQ_BANDS], double *gain_db, double *rest_db)
{
    eq_state e;
    eq_setup(&e, g, rate, 1);
    unsigned n = rate;                       /* a second */
    int32_t *x = malloc(n * 4), *y = malloc(n * 4);
    double amp = 0.25 * 8388607;
    for (unsigned i = 0; i < n; i++)
        x[i] = (int32_t)lrint(amp * sin(2 * M_PI * hz * i / rate));
    eq_run(&e, x, y, n, 24);
    unsigned skip = rate / 4;                /* filters settle */
    double sc = 0, cc = 0, ss = 0, c2 = 0, rest = 0;
    for (unsigned i = skip; i < n; i++) {
        double s = sin(2 * M_PI * hz * i / rate), c = cos(2 * M_PI * hz * i / rate);
        sc += y[i] * s; cc += y[i] * c; ss += s * s; c2 += c * c;
    }
    double a = sc / ss, b = cc / c2;
    for (unsigned i = skip; i < n; i++) {
        double fit = a * sin(2 * M_PI * hz * i / rate) + b * cos(2 * M_PI * hz * i / rate);
        rest += (y[i] - fit) * (y[i] - fit);
    }
    *gain_db = 20 * log10(sqrt(a * a + b * b) / amp);
    *rest_db = 10 * log10(rest / (n - skip) / (8388607.0 * 8388607.0 / 2));
    free(x);
    free(y);
}

static void check(const char *what, unsigned rate, double hz, const int g[EQ_BANDS], double want, double max_rest)
{
    double gain, rest;
    run(rate, hz, g, &gain, &rest);
    int ok = fabs(gain - want) < 0.6 && rest < max_rest;
    printf("%s %-28s %6u Hz %6.0f Hz: %+6.2f dB (want %+5.1f), rest %6.1f dB\n",
           ok ? "ok  " : "FAIL", what, rate, hz, gain, want, rest);
    failures += !ok;
}

int main(void)
{
    int flat[EQ_BANDS] = { 0 };
    /* flat: exact */
    eq_state e;
    eq_setup(&e, flat, 44100, 2);
    int32_t in[1000], out[1000];
    for (int i = 0; i < 1000; i++)
        in[i] = (i * 7919 % 65536) - 32768;
    eq_run(&e, in, out, 500, 16);
    int bad = 0;
    for (int i = 0; i < 1000; i++)
        bad += out[i] != in[i] * 256;
    printf("%s flat passes 16-bit samples through exactly (%d differ)\n", bad ? "FAIL" : "ok  ", bad);
    failures += bad != 0;

    /* what each tone should come out at: the design's response there,
     * less the level the EQ takes off; the fixed-point filters must match */
    for (int b = 0; b < EQ_BANDS; b++) {
        int g[EQ_BANDS] = { 0 };
        char what[40];
        /* near the band, a whole number of cycles in the window */
        double hz = (double)((eq_band_hz[b] + 2) / 4 * 4);
        for (int sign = 1; sign >= -1; sign -= 2) {
            g[b] = 6 * sign;
            float f = (float)hz, db;
            eq_response(g, 44100, &f, 1, &db);
            snprintf(what, sizeof(what), "%+d dB, band at %u Hz", g[b], eq_band_hz[b]);
            check(what, 44100, hz, g, db - eq_headroom_db(g, 44100), -90);
        }
    }
    struct { const char *what; unsigned rate; double hz; int g[EQ_BANDS]; } cases[] = {
        { "31 Hz +9, a mid tone", 44100, 2000, { 9 } },
        { "31 Hz +9 at 192 kHz, 20 Hz", 192000, 20, { 9 } },
        { "31 Hz +9 at 96 kHz, 20 Hz", 96000, 20, { 9 } },
        { "16 kHz +9 at 192 kHz, 16 kHz", 192000, 16000, { [9] = 9 } },
        { "1k, 2k +12: between them", 44100, 1416, { [5] = 12, [6] = 12 } },
        { "all +12", 44100, 1000, { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 } },
        { "rock at 22.05 kHz, 8 kHz", 22050, 8000, { 4, 4, 2, 0, -1, -1, 0, 2, 3, 3 } },
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        float f = (float)cases[k].hz, db;
        eq_response(cases[k].g, cases[k].rate, &f, 1, &db);
        double want = db - eq_headroom_db(cases[k].g, cases[k].rate);
        check(cases[k].what, cases[k].rate, cases[k].hz, cases[k].g, want, -85);
        if (want > 0.01) {
            printf("FAIL %s: would clip (%+.2f dB)\n", cases[k].what, want);
            failures++;
        }
    }
    /* the curve lands on the sliders at each band's frequency, for the
     * presets and a hard zig-zag */
    static const int sets[][EQ_BANDS] = {
        { 6, 6, 5, 3, 1, 0, 0, 0, 0, 0 },  { 4, 4, 3, 1, -1, -1, 0, 2, 3, 3 },
        { -2, -2, -1, 0, 2, 3, 3, 2, 0, -1 }, { 5, 5, 4, 1, 0, -1, 0, 1, 3, 4 },
        { 6, -6, 6, -6, 6, -6, 6, -6, 6, -6 }, { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 },
    };
    for (unsigned s = 0; s < sizeof(sets) / sizeof(sets[0]); s++) {
        for (unsigned rate = 44100; rate <= 96000; rate += 51900) {
            float hz[EQ_BANDS], db[EQ_BANDS], worst = 0;
            for (int b = 0; b < EQ_BANDS; b++)
                hz[b] = (float)eq_band_hz[b];
            eq_response(sets[s], rate, hz, EQ_BANDS, db);
            for (int b = 0; b < EQ_BANDS; b++)
                if (fabsf(db[b] - sets[s][b]) > worst)
                    worst = fabsf(db[b] - sets[s][b]);
            int ok = worst < 0.3f;
            printf("%s sliders set %u at %u Hz: the curve within %.2f dB of them\n", ok ? "ok  " : "FAIL", s,
                   rate, worst);
            failures += !ok;
        }
    }
    /* a cut alone takes (almost) nothing off the level */
    int cut[EQ_BANDS] = { 0, 0, 0, 0, 0, -6, 0, 0, 0, 0 };
    float h = eq_headroom_db(cut, 44100);
    printf("%s a cut alone turns the level down %.2f dB\n", h < 0.3f ? "ok  " : "FAIL", h);
    failures += h >= 0.3f;

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
