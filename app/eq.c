/*
 * EQ (eq.h). Coefficients from Robert Bristow-Johnson's "Cookbook formulae
 * for audio EQ biquad filter coefficients", computed once per change in
 * floating point, then run in fixed point (Q28) with the rounding error fed
 * back, so the filters stay clean on a chip without an FPU.
 *
 * The sliders say what the sound should be at each band's frequency. An
 * octave-wide filter reaches into its neighbours, so set as they are the
 * bands add up to more than the sliders (two neighbours at +6 make about
 * +8). Each filter's own gain is worked out instead so the bands together
 * land on the sliders: how much each filter does at every band's
 * frequency, a set of ten equations solved for the ten gains, then
 * corrected twice for what the straight-line guess misses. (The idea of
 * graphic EQs that account for their bands' overlap is the usual one; this
 * is our own simple form of it.)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>
#include "eq.h"

const unsigned eq_band_hz[EQ_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

#define Q 28
#define PI 3.14159265358979323846
#define BAND_OCTAVES 1.0             /* each band an octave wide */

#define REF_DB 12.0                  /* how hard one band is pushed to see its reach */
#define MOST_DB 20.0                 /* the furthest any one filter goes */

static void peak(double hz, double db, unsigned rate, double c[5]);
static double band_hz(int b, unsigned rate);

static int32_t q(double v)
{
    return (int32_t)lrint(v * (1 << Q));
}

/* A peaking filter's coefficients, normalised (a0 = 1). The width is the
 * cookbook's bandwidth form, which keeps a band an octave wide near the
 * top of the range too (the digital filter squeezes it there otherwise) */
static void peak(double hz, double db, unsigned rate, double c[5])
{
    double A = pow(10.0, db / 40.0), w = 2 * PI * hz / rate;
    double cw = cos(w), sw = sin(w);
    double alpha = sw * sinh(log(2.0) / 2 * BAND_OCTAVES * w / sw);
    double a0 = 1 + alpha / A;
    c[0] = (1 + alpha * A) / a0;     /* b0 */
    c[1] = -2 * cw / a0;             /* b1 */
    c[2] = (1 - alpha * A) / a0;     /* b2 */
    c[3] = -2 * cw / a0;             /* a1 */
    c[4] = (1 - alpha / A) / a0;     /* a2 */
}

/* A band above the rate's Nyquist stays below it */
static double band_hz(int b, unsigned rate)
{
    return eq_band_hz[b] < rate * 0.45 ? eq_band_hz[b] : rate * 0.45;
}

/* The response in dB of these filter gains at each band's frequency */
static void at_bands(const double g[EQ_BANDS], unsigned rate, double out[EQ_BANDS])
{
    double c[EQ_BANDS][5];
    for (int b = 0; b < EQ_BANDS; b++)
        peak(band_hz(b, rate), g[b], rate, c[b]);
    for (int k = 0; k < EQ_BANDS; k++) {
        double w = 2 * PI * band_hz(k, rate) / rate, c1 = cos(w), c2 = 2 * c1 * c1 - 1, sum = 0;
        for (int b = 0; b < EQ_BANDS; b++) {
            if (g[b] == 0)
                continue;
            const double *h = c[b];
            double num = h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + 2 * (h[0] * h[1] + h[1] * h[2]) * c1 +
                         2 * h[0] * h[2] * c2;
            double den = 1 + h[3] * h[3] + h[4] * h[4] + 2 * (h[3] + h[3] * h[4]) * c1 + 2 * h[4] * c2;
            sum += 10 * log10(num / den);
        }
        out[k] = sum;
    }
}

/* Solve m x = y (Gaussian elimination, largest pivot first); m is used up */
static void solve(double m[EQ_BANDS][EQ_BANDS], const double y[EQ_BANDS], double x[EQ_BANDS])
{
    double r[EQ_BANDS];
    memcpy(r, y, sizeof(r));
    for (int i = 0; i < EQ_BANDS; i++) {
        int p = i;
        for (int k = i + 1; k < EQ_BANDS; k++)
            if (fabs(m[k][i]) > fabs(m[p][i]))
                p = k;
        if (p != i) {
            for (int j = 0; j < EQ_BANDS; j++) {
                double t = m[i][j];
                m[i][j] = m[p][j];
                m[p][j] = t;
            }
            double t = r[i];
            r[i] = r[p];
            r[p] = t;
        }
        for (int k = i + 1; k < EQ_BANDS; k++) {
            double f = m[k][i] / m[i][i];
            for (int j = i; j < EQ_BANDS; j++)
                m[k][j] -= f * m[i][j];
            r[k] -= f * r[i];
        }
    }
    for (int i = EQ_BANDS - 1; i >= 0; i--) {
        double v = r[i];
        for (int j = i + 1; j < EQ_BANDS; j++)
            v -= m[i][j] * x[j];
        x[i] = v / m[i][i];
    }
}

/* Each band's reach at each band's frequency, per dB (by rate: bands near
 * the top move with it) */
static unsigned reach_rate;
static double reach[EQ_BANDS][EQ_BANDS];

static void find_reach(unsigned rate)
{
    if (rate == reach_rate)
        return;
    for (int b = 0; b < EQ_BANDS; b++) {
        double g[EQ_BANDS] = { 0 }, r[EQ_BANDS];
        g[b] = REF_DB;
        at_bands(g, rate, r);
        for (int k = 0; k < EQ_BANDS; k++)
            reach[k][b] = r[k] / REF_DB;
    }
    reach_rate = rate;
}

void eq_design(const int gain_db[EQ_BANDS], unsigned rate, double out[EQ_BANDS])
{
    double want[EQ_BANDS], m[EQ_BANDS][EQ_BANDS], got[EQ_BANDS], miss[EQ_BANDS], step[EQ_BANDS];
    bool any = false;
    for (int b = 0; b < EQ_BANDS; b++) {
        int g = gain_db[b];
        want[b] = g < -EQ_MAX_DB ? -EQ_MAX_DB : g > EQ_MAX_DB ? EQ_MAX_DB : g;
        out[b] = 0;
        any |= g != 0;
    }
    if (!any)
        return;
    find_reach(rate);
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) {
            memcpy(miss, want, sizeof(miss));
        } else {
            at_bands(out, rate, got);
            for (int k = 0; k < EQ_BANDS; k++)
                miss[k] = want[k] - got[k];
        }
        memcpy(m, reach, sizeof(m));
        solve(m, miss, step);
        for (int b = 0; b < EQ_BANDS; b++) {
            double g = out[b] + step[b];
            out[b] = g < -MOST_DB ? -MOST_DB : g > MOST_DB ? MOST_DB : g;
        }
    }
    /* what's too small to hear isn't worth running */
    for (int b = 0; b < EQ_BANDS; b++)
        if (fabs(out[b]) < 0.05)
            out[b] = 0;
}

void eq_setup(eq_state *e, const int gain_db[EQ_BANDS], unsigned rate, unsigned channels)
{
    memset(e, 0, sizeof(*e));
    e->rate = rate;
    e->channels = channels;
    double band[EQ_BANDS];
    eq_design(gain_db, rate, band);
    e->flat = true;
    for (int b = 0; b < EQ_BANDS; b++) {
        int g = gain_db[b];
        e->gain_db[b] = g < -EQ_MAX_DB ? -EQ_MAX_DB : g > EQ_MAX_DB ? EQ_MAX_DB : g;
        e->on[b] = band[b] != 0;
        if (band[b] != 0)
            e->flat = false;
        double c[5];
        peak(band_hz(b, rate), band[b], rate, c);
        e->b0[b] = q(c[0]);
        e->b1[b] = q(c[1]);
        e->b2[b] = q(c[2]);
        e->a1[b] = q(c[3]);
        e->a2[b] = q(c[4]);
    }
    float top = e->flat ? 0 : eq_headroom_db(e->gain_db, rate);
    e->headroom_db = top;
    e->pre = q(pow(10.0, -(double)top / 20.0));
}

float eq_headroom_db(const int gain_db[EQ_BANDS], unsigned rate)
{
    /* the curve's highest point, and a little more for what falls
     * between the points looked at (a tenth of an octave apart) */
    float hz[96], db[96], top = 0;
    double hi = rate * 0.45 < 20000 ? rate * 0.45 : 20000;
    for (int i = 0; i < 96; i++)
        hz[i] = (float)(20 * pow(hi / 20, i / 95.0));
    eq_response(gain_db, rate, hz, 96, db);
    for (int i = 0; i < 96; i++)
        if (db[i] > top)
            top = db[i];
    return top > 0.05f ? top * 1.05f : 0;
}

void eq_response(const int gain_db[EQ_BANDS], unsigned rate, const float *hz, int n, float *db)
{
    /* the bands that do something, then each point once: the product of
     * their power responses, and one logarithm (cheap enough to redraw
     * the curve as the wheel turns, in software floating point) */
    double c[EQ_BANDS][5], band[EQ_BANDS];
    int m = 0;
    eq_design(gain_db, rate, band);
    for (int b = 0; b < EQ_BANDS; b++)
        if (band[b] != 0)
            peak(band_hz(b, rate), band[b], rate, c[m++]);
    for (int i = 0; i < n; i++) {
        if (!m) {
            db[i] = 0;
            continue;
        }
        double w = 2 * PI * (double)hz[i] / rate, c1 = cos(w), c2 = 2 * c1 * c1 - 1, power = 1;
        for (int k = 0; k < m; k++) {
            const double *h = c[k];
            /* |H(e^jw)|^2 = |b0 + b1 z^-1 + b2 z^-2|^2 / |1 + a1 z^-1 + a2 z^-2|^2 */
            double num = h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + 2 * (h[0] * h[1] + h[1] * h[2]) * c1 +
                         2 * h[0] * h[2] * c2;
            double den = 1 + h[3] * h[3] + h[4] * h[4] + 2 * (h[3] + h[3] * h[4]) * c1 + 2 * h[4] * c2;
            power *= num / den;
        }
        db[i] = (float)(10 * log10(power));
    }
}

static inline int32_t clamp24(int64_t v)
{
    return v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : (int32_t)v;
}

void eq_run(eq_state *e, const int32_t *in, int32_t *out, unsigned frames, unsigned bits)
{
    unsigned ch = e->channels, n = frames * ch;
    int up = bits < 24 ? 24 - (int)bits : 0, down = bits > 24 ? (int)bits - 24 : 0;

    for (unsigned i = 0; i < n; i++) {
        int32_t x = down ? in[i] >> down : (int32_t)((uint32_t)in[i] << up);
        if (e->flat) {
            out[i] = x;
            continue;
        }
        unsigned c = i % ch;
        int64_t v = ((int64_t)x * e->pre) >> Q;
        for (int b = 0; b < EQ_BANDS; b++) {
            if (!e->on[b])
                continue;
            int64_t acc = (int64_t)e->b0[b] * v + (int64_t)e->b1[b] * e->x1[b][c] +
                          (int64_t)e->b2[b] * e->x2[b][c] - (int64_t)e->a1[b] * e->y1[b][c] -
                          (int64_t)e->a2[b] * e->y2[b][c] + e->err[b][c];
            int64_t y = acc >> Q;
            e->err[b][c] = acc - (y << Q);           /* what rounding dropped */
            e->x2[b][c] = e->x1[b][c];
            e->x1[b][c] = (int32_t)v;
            e->y2[b][c] = e->y1[b][c];
            e->y1[b][c] = clamp24(y);
            v = e->y1[b][c];
        }
        out[i] = clamp24(v);
    }
}
