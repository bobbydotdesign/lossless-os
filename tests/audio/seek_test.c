/*
 * Checks source_seek: decode a file whole, then seek to many places and
 * compare what comes out with the whole decode, sample for sample.
 *
 *   tests/audio/build.sh
 *   build/audio-check/seek_test file.flac file.wav ...   (LOSSY=1 for Opus)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "source.h"
#include "los_sys.h"

void *los_file_open(const char *path) { return fopen(path, "rb"); }
size_t los_file_read(void *f, void *buf, size_t n) { return fread(buf, 1, n, f); }
bool los_file_seek(void *f, uint64_t pos) { return fseeko(f, (off_t)pos, SEEK_SET) == 0; }
void los_file_close(void *f) { fclose(f); }
uint64_t los_file_size(void *f)
{
    off_t at = ftello(f);
    fseeko(f, 0, SEEK_END);
    off_t size = ftello(f);
    fseeko(f, at, SEEK_SET);
    return (uint64_t)size;
}

#define CHECK_FRAMES 4096

/* Lossy formats (Opus): after a seek the decoder settles over its run-up,
 * so samples aren't exact. Instead: close to the whole decode (by signal
 * to difference) and closer than one sample either side (the place is
 * exact). LOSSY=1 in the environment. */
#include <math.h>
static double snr(const int32_t *a, const int32_t *b, size_t n)
{
    double s = 0, e = 0;
    for (size_t i = 0; i < n; i++) {
        s += (double)a[i] * a[i];
        e += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
    }
    return s < 1e6 ? 99 : 10 * log10(s / (e > 0 ? e : 1));
}

int main(int argc, char **argv)
{
    int failures = 0, files = 0, seeks = 0;
    srand(7);
    for (int f = 1; f < argc; f++) {
        los_source a, b;
        if (source_open(&a, argv[f])) {
            printf("skip (can't open): %s\n", argv[f]);
            continue;
        }
        size_t cap = 1 << 20, n = 0;
        unsigned ch = a.channels;
        int32_t *all = malloc(cap * ch * sizeof(int32_t));
        int got;
        while ((got = source_read(&a, all + n * ch, 4096)) > 0) {
            n += (size_t)got;
            if (n + 4096 > cap) {
                cap *= 2;
                all = realloc(all, cap * ch * sizeof(int32_t));
            }
        }
        source_close(&a);
        if (source_open(&b, argv[f]))
            return 1;
        int32_t buf[CHECK_FRAMES * 2];
        int bad = 0;
        for (int k = 0; k < 28; k++) {
            uint64_t t = k == 0 ? 0 : k == 1 ? n - 1 : k == 2 ? n / 2 : (uint64_t)rand() % n;
            if (!source_seek(&b, t)) {
                printf("FAIL seek %llu of %zu: %s\n", (unsigned long long)t, n, argv[f]);
                bad++;
                continue;
            }
            int want = n - t < CHECK_FRAMES ? (int)(n - t) : CHECK_FRAMES, have = 0;
            while (have < want && (got = source_read(&b, buf + have * ch, want - have)) > 0)
                have += got;
            bool wrong;
            if (getenv("LOSSY") && have == want) {
                size_t m = (size_t)want * ch;
                double here = snr(all + t * ch, buf, m);
                double before = t ? snr(all + (t - 1) * ch, buf, m) : -99;
                double after = t + want < n ? snr(all + (t + 1) * ch, buf, m) : -99;
                wrong = here < 30 || (here < 99 && (before >= here || after >= here));
            } else {
                wrong = have != want || memcmp(buf, all + t * ch, (size_t)want * ch * sizeof(int32_t));
            }
            if (wrong) {
                printf("FAIL at %llu (%d of %d frames): %s\n", (unsigned long long)t, have, want, argv[f]);
                bad++;
            }
            seeks++;
        }
        source_close(&b);
        free(all);
        files++;
        failures += bad;
    }
    printf("%d files, %d seeks, %d failed\n", files, seeks, failures);
    return failures != 0;
}
