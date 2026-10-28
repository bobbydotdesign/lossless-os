/*
 * lbt A2DP source: PCM in, SBC media packets out.
 *
 * Takes 16-bit interleaved PCM at the configured rate, encodes it with the
 * SBC encoder (Android's, Apache-2.0: the libsbc module Zephyr ships) and
 * sends as many frames per media packet as fit (at most 15).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_A2DP_H
#define LBT_A2DP_H

#include "lbt_util.h"
#include "avdtp.h"

/* Set the encoder up for this configuration. bitpool 0 means the most the
 * headphones accept (capped at the configuration's maximum). */
bool a2dp_setup(const avdtp_sbc_config *cfg, int bitpool);

/* Bytes in one SBC frame, and samples per channel it holds */
int a2dp_frame_bytes(void);
int a2dp_frame_samples(void);

/* Frames per media packet with the current channel */
int a2dp_frames_per_packet(void);

/* Encode and send whole packets while there's room and enough PCM.
 * 'samples' is per channel. Returns samples per channel used. */
int a2dp_push(const int16_t *pcm, int samples);

/* Encode one frame into out (for tests); returns its length */
int a2dp_encode_frame(const int16_t *pcm, uint8_t *out);

#endif
