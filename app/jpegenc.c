/*
 * A small baseline JPEG encoder (jpegenc.h), written from ITU-T T.81
 * (JPEG): sequential DCT, Huffman coded, 8-bit samples; the example
 * quantization tables of its Annex K.1 and K.2 scaled by quality, and the
 * Huffman tables of Annex K.3. JFIF (APP0) so every reader takes it.
 *
 * Integer only (the players have no floating point unit): colour
 * conversion in 16.16, the DCT as two passes of 8-point sums with a 12-bit
 * cosine table. 4:4:4, so an 8 x 8 block is a whole MCU.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <string.h>
#include "jpegenc.h"

/* Annex K.1, K.2, in row order */
static const uint8_t base_luma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,     12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,     14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,   24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t base_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* Zig-zag: the n-th coefficient sent is this one in row order (Figure 5) */
static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Annex K.3: code counts by length (1..16), then the values */
static const uint8_t dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const uint8_t ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

/* 0.5 * C(u) * cos((2x + 1) u pi / 16) in 1/4096 (C(0) = 1/sqrt 2): one
 * pass of the 8 x 8 DCT is 'sum over x of sample * this' */
static const int16_t cosine[8][8] = {
    { 1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448 },
    { 2009, 1703, 1138, 400, -400, -1138, -1703, -2009 },
    { 1892, 784, -784, -1892, -1892, -784, 784, 1892 },
    { 1703, -400, -2009, -1138, 1138, 2009, 400, -1703 },
    { 1448, -1448, -1448, 1448, 1448, -1448, -1448, 1448 },
    { 1138, -2009, 400, 1703, -1703, -400, 2009, -1138 },
    { 784, -1892, 1892, -784, -784, 1892, -1892, 784 },
    { 400, -1138, 1703, -2009, 2009, -1703, 1138, -400 },
};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} huff;

typedef struct {
    uint8_t *out;
    size_t cap, n;
    uint32_t bits;      /* waiting to go out, the oldest highest */
    int nbits;
    int dc[3];          /* the last DC of each component */
    bool full;
} writer;

/* The codes from the counts and values (Annex C) */
static void huff_make(huff *h, const uint8_t bits[16], const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;
    memset(h, 0, sizeof(*h));
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++, k++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

static void put_byte(writer *w, uint8_t b)
{
    if (w->n < w->cap)
        w->out[w->n++] = b;
    else
        w->full = true;
}

static void put_u16(writer *w, uint16_t v)
{
    put_byte(w, (uint8_t)(v >> 8));
    put_byte(w, (uint8_t)v);
}

/* Entropy-coded bits: a 0xff byte is followed by 0x00 (F.1.2.3) */
static void put_bits(writer *w, uint32_t v, int n)
{
    w->bits = (w->bits << n) | (v & ((1u << n) - 1));
    w->nbits += n;
    while (w->nbits >= 8) {
        uint8_t b = (uint8_t)(w->bits >> (w->nbits - 8));
        w->nbits -= 8;
        put_byte(w, b);
        if (b == 0xff)
            put_byte(w, 0);
    }
}

static void flush_bits(writer *w)
{
    if (w->nbits)
        put_bits(w, 0x7f, 8 - w->nbits);   /* padded with ones */
}

/* A value's category (its bit length) and its bits (F.1.2.1) */
static int category(int v)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    return n;
}

static uint32_t value_bits(int v, int n)
{
    return (uint32_t)(v < 0 ? v - 1 : v) & ((1u << n) - 1);
}

static void block(writer *w, const int16_t s[64], const uint8_t q[64], int comp,
                  const huff *dc, const huff *ac)
{
    int tmp[64], coef[64];
    /* rows, back to sample scale, then columns */
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            int sum = 0;
            for (int x = 0; x < 8; x++)
                sum += s[y * 8 + x] * cosine[u][x];
            tmp[y * 8 + u] = (sum + 2048) >> 12;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            int sum = 0;
            for (int y = 0; y < 8; y++)
                sum += tmp[y * 8 + u] * cosine[v][y];
            /* F(v, u) in 1/4096, over the step, rounded */
            int step = q[v * 8 + u] * 4096;
            coef[v * 8 + u] = sum >= 0 ? (sum + step / 2) / step : -((-sum + step / 2) / step);
        }

    int d = coef[0] - w->dc[comp];
    w->dc[comp] = coef[0];
    int n = category(d);
    put_bits(w, dc->code[n], dc->len[n]);
    if (n)
        put_bits(w, value_bits(d, n), n);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int c = coef[zigzag[k]];
        if (!c) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(w, ac->code[0xf0], ac->len[0xf0]);   /* sixteen zeros */
            run -= 16;
        }
        n = category(c);
        int sym = run << 4 | n;
        put_bits(w, ac->code[sym], ac->len[sym]);
        put_bits(w, value_bits(c, n), n);
        run = 0;
    }
    if (run)
        put_bits(w, ac->code[0x00], ac->len[0x00]);       /* end of block */
}

static void dqt(writer *w, int id, const uint8_t q[64])
{
    put_u16(w, 0xffdb);
    put_u16(w, 67);
    put_byte(w, (uint8_t)id);
    for (int k = 0; k < 64; k++)
        put_byte(w, q[zigzag[k]]);
}

static void dht(writer *w, int cls_id, const uint8_t bits[16], const uint8_t *vals)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += bits[i];
    put_u16(w, 0xffc4);
    put_u16(w, (uint16_t)(3 + 16 + n));
    put_byte(w, (uint8_t)cls_id);
    for (int i = 0; i < 16; i++)
        put_byte(w, bits[i]);
    for (int i = 0; i < n; i++)
        put_byte(w, vals[i]);
}

/* Quality 50 is the tables as given; lower scales them up, higher down */
static void scale_table(uint8_t out[64], const uint8_t base[64], int quality)
{
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int i = 0; i < 64; i++) {
        int t = (base[i] * scale + 50) / 100;
        out[i] = (uint8_t)(t < 1 ? 1 : t > 255 ? 255 : t);
    }
}

size_t jpeg_encode_rgb565(const uint16_t *px, int width, int height, int stride,
                          int quality, uint8_t *out, size_t cap)
{
    static huff dcl, dcc, acl, acc;
    static bool made;
    if (width <= 0 || height <= 0 || width % 8 || height % 8 || width > 65535 || height > 65535)
        return 0;
    if (!made) {
        huff_make(&dcl, dc_luma_bits, dc_vals);
        huff_make(&dcc, dc_chroma_bits, dc_vals);
        huff_make(&acl, ac_luma_bits, ac_luma_vals);
        huff_make(&acc, ac_chroma_bits, ac_chroma_vals);
        made = true;
    }
    quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    uint8_t ql[64], qc[64];
    scale_table(ql, base_luma, quality);
    scale_table(qc, base_chroma, quality);

    writer w = { out, cap, 0, 0, 0, { 0, 0, 0 }, false };
    put_u16(&w, 0xffd8);                                   /* SOI */
    static const uint8_t jfif[16] = { 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1 };
    for (int i = 0; i < 16; i++)
        put_byte(&w, jfif[i]);
    put_byte(&w, 0);
    put_byte(&w, 0);                                       /* no thumbnail */
    dqt(&w, 0, ql);
    dqt(&w, 1, qc);
    put_u16(&w, 0xffc0);                                   /* SOF0: baseline */
    put_u16(&w, 17);
    put_byte(&w, 8);
    put_u16(&w, (uint16_t)height);
    put_u16(&w, (uint16_t)width);
    put_byte(&w, 3);
    for (int c = 0; c < 3; c++) {
        put_byte(&w, (uint8_t)(c + 1));
        put_byte(&w, 0x11);                                /* 1 x 1: 4:4:4 */
        put_byte(&w, c ? 1 : 0);
    }
    dht(&w, 0x00, dc_luma_bits, dc_vals);
    dht(&w, 0x10, ac_luma_bits, ac_luma_vals);
    dht(&w, 0x01, dc_chroma_bits, dc_vals);
    dht(&w, 0x11, ac_chroma_bits, ac_chroma_vals);
    put_u16(&w, 0xffda);                                   /* SOS */
    put_u16(&w, 12);
    put_byte(&w, 3);
    for (int c = 0; c < 3; c++) {
        put_byte(&w, (uint8_t)(c + 1));
        put_byte(&w, c ? 0x11 : 0x00);
    }
    put_byte(&w, 0);
    put_byte(&w, 63);
    put_byte(&w, 0);

    int16_t yb[64], cb[64], cr[64];
    for (int by = 0; by < height && !w.full; by += 8)
        for (int bx = 0; bx < width && !w.full; bx += 8) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    uint16_t p = px[(by + y) * stride + bx + x];
                    int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                    r = r << 3 | r >> 2;
                    g = g << 2 | g >> 4;
                    b = b << 3 | b >> 2;
                    /* JFIF's YCbCr, in 16.16, centred on 0 for the DCT */
                    int i = y * 8 + x;
                    yb[i] = (int16_t)(((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128);
                    cb[i] = (int16_t)((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16);
                    cr[i] = (int16_t)((32768 * r - 27439 * g - 5329 * b + 32768) >> 16);
                }
            block(&w, yb, ql, 0, &dcl, &acl);
            block(&w, cb, qc, 1, &dcc, &acc);
            block(&w, cr, qc, 2, &dcc, &acc);
        }
    flush_bits(&w);
    put_u16(&w, 0xffd9);                                   /* EOI */
    return w.full ? 0 : w.n;
}
