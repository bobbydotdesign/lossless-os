/*
 * lbt AVDTP: setting up and running an audio stream to headphones, as an
 * A2DP source with SBC (AVDTP 1.3, A2DP 1.3).
 *
 * We usually lead: open signaling, discover the headphones' endpoints,
 * read their capabilities, choose SBC settings, configure, open the media
 * channel, then start and suspend as playback starts and pauses. The other
 * side can also lead (configure us, start, suspend, close); both work.
 *
 * Media packets are RTP with an SBC payload header; a2dp.c fills them with
 * frames from the SBC encoder.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_AVDTP_H
#define LBT_AVDTP_H

#include "lbt_util.h"

#define AVDTP_TIMEOUT_MS 5000
#define AVDTP_THEIRS_MS  2000   /* their signaling channel: their turn to lead first */

typedef struct {
    int rate;            /* 44100 or 48000 */
    int channels;        /* 1 or 2 */
    uint8_t mode;        /* SBC channel mode bit: 8 mono, 4 dual, 2 stereo, 1 joint */
    int blocks;          /* 4, 8, 12, 16 */
    int subbands;        /* 4, 8 */
    bool loudness;       /* allocation: loudness (else SNR) */
    int min_bitpool, max_bitpool;
    uint8_t raw[4];      /* as sent in SET_CONFIGURATION */
} avdtp_sbc_config;

typedef struct {
    /* the codec is chosen (by us or by them) */
    void (*configured)(const avdtp_sbc_config *cfg);
    /* the media channel is open and ready to start (or setup failed) */
    void (*open)(bool ok);
    void (*started)(void);
    void (*suspended)(void);
    /* the stream is gone (closed, aborted or the link dropped) */
    void (*closed)(void);
} avdtp_ops;

void avdtp_init(const avdtp_ops *ops);

/* Set up a stream to the headphones on this (secured) connection */
bool avdtp_connect(uint16_t handle);

bool avdtp_start(void);
bool avdtp_suspend(void);
void avdtp_close(void);

/* Call every few hundred ms: answer timeouts */
void avdtp_tick(void);

/* Most SBC frame bytes one media packet can carry */
uint16_t avdtp_max_media(void);

/* Send SBC frames ('frames' of them, 'samples' samples per channel in
 * total). False if not streaming or no room right now. */
bool avdtp_send_sbc(const uint8_t *data, uint16_t len, uint8_t frames, uint32_t samples);

bool avdtp_is_streaming(void);

/* Could a media packet with len bytes of SBC frames go right now? */
bool avdtp_can_send(uint16_t len);

/* Choose SBC settings from the headphones' capabilities (4 bytes, as in
 * GET_CAPABILITIES). False if nothing fits. Exposed for tests. */
bool avdtp_choose_sbc(const uint8_t remote[4], avdtp_sbc_config *cfg);

#endif
