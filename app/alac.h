/*
 * ALAC (Apple Lossless) packets to samples. The container (MP4, mp4.h)
 * finds the packets; this decodes one at a time.
 *
 * Mono and stereo, 16, 20, 24 and 32 bits. Samples come out exactly as
 * stored: interleaved int32 holding 'bits'-bit values.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_ALAC_H
#define LOS_ALAC_H

#include <stddef.h>
#include <stdint.h>

#define ALAC_MAX_FRAMES 8192     /* samples per channel in one packet */

/* The "magic cookie" in the MP4's sample description (24 bytes) */
typedef struct {
    uint32_t frame_length;       /* samples per channel in a full packet */
    uint8_t bits, pb, mb, kb, channels;
    uint16_t max_run;
    uint32_t max_packet;         /* bytes; 0 if unknown */
    uint32_t rate;
} alac_config;

/* From the cookie's 24 bytes. 0, or negative if it's not something we play. */
int alac_config_read(alac_config *c, const uint8_t *cookie, size_t len);

/* One packet into out (frames x channels values). 'packet' must have 8
 * readable bytes after its end (the bit reader reads ahead). Returns the
 * frames decoded, or negative if the packet is damaged. */
int alac_decode(const alac_config *c, const uint8_t *packet, size_t len, int32_t *out);

#endif
