/*
 * AAC in M4A files (from Apple, and most encoders): to samples for
 * source.c (16 bits), with the encoder's delay and padding cut (gapless).
 * Plain AAC (AAC LC); HE-AAC isn't played.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_AACFILE_H
#define LOS_AACFILE_H

#include <stdbool.h>
#include <stdint.h>
#include "source.h"

/* With s->file open and s->mp4 read (an AAC track): 0 if it's AAC we play,
 * else negative */
int aacfile_open(los_source *s);
int aacfile_read(los_source *s, int32_t *out, int frames);
/* Exactly to that frame (AAC has 1024 frames a packet and an index) */
bool aacfile_seek(los_source *s, uint64_t frame);
void aacfile_close(los_source *s);

/* An AudioSpecificConfig (ISO/IEC 14496-3, 1.6.2.1): object type (2 is
 * AAC LC; 5 and 29 are HE-AAC), sample rate, channels. False if unreadable. */
bool aac_config(const uint8_t *asc, unsigned len, unsigned *object, unsigned *rate,
                unsigned *channels);

/* Frames of music in a file: after the delay, without the padding */
uint64_t aac_music_frames(const mp4_info *m);

#endif
