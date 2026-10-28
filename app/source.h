/*
 * Lossless OS: audio files as a stream of samples, whatever the format
 * (FLAC, ALAC, WAV, AIFF, Opus, Ogg Vorbis, MP3). Samples come out exactly as stored: interleaved int32
 * holding 'bits'-bit values, nothing converted.
 *
 * Used on the audio thread only: files through los_file_*, no LVGL.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_SOURCE_H
#define LOS_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include "alac.h"
#include "flac.h"
#include "mp4.h"

typedef enum { SRC_NONE, SRC_FLAC, SRC_PCM, SRC_ALAC, SRC_OPUS, SRC_VORBIS, SRC_MP3 } los_source_kind;

typedef struct {
    unsigned rate, bits, channels;
    uint64_t frames;             /* total, 0 if unknown */
    const char *format;          /* "FLAC", "ALAC", "WAV", "AIFF", "OPUS" */

    /* private */
    los_source_kind kind;
    void *file;
    uint64_t offset;             /* where the file is read next */
    flac_dec flac;
    unsigned flac_used;          /* samples of the current frame already given */
    uint64_t pcm_left;           /* PCM: data bytes still to read */
    uint64_t audio_start;        /* where the first frame / sample is */
    uint64_t data_bytes;         /* PCM: all of its sample data */
    unsigned pcm_bytes;          /* bytes per sample */
    bool pcm_big_endian;
    mp4_info mp4;                /* ALAC: the file's layout */
    alac_config alac;
    uint32_t packet;             /* ALAC: the next packet to decode */
    unsigned alac_frames, alac_used;   /* in the decoded packet */
} los_source;

/* The audio thread's decoder memory: one file is open at a time, so the
 * formats share it (each lays its own state over it) */
#define DECODER_MEM_BYTES (680 * 1024)
extern uint64_t decoder_mem[DECODER_MEM_BYTES / 8];

/* The library's Ogg reading (opusfile_info, oggvorbis_info): one at a
 * time, on the UI thread */
void *ogg_info_mem(void);

/* Open a file (path without a drive letter). 0, or negative if it can't be
 * played (unknown format, unsupported variant, unreadable). */
int source_open(los_source *s, const char *path);

/* Up to 'frames' frames into out (frames * channels values). Returns how
 * many; 0 at the end. */
int source_read(los_source *s, int32_t *out, int frames);

/* Go to this frame (sample per channel) from the start; the next read
 * starts there exactly. False if the file can't seek. */
bool source_seek(los_source *s, uint64_t frame);

void source_close(los_source *s);

#endif
