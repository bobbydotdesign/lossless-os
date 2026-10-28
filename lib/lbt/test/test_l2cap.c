/*
 * lbt host tests: L2CAP.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../lbt_platform.h"

#define H 0x000b
static const uint8_t airpods[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static int opened_calls, closed_calls, data_calls;
static uint16_t opened_result;
static l2cap_chan *last_chan;
static uint8_t last_data[64];
static uint16_t last_len;

static void on_opened(l2cap_chan *c, uint16_t result)
{
    opened_calls++;
    opened_result = result;
    last_chan = c;
}
static void on_data(l2cap_chan *c, const uint8_t *d, uint16_t len)
{
    (void)c;
    data_calls++;
    last_len = len;
    memcpy(last_data, d, len < sizeof(last_data) ? len : sizeof(last_data));
}
static void on_closed(l2cap_chan *c) { (void)c; closed_calls++; }
static const l2cap_ops ops = { on_opened, on_data, on_closed };

static void setup(void)
{
    fake_start(1021, 8);
    l2cap_init();
    fake_connect(H, airpods);
    opened_calls = closed_calls = data_calls = 0;
    opened_result = 0xffff;
    last_chan = NULL;
}

/* A signaling command from the headphones */
static void signal_in(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t pdu[8 + 64];
    lbt_put16(pdu, 4 + len);
    lbt_put16(pdu + 2, 0x0001);
    pdu[4] = code;
    pdu[5] = id;
    lbt_put16(pdu + 6, len);
    memcpy(pdu + 8, d, len);
    fake_acl(H, 2, pdu, 8 + len);
}

/* The next ACL packet's L2CAP part (after the 4-byte ACL header) */
static struct sent_packet *next_l2cap(void)
{
    static struct sent_packet out;
    struct sent_packet *p = fake_take();
    if (!p || p->type != LBT_H4_ACL || p->len < 4)
        return NULL;
    out.type = p->type;
    out.len = p->len - 4;
    memcpy(out.data, p->data + 4, out.len);
    return &out;
}

/* Our side opens AVDTP; returns our request ids through *ids */
static l2cap_chan *open_avdtp(void)
{
    l2cap_chan *c = l2cap_connect(H, L2CAP_PSM_AVDTP, &ops, 1000, NULL);
    struct sent_packet *p = next_l2cap();   /* connection request */
    uint8_t id = p ? p->data[5] : 0;
    signal_in(0x03, id, (const uint8_t[]){ 0x50, 0, 0x40, 0, 0, 0, 0, 0 }, 8);
    p = next_l2cap();                        /* our configuration request */
    uint8_t cid = p ? p->data[5] : 0;
    signal_in(0x04, 7, (const uint8_t[]){ 0x40, 0, 0, 0, 0x01, 2, 0x7f, 0x03 }, 8);
    next_l2cap();                            /* our response to theirs */
    signal_in(0x05, cid, (const uint8_t[]){ 0x40, 0, 0, 0, 0, 0 }, 6);
    return c;
}

TEST(we_open_a_channel)
{
    setup();
    l2cap_chan *c = l2cap_connect(H, L2CAP_PSM_AVDTP, &ops, 1000, (void *)0x1);
    CHECK(c != NULL);
    struct sent_packet *p = next_l2cap();
    CHECK_BYTES(p, 0x08, 0x00, 0x01, 0x00,  0x02, 0x01, 0x04, 0x00,  0x19, 0x00, 0x40, 0x00);

    /* they accept: CID 0x0050 */
    signal_in(0x03, 0x01, (const uint8_t[]){ 0x50, 0x00, 0x40, 0x00, 0, 0, 0, 0 }, 8);
    p = next_l2cap();   /* our configuration: MTU 1000 */
    CHECK_BYTES(p, 0x0c, 0x00, 0x01, 0x00,  0x04, 0x02, 0x08, 0x00,
                0x50, 0x00, 0x00, 0x00, 0x01, 0x02, 0xe8, 0x03);

    /* theirs: MTU 895 */
    signal_in(0x04, 0x09, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00, 0x01, 0x02, 0x7f, 0x03 }, 8);
    p = next_l2cap();
    CHECK_BYTES(p, 0x0a, 0x00, 0x01, 0x00,  0x05, 0x09, 0x06, 0x00,
                0x50, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(opened_calls == 0);

    /* they accept ours: open */
    signal_in(0x05, 0x02, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00, 0x00, 0x00 }, 6);
    CHECK(opened_calls == 1 && opened_result == 0 && last_chan == c);
    CHECK(l2cap_is_open(c) && l2cap_max_send(c) == 895);
    CHECK(l2cap_user(c) == (void *)0x1);

    /* data out goes to their CID */
    CHECK(l2cap_send(c, (const uint8_t[]){ 0xaa, 0xbb }, 2));
    CHECK_BYTES(next_l2cap(), 0x02, 0x00, 0x50, 0x00, 0xaa, 0xbb);
    /* data in on our CID */
    fake_acl(H, 2, (const uint8_t[]){ 0x03, 0x00, 0x40, 0x00, 1, 2, 3 }, 7);
    CHECK(data_calls == 1 && last_len == 3 && last_data[2] == 3);
    /* too long for them */
    static uint8_t big[900];
    CHECK(!l2cap_send(c, big, 896));
}

TEST(pending_then_refused)
{
    setup();
    l2cap_connect(H, L2CAP_PSM_AVDTP, &ops, 1000, NULL);
    struct sent_packet *p = next_l2cap();
    uint8_t id = p->data[5];
    signal_in(0x03, id, (const uint8_t[]){ 0, 0, 0x40, 0, 0x01, 0, 0x02, 0 }, 8);   /* pending */
    CHECK(opened_calls == 0);
    signal_in(0x03, id, (const uint8_t[]){ 0, 0, 0x40, 0, 0x03, 0, 0, 0 }, 8);      /* security block */
    CHECK(opened_calls == 1 && opened_result == L2CAP_RESULT_SECURITY_BLOCK);
}

TEST(they_open_a_channel)
{
    setup();
    CHECK(l2cap_listen(L2CAP_PSM_AVCTP, &ops, 672));
    signal_in(0x02, 0x05, (const uint8_t[]){ 0x17, 0x00, 0x41, 0x00 }, 4);
    /* accepted with our CID 0x0040, then our configuration */
    CHECK_BYTES(next_l2cap(), 0x0c, 0x00, 0x01, 0x00,  0x03, 0x05, 0x08, 0x00,
                0x40, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00);
    struct sent_packet *p = next_l2cap();
    CHECK(p && p->data[4] == 0x04 && lbt_get16(p->data + 8) == 0x0041 &&
          lbt_get16(p->data + 14) == 672);
    uint8_t id = p ? p->data[5] : 0;
    signal_in(0x04, 0x06, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00 }, 4);   /* no options */
    next_l2cap();
    signal_in(0x05, id, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00, 0x00, 0x00 }, 6);
    CHECK(opened_calls == 1 && opened_result == 0);
    CHECK(l2cap_psm(last_chan) == L2CAP_PSM_AVCTP);
    CHECK(l2cap_max_send(last_chan) == L2CAP_DEFAULT_MTU);   /* no MTU given */
}

TEST(unknown_psm_refused)
{
    setup();
    signal_in(0x02, 0x05, (const uint8_t[]){ 0x03, 0x00, 0x41, 0x00 }, 4);   /* RFCOMM */
    CHECK_BYTES(next_l2cap(), 0x0c, 0x00, 0x01, 0x00,  0x03, 0x05, 0x08, 0x00,
                0x00, 0x00, 0x41, 0x00, 0x02, 0x00, 0x00, 0x00);
}

TEST(only_basic_mode)
{
    setup();
    l2cap_listen(L2CAP_PSM_AVCTP, &ops, 672);
    signal_in(0x02, 0x05, (const uint8_t[]){ 0x17, 0x00, 0x41, 0x00 }, 4);
    next_l2cap();
    next_l2cap();
    /* they ask for ERTM (mode 3) */
    signal_in(0x04, 0x06, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00,
              0x04, 9, 0x03, 1, 1, 0, 0, 0, 0, 0, 0 }, 15);
    struct sent_packet *p = next_l2cap();
    CHECK(p && p->data[4] == 0x05 && lbt_get16(p->data + 12) == 0x0001);   /* unacceptable */
    CHECK(p && p->data[14] == 0x04 && p->data[16] == 0x00);              /* basic offered */
    CHECK(opened_calls == 0);
}

TEST(unknown_option_named)
{
    setup();
    l2cap_listen(L2CAP_PSM_AVCTP, &ops, 672);
    signal_in(0x02, 0x05, (const uint8_t[]){ 0x17, 0x00, 0x41, 0x00 }, 4);
    next_l2cap();
    next_l2cap();
    signal_in(0x04, 0x06, (const uint8_t[]){ 0x40, 0x00, 0x00, 0x00,
              0x09, 1, 0x00,    /* unknown, not a hint */
              0x8a, 1, 0x00 }, 10);   /* unknown hint: ignored */
    CHECK_BYTES(next_l2cap(), 0x0b, 0x00, 0x01, 0x00,  0x05, 0x06, 0x07, 0x00,
                0x41, 0x00, 0x00, 0x00, 0x03, 0x00, 0x09);
}

TEST(they_close_it)
{
    setup();
    open_avdtp();
    CHECK(opened_calls == 1);
    signal_in(0x06, 0x0a, (const uint8_t[]){ 0x40, 0x00, 0x50, 0x00 }, 4);
    CHECK_BYTES(next_l2cap(), 0x08, 0x00, 0x01, 0x00,  0x07, 0x0a, 0x04, 0x00,
                0x40, 0x00, 0x50, 0x00);
    CHECK(closed_calls == 1);
}

TEST(we_close_it)
{
    setup();
    l2cap_chan *c = open_avdtp();
    l2cap_disconnect(c);
    struct sent_packet *p = next_l2cap();
    CHECK(p && p->data[4] == 0x06 && lbt_get16(p->data + 8) == 0x0050 &&
          lbt_get16(p->data + 10) == 0x0040);
    CHECK(closed_calls == 0);
    signal_in(0x07, p ? p->data[5] : 0, (const uint8_t[]){ 0x50, 0x00, 0x40, 0x00 }, 4);
    CHECK(closed_calls == 1);
}

TEST(link_lost_closes_channels)
{
    setup();
    open_avdtp();
    l2cap_connect(H, L2CAP_PSM_AVCTP, &ops, 672, NULL);   /* still opening */
    fake_event(HCI_EV_DISCONNECTION_COMPLETE, (const uint8_t[]){ 0, H, 0, 0x08 }, 4);
    CHECK(closed_calls == 1);                              /* the open one */
    CHECK(opened_calls == 2 && opened_result == L2CAP_RESULT_LINK_LOST);
}

TEST(echo_and_info)
{
    setup();
    signal_in(0x08, 0x21, (const uint8_t[]){ 'h', 'i' }, 2);
    CHECK_BYTES(next_l2cap(), 0x06, 0x00, 0x01, 0x00,  0x09, 0x21, 0x02, 0x00, 'h', 'i');
    signal_in(0x0a, 0x22, (const uint8_t[]){ 0x02, 0x00 }, 2);   /* extended features */
    CHECK_BYTES(next_l2cap(), 0x0c, 0x00, 0x01, 0x00,  0x0b, 0x22, 0x08, 0x00,
                0x02, 0x00, 0x00, 0x00, 0xa8, 0x00, 0x00, 0x00);   /* ERTM, FCS, fixed channels */
    signal_in(0x42, 0x23, NULL, 0);                               /* nonsense */
    CHECK_BYTES(next_l2cap(), 0x06, 0x00, 0x01, 0x00,  0x01, 0x23, 0x02, 0x00, 0x00, 0x00);
}

/* ---- ERTM (a car fetching a cover over OBEX) ---------------------------------- */

#define PSM_OBEX 0x1005
static uint16_t ertm_cid;                 /* ours */
static uint8_t big[4096];

static uint16_t fcs(const uint8_t *d, uint16_t n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xa001) : (uint16_t)(c >> 1);
    }
    return c;
}

/* An ERTM frame from the car: control, payload, FCS (bad: a wrong one) */
static void frame_in(uint16_t ctrl, const uint8_t *d, uint16_t n, bool bad)
{
    uint8_t pdu[4 + 2 + 400 + 2];
    lbt_put16(pdu, 2 + n + 2);
    lbt_put16(pdu + 2, ertm_cid);
    lbt_put16(pdu + 4, ctrl);
    memcpy(pdu + 6, d, n);
    lbt_put16(pdu + 6 + n, (uint16_t)(fcs(pdu, 6 + n) ^ (bad ? 1 : 0)));
    fake_acl(H, 2, pdu, 6 + n + 2);
}

/* Our next ERTM frame: its control field, FCS checked (0xffff: none, or bad) */
static uint16_t frame_out(struct sent_packet **out)
{
    struct sent_packet *p = next_l2cap();
    if (out)
        *out = p;
    if (p)
        fake_completed(H, 1);                /* gone out: its buffer is free again */
    if (!p || p->len < 8 || lbt_get16(p->data) + 4 != p->len)
        return 0xffff;
    if (fcs(p->data, p->len - 2) != lbt_get16(p->data + p->len - 2))
        return 0xffff;
    return lbt_get16(p->data + 4);
}

/* The car opens our OBEX PSM in ERTM: window 4, MPS 300 towards it */
static void open_ertm(void)
{
    l2cap_listen_ertm(PSM_OBEX, &ops, 1691);
    signal_in(0x02, 0x30, (const uint8_t[]){ 0x05, 0x10, 0x60, 0x00 }, 4);
    struct sent_packet *p = next_l2cap();                    /* accepted */
    ertm_cid = p ? lbt_get16(p->data + 8) : 0;
    p = next_l2cap();                                        /* ours: MTU, ERTM, FCS */
    CHECK(p && p->data[4] == 0x04 && p->data[12] == 0x01 && p->data[16] == 0x04 &&
          p->data[18] == 0x03 && p->data[19] == 8 && lbt_get16(p->data + 25) == 1004 &&
          p->data[27] == 0x05 && p->data[29] == 0x01);
    uint8_t our_id = p ? p->data[5] : 0;
    uint8_t req[4 + 4 + 11 + 3] = { 0, 0, 0, 0, 0x01, 0x02, 0x9b, 0x06,       /* MTU 1691 */
                                    0x04, 0x09, 0x03, 4, 3, 0, 0, 0, 0, 0x2c, 0x01,  /* ERTM, MPS 300 */
                                    0x05, 0x01, 0x01 };
    lbt_put16(req, ertm_cid);
    signal_in(0x04, 0x31, req, sizeof(req));
    p = next_l2cap();                                        /* our answer: theirs, with our times */
    CHECK(p && p->data[4] == 0x05 && lbt_get16(p->data + 12) == 0 && p->data[14] == 0x04 &&
          p->data[16] == 0x03 && lbt_get16(p->data + 19) == 2000 &&
          lbt_get16(p->data + 21) == 12000 && lbt_get16(p->data + 23) == 300);
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ertm_cid);
    signal_in(0x05, our_id, ok, 6);
    CHECK(opened_calls == 1 && opened_result == 0);
    fake_completed(H, 3);                    /* the signaling went out */
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = (uint8_t)(i * 7);
}

TEST(ertm_takes_a_request_and_acknowledges_it)
{
    setup();
    open_ertm();
    frame_in(0x0000, (const uint8_t *)"hello", 5, false);    /* I, TxSeq 0 */
    CHECK(data_calls == 1 && last_len == 5 && !memcmp(last_data, "hello", 5));
    CHECK(frame_out(NULL) == 0x0101);                         /* RR, ReqSeq 1 */
    frame_in(0x0002, (const uint8_t *)"dam", 3, true);       /* a damaged one */
    CHECK(data_calls == 1 && next_l2cap() == NULL);
    frame_in(0x0002, (const uint8_t *)"aged", 4, false);     /* TxSeq 1, again */
    CHECK(data_calls == 2 && frame_out(NULL) == 0x0201);
}

TEST(ertm_puts_their_segments_together)
{
    setup();
    open_ertm();
    uint8_t start[6] = { 10, 0, 'a', 'b', 'c', 'd' };
    frame_in(0x4000, start, 6, false);                        /* start, SDU 10 */
    frame_in(0xc002, (const uint8_t *)"efg", 3, false);      /* continuation */
    CHECK(data_calls == 0);
    frame_in(0x8004, (const uint8_t *)"hij", 3, false);      /* end */
    CHECK(data_calls == 1 && last_len == 10 && !memcmp(last_data, "abcdefghij", 10));
}

TEST(ertm_sends_in_segments_and_keeps_them_until_acknowledged)
{
    setup();
    open_ertm();
    CHECK(l2cap_send(last_chan, big, 700));
    struct sent_packet *p;
    uint16_t c = frame_out(&p);
    CHECK(c == 0x4000 && lbt_get16(p->data + 6) == 700 && lbt_get16(p->data) == 2 + 2 + 298 + 2 &&
          !memcmp(p->data + 8, big, 298));                    /* start: SDU length, 298 */
    c = frame_out(&p);
    CHECK(c == 0xc002 && lbt_get16(p->data) == 2 + 300 + 2 && !memcmp(p->data + 6, big + 298, 300));
    c = frame_out(&p);
    CHECK(c == 0x8004 && lbt_get16(p->data) == 2 + 102 + 2 && !memcmp(p->data + 6, big + 598, 102));
    CHECK(!l2cap_can_send(last_chan, 10) && !l2cap_send(last_chan, big, 10));
    frame_in(0x0301, NULL, 0, false);                         /* RR, ReqSeq 3: all of it */
    CHECK(l2cap_can_send(last_chan, 10));
}

TEST(ertm_keeps_to_their_window)
{
    setup();
    open_ertm();
    CHECK(l2cap_send(last_chan, big, 1600));                 /* 6 frames; their window is 4 */
    for (int i = 0; i < 4; i++)
        CHECK(((frame_out(NULL) >> 1) & 63) == i);
    CHECK(next_l2cap() == NULL);
    frame_in(0x0201, NULL, 0, false);                         /* two acknowledged */
    CHECK(((frame_out(NULL) >> 1) & 63) == 4 && ((frame_out(NULL) >> 1) & 63) == 5);
    CHECK(next_l2cap() == NULL);
}

/* No room in the controller: the frame waits for the tick */
TEST(ertm_waits_for_room)
{
    setup();
    open_ertm();
    for (int i = 0; i < 8; i++)                              /* fill HCI's buffers */
        CHECK(hci_send_pdu(H, (const uint8_t[]){ 0, 0, 0x40, 0 }, 4));
    CHECK(l2cap_send(last_chan, big, 100));
    for (int i = 0; i < 8; i++)
        next_l2cap();
    CHECK(next_l2cap() == NULL);                             /* held back */
    fake_completed(H, 8);
    l2cap_tick();
    CHECK(frame_out(NULL) == 0x0000);
}

TEST(ertm_sends_again_from_a_reject)
{
    setup();
    open_ertm();
    l2cap_send(last_chan, big, 700);
    frame_out(NULL); frame_out(NULL); frame_out(NULL);
    frame_in(0x0105, NULL, 0, false);                         /* REJ, ReqSeq 1 */
    CHECK(((frame_out(NULL) >> 1) & 63) == 1 && ((frame_out(NULL) >> 1) & 63) == 2);
}

TEST(ertm_polls_when_nothing_is_acknowledged)
{
    setup();
    open_ertm();
    l2cap_send(last_chan, big, 100);
    CHECK(frame_out(NULL) == 0x0000);
    l2cap_tick();
    CHECK(next_l2cap() == NULL);
    fake_now_ms += 2001;
    l2cap_tick();
    CHECK(frame_out(NULL) == 0x0011);                         /* RR with the poll bit */
    frame_in(0x0081, NULL, 0, false);                         /* their final: ReqSeq 0 */
    CHECK(frame_out(NULL) == 0x0000);                         /* sent again */
    frame_in(0x0101, NULL, 0, false);
    CHECK(l2cap_can_send(last_chan, 10));
}

TEST(ertm_answers_their_poll)
{
    setup();
    open_ertm();
    frame_in(0x0011, NULL, 0, false);                         /* RR, poll */
    CHECK(frame_out(NULL) == 0x0081);                         /* RR, final */
}

int main(void)
{
    RUN(we_open_a_channel);
    RUN(pending_then_refused);
    RUN(they_open_a_channel);
    RUN(unknown_psm_refused);
    RUN(only_basic_mode);
    RUN(unknown_option_named);
    RUN(they_close_it);
    RUN(we_close_it);
    RUN(link_lost_closes_channels);
    RUN(echo_and_info);
    RUN(ertm_takes_a_request_and_acknowledges_it);
    RUN(ertm_puts_their_segments_together);
    RUN(ertm_sends_in_segments_and_keeps_them_until_acknowledged);
    RUN(ertm_keeps_to_their_window);
    RUN(ertm_waits_for_room);
    RUN(ertm_sends_again_from_a_reject);
    RUN(ertm_polls_when_nothing_is_acknowledged);
    RUN(ertm_answers_their_poll);
    printf(test_failures ? "\n%d FAILED\n" : "\nall L2CAP tests passed\n", test_failures);
    return test_failures != 0;
}
