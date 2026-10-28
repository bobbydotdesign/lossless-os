/*
 * lbt host tests: cover art (OBEX over L2CAP ERTM, BIP cover art), the
 * way a car asks for it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../coverart.h"
#include "../lbt_platform.h"

#define H 0x000b
static const uint8_t car[6] = { 0x19, 0x95, 0x00, 0x11, 0x22, 0x33 };
static const uint8_t uuid[16] = { 0x71, 0x63, 0xdd, 0x54, 0x4a, 0x7e, 0x11, 0xe2,
                                  0xb4, 0x7c, 0x00, 0x50, 0xc2, 0x49, 0x00, 0x48 };

static uint8_t jpeg[3000];
static int image_calls, up_calls, down_calls;

static bool image(const char *handle, const uint8_t **data, uint32_t *len)
{
    image_calls++;
    if (strcmp(handle, "0000001"))
        return false;
    *data = jpeg;
    *len = sizeof(jpeg);
    return true;
}
static void connected(bool up) { if (up) up_calls++; else down_calls++; }
static const coverart_ops ops = { image, connected };

static uint16_t cid;                     /* ours */
static uint8_t txseq;                    /* the car's next I-frame */
static uint8_t got_seq;                  /* our next I-frame it expects */

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

static struct sent_packet *next_l2cap(void)
{
    static struct sent_packet out;
    struct sent_packet *p = fake_take();
    if (!p || p->type != LBT_H4_ACL || p->len < 4)
        return NULL;
    fake_completed(H, 1);
    out.type = p->type;
    out.len = p->len - 4;
    memcpy(out.data, p->data + 4, out.len);
    return &out;
}

/* One OBEX packet from the car, in one I-frame */
static void obex_in(const uint8_t *d, uint16_t n)
{
    uint8_t pdu[4 + 2 + 200 + 2];
    lbt_put16(pdu, 2 + n + 2);
    lbt_put16(pdu + 2, cid);
    lbt_put16(pdu + 4, (uint16_t)(txseq << 1 | got_seq << 8));
    memcpy(pdu + 6, d, n);
    lbt_put16(pdu + 6 + n, fcs(pdu, 6 + n));
    fake_acl(H, 2, pdu, 6 + n + 2);
    txseq = (txseq + 1) & 63;
}

/* Our next OBEX packet: our I-frames put together (S-frames passed over) */
static uint16_t obex_out(uint8_t *out)
{
    uint16_t n = 0, total = 0;
    struct sent_packet *p;
    while ((p = next_l2cap()) != NULL) {
        if (p->len < 8 || fcs(p->data, p->len - 2) != lbt_get16(p->data + p->len - 2))
            return 0;
        uint16_t ctrl = lbt_get16(p->data + 4);
        if (ctrl & 1)
            continue;                                   /* an RR */
        CHECK(((ctrl >> 1) & 63) == got_seq);
        got_seq = (got_seq + 1) & 63;
        uint8_t sar = ctrl >> 14;
        const uint8_t *d = p->data + 6;
        uint16_t len = p->len - 8;
        if (sar == 1) {
            total = lbt_get16(d);
            d += 2;
            len -= 2;
        }
        memcpy(out + n, d, len);
        n += len;
        if (sar == 0 || sar == 2) {
            /* the car acknowledges what it has */
            uint8_t rr[4 + 2 + 2];
            lbt_put16(rr, 4);
            lbt_put16(rr + 2, cid);
            lbt_put16(rr + 4, (uint16_t)(0x0001 | got_seq << 8));
            lbt_put16(rr + 6, fcs(rr, 6));
            fake_acl(H, 2, rr, 8);
            CHECK(sar == 0 || n == total);
            return n;
        }
    }
    return 0;
}

static void setup(void)
{
    fake_start(1021, 8);
    l2cap_init();
    coverart_init(&ops);
    fake_connect(H, car);
    image_calls = up_calls = down_calls = 0;
    txseq = got_seq = 0;
    for (int i = 0; i < (int)sizeof(jpeg); i++)
        jpeg[i] = (uint8_t)(i * 13 + 1);
    /* the car opens the cover art channel in ERTM: window 10, MPS 300 */
    signal_in(0x02, 0x30, (const uint8_t[]){ 0x05, 0x10, 0x60, 0x00 }, 4);
    struct sent_packet *p = next_l2cap();
    cid = p ? lbt_get16(p->data + 8) : 0;
    p = next_l2cap();
    uint8_t our_id = p ? p->data[5] : 0;
    uint8_t req[4 + 4 + 11 + 3] = { 0, 0, 0, 0, 0x01, 0x02, 0x9b, 0x06,
                                    0x04, 0x09, 0x03, 10, 3, 0, 0, 0, 0, 0x2c, 0x01,
                                    0x05, 0x01, 0x01 };
    lbt_put16(req, cid);
    signal_in(0x04, 0x31, req, sizeof(req));
    next_l2cap();
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, cid);
    signal_in(0x05, our_id, ok, 6);
}

/* CONNECT to the cover art service, largest packet 1024 */
static void obex_connect(void)
{
    uint8_t c[7 + 19] = { 0x80, 0, 26, 0x10, 0x00, 0x04, 0x00, 0x46, 0x00, 19 };
    memcpy(c + 10, uuid, 16);
    obex_in(c, sizeof(c));
}

/* GET, final: type and handle */
static void obex_get(const char *type, const char *handle)
{
    uint8_t g[200];
    uint16_t n = 3;
    g[0] = 0x83;
    g[n++] = 0xcb;
    lbt_put32be(g + n, 1);
    n += 4;
    if (type) {
        uint16_t tl = (uint16_t)strlen(type) + 1;
        g[n++] = 0x42;
        lbt_put16be(g + n, (uint16_t)(3 + tl));
        n += 2;
        memcpy(g + n, type, tl);
        n += tl;
    }
    if (handle) {
        uint16_t hl = (uint16_t)(2 * (strlen(handle) + 1));
        g[n++] = 0x30;
        lbt_put16be(g + n, (uint16_t)(3 + hl));
        n += 2;
        for (size_t i = 0; i <= strlen(handle); i++) {
            g[n++] = 0;
            g[n++] = (uint8_t)handle[i];
        }
    }
    lbt_put16be(g + 1, n);
    obex_in(g, n);
}

TEST(car_connects_for_covers)
{
    setup();
    CHECK(!coverart_connected());
    obex_connect();
    uint8_t r[2048];
    uint16_t n = obex_out(r);
    CHECK(n == 31 && r[0] == 0xa0 && lbt_get16be(r + 1) == 31 && r[3] == 0x10 &&
          lbt_get16be(r + 5) == 1691);                       /* success, version, our largest */
    CHECK(r[7] == 0xcb && lbt_get32be(r + 8) == 1);          /* connection id */
    CHECK(r[12] == 0x4a && lbt_get16be(r + 13) == 19 && !memcmp(r + 15, uuid, 16));   /* who */
    CHECK(coverart_connected() && up_calls == 1);
}

TEST(car_gets_the_thumbnail_in_parts)
{
    setup();
    obex_connect();
    uint8_t r[2048], img[4000];
    obex_out(r);
    uint32_t have = 0;
    obex_get("x-bt/img-thm", "0000001");
    for (int parts = 0; parts < 10; parts++) {
        uint16_t n = obex_out(r);
        CHECK(n >= 6 && n <= 1024 && lbt_get16be(r + 1) == n);
        uint16_t blen = lbt_get16be(r + 4) - 3;
        memcpy(img + have, r + 6, blen);
        have += blen;
        if (r[0] == 0xa0) {
            CHECK(r[3] == 0x49);                              /* end of body */
            break;
        }
        CHECK(r[0] == 0x90 && r[3] == 0x48);                  /* continue, body */
        obex_get(NULL, NULL);                                 /* the next part */
    }
    CHECK(have == sizeof(jpeg) && !memcmp(img, jpeg, sizeof(jpeg)));
}

TEST(car_reads_the_image_properties)
{
    setup();
    obex_connect();
    uint8_t r[2048];
    obex_out(r);
    obex_get("x-bt/img-properties", "0000001");
    uint16_t n = obex_out(r);
    CHECK(n > 6 && r[0] == 0xa0 && r[3] == 0x49);
    r[n] = 0;
    CHECK(strstr((char *)r + 6, "handle=\"0000001\"") && strstr((char *)r + 6, "pixel=\"200*200\"") &&
          strstr((char *)r + 6, "size=\"3000\""));
}

TEST(a_cover_we_dont_have)
{
    setup();
    obex_connect();
    uint8_t r[2048];
    obex_out(r);
    obex_get("x-bt/img-img", "0000009");
    CHECK(obex_out(r) == 3 && r[0] == 0xc4);                 /* not found */
}

TEST(not_the_cover_art_service)
{
    setup();
    uint8_t c[7 + 19] = { 0x80, 0, 26, 0x10, 0x00, 0x04, 0x00, 0x46, 0x00, 19 };
    obex_in(c, sizeof(c));                                    /* a target of zeros */
    uint8_t r[64];
    CHECK(obex_out(r) == 3 && r[0] == 0xd3 && !coverart_connected());
}

TEST(car_goes)
{
    setup();
    obex_connect();
    uint8_t r[64];
    obex_out(r);
    obex_in((const uint8_t[]){ 0x81, 0x00, 0x03 }, 3);        /* disconnect */
    CHECK(obex_out(r) == 3 && r[0] == 0xa0);
    CHECK(!coverart_connected());
}

int main(void)
{
    RUN(car_connects_for_covers);
    RUN(car_gets_the_thumbnail_in_parts);
    RUN(car_reads_the_image_properties);
    RUN(a_cover_we_dont_have);
    RUN(not_the_cover_art_service);
    RUN(car_goes);
    printf(test_failures ? "\n%d FAILED\n" : "\nall cover art tests passed\n", test_failures);
    return test_failures != 0;
}
