/*
 * lbt host tests: the SDP server, against the nine requests the AirPods
 * Pro 3 sent in the recording.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../sdp.h"

static uint8_t rsp[1024];

static bool contains(const uint8_t *p, int len, const uint8_t *want, int wlen)
{
    for (int i = 0; i + wlen <= len; i++)
        if (!memcmp(p + i, want, wlen))
            return true;
    return false;
}

#define REQ(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })

static uint16_t ask(const uint8_t *req, uint16_t len)
{
    return sdp_handle(req, len, rsp, 256);   /* their MTU was 256 */
}

static void setup(void)
{
    fake_start(1021, 8);
    l2cap_init();
    sdp_init();
}

/* The empty answer: no record matches */
static void check_empty(uint16_t n, uint16_t tid)
{
    CHECK(n == 11);
    CHECK(rsp[0] == 0x07 && lbt_get16be(rsp + 1) == tid);
    CHECK(lbt_get16be(rsp + 5) == 3 && rsp[7] == 0x36 && rsp[8] == 0 && rsp[9] == 0 && rsp[10] == 0);
}

TEST(airpods_ask_for_things_we_dont_have)
{
    setup();
    /* PnP information (0x1200) */
    check_empty(ask(REQ(0x06, 0x00, 0x01, 0x00, 0x1f, 0x35, 0x03, 0x19, 0x12, 0x00, 0x00, 0x40,
        0x35, 0x15, 0x09, 0x02, 0x01, 0x09, 0x02, 0x02, 0x09, 0x02, 0x03, 0x09, 0x02, 0x05,
        0x09, 0xa0, 0x00, 0x09, 0xa0, 0x01, 0x09, 0xaf, 0xff, 0x00)), 1);
    /* hands-free audio gateway (0x111f) */
    check_empty(ask(REQ(0x06, 0x00, 0x02, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x11, 0x1f, 0x00, 0x40,
        0x35, 0x03, 0x09, 0x00, 0x04, 0x00)), 2);
    /* an Apple 128-bit service */
    check_empty(ask(REQ(0x06, 0x00, 0x05, 0x00, 0x1b, 0x35, 0x11, 0x1c, 0x1f, 0xf3, 0x19, 0x36,
        0x57, 0x2e, 0x4b, 0x36, 0xa2, 0xbf, 0xb2, 0x40, 0x9b, 0x1a, 0xa6, 0xf4, 0x00, 0x40,
        0x35, 0x03, 0x09, 0x00, 0x04, 0x00)), 5);
    /* device information (0x180a) */
    check_empty(ask(REQ(0x06, 0x00, 0x07, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x18, 0x0a, 0x00, 0x40,
        0x35, 0x03, 0x09, 0x00, 0x09, 0x00)), 7);
}

TEST(airpods_find_our_audio_source)
{
    setup();
    /* A2DP source (0x110a), protocol descriptor list (0x0004) */
    uint16_t n = ask(REQ(0x06, 0x00, 0x03, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x11, 0x0a, 0x00, 0x40,
                         0x35, 0x03, 0x09, 0x00, 0x04, 0x00));
    CHECK(rsp[0] == 0x07 && lbt_get16be(rsp + 1) == 3);
    CHECK(rsp[n - 1] == 0);                       /* no continuation */
    /* L2CAP on PSM 0x19, AVDTP 1.3 */
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x09, 0x00, 0x04 }, 3));
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x19, 0x01, 0x00, 0x09, 0x00, 0x19 }, 6));
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x19, 0x00, 0x19, 0x09, 0x01, 0x03 }, 6));
    /* only the attribute asked for */
    CHECK(!contains(rsp, n, (const uint8_t[]){ 0x09, 0x01, 0x00, 0x25 }, 4));
}

TEST(airpods_find_our_remote_control_target)
{
    setup();
    /* AVRCP target (0x110c), profile descriptor list (0x0009) */
    uint16_t n = ask(REQ(0x06, 0x00, 0x04, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x11, 0x0c, 0x00, 0x40,
                         0x35, 0x03, 0x09, 0x00, 0x09, 0x00));
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x09, 0x00, 0x09 }, 3));
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x19, 0x11, 0x0e, 0x09, 0x01, 0x06 }, 6));   /* AVRCP 1.6 */
}

/* A car looking for cover art: the feature, and the OBEX channel for it */
TEST(car_finds_our_cover_art)
{
    setup();
    uint16_t n = ask(REQ(0x06, 0x00, 0x05, 0x00, 0x0f, 0x35, 0x03, 0x19, 0x11, 0x0c, 0x01, 0x00,
                         0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00));
    CHECK(rsp[0] == 0x07);
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x09, 0x00, 0x0d, 0x35, 0x0f, 0x35, 0x0d,
                                              0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x10, 0x05,
                                              0x35, 0x03, 0x19, 0x00, 0x08 }, 20));   /* L2CAP 0x1005, OBEX */
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x09, 0x03, 0x11, 0x09, 0x01, 0x01 }, 6));  /* cover art */
}

TEST(service_search_and_attributes)
{
    setup();
    /* who has AVCTP? the target and the controller */
    uint16_t n = ask(REQ(0x02, 0x00, 0x10, 0x00, 0x08, 0x35, 0x03, 0x19, 0x00, 0x17, 0x00, 0x10, 0x00));
    CHECK(rsp[0] == 0x03 && lbt_get16be(rsp + 5) == 2 && n == 18);
    CHECK(lbt_get32be(rsp + 9) == SDP_HANDLE_AVRCP_TARGET);
    CHECK(lbt_get32be(rsp + 13) == SDP_HANDLE_AVRCP_CTRL);
    /* the source record's name and features, by handle */
    n = ask(REQ(0x04, 0x00, 0x11, 0x00, 0x0f, 0x00, 0x01, 0x00, 0x01, 0x00, 0x80,
                0x35, 0x06, 0x09, 0x01, 0x00, 0x09, 0x03, 0x11, 0x00));
    CHECK(rsp[0] == 0x05);
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x25, 12, 'A', 'u', 'd', 'i', 'o', ' ' }, 8));
    CHECK(contains(rsp, n, (const uint8_t[]){ 0x09, 0x03, 0x11, 0x09, 0x00, 0x01 }, 6));
    /* a handle we don't have */
    n = ask(REQ(0x04, 0x00, 0x12, 0x00, 0x0c, 0x00, 0x09, 0x09, 0x09, 0x00, 0x80,
                0x35, 0x03, 0x09, 0x00, 0x00, 0x00));
    CHECK(n == 7 && rsp[0] == 0x01 && lbt_get16be(rsp + 5) == 0x0002);
    /* nonsense */
    n = ask(REQ(0x02, 0x00, 0x13, 0x00, 0x03, 0x09, 0x00, 0x01));
    CHECK(rsp[0] == 0x01 && lbt_get16be(rsp + 5) == 0x0003);
}

TEST(long_answers_continue)
{
    setup();
    /* every attribute of every record that has L2CAP, 32 bytes at a time */
    static uint8_t joined[1024];
    int total = 0, rounds = 0;
    uint8_t req[64] = { 0x06, 0x00, 0x20, 0x00, 0x00,
                        0x35, 0x03, 0x19, 0x01, 0x00,       /* L2CAP */
                        0x00, 0x20,                         /* 32 bytes max */
                        0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff,  /* all ids */
                        0x00 };
    int base = 19, len = 20;
    for (;;) {
        lbt_put16be(req + 3, (uint16_t)(len - 5));
        sdp_handle(req, (uint16_t)len, rsp, 256);
        CHECK(rsp[0] == 0x07);
        uint16_t count = lbt_get16be(rsp + 5);
        CHECK(count <= 32);
        memcpy(joined + total, rsp + 7, count);
        total += count;
        uint8_t cont = rsp[7 + count];
        if (++rounds > 40 || cont == 0)
            break;
        memcpy(req + base, rsp + 7 + count, 1 + cont);
        len = base + 1 + cont;
    }
    CHECK(rounds > 3);
    /* the joined answer is one sequence holding the three records */
    CHECK(joined[0] == 0x36 && lbt_get16be(joined + 1) + 3 == total);
    CHECK(contains(joined, total, (const uint8_t[]){ 'A', 'V', 'R', 'C', 'P', ' ', 'C' }, 7));
    printf("  %d bytes in %d answers\n", total, rounds);
}

int main(void)
{
    RUN(airpods_ask_for_things_we_dont_have);
    RUN(airpods_find_our_audio_source);
    RUN(airpods_find_our_remote_control_target);
    RUN(car_finds_our_cover_art);
    RUN(service_search_and_attributes);
    RUN(long_answers_continue);
    printf(test_failures ? "\n%d FAILED\n" : "\nall SDP tests passed\n", test_failures);
    return test_failures != 0;
}
