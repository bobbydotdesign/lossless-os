/*
 * Decode a FLAC file with app/flac.c and write its samples as raw
 * little-endian PCM at the file's own bit depth (what afconvert writes as
 * LEI16 / LEI24), for checking against another decoder. See check.sh.
 *
 *   flac_dump in.flac out.raw
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include "flac.h"

static size_t read_file(void *ctx, uint8_t *buf, size_t n)
{
    if (!buf)
        return fseek(ctx, (long)n, SEEK_CUR) == 0 ? n : 0;
    return fread(buf, 1, n, ctx);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: flac_dump in.flac out.raw\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out)
        return 2;
    static flac_dec d;
    if (flac_open(&d, read_file, in, malloc)) {
        fprintf(stderr, "not FLAC\n");
        return 1;
    }
    int bytes = (int)(d.bits + 7) / 8, errors = 0, n;
    uint64_t total = 0;
    while ((n = flac_next(&d)) != 0) {
        if (n < 0) {
            errors++;
            continue;
        }
        for (int i = 0; i < n; i++)
            for (unsigned c = 0; c < d.channels; c++) {
                int32_t v = d.samples[c][i];
                for (int b = 0; b < bytes; b++)
                    fputc((v >> (8 * b)) & 0xff, out);
            }
        total += (uint64_t)n;
    }
    printf("%u Hz %u bit %u ch, %llu samples (header says %llu), %d bad frames\n",
           d.rate, d.bits, d.channels, (unsigned long long)total,
           (unsigned long long)d.total_samples, errors);
    flac_close(&d, free);
    fclose(out);
    return errors ? 1 : 0;
}
