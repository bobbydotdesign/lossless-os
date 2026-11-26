/*
 * ALAC decoding (alac.h), written from Apple's published description of the
 * format (Apple's own decoder is open source under the Apache License 2.0).
 *
 * A packet holds elements, each led by a 3-bit tag: one channel (SCE), a
 * channel pair (CPE), filler and data to skip, and an end tag. A channel's
 * samples are either stored as they are ("escape"), or as residuals: an
 * adaptive Golomb-Rice code, run through an adaptive linear predictor.
 * A pair is coded as mid/side ("mix"), and wide samples have their low
 * bytes stored apart ("shifted"), uncompressed.
 *
 * Arithmetic that can overflow is done on uint32_t: the encoder wraps at 32
 * bits, and the samples only come out exact if we wrap the same way.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "alac.h"

enum { EL_SCE, EL_CPE, EL_CCE, EL_LFE, EL_DSE, EL_PCE, EL_FIL, EL_END };

/* Adaptive Golomb-Rice constants */
#define QB_SHIFT 9
#define QB (1 << QB_SHIFT)
#define MMUL_SHIFT 2
#define MDEN_SHIFT (QB_SHIFT - MMUL_SHIFT - 1)
#define MOFF (1 << (MDEN_SHIFT - 2))
#define MEAN_CLAMP 0xffff
#define BITOFF 24
#define MAX_PREFIX 9
#define RUN_BITS 16

static int32_t residual[ALAC_MAX_FRAMES];
static int32_t mix[2][ALAC_MAX_FRAMES];
static uint16_t shifted[2 * ALAC_MAX_FRAMES];

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

int alac_config_read(alac_config *c, const uint8_t *k, size_t len)
{
    if (len < 24)
        return -1;
    c->frame_length = rd32(k);
    /* k[4]: compatible version, 0 */
    c->bits = k[5];
    c->pb = k[6];
    c->mb = k[7];
    c->kb = k[8];
    c->channels = k[9];
    c->max_run = (uint16_t)(k[10] << 8 | k[11]);
    c->max_packet = rd32(k + 12);
    /* k + 16: average bit rate */
    c->rate = rd32(k + 20);
    if (k[4] != 0 || !c->frame_length || c->frame_length > ALAC_MAX_FRAMES ||
        c->channels < 1 || c->channels > 2 || !c->rate || c->kb > 31)
        return -2;
    if (c->bits != 16 && c->bits != 20 && c->bits != 24 && c->bits != 32)
        return -2;
    return 0;
}

/* ---- bits ---- */

typedef struct {
    const uint8_t *data;
    uint32_t pos, end;           /* in bits */
} bits;

/* Up to 32 bits at 'pos' (not moving) */
static uint32_t peek(const bits *b, uint32_t pos, unsigned n)
{
    if (!n)
        return 0;
    const uint8_t *p = b->data + (pos >> 3);
    uint64_t v = (uint64_t)rd32(p) << 8 | p[4];
    return (uint32_t)(v << (24 + (pos & 7)) >> (64 - n));
}

static uint32_t take(bits *b, unsigned n)
{
    uint32_t v = peek(b, b->pos, n);
    b->pos += n;
    return v;
}

static int32_t take_signed(bits *b, unsigned n)
{
    uint32_t v = take(b, n);
    return n < 32 && (v >> (n - 1)) ? (int32_t)(v | ~0u << n) : (int32_t)v;
}

static unsigned ones(uint32_t v)        /* leading 1 bits */
{
    unsigned n = 0;
    while (n < 32 && (v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

static unsigned zeros(uint32_t v)       /* leading 0 bits */
{
    return v ? (unsigned)__builtin_clz(v) : 32;
}

/* One Golomb-Rice value with parameter k (m = 2^k - 1): a run of 1s ended
 * by a 0, then k bits. A k-bit value of 0 or 1 means only k - 1 of those
 * bits belong to this value. Nine or more 1s: the value follows as
 * 'escape_bits' plain bits. */
static uint32_t rice(bits *b, unsigned k, uint32_t m, unsigned escape_bits)
{
    unsigned q = ones(peek(b, b->pos, 32));
    if (q >= MAX_PREFIX) {
        b->pos += MAX_PREFIX;
        return take(b, escape_bits);
    }
    b->pos += q + 1;
    uint32_t v = peek(b, b->pos, k);
    if (v < 2) {
        b->pos += k - 1;
        return q * m;
    }
    b->pos += k;
    return q * m + v - 1;
}

static unsigned lg3a(uint32_t x)
{
    return 31 - zeros(x + 3);
}

/* The residuals of one channel */
static int residuals(bits *b, const alac_config *c, unsigned pb_factor, int32_t *out,
                     unsigned n, unsigned sample_bits)
{
    uint32_t pb = (uint32_t)c->pb * pb_factor / 4, wb = (1u << c->kb) - 1;
    uint32_t mb = c->mb;
    unsigned zmode = 0, i = 0;
    while (i < n) {
        if (b->pos >= b->end)
            return -1;
        unsigned k = lg3a(mb >> QB_SHIFT);
        if (k > c->kb)
            k = c->kb;
        uint32_t v = rice(b, k, (1u << k) - 1, sample_bits);
        uint32_t d = v + zmode;
        out[i++] = (d & 1) ? -(int32_t)((d + 1) >> 1) : (int32_t)(d >> 1);
        mb = pb * (v + zmode) + mb - ((pb * mb) >> QB_SHIFT);
        if (v > MEAN_CLAMP)
            mb = MEAN_CLAMP;
        zmode = 0;
        if ((mb << MMUL_SHIFT) < QB && i < n) {
            /* a run of zeros */
            zmode = 1;
            unsigned rk = zeros(mb) - BITOFF + ((mb + MOFF) >> MDEN_SHIFT);
            uint32_t run = rice(b, rk, ((1u << rk) - 1) & wb, RUN_BITS);
            if (run > n - i)
                return -1;
            memset(out + i, 0, run * sizeof(int32_t));
            i += run;
            if (run >= 65535)
                zmode = 0;
            mb = 0;
        }
    }
    return 0;
}

/* ---- the predictor ---- */

static int sign(int32_t v) { return (v > 0) - (v < 0); }

static int32_t wrap(uint32_t v, unsigned shift)   /* keep the low (32 - shift) bits, signed */
{
    return (int32_t)(v << shift) >> shift;
}

/* Residuals 'in' to samples 'out' through an adaptive predictor with
 * 'order' coefficients (31: just a running sum). in and out may be the same
 * only for order 31. */
static void predict(const int32_t *in, int32_t *out, unsigned n, int16_t *coef, unsigned order,
                    unsigned sample_bits, unsigned den_shift)
{
    unsigned shift = 32 - sample_bits;
    out[0] = in[0];
    if (order == 0) {
        if (out != in)
            memcpy(out + 1, in + 1, (n - 1) * sizeof(int32_t));
        return;
    }
    if (order == 31) {
        int32_t prev = out[0];
        for (unsigned j = 1; j < n; j++) {
            prev = wrap((uint32_t)in[j] + (uint32_t)prev, shift);
            out[j] = prev;
        }
        return;
    }
    for (unsigned j = 1; j <= order && j < n; j++)
        out[j] = wrap((uint32_t)in[j] + (uint32_t)out[j - 1], shift);
    uint32_t half = den_shift ? 1u << (den_shift - 1) : 0;
    for (unsigned j = order + 1; j < n; j++) {
        const int32_t *past = out + j - 1;
        int32_t top = out[j - order - 1];
        uint32_t sum = 0;
        for (unsigned k = 0; k < order; k++)
            sum += (uint32_t)(int32_t)coef[k] * ((uint32_t)past[-(int)k] - (uint32_t)top);
        int32_t d = in[j], d0 = d;
        int s = sign(d);
        out[j] = wrap((uint32_t)d + (uint32_t)top + (uint32_t)((int32_t)(sum + half) >> den_shift), shift);
        /* the coefficients follow the error */
        if (s > 0) {
            for (int k = (int)order - 1; k >= 0; k--) {
                int32_t dd = (int32_t)((uint32_t)top - (uint32_t)past[-k]);
                int sg = sign(dd);
                coef[k] = (int16_t)(coef[k] - sg);
                d0 -= (int32_t)(order - (unsigned)k) * ((sg * dd) >> den_shift);
                if (d0 <= 0)
                    break;
            }
        } else if (s < 0) {
            for (int k = (int)order - 1; k >= 0; k--) {
                int32_t dd = (int32_t)((uint32_t)top - (uint32_t)past[-k]);
                int sg = sign(dd);
                coef[k] = (int16_t)(coef[k] + sg);
                d0 -= (int32_t)(order - (unsigned)k) * ((-sg * dd) >> den_shift);
                if (d0 >= 0)
                    break;
            }
        }
    }
}

/* ---- elements ---- */

typedef struct {
    unsigned mode, den_shift, pb_factor, order;
    int16_t coef[32];
} channel_params;

static void read_params(bits *b, channel_params *p)
{
    uint32_t h = take(b, 8);
    p->mode = h >> 4;
    p->den_shift = h & 15;
    h = take(b, 8);
    p->pb_factor = h >> 5;
    p->order = h & 31;
    for (unsigned i = 0; i < p->order; i++)
        p->coef[i] = (int16_t)take(b, 16);
}

static int decode_channel(bits *b, const alac_config *c, channel_params *p, int32_t *out,
                          unsigned n, unsigned sample_bits)
{
    if (residuals(b, c, p->pb_factor, residual, n, sample_bits))
        return -1;
    if (p->mode != 0) {
        /* first a running sum, then the predictor */
        predict(residual, residual, n, NULL, 31, sample_bits, 0);
    }
    predict(residual, out, n, p->coef, p->order, sample_bits, p->den_shift);
    return 0;
}

/* One channel (channels == 1) or a pair, into out at 'first' of 'stride' */
static int element(bits *b, const alac_config *c, unsigned pair, int32_t *out, unsigned stride,
                   unsigned first, unsigned *frames)
{
    take(b, 4);                                   /* element instance */
    take(b, 12);                                  /* unused */
    uint32_t h = take(b, 4);
    unsigned partial = h >> 3, bytes_shifted = (h >> 1) & 3, escape = h & 1;
    unsigned n = c->frame_length;
    if (partial)
        n = take(b, 32);
    if (n > c->frame_length || n == 0 || bytes_shifted * 8 >= c->bits)
        return -1;
    unsigned nch = pair ? 2 : 1;
    unsigned sample_bits = c->bits - bytes_shifted * 8 + (pair ? 1 : 0);
    unsigned mix_bits = 0;
    int mix_res = 0;

    if (!escape) {
        mix_bits = take(b, 8);
        mix_res = (int8_t)take(b, 8);
        channel_params p[2];
        for (unsigned ch = 0; ch < nch; ch++)
            read_params(b, &p[ch]);
        bits low = *b;                            /* the shifted-out bytes come first */
        b->pos += bytes_shifted * 8 * nch * n;
        for (unsigned ch = 0; ch < nch; ch++)
            if (decode_channel(b, c, &p[ch], mix[ch], n, sample_bits))
                return -1;
        for (unsigned i = 0; i < n * nch && bytes_shifted; i++)
            shifted[i] = (uint16_t)take(&low, bytes_shifted * 8);
    } else {
        /* stored as they are, at full width, channels interleaved */
        sample_bits = c->bits;
        for (unsigned i = 0; i < n; i++)
            for (unsigned ch = 0; ch < nch; ch++)
                mix[ch][i] = take_signed(b, sample_bits);
        bytes_shifted = 0;
    }
    if (b->pos > b->end)
        return -1;

    unsigned shift = bytes_shifted * 8;
    for (unsigned i = 0; i < n; i++) {
        int32_t l = mix[0][i], r;
        if (pair) {
            int32_t u = mix[0][i], v = mix[1][i];
            if (mix_res) {
                l = (int32_t)((uint32_t)u + (uint32_t)v - (uint32_t)((mix_res * v) >> mix_bits));
                r = (int32_t)((uint32_t)l - (uint32_t)v);
            } else {
                l = u;
                r = v;
            }
            if (shift) {
                l = (int32_t)((uint32_t)l << shift | shifted[2 * i]);
                r = (int32_t)((uint32_t)r << shift | shifted[2 * i + 1]);
            }
            out[(size_t)i * stride + first + 1] = r;
        } else if (shift) {
            l = (int32_t)((uint32_t)l << shift | shifted[i]);
        }
        out[(size_t)i * stride + first] = l;
    }
    *frames = n;
    return 0;
}

int alac_decode(const alac_config *c, const uint8_t *packet, size_t len, int32_t *out)
{
    bits b = { packet, 0, (uint32_t)len * 8 };
    unsigned ch = 0, frames = 0;
    for (;;) {
        if (b.pos + 3 > b.end)
            return -1;
        unsigned tag = take(&b, 3);
        switch (tag) {
        case EL_SCE:
        case EL_LFE:
        case EL_CPE: {
            unsigned pair = tag == EL_CPE, n;
            if (ch + 1 + pair > c->channels)
                return -1;
            if (element(&b, c, pair, out, c->channels, ch, &n))
                return -1;
            if (ch && n != frames)
                return -1;
            frames = n;
            ch += 1 + pair;
            break;
        }
        case EL_DSE: {
            take(&b, 4);
            unsigned align = take(&b, 1), count = take(&b, 8);
            if (count == 255)
                count += take(&b, 8);
            if (align)
                b.pos = (b.pos + 7) & ~7u;
            b.pos += count * 8;
            break;
        }
        case EL_FIL: {
            unsigned count = take(&b, 4);
            if (count == 15)
                count += take(&b, 8) - 1;
            b.pos += count * 8;
            break;
        }
        case EL_END:
            return ch == c->channels ? (int)frames : -1;
        default:
            return -1;                            /* CCE, PCE: never written by encoders */
        }
        if (b.pos > b.end)
            return -1;
    }
}
