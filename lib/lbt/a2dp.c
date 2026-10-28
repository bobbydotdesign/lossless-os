/*
 * lbt A2DP source: encoding and packing (A2DP 1.3 section 4.3, SBC).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "a2dp.h"
#include "sbc_encoder.h"

static SBC_ENC_PARAMS enc;
static int frame_bytes, frame_samples, channels;
static uint8_t packet[15 * 512];

/* SBC frame length (A2DP 1.3, 12.9) */
static int sbc_frame_length(int mode, int subbands, int blocks, int bitpool)
{
    int nch = mode == SBC_MONO ? 1 : 2;
    int bits;
    if (mode == SBC_MONO || mode == SBC_DUAL)
        bits = blocks * nch * bitpool;
    else
        bits = (mode == SBC_JOINT_STEREO ? subbands : 0) + blocks * bitpool;
    return 4 + (4 * subbands * nch) / 8 + (bits + 7) / 8;
}

bool a2dp_setup(const avdtp_sbc_config *cfg, int bitpool)
{
    memset(&enc, 0, sizeof(enc));
    switch (cfg->rate) {
    case 44100: enc.s16SamplingFreq = SBC_sf44100; break;
    case 48000: enc.s16SamplingFreq = SBC_sf48000; break;
    case 32000: enc.s16SamplingFreq = SBC_sf32000; break;
    case 16000: enc.s16SamplingFreq = SBC_sf16000; break;
    default: return false;
    }
    switch (cfg->mode) {
    case 0x08: enc.s16ChannelMode = SBC_MONO; break;
    case 0x04: enc.s16ChannelMode = SBC_DUAL; break;
    case 0x02: enc.s16ChannelMode = SBC_STEREO; break;
    default:   enc.s16ChannelMode = SBC_JOINT_STEREO; break;
    }
    channels = cfg->channels;
    enc.s16NumOfChannels = (int16_t)channels;
    enc.s16NumOfSubBands = (int16_t)cfg->subbands;
    enc.s16NumOfBlocks = (int16_t)cfg->blocks;
    enc.s16AllocationMethod = cfg->loudness ? SBC_LOUDNESS : SBC_SNR;
    if (bitpool <= 0 || bitpool > cfg->max_bitpool)
        bitpool = cfg->max_bitpool;
    if (bitpool < cfg->min_bitpool)
        bitpool = cfg->min_bitpool;
    enc.s16BitPool = (int16_t)bitpool;
    enc.Format = SBC_FORMAT_GENERAL;
    SBC_Encoder_Init(&enc);
    enc.s16BitPool = (int16_t)bitpool;   /* init may recompute it */
    frame_samples = cfg->blocks * cfg->subbands;
    frame_bytes = sbc_frame_length(enc.s16ChannelMode, cfg->subbands, cfg->blocks, bitpool);
    return true;
}

int a2dp_frame_bytes(void) { return frame_bytes; }
int a2dp_frame_samples(void) { return frame_samples; }

int a2dp_frames_per_packet(void)
{
    int room = avdtp_max_media();
    if (frame_bytes <= 0 || room <= 0)
        return 0;
    int n = room / frame_bytes;
    if (n > 15) n = 15;
    if (n * frame_bytes > (int)sizeof(packet)) n = (int)sizeof(packet) / frame_bytes;
    return n < 1 ? 0 : n;
}

int a2dp_encode_frame(const int16_t *pcm, uint8_t *out)
{
    return (int)SBC_Encode(&enc, (int16_t *)pcm, out);
}

int a2dp_push(const int16_t *pcm, int samples)
{
    int used = 0;
    int n = a2dp_frames_per_packet();
    if (n == 0)
        return 0;
    while (samples - used >= n * frame_samples &&
           avdtp_can_send((uint16_t)(n * frame_bytes))) {
        int len = 0;
        for (int i = 0; i < n; i++) {
            len += a2dp_encode_frame(pcm + (size_t)(used + i * frame_samples) * channels,
                                     packet + len);
        }
        if (!avdtp_send_sbc(packet, (uint16_t)len, (uint8_t)n, (uint32_t)(n * frame_samples)))
            break;
        used += n * frame_samples;
    }
    return used;
}
