/*
 * MP3 files (MPEG-1/2/2.5 Layer III): to samples for source.c (16 bits),
 * and their facts, tags (ID3v2, else ID3v1) and cover for the library.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_MP3FILE_H
#define LOS_MP3FILE_H

#include <stdbool.h>
#include <stdint.h>
#include "source.h"

/* With s->file open: 0 if it's MP3 we play, else negative */
int mp3file_open(los_source *s);
int mp3file_read(los_source *s, int32_t *out, int frames);
/* Near the frame asked for: exact for files of one bit rate, within a
 * frame or two otherwise (MP3 has no index) */
bool mp3file_seek(los_source *s, uint64_t frame);
void mp3file_close(los_source *s);

/* For the library (not the audio thread): rate, channels, length, each
 * tag as "KEY=value" (TITLE, ARTIST, ALBUMARTIST, ALBUM, TRACKNUMBER,
 * DATE), and where an embedded JPEG cover starts (0 if none) */
bool mp3file_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                  void (*tag)(const char *comment, void *ctx), void *ctx, uint32_t *cover);

#endif
