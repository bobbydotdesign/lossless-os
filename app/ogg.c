/*
 * Ogg pages and packets (ogg.h). A page: "OggS", version, flags (1 a
 * packet carries on, 2 first, 4 last), granule position, stream serial,
 * page number, CRC, segment count and table, then the data. A packet is
 * its segments up to one shorter than 255 bytes.
 *
 * Source: RFC 3533.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "ogg.h"
#include "source.h"
#include "los_sys.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++)
            r = r & 0x80000000u ? r << 1 ^ 0x04c11db7u : r << 1;
        crc_table[i] = r;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    while (n--)
        c = c << 8 ^ crc_table[(c >> 24) ^ *p++];
    return c;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

void ogg_open(ogg_reader *o, void *file, uint8_t *packet_buf)
{
    crc_init();
    memset(o, 0, sizeof(*o));
    o->file = file;
    o->packet = packet_buf;
}

static bool at(void *f, uint64_t pos, void *buf, size_t n)
{
    return los_file_seek(f, pos) && los_file_read(f, buf, n) == n;
}

/* A page header at exactly 'pos' with a good CRC: read its table */
static bool page_here(ogg_reader *o, uint64_t pos, uint64_t *next)
{
    uint8_t h[27];
    if (!at(o->file, pos, h, 27) || memcmp(h, "OggS", 4) || h[4] != 0)
        return false;
    int n = h[26];
    uint8_t segs[255];
    if (n && los_file_read(o->file, segs, (size_t)n) != (size_t)n)
        return false;
    size_t body = 0;
    for (int i = 0; i < n; i++)
        body += segs[i];
    /* the CRC is over the page with its CRC field as zero */
    uint32_t want = le32(h + 22), c;
    memset(h + 22, 0, 4);
    c = crc(0, h, 27);
    c = crc(c, segs, (size_t)n);
    if (body && los_file_read(o->file, o->data, body) != body)
        return false;
    c = crc(c, o->data, body);
    if (c != want)
        return false;
    uint32_t serial = le32(h + 14);
    if (o->have_serial && serial != o->serial) {
        *next = pos + 27 + (uint64_t)n + body;  /* another stream's page: skip it */
        o->nseg = -1;
        return true;
    }
    o->serial = serial;
    o->have_serial = true;
    memcpy(o->segs, segs, (size_t)n);
    o->nseg = n;
    /* the page's granule position belongs to the last packet ending on it */
    o->last_end = -1;
    for (int i = 0; i < n; i++)
        if (segs[i] < 255)
            o->last_end = i;
    o->seg = 0;
    o->used = 0;
    o->granule = (int64_t)((uint64_t)le32(h + 6) | (uint64_t)le32(h + 10) << 32);
    o->page_eos = h[5] & 4;
    o->continued = h[5] & 1;
    *next = pos + 27 + (uint64_t)n + body;
    return true;
}

/* The next page of our stream from o->pos, searching if need be */
static bool next_page(ogg_reader *o)
{
    for (int tries = 0; tries < 4096; tries++) {
        uint64_t next;
        o->page_start = o->pos;
        if (page_here(o, o->pos, &next)) {
            o->pos = next;
            if (o->nseg >= 0)
                return true;
            continue;
        }
        /* lost: look for "OggS" further on */
        uint8_t buf[512];
        if (!los_file_seek(o->file, o->pos + 1))
            return false;
        size_t got = los_file_read(o->file, buf, sizeof(buf));
        if (got < 4)
            return false;
        size_t i;
        for (i = 0; i + 4 <= got && memcmp(buf + i, "OggS", 4); i++) {
        }
        o->pos += 1 + i;
    }
    return false;
}

bool ogg_next(ogg_reader *o, int64_t *granule)
{
    o->len = 0;
    o->too_big = false;
    for (;;) {
        if (o->seg >= o->nseg) {
            if (!next_page(o))
                return false;
            if (!o->continued)
                o->len = 0;                     /* a packet cut short is dropped */
            if (o->continued && o->drop) {
                /* the end of a packet we didn't see begin: skip it */
                while (o->seg < o->nseg) {
                    unsigned n = o->segs[o->seg++];
                    o->used += n;
                    if (n < 255) {
                        o->drop = false;
                        break;
                    }
                }
                if (o->drop)
                    continue;
            }
            o->drop = false;
        }
        while (o->seg < o->nseg) {
            unsigned n = o->segs[o->seg++];
            if (o->len + n <= OGG_PACKET_MAX) {
                memcpy(o->packet + o->len, o->data + o->used, n);
                o->len += n;
            } else {
                o->too_big = true;
            }
            o->used += n;
            if (n < 255) {
                *granule = o->seg - 1 == o->last_end ? o->granule : -1;
                if (o->too_big) {
                    o->len = 0;
                    o->too_big = false;
                    continue;
                }
                return true;
            }
        }
    }
}

bool ogg_page_at(ogg_reader *o, uint64_t pos, uint64_t *start, int64_t *granule)
{
    o->pos = pos;
    o->nseg = o->seg = 0;
    for (;;) {
        if (!next_page(o))
            return false;
        uint64_t page_start = o->page_start;
        if (o->granule == -1)
            continue;                           /* no packet ends on it */
        *start = page_start;
        *granule = o->granule;
        o->seg = o->nseg;                       /* packets come from the page after */
        return true;
    }
}

uint64_t ogg_seek_from(ogg_reader *o, uint64_t audio_start, int64_t want)
{
    uint64_t lo = audio_start, hi = los_file_size(o->file), start, from = audio_start;
    int64_t g;
    if (want <= 0)
        return audio_start;
    while (hi - lo > 65536) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (ogg_page_at(o, mid, &start, &g) && g <= want)
            lo = mid;
        else
            hi = mid;
    }
    /* from lo, page by page to the last one at or before 'want' */
    uint64_t at = lo;
    while (ogg_page_at(o, at, &start, &g) && g <= want) {
        at = o->pos;
        from = at;
    }
    return from;
}

/* The library's reader (source.h): a page and a packet */
void *ogg_info_mem(void)
{
    static struct { ogg_reader rd; uint8_t packet[OGG_PACKET_MAX]; } m;
    return &m;
}

int64_t ogg_last_granule(ogg_reader *o)
{
    uint64_t size = los_file_size(o->file);
    int64_t last = -1;
    for (uint64_t back = 65536; back <= 16 * 65536u; back *= 4) {
        uint64_t from = size > back ? size - back : 0, start;
        int64_t g;
        if (ogg_page_at(o, from, &start, &g)) {
            last = g;
            while (next_page(o))
                if (o->granule != -1)
                    last = o->granule;
            return last;
        }
        if (!from)
            break;
    }
    return last;
}
