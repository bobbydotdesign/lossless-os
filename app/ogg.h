/*
 * Ogg files: pages, and the packets in them (RFC 3533). One logical
 * stream (the first one found); files through los_file_*. A reader is
 * big (a whole page): keep it static.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_OGG_H
#define LOS_OGG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OGG_PACKET_MAX (64 * 1024)

typedef struct {
    void *file;
    uint64_t pos;                /* the next page starts here (or after) */
    uint32_t serial;
    bool have_serial;
    /* the page being read */
    uint8_t segs[255];
    int nseg, seg;
    int last_end;                /* the last segment that ends a packet, -1 none */
    uint64_t page_start;
    uint8_t data[65025];         /* its data */
    size_t used;                 /* of data, given out */
    int64_t granule;             /* the page's granule position */
    bool page_eos;
    bool continued;              /* the packet carries on from the last page */
    /* the packet being put together */
    uint8_t *packet;             /* OGG_PACKET_MAX bytes, given by the caller */
    size_t len;
    bool too_big;
    bool drop;                   /* drop a packet that began on an earlier page */
} ogg_reader;

void ogg_open(ogg_reader *o, void *file, uint8_t *packet_buf);

/* The next packet (in o->packet, o->len). *granule: the page's granule
 * position if this is the last packet ending on the page, else -1. False at the end. */
bool ogg_next(ogg_reader *o, int64_t *granule);

/* The first good page of our stream at or after 'pos': where it starts and
 * its granule position. False if none before the end. Packets are then
 * read from the page after it. */
bool ogg_page_at(ogg_reader *o, uint64_t pos, uint64_t *start, int64_t *granule);

/* For seeking: where to read packets from so the last position before
 * them is at most 'want': just after the last page ending at or before
 * 'want' (found by halving the file), or 'audio_start' if none */
uint64_t ogg_seek_from(ogg_reader *o, uint64_t audio_start, int64_t want);

/* The granule position of the last page of the stream (its length) */
int64_t ogg_last_granule(ogg_reader *o);

#endif
