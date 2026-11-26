/*
 * MP4 / M4A files (ISO base media file format): the Apple Lossless or AAC
 * track, where its packets are, how much of it is music (gapless), and the
 * iTunes-style tags.
 *
 * Files through los_file_* (paths without a drive letter). The packet
 * tables stay in the file; a few entries at a time are read as needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_MP4_H
#define LOS_MP4_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t pos;                /* first entry in the file */
    uint32_t count;
} mp4_table;

enum { MP4_ALAC = 1, MP4_AAC = 2 };

typedef struct {
    /* the audio track: ALAC or AAC */
    int codec;                   /* MP4_ALAC, MP4_AAC */
    uint8_t cookie[24];          /* ALAC: its "magic cookie" (alac.h) */
    uint8_t asc[16];             /* AAC: its AudioSpecificConfig (ISO/IEC 14496-3) */
    uint8_t asc_len;
    /* AAC: frames the encoder put in front (0 if not said) and the music's
     * length after the cuts (0: frames - delay), from iTunSMPB or the edit list */
    uint32_t delay;
    uint64_t valid;
    uint64_t frames;             /* samples per channel, all packets */
    uint32_t packets;
    uint32_t fixed_size;         /* every packet's size, or 0: see the table */
    mp4_table sizes, chunks_runs, chunks;
    bool chunks64;               /* chunk offsets are 64-bit */

    /* tags ("" / 0 if none) */
    char title[72], artist[72], album_artist[72], album[72];
    int year, track;
    uint64_t cover;              /* where the cover's JPEG data starts, 0 if none */
} mp4_info;

/* Where a packet is: a few table entries kept from the last lookups */
typedef struct {
    uint32_t first[3], n[3];
    uint32_t v[3][3 * 64];
} mp4_cursor;

/* Read the file's layout and tags. 0 if it has an ALAC or AAC track; -2
 * if it's an MP4 without one (video, other codecs); -1 if it isn't readable. */
int mp4_open(void *file, mp4_info *m);

void mp4_cursor_reset(mp4_cursor *c);

/* Packet 'i': where it starts and how long it is. Moves the file's read
 * position. False past the end or if the tables can't be read. */
bool mp4_packet(void *file, const mp4_info *m, mp4_cursor *c, uint32_t i, uint64_t *offset,
                uint32_t *size);

#endif
