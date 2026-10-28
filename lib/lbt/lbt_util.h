/*
 * lbt: the lossless Bluetooth stack. Small helpers shared by all layers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_UTIL_H
#define LBT_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A device address, in the order it travels on the wire (least
 * significant byte first). Printed most significant byte first. */
typedef struct {
    uint8_t b[6];
} lbt_addr;

static inline bool lbt_addr_equal(const lbt_addr *a, const lbt_addr *b)
{
    return memcmp(a->b, b->b, 6) == 0;
}

static inline uint16_t lbt_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t lbt_get24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t lbt_get32(const uint8_t *p)
{
    return lbt_get24(p) | ((uint32_t)p[3] << 24);
}

static inline void lbt_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void lbt_put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static inline void lbt_put32(uint8_t *p, uint32_t v)
{
    lbt_put24(p, v);
    p[3] = (uint8_t)(v >> 24);
}

/* Big-endian, as SDP, AVDTP and AVRCP use */
static inline uint16_t lbt_get16be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t lbt_get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void lbt_put16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void lbt_put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#define LBT_ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
