/*
 * lbt host tests: SBC encoding and packing.
 *
 * Encodes a stereo test tone with the settings the AirPods Pro 3 took,
 * decodes it again (Android's decoder, also in libsbc) and measures how
 * close it comes back; then checks a2dp_push fills media packets right.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../avdtp.h"
#include "../a2dp.h"
#include "../lbt_platform.h"
#include "oi_codec_sbc.h"

#define RATE 44100
static int16_t pcm[RATE * 2];        /* one second, stereo */
static int16_t decoded[RATE * 2 + 4096];

static void make_tone(void)
{
    for (int i = 0; i < RATE; i++) {
        pcm[2 * i] = (int16_t)(12000 * sin(2 * M_PI * 1000.0 * i / RATE));
        pcm[2 * i + 1] = (int16_t)(12000 * sin(2 * M_PI * 440.0 * i / RATE));
    }
}

/* the AirPods' configuration from the recording */
static avdtp_sbc_config airpods_cfg(void)
{
    avdtp_sbc_config c;
    avdtp_choose_sbc((const uint8_t[]){ 0x3f, 0xff, 0x02, 0x35 }, &c);
    return c;
}

/* Signal-to-noise ratio in dB of 'out' against 'in' delayed by d samples */
static double snr(const int16_t *in, const int16_t *out, int n, int ch, int d)
{
    double sig = 0, err = 0;
    for (int i = 2000; i < n - 2000; i++) {
        double a = in[2 * i + ch], b = out[2 * (i + d) + ch];
        sig += a * a;
        err += (a - b) * (a - b);
    }
    return 10 * log10(sig / (err + 1e-9));
}

TEST(encode_decode_tone)
{
    make_tone();
    avdtp_sbc_config c = airpods_cfg();
    CHECK(a2dp_setup(&c, 0));
    CHECK(a2dp_frame_bytes() == 119 && a2dp_frame_samples() == 128);

    static uint32_t dec_data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
    OI_CODEC_SBC_DECODER_CONTEXT ctx;
    CHECK(OI_CODEC_SBC_DecoderReset(&ctx, dec_data, sizeof(dec_data), 2, 2, 0) == OI_OK);

    int frames = RATE / 128, out_samples = 0, bad = 0;
    for (int f = 0; f < frames; f++) {
        uint8_t frame[512];
        int len = a2dp_encode_frame(pcm + f * 128 * 2, frame);
        if (len != 119 || frame[0] != 0x9c)
            bad++;
        const OI_BYTE *p = frame;
        uint32_t left = (uint32_t)len, pcm_bytes = 128 * 2 * 2;
        if (OI_CODEC_SBC_DecodeFrame(&ctx, &p, &left, decoded + out_samples * 2, &pcm_bytes) != OI_OK)
            bad++;
        out_samples += (int)(pcm_bytes / 4);
    }
    CHECK(bad == 0);
    CHECK(out_samples == frames * 128);

    /* the filters delay the sound a little: find the delay, then measure */
    int best_d = 0;
    double best = -100;
    for (int d = 0; d < 300; d++) {
        double v = snr(pcm, decoded, out_samples - 300, 0, d);
        if (v > best) { best = v; best_d = d; }
    }
    double right = snr(pcm, decoded, out_samples - 300, 1, best_d);
    printf("  frame 119 bytes; delay %d samples; SNR left %.1f dB, right %.1f dB\n",
           best_d, best, right);
    CHECK(best > 30 && right > 30);
}

/* ---- packing into media packets (the headphones lead the setup) -------------- */

#define H 0x000c
static const uint8_t airpods[6] = { 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa };
static void nop_cfg(const avdtp_sbc_config *c) { (void)c; }
static void nop_ok(bool ok) { (void)ok; }
static void nop(void) {}
static const avdtp_ops ops = { nop_cfg, nop_ok, nop, nop, nop };
static uint16_t sig_cid;

static void signal_in(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t pdu[8 + 64];
    lbt_put16(pdu, 4 + len);
    lbt_put16(pdu + 2, 0x0001);
    pdu[4] = code; pdu[5] = id;
    lbt_put16(pdu + 6, len);
    memcpy(pdu + 8, d, len);
    fake_acl(H, 2, pdu, 8 + len);
}

static struct sent_packet *next_l2cap(void)
{
    static struct sent_packet out;
    struct sent_packet *p;
    while ((p = fake_take()) != NULL)
        if (p->type == LBT_H4_ACL)
            break;
    if (!p)
        return NULL;
    out.len = p->len - 4;
    memcpy(out.data, p->data + 4, out.len);
    fake_completed(H, 1);
    return &out;
}

static uint16_t open_theirs(uint16_t their_cid)
{
    uint8_t req[4] = { 0x19, 0x00 };
    lbt_put16(req + 2, their_cid);
    signal_in(0x02, 0x41, req, 4);
    struct sent_packet *p = next_l2cap();
    uint16_t ours = p ? lbt_get16(p->data + 8) : 0;
    p = next_l2cap();
    uint8_t id = p ? p->data[5] : 0;
    uint8_t creq[8] = { 0, 0, 0, 0, 0x01, 0x02, 0xff, 0x03 };
    lbt_put16(creq, ours);
    signal_in(0x04, 0x42, creq, 8);
    next_l2cap();
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ours);
    signal_in(0x05, id, ok, 6);
    return ours;
}

static void avdtp_in(const uint8_t *d, uint16_t len)
{
    uint8_t pdu[4 + 64];
    lbt_put16(pdu, len);
    lbt_put16(pdu + 2, sig_cid);
    memcpy(pdu + 4, d, len);
    fake_acl(H, 2, pdu, 4 + len);
    next_l2cap();   /* our answer */
}
#define IN(...) avdtp_in((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

TEST(push_fills_packets)
{
    make_tone();
    fake_start(1021, 8);
    l2cap_init();
    avdtp_init(&ops);
    fake_connect(H, airpods);
    sig_cid = open_theirs(0x0050);
    IN(0x20, 0x03, 0x04, 0x08, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35);
    IN(0x40, 0x06, 0x04);
    open_theirs(0x0051);
    IN(0x50, 0x07, 0x04);
    CHECK(avdtp_is_streaming());

    avdtp_sbc_config c = airpods_cfg();
    a2dp_setup(&c, 0);
    CHECK(a2dp_frames_per_packet() == 8);          /* 8 x 119 fits in 1010 */

    /* 2.5 packets' worth of audio: two packets go, the rest waits */
    int used = a2dp_push(pcm, 8 * 128 * 2 + 300);
    CHECK(used == 8 * 128 * 2);
    struct sent_packet *m = next_l2cap();
    CHECK(m && m->len == 4 + 13 + 8 * 119 && m->data[16] == 8 && m->data[17] == 0x9c);
    m = next_l2cap();
    CHECK(m && lbt_get32be(m->data + 8) == 8 * 128);   /* RTP timestamp moved on */

    /* when HCI is full, nothing is encoded and nothing is lost */
    for (int i = 0; i < 20; i++)
        a2dp_push(pcm, 8 * 128);
    int before = fake_sent_count;
    used = a2dp_push(pcm, 8 * 128);
    CHECK(used == 0 || fake_sent_count > before);
}

int main(void)
{
    RUN(encode_decode_tone);
    RUN(push_fills_packets);
    printf(test_failures ? "\n%d FAILED\n" : "\nall SBC tests passed\n", test_failures);
    return test_failures != 0;
}
