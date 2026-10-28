/*
 * Lossless OS: FLAC decoding, from the specification (RFC 9639).
 *
 * A FLAC stream is "fLaC", metadata blocks (STREAMINFO first), then frames.
 * Each frame holds one block of samples per channel; each channel is a
 * subframe: a constant, the samples as they are, or a prediction (fixed
 * polynomial or LPC) plus a Rice-coded residual. Stereo frames can store
 * left/side, side/right or mid/side, undone at the end.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "flac.h"

/* ---- bits ---------------------------------------------------------------------- */

static bool load_byte(flac_dec *d)
{
    if (d->pos >= d->len) {
        if (d->eof)
            return false;
        d->len = d->read(d->ctx, d->buf, sizeof(d->buf));
        d->pos = 0;
        if (d->len == 0) {
            d->eof = true;
            return false;
        }
    }
    d->bits_cache = (d->bits_cache << 8) | d->buf[d->pos++];
    d->bits_left += 8;
    return true;
}

/* n bits (0..32), most significant first; -1 in *ok at the end */
static uint32_t get_bits(flac_dec *d, int n, bool *ok)
{
    if (n == 0)
        return 0;
    while (d->bits_left < n)
        if (!load_byte(d)) {
            *ok = false;
            return 0;
        }
    d->bits_left -= n;
    return (uint32_t)(d->bits_cache >> d->bits_left) & (uint32_t)(((uint64_t)1 << n) - 1);
}

static int32_t get_signed(flac_dec *d, int n, bool *ok)
{
    if (n == 0)
        return 0;
    uint32_t v = get_bits(d, n, ok);
    if (n < 32 && (v & (1u << (n - 1))))
        v |= ~0u << n;
    return (int32_t)v;
}

/* Count 0 bits up to (and past) the next 1 */
static uint32_t get_unary(flac_dec *d, bool *ok)
{
    uint32_t n = 0;
    for (;;) {
        if (d->bits_left == 0 && !load_byte(d)) {
            *ok = false;
            return 0;
        }
        uint64_t v = d->bits_cache << (64 - d->bits_left);
        if (v) {
            int z = __builtin_clzll(v);
            n += (uint32_t)z;
            d->bits_left -= z + 1;
            return n;
        }
        n += (uint32_t)d->bits_left;
        d->bits_left = 0;
    }
}

static void align_byte(flac_dec *d)
{
    d->bits_left -= d->bits_left % 8;
}

/* Skip n bytes (byte-aligned): what's buffered, then the read callback's
 * skip (buf NULL) so big pictures needn't be read */
static void skip_bytes(flac_dec *d, uint32_t n)
{
    align_byte(d);
    while (n && d->bits_left >= 8) {
        d->bits_left -= 8;
        n--;
    }
    size_t have = d->len - d->pos;
    if (n <= have) {
        d->pos += n;
        return;
    }
    n -= (uint32_t)have;
    d->pos = d->len;
    size_t done = d->read(d->ctx, NULL, n);
    if (done < n)
        d->eof = true;
}

/* ---- header -------------------------------------------------------------------- */

int flac_open(flac_dec *d, flac_read_fn read, void *ctx, void *(*malloc_fn)(size_t))
{
    memset(d, 0, sizeof(*d));
    d->read = read;
    d->ctx = ctx;
    bool ok = true;

    if (get_bits(d, 32, &ok) != 0x664c6143 || !ok)   /* "fLaC" */
        return -1;
    bool have_info = false, last = false;
    while (!last && ok) {
        last = get_bits(d, 1, &ok);
        uint32_t type = get_bits(d, 7, &ok), len = get_bits(d, 24, &ok);
        if (type == 0 && len >= 34) {
            get_bits(d, 16, &ok);                       /* min block */
            d->max_block = get_bits(d, 16, &ok);
            get_bits(d, 24, &ok);                       /* min, max frame */
            get_bits(d, 24, &ok);
            d->rate = get_bits(d, 20, &ok);
            d->channels = get_bits(d, 3, &ok) + 1;
            d->bits = get_bits(d, 5, &ok) + 1;
            d->total_samples = (uint64_t)get_bits(d, 4, &ok) << 32;
            d->total_samples |= get_bits(d, 32, &ok);
            skip_bytes(d, 16 + (len - 34));             /* MD5 */
            have_info = true;
        } else {
            skip_bytes(d, len);
        }
    }
    if (!ok || !have_info || d->channels > FLAC_MAX_CHANNELS || d->bits > 32 ||
        d->max_block == 0)
        return -2;

    int32_t *mem = malloc_fn((size_t)d->channels * d->max_block * sizeof(int32_t));
    if (!mem)
        return -3;
    d->store = mem;
    for (unsigned c = 0; c < d->channels; c++)
        d->samples[c] = mem + (size_t)c * d->max_block;
    return 0;
}

void flac_reset(flac_dec *d)
{
    d->len = d->pos = 0;
    d->bits_cache = 0;
    d->bits_left = 0;
    d->eof = false;
    d->block = 0;
}

size_t flac_buffered(const flac_dec *d)
{
    return (d->len - d->pos) + (size_t)(d->bits_left / 8);
}

void flac_close(flac_dec *d, void (*free_fn)(void *))
{
    if (d->store)
        free_fn(d->store);
    d->store = NULL;
}

/* ---- frames -------------------------------------------------------------------- */

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static int residual(flac_dec *d, int32_t *out, unsigned block, unsigned order)
{
    bool ok = true;
    unsigned method = get_bits(d, 2, &ok);
    if (method > 1)
        return -1;
    int param_bits = method ? 5 : 4;
    uint32_t escape = method ? 31 : 15;
    unsigned porder = get_bits(d, 4, &ok);
    unsigned parts = 1u << porder;
    if ((block >> porder) < order && porder)
        return -1;
    unsigned i = order;
    for (unsigned p = 0; p < parts && ok; p++) {
        unsigned n = (block >> porder) - (p == 0 ? order : 0);
        uint32_t k = get_bits(d, param_bits, &ok);
        if (k == escape) {
            unsigned nb = get_bits(d, 5, &ok);
            for (unsigned j = 0; j < n; j++)
                out[i++] = get_signed(d, (int)nb, &ok);
        } else {
            for (unsigned j = 0; j < n && ok; j++) {
                uint32_t q = get_unary(d, &ok);
                uint32_t u = (q << k) | get_bits(d, (int)k, &ok);
                out[i++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
    }
    return ok ? 0 : -1;
}

static int subframe(flac_dec *d, int32_t *out, unsigned block, int bps)
{
    bool ok = true;
    if (get_bits(d, 1, &ok))
        return -1;                                     /* padding bit must be 0 */
    unsigned type = get_bits(d, 6, &ok);
    int wasted = 0;
    if (get_bits(d, 1, &ok))
        wasted = (int)get_unary(d, &ok) + 1;
    bps -= wasted;
    if (!ok || bps <= 0)
        return -1;

    if (type == 0) {                                   /* constant */
        int32_t v = get_signed(d, bps, &ok);
        for (unsigned i = 0; i < block; i++)
            out[i] = v;
    } else if (type == 1) {                            /* verbatim */
        for (unsigned i = 0; i < block; i++)
            out[i] = get_signed(d, bps, &ok);
    } else if (type >= 8 && type <= 12) {              /* fixed, order 0-4 */
        unsigned order = type - 8;
        if (order > block)
            return -1;
        for (unsigned i = 0; i < order; i++)
            out[i] = get_signed(d, bps, &ok);
        if (!ok || residual(d, out, block, order))
            return -1;
        for (unsigned i = order; i < block; i++) {
            int64_t p;
            switch (order) {
            case 0: p = 0; break;
            case 1: p = out[i - 1]; break;
            case 2: p = 2 * (int64_t)out[i - 1] - out[i - 2]; break;
            case 3: p = 3 * ((int64_t)out[i - 1] - out[i - 2]) + out[i - 3]; break;
            default:
                p = 4 * ((int64_t)out[i - 1] + out[i - 3]) - 6 * (int64_t)out[i - 2] - out[i - 4];
                break;
            }
            out[i] += (int32_t)p;
        }
    } else if (type >= 32) {                           /* LPC, order 1-32 */
        unsigned order = type - 31;
        if (order > block)
            return -1;
        for (unsigned i = 0; i < order; i++)
            out[i] = get_signed(d, bps, &ok);
        unsigned prec = get_bits(d, 4, &ok) + 1;
        int shift = get_signed(d, 5, &ok);
        if (prec == 16 || shift < 0)
            return -1;
        int32_t coef[32];
        for (unsigned j = 0; j < order; j++)
            coef[j] = get_signed(d, (int)prec, &ok);
        if (!ok || residual(d, out, block, order))
            return -1;
        for (unsigned i = order; i < block; i++) {
            int64_t sum = 0;
            for (unsigned j = 0; j < order; j++)
                sum += (int64_t)coef[j] * out[i - 1 - j];
            out[i] += (int32_t)(sum >> shift);
        }
    } else {
        return -1;
    }
    if (!ok)
        return -1;
    if (wasted)
        for (unsigned i = 0; i < block; i++)
            out[i] = (int32_t)((uint32_t)out[i] << wasted);
    return 0;
}

static const unsigned rates[12] = {
    0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000,
};
static const unsigned sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

int flac_next(flac_dec *d)
{
    bool ok = true;
    uint8_t h[16];
    unsigned n;

    align_byte(d);
    /* find the sync code: 0xff, then 0xf8 or 0xf9 */
    uint32_t prev = 0, sync;
    for (;;) {
        sync = get_bits(d, 8, &ok);
        if (!ok)
            return 0;
        if (prev == 0xff && (sync & 0xfe) == 0xf8)
            break;
        prev = sync;
    }
    h[0] = 0xff;
    h[1] = (uint8_t)sync;
    h[2] = (uint8_t)get_bits(d, 8, &ok);
    h[3] = (uint8_t)get_bits(d, 8, &ok);
    n = 4;
    /* frame or sample number, UTF-8 style: count the leading 1s */
    uint8_t first = (uint8_t)get_bits(d, 8, &ok);
    h[n++] = first;
    int more = 0;
    if (first & 0x80) {
        for (uint8_t m = 0x40; first & m; m >>= 1)
            more++;
        if (more == 0 || more > 6)
            return -1;
    }
    uint64_t number = more ? first & (0x3fu >> more) : first;
    for (int i = 0; i < more; i++) {
        h[n++] = (uint8_t)get_bits(d, 8, &ok);
        number = number << 6 | (h[n - 1] & 0x3f);
    }

    unsigned bs_code = h[2] >> 4, sr_code = h[2] & 15;
    unsigned ch_code = h[3] >> 4, ss_code = (h[3] >> 1) & 7;
    unsigned block;
    if (bs_code == 1)
        block = 192;
    else if (bs_code >= 2 && bs_code <= 5)
        block = 576u << (bs_code - 2);
    else if (bs_code == 6)
        block = (h[n++] = (uint8_t)get_bits(d, 8, &ok)) + 1;
    else if (bs_code == 7) {
        h[n++] = (uint8_t)get_bits(d, 8, &ok);
        h[n++] = (uint8_t)get_bits(d, 8, &ok);
        block = ((unsigned)h[n - 2] << 8 | h[n - 1]) + 1;
    } else if (bs_code >= 8)
        block = 256u << (bs_code - 8);
    else
        return -1;

    unsigned rate = d->rate;
    if (sr_code >= 1 && sr_code <= 11)
        rate = rates[sr_code];
    else if (sr_code == 12)
        rate = (h[n++] = (uint8_t)get_bits(d, 8, &ok)) * 1000u;
    else if (sr_code == 13 || sr_code == 14) {
        h[n++] = (uint8_t)get_bits(d, 8, &ok);
        h[n++] = (uint8_t)get_bits(d, 8, &ok);
        rate = ((unsigned)h[n - 2] << 8 | h[n - 1]) * (sr_code == 14 ? 10u : 1u);
    } else if (sr_code == 15)
        return -1;
    (void)rate;

    uint8_t crc = (uint8_t)get_bits(d, 8, &ok);
    if (!ok)
        return 0;
    if (crc != crc8(h, n) || (h[3] & 1) || ch_code > 10 || ss_code == 3 ||
        block > d->max_block)
        return -1;                                     /* not a real frame: search on */

    int bps = ss_code ? (int)sizes[ss_code] : (int)d->bits;
    unsigned channels = ch_code < 8 ? ch_code + 1 : 2;
    if (channels != d->channels)
        return -1;

    for (unsigned c = 0; c < channels; c++) {
        int extra = (ch_code == 8 && c == 1) || (ch_code == 9 && c == 0) ||
                    (ch_code == 10 && c == 1);         /* the side channel */
        if (subframe(d, d->samples[c], block, bps + extra))
            return -1;
    }
    align_byte(d);
    get_bits(d, 16, &ok);                              /* CRC-16 */

    int32_t *a = d->samples[0], *b = d->samples[1];
    if (ch_code == 8) {                                /* left, side */
        for (unsigned i = 0; i < block; i++)
            b[i] = a[i] - b[i];
    } else if (ch_code == 9) {                         /* side, right */
        for (unsigned i = 0; i < block; i++)
            a[i] += b[i];
    } else if (ch_code == 10) {                        /* mid, side */
        for (unsigned i = 0; i < block; i++) {
            int32_t mid = (int32_t)((uint32_t)a[i] << 1) | (b[i] & 1), side = b[i];
            a[i] = (mid + side) >> 1;
            b[i] = (mid - side) >> 1;
        }
    }
    /* where this frame starts, from its header: a sample number (variable
     * block sizes) or a frame number (fixed: every frame but the last is
     * max_block long), so it's right after a seek too */
    d->position = (h[1] & 1) ? number : number * d->max_block;
    d->block = block;
    return (int)block;
}
