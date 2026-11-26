/*
 * LZO1X decompression (lzo1x.h), written from the format: a stream of
 * literal runs and back-references, each instruction's first byte saying
 * which kind it is by its range:
 *
 *   0-15    a literal run (after a match: a short match, see M1)
 *   16-31   M4: distance 16384 and up, its length in the low 3 bits
 *   32-63   M3: distance up to 16384, its length in the low 5 bits
 *   64-255  M2: distance up to 2048, length 3 to 8, all in two bytes
 *
 * A length of 0 in an instruction's own bits continues in the next
 * bytes: each 0 byte adds 255, then the first non-zero byte ends it. The
 * low 2 bits of the last byte of a match (the distance's low byte, or the
 * instruction itself for M1 and M2) are how many literals follow it, 0 to
 * 3. An M4 with distance 0 ends the stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "lzo1x.h"

#define LZO_LONG_RUN (1u << 24)     /* no length in a 4 KB block comes near this */

int lzo1x_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    const uint8_t *ip = in, *const ie = in + n;
    uint8_t *op = out, *const oe = out + cap;
    size_t t, dist;

    *out_len = 0;

#define NEED_IN(k)  do { if ((size_t)(ie - ip) < (size_t)(k)) return -1; } while (0)
#define NEED_OUT(k) do { if ((size_t)(oe - op) < (size_t)(k)) return -2; } while (0)
#define LITERALS(k) do { size_t k_ = (k); NEED_IN(k_); NEED_OUT(k_); \
                         memcpy(op, ip, k_); op += k_; ip += k_; } while (0)
    /* byte by byte: a match may overlap what it writes */
#define BACK(d, k)  do { size_t d_ = (d), k_ = (k); \
                         if ((size_t)(op - out) < d_) \
                             return -3; \
                         NEED_OUT(k_); \
                         const uint8_t *m_ = op - d_; \
                         for (size_t i_ = 0; i_ < k_; i_++) op[i_] = m_[i_]; \
                         op += k_; } while (0)
    /* a length continued in the bytes after the instruction */
#define LONG(base)  do { NEED_IN(1); \
                         while (*ip == 0) { t += 255; ip++; NEED_IN(1); \
                                            if (t > LZO_LONG_RUN) return -4; } \
                         t += (base) + *ip++; } while (0)

    NEED_IN(1);
    if (*ip > 17) {
        /* a stream may open with a literal run in one byte */
        t = (size_t)*ip++ - 17;
        LITERALS(t);
        if (t < 4) {
            NEED_IN(1);
            t = *ip++;
            goto match;
        }
        goto after_run;
    }

next:
    NEED_IN(1);
    t = *ip++;
    if (t >= 16)
        goto match;
    if (t == 0)
        LONG(15);
    LITERALS(t + 3);

after_run:
    /* right after a literal run of 4 or more, 0-15 is a 3-byte match
     * reaching past M2's range */
    NEED_IN(1);
    t = *ip++;
    if (t >= 16)
        goto match;
    NEED_IN(1);
    dist = 1 + 0x800 + (t >> 2) + ((size_t)*ip++ << 2);
    BACK(dist, 3);
    goto literals_after;

match:
    if (t >= 64) {                                   /* M2 */
        NEED_IN(1);
        dist = 1 + ((t >> 2) & 7) + ((size_t)*ip++ << 3);
        BACK(dist, (t >> 5) + 1);
    } else if (t >= 32) {                            /* M3 */
        t &= 31;
        if (t == 0)
            LONG(31);
        NEED_IN(2);
        dist = 1 + ((size_t)(ip[0] | ip[1] << 8) >> 2);
        ip += 2;
        BACK(dist, t + 2);
    } else if (t >= 16) {                            /* M4, or the end */
        size_t far = (t & 8) << 11;
        t &= 7;
        if (t == 0)
            LONG(7);
        NEED_IN(2);
        dist = far + ((size_t)(ip[0] | ip[1] << 8) >> 2);
        ip += 2;
        if (dist == 0) {
            *out_len = (size_t)(op - out);
            return 0;
        }
        BACK(dist + 0x4000, t + 2);
    } else {                                         /* M1: 2 bytes, close by */
        NEED_IN(1);
        dist = 1 + (t >> 2) + ((size_t)*ip++ << 2);
        BACK(dist, 2);
    }

literals_after:
    t = ip[-2] & 3;
    if (t == 0)
        goto next;
    LITERALS(t);
    NEED_IN(1);
    t = *ip++;
    goto match;

#undef NEED_IN
#undef NEED_OUT
#undef LITERALS
#undef BACK
#undef LONG
}
