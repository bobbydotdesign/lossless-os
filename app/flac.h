/*
 * Lossless OS: a FLAC decoder, written from the format's specification
 * (RFC 9639). Streams through a read callback, one frame at a time, so it
 * needs only a frame's worth of memory (a few tens of KB).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_FLAC_H
#define LOS_FLAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLAC_MAX_CHANNELS 8

/* Fill buf with up to n bytes; return how many (0 at the end) */
typedef size_t (*flac_read_fn)(void *ctx, uint8_t *buf, size_t n);

typedef struct {
    /* the stream, from STREAMINFO */
    unsigned rate, bits, channels;
    uint64_t total_samples;      /* per channel; 0 if unknown */
    unsigned max_block;

    /* the last decoded frame: block samples per channel, as decoded (bits
     * wide, sign-extended) */
    int32_t *samples[FLAC_MAX_CHANNELS];
    unsigned block;
    uint64_t position;           /* samples before this frame */

    /* private */
    flac_read_fn read;
    void *ctx;
    uint8_t buf[4096];
    size_t len, pos;
    uint64_t bits_cache;
    int bits_left;
    bool eof;
    void *store;                 /* samples' memory */
} flac_dec;

/* Read the header (to the first frame). 0, or negative if it isn't FLAC
 * or isn't supported. Allocates the sample buffers with malloc_fn. */
int flac_open(flac_dec *d, flac_read_fn read, void *ctx, void *(*malloc_fn)(size_t));

/* Decode the next frame into d->samples. Returns the samples per channel
 * (d->block), 0 at the end, negative on a broken stream (the caller can
 * call again: it looks for the next frame). */
int flac_next(flac_dec *d);

/* The caller moved the stream (a seek): forget what was read ahead. The
 * next flac_next looks for a frame from the new place; d->position comes
 * from that frame's header. */
void flac_reset(flac_dec *d);
/* Bytes read from the stream but not yet used (to find where frames start) */
size_t flac_buffered(const flac_dec *d);

/* Release the sample buffers */
void flac_close(flac_dec *d, void (*free_fn)(void *));

#endif
