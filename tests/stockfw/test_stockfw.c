/*
 * The original firmware reader (app/stockfw.c, app/lzo1x.c) on the Mac,
 * against HiFi Walker's V2.3 image (tests/stockfw/run.sh).
 *
 *   test_stockfw lzo system.ubifs          every compressed block decodes
 *   test_stockfw patch flash.bin SHA256    the patch, through the index
 *   test_stockfw scan flash.bin SHA256     the patch, the index broken
 *   test_stockfw none flash.bin            no patch, and a reason
 *   test_stockfw fuzz system.ubifs         damaged blocks fail cleanly
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzo1x.h"
#include "stockfw.h"
#include "los_sys.h"

static FILE *flash;
static uint32_t flash_blocks;

bool los_stock_info(uint32_t *block_size, uint32_t *blocks)
{
    *block_size = 131072;
    *blocks = flash_blocks;
    return flash != NULL;
}

bool los_stock_read(uint32_t block, uint32_t offset, void *buf, uint32_t len)
{
    if (block >= flash_blocks || offset + len > 131072)
        return false;
    uint8_t first[4];
    /* a bad block: all zeros where its marker is (tests/stockfw/mkflash.py) */
    if (fseek(flash, (long)block * 131072, SEEK_SET) || fread(first, 1, 4, flash) != 4)
        return false;
    if (!first[0] && !first[1] && !first[2] && !first[3])
        return false;
    return !fseek(flash, (long)block * 131072 + offset, SEEK_SET) && fread(buf, 1, len, flash) == len;
}

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*n);
    if (fread(p, 1, *n, f) != *n)
        *n = 0;
    fclose(f);
    return p;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* SHA-256, for comparing with what Python saw */
static void sha256(const uint8_t *p, size_t n, char hex[65])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t total = ((n + 9 + 63) / 64) * 64;
    uint8_t *m = calloc(total, 1);
    memcpy(m, p, n);
    m[n] = 0x80;
    for (int i = 0; i < 8; i++)
        m[total - 1 - i] = (uint8_t)(((uint64_t)n * 8) >> (8 * i));
#define R(x, c) ((x) >> (c) | (x) << (32 - (c)))
    for (size_t at = 0; at < total; at += 64) {
        uint32_t w[64], a[8];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[at + 4 * i] << 24 | m[at + 4 * i + 1] << 16 | m[at + 4 * i + 2] << 8 | m[at + 4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10));
        memcpy(a, h, sizeof(a));
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = a[7] + (R(a[4], 6) ^ R(a[4], 11) ^ R(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k[i] + w[i];
            uint32_t t2 = (R(a[0], 2) ^ R(a[0], 13) ^ R(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
            memmove(a + 1, a, 7 * sizeof(uint32_t));
            a[4] += t1;
            a[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++)
            h[i] += a[i];
    }
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", h[i]);
    free(m);
}

/* Every data node of a bare UBIFS image, decoded */
static int lzo_all(const char *path, bool fuzz)
{
    size_t n;
    uint8_t *d = load(path, &n), out[4096];
    int lzo = 0, bad = 0, rejected = 0;
    srand(7);
    for (size_t leb = 0; leb + 126976 <= n; leb += 126976)
        for (size_t off = 0; off + 24 <= 126976;) {
            const uint8_t *p = d + leb + off;
            uint32_t len = le32(p + 16);
            if (le32(p) != 0x06101831u || len < 24 || off + len > 126976) {
                off += 8;
                continue;
            }
            if (p[20] == 1 && p[44] == 1) {
                size_t got;
                uint32_t size = le32(p + 40);
                if (!fuzz) {
                    if (lzo1x_decode(p + 48, len - 48, out, sizeof(out), &got) || got != size)
                        bad++;
                    lzo++;
                } else {
                    /* cut short, too little room, a byte changed: never past the buffers */
                    uint8_t copy[8192];
                    memcpy(copy, p + 48, len - 48);
                    copy[rand() % (len - 48)] ^= (uint8_t)(1 + rand() % 255);
                    if (lzo1x_decode(copy, len - 48, out, sizeof(out), &got) || got != size)
                        rejected++;
                    if (!lzo1x_decode(p + 48, (len - 48) / 2, out, sizeof(out), &got))
                        bad++;
                    if (size > 1 && !lzo1x_decode(p + 48, len - 48, out, size - 1, &got))
                        bad++;
                    lzo++;
                }
            }
            off += (len + 7) & ~7u;
        }
    free(d);
    if (fuzz)
        printf("fuzz: %d blocks, %d with a changed byte rejected, %d cut short or cramped accepted\n",
               lzo, rejected, bad);
    else
        printf("lzo: %d blocks, %d wrong\n", lzo, bad);
    return lzo > 0 && !bad ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && (!strcmp(argv[1], "lzo") || !strcmp(argv[1], "fuzz")))
        return lzo_all(argv[2], !strcmp(argv[1], "fuzz"));
    if (argc < 3)
        return 2;
    flash = fopen(argv[2], "rb");
    if (!flash)
        return 2;
    fseek(flash, 0, SEEK_END);
    flash_blocks = (uint32_t)(ftell(flash) / 131072);

    static uint8_t buf[65536];
    char note[160], hex[65];
    bool thorough = !strcmp(argv[1], "scan");
    size_t n = stockfw_bt_patch(buf, sizeof(buf), thorough, note, sizeof(note));
    if (!strcmp(argv[1], "none")) {
        printf("none: %zu bytes, note \"%s\"\n", n, note);
        return n == 0 && note[0] ? 0 : 1;
    }
    if (!n) {
        printf("%s: not found: %s\n", argv[1], note);
        return 1;
    }
    sha256(buf, n, hex);
    printf("%s: %s, %zu bytes, %s\n", argv[1], note, n, hex);
    if (argc > 3 && strcmp(hex, argv[3])) {
        printf("FAIL: want %s\n", argv[3]);
        return 1;
    }
    /* thorough off with the index broken must say so, not take long */
    return 0;
}
