/*
 * Opus files (.opus): to samples for source.c, and their facts and tags
 * for the library. 48 kHz, 16 bits, mono or stereo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_OPUSFILE_H
#define LOS_OPUSFILE_H

#include <stdbool.h>
#include <stdint.h>
#include "source.h"

/* With s->file open: 0 if it's Opus we play, else negative */
int opusfile_open(los_source *s);
int opusfile_read(los_source *s, int32_t *out, int frames);
bool opusfile_seek(los_source *s, uint64_t frame);
void opusfile_close(los_source *s);

/* For the library (its own reader, any thread but the audio one): the
 * rate, channels, length, and each "KEY=value" tag to 'tag' */
bool opusfile_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                   void (*tag)(const char *comment, void *ctx), void *ctx);

#endif
