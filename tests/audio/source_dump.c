/*
 * Read an audio file through app/source.c (any format it knows) and write
 * its samples as raw little-endian PCM at the file's bit depth, for
 * checking against another decoder. See check_sources.sh.
 *
 *   source_dump in.wav out.raw
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include "source.h"
#include "los_sys.h"

void *los_file_open(const char *path) { return fopen(path, "rb"); }
size_t los_file_read(void *f, void *buf, size_t n) { return fread(buf, 1, n, f); }
bool los_file_seek(void *f, uint64_t pos) { return fseek(f, (long)pos, SEEK_SET) == 0; }
void los_file_close(void *f) { fclose(f); }
uint64_t los_file_size(void *f)
{
    long at = ftell(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, at, SEEK_SET);
    return n > 0 ? (uint64_t)n : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    static los_source s;
    int err = source_open(&s, argv[1]);
    if (err) {
        fprintf(stderr, "can't play (%d)\n", err);
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    static int32_t buf[4096 * 2];
    int bytes = (int)(s.bits + 7) / 8, n;
    unsigned long long total = 0;
    while ((n = source_read(&s, buf, 4096)) > 0) {
        for (int i = 0; i < n * (int)s.channels; i++)
            for (int b = 0; b < bytes; b++)
                fputc((buf[i] >> (8 * b)) & 0xff, out);
        total += (unsigned long long)n;
    }
    printf("%s %u Hz %u bit %u ch, %llu frames (header %llu)\n", s.format, s.rate, s.bits,
           s.channels, total, (unsigned long long)s.frames);
    source_close(&s);
    fclose(out);
    return 0;
}
