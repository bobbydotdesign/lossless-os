/*
 * lbt cover art (coverart.h): an OBEX server with the BIP cover art
 * service's three requests.
 *
 * OBEX (IrOBEX 1.5): a packet is its opcode, its length (2 bytes, big
 * endian, the whole packet) and headers; CONNECT and its answer carry
 * version, flags and the largest packet first. A header's id says its
 * form by its top two bits: text (UTF-16, 2-byte length), bytes (2-byte
 * length), one byte, four bytes. A GET's answer comes in parts: Continue
 * with a Body, and Success with the End of Body, each part asked for by
 * another GET. Over L2CAP each OBEX packet is one SDU (GOEP 2.0).
 *
 * BIP cover art (BIP 1.2, AVRCP 1.6 5.14): the target names the service
 * (UUID 7163DD54-4A7E-11E2-B47C-0050C2490048); GET with type
 * x-bt/img-img (the image), x-bt/img-thm (its thumbnail: JPEG, 200 x 200)
 * or x-bt/img-properties (an XML description), and the image handle in an
 * Img-Handle header (0x30, text).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "coverart.h"
#include "l2cap.h"
#include "lbt_platform.h"

#define OP_CONNECT     0x80
#define OP_DISCONNECT  0x81
#define OP_GET         0x03
#define OP_FINAL       0x80
#define OP_ABORT       0xff

#define RSP_CONTINUE        0x90
#define RSP_SUCCESS         0xa0
#define RSP_BAD_REQUEST     0xc0
#define RSP_NOT_FOUND       0xc4
#define RSP_NOT_ACCEPTABLE  0xc6
#define RSP_NOT_IMPLEMENTED 0xd1
#define RSP_UNAVAILABLE     0xd3

#define HI_TYPE        0x42
#define HI_TARGET      0x46
#define HI_BODY        0x48
#define HI_END_OF_BODY 0x49
#define HI_WHO         0x4a
#define HI_CONN_ID     0xcb
#define HI_IMG_HANDLE  0x30

#define OBEX_VERSION   0x10
#define OBEX_MAX       1691   /* the largest packet we take */
#define OBEX_MIN       255    /* the smallest anyone may say */
#define CONN_ID        1

static const uint8_t cover_art_uuid[16] = {
    0x71, 0x63, 0xdd, 0x54, 0x4a, 0x7e, 0x11, 0xe2,
    0xb4, 0x7c, 0x00, 0x50, 0xc2, 0x49, 0x00, 0x48,
};

static const coverart_ops *ops;

static struct {
    l2cap_chan *chan;
    bool connected;            /* OBEX CONNECT done */
    uint16_t their_max;        /* the largest packet they take */
    /* the GET in progress */
    bool getting;
    char type[24];
    char handle[COVERART_HANDLE];
    const uint8_t *data;
    uint32_t len, sent;
    char props[256];
    /* an answer waiting for room */
    uint8_t out[OBEX_MAX];
    uint16_t out_len;
} s;

/* ---- sending ------------------------------------------------------------------ */

static uint16_t most(void)
{
    uint16_t m = s.their_max ? s.their_max : OBEX_MIN;
    uint16_t l = s.chan ? l2cap_max_send(s.chan) : 0;
    if (l < m)
        m = l;
    return m > OBEX_MAX ? OBEX_MAX : m;
}

static void send_out(void)
{
    if (s.out_len && s.chan && l2cap_can_send(s.chan, s.out_len) &&
        l2cap_send(s.chan, s.out, s.out_len))
        s.out_len = 0;
}

/* Answer: response code, then 'n' bytes of fields and headers already at s.out + 3 */
static void answer(uint8_t code, uint16_t n)
{
    s.out[0] = code;
    lbt_put16be(s.out + 1, (uint16_t)(3 + n));
    s.out_len = (uint16_t)(3 + n);
    send_out();
}

/* The next part of what's being got: as much as fits in one packet */
static void next_part(void)
{
    uint16_t room = most() > 6 ? (uint16_t)(most() - 6) : 0;   /* opcode, length, body header */
    uint32_t left = s.len - s.sent;
    bool last = left <= room;
    uint16_t n = (uint16_t)(last ? left : room);
    s.out[3] = last ? HI_END_OF_BODY : HI_BODY;
    lbt_put16be(s.out + 4, (uint16_t)(3 + n));
    memcpy(s.out + 6, s.data + s.sent, n);
    s.sent += n;
    if (last)
        s.getting = false;
    answer(last ? RSP_SUCCESS : RSP_CONTINUE, (uint16_t)(3 + n));
}

/* ---- their requests ------------------------------------------------------------- */

/* Walk the headers from 'at': type, handle, target */
static bool headers(const uint8_t *p, uint16_t len, uint16_t at, bool *target_ok)
{
    while (at < len) {
        uint8_t hi = p[at];
        uint16_t hlen;
        switch (hi >> 6) {
        case 0:
        case 1:
            if (at + 3 > len)
                return false;
            hlen = lbt_get16be(p + at + 1);
            break;
        case 2:
            hlen = 2;
            break;
        default:
            hlen = 5;
            break;
        }
        if (hlen < 2 || at + hlen > len)
            return false;
        const uint8_t *v = p + at + 3;
        uint16_t vlen = hlen >= 3 ? (uint16_t)(hlen - 3) : 0;
        if (hi == HI_TYPE) {
            uint16_t n = vlen < sizeof(s.type) - 1 ? vlen : (uint16_t)(sizeof(s.type) - 1);
            memcpy(s.type, v, n);
            s.type[n] = '\0';
        } else if (hi == HI_IMG_HANDLE) {
            /* UTF-16, big endian, ending in a zero: digits */
            int k = 0;
            for (uint16_t i = 0; i + 1 < vlen && k < COVERART_HANDLE - 1; i += 2)
                if (v[i + 1])
                    s.handle[k++] = (char)v[i + 1];
            s.handle[k] = '\0';
        } else if (hi == HI_TARGET && target_ok) {
            *target_ok = vlen == 16 && !memcmp(v, cover_art_uuid, 16);
        }
        at = (uint16_t)(at + hlen);
    }
    return true;
}

static void connect_request(const uint8_t *p, uint16_t len)
{
    bool target_ok = false;
    if (len < 7 || !headers(p, len, 7, &target_ok)) {
        answer(RSP_BAD_REQUEST, 0);
        return;
    }
    if (!target_ok) {
        answer(RSP_UNAVAILABLE, 0);            /* not a service we have */
        return;
    }
    uint16_t max = lbt_get16be(p + 5);
    s.their_max = max < OBEX_MIN ? OBEX_MIN : max;
    s.connected = true;
    s.getting = false;
    uint8_t *o = s.out + 3;
    o[0] = OBEX_VERSION;
    o[1] = 0;
    lbt_put16be(o + 2, OBEX_MAX);
    o[4] = HI_CONN_ID;
    lbt_put32be(o + 5, CONN_ID);
    o[9] = HI_WHO;
    lbt_put16be(o + 10, 3 + 16);
    memcpy(o + 12, cover_art_uuid, 16);
    answer(RSP_SUCCESS, 28);
    lbt_platform_log("coverart: a car connected for covers");
    if (ops && ops->connected)
        ops->connected(true);
}

/* Set up a GET: the image or thumbnail (both our JPEG), or its properties */
static bool start_get(void)
{
    const uint8_t *data;
    uint32_t len;
    bool image = !strcmp(s.type, "x-bt/img-img") || !strcmp(s.type, "x-bt/img-thm");
    bool props = !strcmp(s.type, "x-bt/img-properties");
    if (!image && !props) {
        answer(RSP_NOT_IMPLEMENTED, 0);
        return false;
    }
    if (!s.handle[0] || !ops || !ops->image || !ops->image(s.handle, &data, &len) || !len) {
        answer(RSP_NOT_FOUND, 0);
        return false;
    }
    if (image) {
        s.data = data;
        s.len = len;
    } else {
        int n = snprintf(s.props, sizeof(s.props),
                         "<image-properties version=\"1.0\" handle=\"%s\">\n"
                         "<native encoding=\"JPEG\" pixel=\"200*200\" size=\"%u\"/>\n"
                         "</image-properties>\n",
                         s.handle, (unsigned)len);
        s.data = (const uint8_t *)s.props;
        s.len = (uint32_t)n;
    }
    s.sent = 0;
    s.getting = true;
    return true;
}

static void get_request(const uint8_t *p, uint16_t len, bool final)
{
    if (!s.getting) {
        /* a new GET: its headers (in one packet or several) */
        if (!headers(p, len, 3, NULL)) {
            answer(RSP_BAD_REQUEST, 0);
            return;
        }
        if (!final) {
            answer(RSP_CONTINUE, 0);           /* more of the request to come */
            return;
        }
        lbt_platform_log("coverart: get %s %s", s.type, s.handle);
        if (!start_get())
            return;
    }
    next_part();
}

static void request(const uint8_t *p, uint16_t len)
{
    if (len < 3 || lbt_get16be(p + 1) != len)
        return;
    uint8_t op = p[0];
    if (op == OP_CONNECT) {
        connect_request(p, len);
    } else if (op == OP_DISCONNECT) {
        s.connected = s.getting = false;
        answer(RSP_SUCCESS, 0);
    } else if (op == OP_ABORT) {
        s.getting = false;
        answer(RSP_SUCCESS, 0);
    } else if ((op & ~OP_FINAL) == OP_GET && s.connected) {
        if (!s.getting) {
            s.type[0] = '\0';
            s.handle[0] = '\0';
        }
        get_request(p, len, op & OP_FINAL);
    } else {
        answer(RSP_NOT_IMPLEMENTED, 0);
    }
}

/* ---- L2CAP ---------------------------------------------------------------------- */

static void chan_opened(l2cap_chan *c, uint16_t result)
{
    if (result)
        return;
    if (s.chan && c != s.chan) {
        l2cap_disconnect(c);                   /* one car at a time */
        return;
    }
    memset(&s, 0, sizeof(s));
    s.chan = c;
}

static void chan_data(l2cap_chan *c, const uint8_t *d, uint16_t len)
{
    if (c == s.chan)
        request(d, len);
}

static void chan_closed(l2cap_chan *c)
{
    if (c != s.chan)
        return;
    bool was = s.connected;
    memset(&s, 0, sizeof(s));
    if (was && ops && ops->connected)
        ops->connected(false);
}

static const l2cap_ops coverart_l2cap_ops = { chan_opened, chan_data, chan_closed };

/* ---- API ------------------------------------------------------------------------ */

void coverart_init(const coverart_ops *o)
{
    ops = o;
    memset(&s, 0, sizeof(s));
    l2cap_listen_ertm(COVERART_PSM, &coverart_l2cap_ops, OBEX_MAX);
}

bool coverart_connected(void)
{
    return s.chan && s.connected;
}

void coverart_tick(void)
{
    send_out();
}
