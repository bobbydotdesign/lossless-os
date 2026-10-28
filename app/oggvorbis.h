/*
 * Ogg Vorbis files (.ogg): to samples for source.c (24 bits out), and
 * their facts and tags for the library.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_OGGVORBIS_H
#define LOS_OGGVORBIS_H

#include <stdbool.h>
#include <stdint.h>
#include "source.h"

/* With s->file open: 0 if it's Vorbis we play, else negative */
int oggvorbis_open(los_source *s);
int oggvorbis_read(los_source *s, int32_t *out, int frames);
bool oggvorbis_seek(los_source *s, uint64_t frame);
void oggvorbis_close(los_source *s);

/* For the library (its own reader; not the audio thread) */
bool oggvorbis_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                    void (*tag)(const char *comment, void *ctx), void *ctx);

#endif
