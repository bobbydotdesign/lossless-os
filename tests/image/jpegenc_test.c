/*
 * jpegenc check: a 200 x 200 picture (gradients, a disc, lines, text-like
 * detail) as a JPEG, written out for a reference decoder to compare
 * (tests/image/check_jpegenc.sh).
 *
 *   cc -O2 -Iapp tests/image/jpegenc_test.c app/jpegenc.c -o build/jt
 *   build/jt out.jpg out.rgb
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include "jpegenc.h"

#define W 200
#define H 200

int main(int argc, char **argv)
{
    static uint16_t px[W * H];
    static uint8_t rgb[W * H * 3], out[200000];
    if (argc < 3)
        return 2;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int r = x * 255 / (W - 1), g = y * 255 / (H - 1), b = (x + y) * 255 / (W + H - 2);
            int dx = x - 120, dy = y - 80;
            if (dx * dx + dy * dy < 40 * 40) { r = 240; g = 200; b = 40; }      /* a disc */
            if (y > 150 && y < 190 && (x / 3) % 2) { r = g = b = 20; }         /* fine stripes */
            if (x == y) { r = g = b = 255; }                                   /* a thin line */
            uint16_t p = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            px[y * W + x] = p;
            /* what the encoder sees: the same 565 values, expanded */
            int r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
            rgb[(y * W + x) * 3] = (uint8_t)(r5 << 3 | r5 >> 2);
            rgb[(y * W + x) * 3 + 1] = (uint8_t)(g6 << 2 | g6 >> 4);
            rgb[(y * W + x) * 3 + 2] = (uint8_t)(b5 << 3 | b5 >> 2);
        }
    size_t n = jpeg_encode_rgb565(px, W, H, W, 85, out, sizeof(out));
    if (!n) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    if (jpeg_encode_rgb565(px, W, H, W, 85, out, 1000) != 0) {
        fprintf(stderr, "a buffer too small wasn't refused\n");
        return 1;
    }
    if (jpeg_encode_rgb565(px, 196, H, W, 85, out, sizeof(out)) != 0) {
        fprintf(stderr, "a width not in whole blocks wasn't refused\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "wb");
    fwrite(out, 1, n, f);
    fclose(f);
    f = fopen(argv[2], "wb");
    fwrite(rgb, 1, sizeof(rgb), f);
    fclose(f);
    printf("%zu bytes\n", n);
    return 0;
}
