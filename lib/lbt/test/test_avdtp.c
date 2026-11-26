/*
 * lbt host tests: AVDTP audio setup, using the AirPods Pro 3's answers
 * from the recording (with our transaction labels).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../avdtp.h"
#include "../lbt_platform.h"

#define H 0x000c
static const uint8_t airpods[6] = { 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa };

static int configured_calls, open_calls, started_calls, suspended_calls, closed_calls;
static bool open_ok;
static avdtp_sbc_config cfg;

static void on_configured(const avdtp_sbc_config *c) { configured_calls++; cfg = *c; }
static void on_open(bool ok) { open_calls++; open_ok = ok; }
static void on_started(void) { started_calls++; }
static void on_suspended(void) { suspended_calls++; }
static void on_closed(void) { closed_calls++; }
static const avdtp_ops ops = { on_configured, on_open, on_started, on_suspended, on_closed };

static void setup(void)
{
    fake_start(1021, 8);
    l2cap_init();
    avdtp_init(&ops);
    fake_connect(H, airpods);
    configured_calls = open_calls = started_calls = suspended_calls = closed_calls = 0;
}

/* ---- L2CAP plumbing --------------------------------------------------------------- */

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

/* next ACL packet's L2CAP part */
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
    /* like the controller: the packet went out, the buffer is free again */
    fake_completed(lbt_get16(p->data) & 0x0fff, 1);
    return &out;
}

/* They accept the channel we're opening; returns our CID */
static uint16_t accept_ours(uint16_t their_cid)
{
    struct sent_packet *p = next_l2cap();            /* connection request */
    if (!p || p->data[4] != 0x02) { CHECK(0); return 0; }
    uint8_t id = p->data[5];
    uint16_t ours = lbt_get16(p->data + 10);
    uint8_t rsp[8] = { 0 };
    lbt_put16(rsp, their_cid);
    lbt_put16(rsp + 2, ours);
    signal_in(0x03, id, rsp, 8);
    p = next_l2cap();                                /* our configuration */
    uint8_t cid_id = p ? p->data[5] : 0;
    uint8_t req[8] = { 0, 0, 0, 0, 0x01, 0x02, 0xff, 0x03 };   /* MTU 1023 */
    lbt_put16(req, ours);
    signal_in(0x04, 0x40, req, 8);
    next_l2cap();                                    /* our answer */
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ours);
    signal_in(0x05, cid_id, ok, 6);
    return ours;
}

/* They open a channel to us (PSM 0x19); returns our CID */
static uint16_t open_theirs(uint16_t their_cid)
{
    uint8_t req[4] = { 0x19, 0x00 };
    lbt_put16(req + 2, their_cid);
    signal_in(0x02, 0x41, req, 4);
    struct sent_packet *p = next_l2cap();            /* accepted */
    uint16_t ours = p ? lbt_get16(p->data + 8) : 0;
    p = next_l2cap();                                /* our configuration */
    uint8_t cid_id = p ? p->data[5] : 0;
    uint8_t creq[8] = { 0, 0, 0, 0, 0x01, 0x02, 0xff, 0x03 };
    lbt_put16(creq, ours);
    signal_in(0x04, 0x42, creq, 8);
    next_l2cap();
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ours);
    signal_in(0x05, cid_id, ok, 6);
    return ours;
}

static uint16_t sig_cid;

/* An AVDTP message from the AirPods on the signaling channel */
static void avdtp_in(const uint8_t *d, uint16_t len)
{
    uint8_t pdu[4 + 64];
    lbt_put16(pdu, len);
    lbt_put16(pdu + 2, sig_cid);
    memcpy(pdu + 4, d, len);
    fake_acl(H, 2, pdu, 4 + len);
}
#define IN(...) avdtp_in((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* the next AVDTP message we sent (after the 4-byte L2CAP header) */
static struct sent_packet *next_avdtp(void)
{
    static struct sent_packet out;
    struct sent_packet *p = next_l2cap();
    if (!p)
        return NULL;
    out.len = p->len - 4;
    memcpy(out.data, p->data + 4, out.len);
    return &out;
}

/* ---- tests ---------------------------------------------------------------------- */

TEST(choose_sbc_like_the_recording)
{
    avdtp_sbc_config c;
    CHECK(avdtp_choose_sbc((const uint8_t[]){ 0x3f, 0xff, 0x02, 0x35 }, &c));
    CHECK(c.raw[0] == 0x21 && c.raw[1] == 0x15 && c.raw[2] == 2 && c.raw[3] == 53);
    CHECK(c.rate == 44100 && c.channels == 2 && c.blocks == 16 && c.subbands == 8 && c.loudness);
    /* 48 kHz stereo only, low bitpool, SNR allocation, 4 subbands */
    CHECK(avdtp_choose_sbc((const uint8_t[]){ 0x12, 0x8a, 0x10, 0x23 }, &c));
    CHECK(c.rate == 48000 && c.mode == 0x02 && c.blocks == 4 && c.subbands == 4 &&
          !c.loudness && c.min_bitpool == 16 && c.max_bitpool == 35);
    /* 16 kHz only: nothing we encode */
    CHECK(!avdtp_choose_sbc((const uint8_t[]){ 0x81, 0xff, 0x02, 0x35 }, &c));
}

/* Lead the setup exactly as the recording did */
static void lead_to_open(void)
{
    CHECK(avdtp_connect(H));
    sig_cid = accept_ours(0x2005);
    CHECK_BYTES(next_avdtp(), 0x00, 0x01);                       /* DISCOVER */
    IN(0x02, 0x01, 0x04, 0x08, 0x08, 0x08, 0x0c, 0x08);          /* three sinks */
    CHECK_BYTES(next_avdtp(), 0x10, 0x0c, 0x04);                 /* caps of seid 1 */
    IN(0x12, 0x0c, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xff, 0x02, 0x35, 0x08, 0x00);
    CHECK_BYTES(next_avdtp(), 0x20, 0x0c, 0x08);                 /* seid 2 */
    IN(0x22, 0x0c, 0x01, 0x00, 0x07, 0x08, 0x00, 0x02, 0xc0, 0x01, 0x8c, 0x83, 0xe8, 0x00, 0x08, 0x00);
    CHECK_BYTES(next_avdtp(), 0x30, 0x0c, 0x0c);                 /* seid 3 */
    IN(0x32, 0x0c, 0x01, 0x00, 0x07, 0x10, 0x00, 0xff, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x80,
       0x00, 0x80, 0x00, 0x8c, 0x00, 0x83, 0xe8, 0x00, 0x08, 0x00);
    /* the same configuration as in the recording */
    CHECK_BYTES(next_avdtp(), 0x40, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
                0x21, 0x15, 0x02, 0x35);
    IN(0x42, 0x03);
    CHECK(configured_calls == 1 && cfg.rate == 44100 && cfg.max_bitpool == 53);
    CHECK_BYTES(next_avdtp(), 0x50, 0x06, 0x04);                 /* OPEN */
    IN(0x52, 0x06);
    CHECK(open_calls == 0);
    accept_ours(0x240e);                                         /* media channel */
    CHECK(open_calls == 1 && open_ok);
}

TEST(we_lead_like_the_recording)
{
    setup();
    lead_to_open();
    CHECK(avdtp_start());
    CHECK_BYTES(next_avdtp(), 0x60, 0x07, 0x04);                 /* START */
    IN(0x62, 0x07);
    CHECK(started_calls == 1 && avdtp_is_streaming());

    /* media: RTP, then the SBC header with the frame count */
    uint8_t frames[100];
    for (int i = 0; i < 100; i++)
        frames[i] = (uint8_t)i;
    CHECK(avdtp_max_media() == 1023 - 13);
    CHECK(avdtp_send_sbc(frames, 100, 7, 7 * 128));
    CHECK(avdtp_send_sbc(frames, 100, 7, 7 * 128));
    struct sent_packet *m = next_l2cap();
    CHECK(m && m->len == 4 + 113 && lbt_get16(m->data + 2) == 0x240e);
    CHECK(m && m->data[4] == 0x80 && m->data[5] == 96 && lbt_get16be(m->data + 6) == 0 &&
          lbt_get32be(m->data + 8) == 0 && m->data[16] == 7 && m->data[17] == 0);
    m = next_l2cap();
    CHECK(m && lbt_get16be(m->data + 6) == 1 && lbt_get32be(m->data + 8) == 896);

    /* they take an AirPod out: they suspend */
    IN(0x30, 0x09, 0x04);
    CHECK_BYTES(next_avdtp(), 0x32, 0x09);
    CHECK(suspended_calls == 1 && !avdtp_is_streaming());
    CHECK(!avdtp_send_sbc(frames, 100, 7, 896));

    /* we close the stream */
    avdtp_close();
    CHECK_BYTES(next_avdtp(), 0x70, 0x08, 0x04);
    IN(0x72, 0x08);
    struct sent_packet *d = next_l2cap();                        /* media channel goes */
    CHECK(d && d->data[4] == 0x06);
    signal_in(0x07, d ? d->data[5] : 0, (const uint8_t[]){ 0x0e, 0x24, 0x41, 0x00 }, 4);
    CHECK(closed_calls == 1);
}

TEST(older_device_without_get_all)
{
    setup();
    avdtp_connect(H);
    sig_cid = accept_ours(0x2005);
    next_avdtp();
    IN(0x02, 0x01, 0x04, 0x08);                                  /* one sink */
    CHECK_BYTES(next_avdtp(), 0x10, 0x0c, 0x04);
    IN(0x11, 0x0c);                                              /* general reject */
    CHECK_BYTES(next_avdtp(), 0x20, 0x02, 0x04);                 /* plain GET_CAPABILITIES */
    IN(0x22, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x22, 0x15, 0x02, 0x30);
    struct sent_packet *p = next_avdtp();
    CHECK(p && p->data[1] == 0x03 && p->data[10] == 0x22 && p->data[13] == 0x30);
}

TEST(no_sbc_means_no_stream)
{
    setup();
    avdtp_connect(H);
    sig_cid = accept_ours(0x2005);
    next_avdtp();
    IN(0x02, 0x01, 0x04, 0x08);
    next_avdtp();
    IN(0x12, 0x0c, 0x01, 0x00, 0x07, 0x08, 0x00, 0x02, 0xc0, 0x01, 0x8c, 0x83, 0xe8, 0x00);  /* AAC only */
    CHECK(open_calls == 1 && !open_ok);
}

TEST(headphones_lead)
{
    setup();
    sig_cid = open_theirs(0x0050);
    IN(0x00, 0x01);                                              /* DISCOVER */
    CHECK_BYTES(next_avdtp(), 0x02, 0x01, 0x04, 0x00);           /* seid 1, audio source */
    IN(0x10, 0x02, 0x04);                                        /* GET_CAPABILITIES */
    CHECK_BYTES(next_avdtp(), 0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xff, 0x02, 0x35);
    IN(0x20, 0x03, 0x04, 0x08, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35);
    CHECK_BYTES(next_avdtp(), 0x22, 0x03);
    CHECK(configured_calls == 1 && cfg.rate == 44100);
    IN(0x30, 0x03, 0x04, 0x08, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35);
    struct sent_packet *p = next_avdtp();                        /* already configured */
    CHECK(p && p->data[0] == 0x33 && p->data[3] == 0x13);
    IN(0x40, 0x06, 0x04);                                        /* OPEN */
    CHECK_BYTES(next_avdtp(), 0x42, 0x06);
    open_theirs(0x0051);                                         /* their media channel */
    CHECK(open_calls == 1 && open_ok);
    IN(0x50, 0x07, 0x04);                                        /* START */
    CHECK_BYTES(next_avdtp(), 0x52, 0x07);
    CHECK(started_calls == 1 && avdtp_is_streaming());
    IN(0x60, 0x2a);                                              /* something unknown */
    CHECK_BYTES(next_avdtp(), 0x61, 0x2a);                       /* general reject */
}

TEST(link_loss_ends_the_stream)
{
    setup();
    lead_to_open();
    fake_event(0x05, (const uint8_t[]){ 0, H, 0, 0x08 }, 4);     /* disconnection */
    CHECK(closed_calls == 1);
    /* a new stream can start on a new link */
    fake_connect(0x000d, airpods);
    CHECK(avdtp_connect(0x000d));
}

TEST(no_answer_times_out)
{
    setup();
    avdtp_connect(H);
    sig_cid = accept_ours(0x2005);
    next_avdtp();                                                /* DISCOVER, unanswered */
    fake_now_ms += AVDTP_TIMEOUT_MS + 1;
    avdtp_tick();
    CHECK(open_calls == 1 && !open_ok);
}

/* Our signaling channel request, unanswered so far */
static struct { uint8_t id; uint16_t cid; } req;
static void our_request(void)
{
    struct sent_packet *p = next_l2cap();
    CHECK(p && p->data[4] == 0x02);
    req.id = p ? p->data[5] : 0;
    req.cid = p ? lbt_get16(p->data + 10) : 0;
}

static void refuse(uint16_t result)
{
    uint8_t rsp[8] = { 0 };
    lbt_put16(rsp + 2, req.cid);
    lbt_put16(rsp + 4, result);
    signal_in(0x03, req.id, rsp, 8);
}

static bool we_discover(void)
{
    struct sent_packet *p = next_avdtp();
    return p && (p->data[0] & 0x0f) == 0 && p->data[1] == 0x01;
}

/* A car: right after pairing it opens signaling to us while ours is on
 * its way, then refuses ours. Theirs is used; nothing fails. */
TEST(crossed_signaling_uses_theirs)
{
    setup();
    CHECK(avdtp_connect(H));
    our_request();
    sig_cid = open_theirs(0x0050);
    refuse(0x0004);                                          /* no resources */
    CHECK(open_calls == 0);
    IN(0x00, 0x01);                                              /* they lead: DISCOVER */
    CHECK_BYTES(next_avdtp(), 0x02, 0x01, 0x04, 0x00);
}

/* They open theirs but don't lead: we do, on theirs */
TEST(crossed_signaling_we_lead_on_theirs)
{
    setup();
    CHECK(avdtp_connect(H));
    our_request();
    sig_cid = open_theirs(0x0050);
    fake_now_ms += AVDTP_THEIRS_MS + 1;
    avdtp_tick();
    CHECK(we_discover());
    CHECK(open_calls == 0);
}

/* They refuse ours first, then open theirs */
TEST(refused_then_theirs)
{
    setup();
    CHECK(avdtp_connect(H));
    our_request();
    refuse(0x0004);
    CHECK(open_calls == 0);
    sig_cid = open_theirs(0x0050);
    fake_now_ms += AVDTP_THEIRS_MS + 1;
    avdtp_tick();
    CHECK(we_discover());
    CHECK(open_calls == 0);
}

/* Refused, and nothing of theirs comes: setup fails, saying so */
TEST(refused_and_nothing)
{
    setup();
    CHECK(avdtp_connect(H));
    our_request();
    refuse(0x0003);                               /* security block */
    fake_now_ms += AVDTP_THEIRS_MS + 1;
    avdtp_tick();
    CHECK(open_calls == 1 && !open_ok);
}

/* They opened signaling but set nothing up: avdtp_connect leads on it */
TEST(connect_leads_on_their_channel)
{
    setup();
    sig_cid = open_theirs(0x0050);
    CHECK(avdtp_connect(H));
    CHECK(we_discover());
}

int main(void)
{
    RUN(choose_sbc_like_the_recording);
    RUN(we_lead_like_the_recording);
    RUN(older_device_without_get_all);
    RUN(no_sbc_means_no_stream);
    RUN(headphones_lead);
    RUN(link_loss_ends_the_stream);
    RUN(no_answer_times_out);
    RUN(crossed_signaling_uses_theirs);
    RUN(crossed_signaling_we_lead_on_theirs);
    RUN(refused_then_theirs);
    RUN(refused_and_nothing);
    RUN(connect_leads_on_their_channel);
    printf(test_failures ? "\n%d FAILED\n" : "\nall AVDTP tests passed\n", test_failures);
    return test_failures != 0;
}
