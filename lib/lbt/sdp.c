/*
 * lbt SDP server (Bluetooth Core, Vol 3 Part B, sections 3 and 4).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "sdp.h"
#include "coverart.h"
#include "l2cap.h"
#include "lbt_platform.h"

/* PDU ids */
#define SDP_ERROR_RSP                 0x01
#define SDP_SERVICE_SEARCH_REQ        0x02
#define SDP_SERVICE_SEARCH_RSP        0x03
#define SDP_SERVICE_ATTR_REQ          0x04
#define SDP_SERVICE_ATTR_RSP          0x05
#define SDP_SERVICE_SEARCH_ATTR_REQ   0x06
#define SDP_SERVICE_SEARCH_ATTR_RSP   0x07

/* error codes */
#define SDP_ERR_INVALID_HANDLE        0x0002
#define SDP_ERR_INVALID_SYNTAX        0x0003
#define SDP_ERR_INVALID_CONTINUATION  0x0005

/* UUIDs (16-bit) */
#define UUID_L2CAP              0x0100
#define UUID_OBEX               0x0008
#define UUID_AVCTP              0x0017
#define UUID_AVDTP              0x0019
#define UUID_PUBLIC_BROWSE      0x1002
#define UUID_AUDIO_SOURCE       0x110a
#define UUID_AV_REMOTE_TARGET   0x110c
#define UUID_AV_REMOTE          0x110e
#define UUID_AV_REMOTE_CTRL     0x110f
#define UUID_ADVANCED_AUDIO     0x110d

/* ---- records ---------------------------------------------------------------------- */

#define MAX_ATTRS   8
#define MAX_VALUE   40

struct sdp_attr {
    uint16_t id;
    uint8_t len;
    uint8_t value[MAX_VALUE];   /* one encoded data element */
};

struct sdp_record {
    uint32_t handle;
    int nattrs;
    struct sdp_attr attrs[MAX_ATTRS];   /* ascending ids */
};

static struct sdp_record records[3];

/* data element builder */
typedef struct { uint8_t *p; int n; int max; } builder;

static void put(builder *b, uint8_t v)
{
    if (b->n < b->max)
        b->p[b->n] = v;
    b->n++;
}
static void de_u16(builder *b, uint16_t v) { put(b, 0x09); put(b, v >> 8); put(b, (uint8_t)v); }
static void de_u32(builder *b, uint32_t v)
{
    put(b, 0x0a); put(b, v >> 24); put(b, v >> 16); put(b, v >> 8); put(b, (uint8_t)v);
}
static void de_uuid(builder *b, uint16_t v) { put(b, 0x19); put(b, v >> 8); put(b, (uint8_t)v); }
static void de_text(builder *b, const char *s)
{
    size_t n = strlen(s);
    put(b, 0x25);
    put(b, (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        put(b, (uint8_t)s[i]);
}
static int seq_begin(builder *b) { put(b, 0x35); put(b, 0); return b->n; }
static void seq_end(builder *b, int start)
{
    if (start - 1 < b->max)
        b->p[start - 1] = (uint8_t)(b->n - start);
}

static struct sdp_attr *attr_begin(struct sdp_record *r, uint16_t id, builder *b)
{
    struct sdp_attr *a = &r->attrs[r->nattrs++];
    a->id = id;
    b->p = a->value;
    b->n = 0;
    b->max = MAX_VALUE;
    return a;
}

static void attr_end(struct sdp_attr *a, builder *b)
{
    a->len = (uint8_t)b->n;
}

/* A record with the usual attributes: handle, class list, protocols over
 * L2CAP, public browse group, profile, name and supported features */
static void make_record(struct sdp_record *r, uint32_t handle,
                        const uint16_t *classes, int nclasses,
                        uint16_t psm, uint16_t proto, uint16_t proto_version,
                        uint16_t profile, uint16_t profile_version,
                        const char *name, uint16_t features)
{
    builder b;
    struct sdp_attr *a;
    int s, s2;

    memset(r, 0, sizeof(*r));
    r->handle = handle;

    a = attr_begin(r, 0x0000, &b);              /* service record handle */
    de_u32(&b, handle);
    attr_end(a, &b);

    a = attr_begin(r, 0x0001, &b);              /* service class id list */
    s = seq_begin(&b);
    for (int i = 0; i < nclasses; i++)
        de_uuid(&b, classes[i]);
    seq_end(&b, s);
    attr_end(a, &b);

    a = attr_begin(r, 0x0004, &b);              /* protocol descriptor list */
    s = seq_begin(&b);
    s2 = seq_begin(&b); de_uuid(&b, UUID_L2CAP); de_u16(&b, psm); seq_end(&b, s2);
    s2 = seq_begin(&b); de_uuid(&b, proto); de_u16(&b, proto_version); seq_end(&b, s2);
    seq_end(&b, s);
    attr_end(a, &b);

    a = attr_begin(r, 0x0005, &b);              /* browse group list */
    s = seq_begin(&b); de_uuid(&b, UUID_PUBLIC_BROWSE); seq_end(&b, s);
    attr_end(a, &b);

    a = attr_begin(r, 0x0009, &b);              /* profile descriptor list */
    s = seq_begin(&b);
    s2 = seq_begin(&b); de_uuid(&b, profile); de_u16(&b, profile_version); seq_end(&b, s2);
    seq_end(&b, s);
    attr_end(a, &b);

    a = attr_begin(r, 0x0100, &b);              /* service name */
    de_text(&b, name);
    attr_end(a, &b);

    a = attr_begin(r, 0x0311, &b);              /* supported features */
    de_u16(&b, features);
    attr_end(a, &b);
}

static void build_records(void)
{
    static const uint16_t source[] = { UUID_AUDIO_SOURCE };
    static const uint16_t target[] = { UUID_AV_REMOTE_TARGET };
    static const uint16_t ctrl[] = { UUID_AV_REMOTE, UUID_AV_REMOTE_CTRL };

    /* A2DP 1.3 source over AVDTP 1.3; features: player */
    make_record(&records[0], SDP_HANDLE_A2DP_SOURCE, source, 1,
                0x0019, UUID_AVDTP, 0x0103, UUID_ADVANCED_AUDIO, 0x0103,
                "Audio Source", 0x0001);
    /* AVRCP 1.6 over AVCTP 1.4; features: category 1 (player), cover art
     * (bit 8), whose OBEX channel goes in the additional protocols
     * (AVRCP 1.6 section 8, table 8.2) */
    make_record(&records[1], SDP_HANDLE_AVRCP_TARGET, target, 1,
                0x0017, UUID_AVCTP, 0x0104, UUID_AV_REMOTE, 0x0106,
                "AVRCP Target", 0x0101);
    {
        struct sdp_record *r = &records[1];
        int at = 0;
        while (at < r->nattrs && r->attrs[at].id < 0x000d)
            at++;
        memmove(&r->attrs[at + 1], &r->attrs[at], (size_t)(r->nattrs - at) * sizeof(r->attrs[0]));
        r->nattrs++;
        struct sdp_attr *a = &r->attrs[at];
        builder b = { a->value, 0, MAX_VALUE };
        a->id = 0x000d;                             /* additional protocol descriptor lists */
        int s0 = seq_begin(&b);
        int s1 = seq_begin(&b);
        int s2 = seq_begin(&b); de_uuid(&b, UUID_L2CAP); de_u16(&b, COVERART_PSM); seq_end(&b, s2);
        s2 = seq_begin(&b); de_uuid(&b, UUID_OBEX); seq_end(&b, s2);
        seq_end(&b, s1);
        seq_end(&b, s0);
        a->len = (uint8_t)b.n;
    }
    make_record(&records[2], SDP_HANDLE_AVRCP_CTRL, ctrl, 2,
                0x0017, UUID_AVCTP, 0x0104, UUID_AV_REMOTE, 0x0106,
                "AVRCP Controller", 0x0001);
}

/* ---- data element parsing ------------------------------------------------------------- */

/* Header of the data element at p: returns header length (0 if bad) and
 * sets type and data length */
static int de_header(const uint8_t *p, int len, uint8_t *type, uint32_t *dlen)
{
    if (len < 1)
        return 0;
    *type = p[0] >> 3;
    uint8_t size = p[0] & 7;
    static const uint8_t fixed[5] = { 1, 2, 4, 8, 16 };
    if (*type == 0) {          /* nil */
        *dlen = 0;
        return 1;
    }
    if (size <= 4) {
        *dlen = fixed[size];
        return 1;
    }
    int extra = size == 5 ? 1 : size == 6 ? 2 : 4;
    if (len < 1 + extra)
        return 0;
    *dlen = extra == 1 ? p[1] : extra == 2 ? lbt_get16be(p + 1) : lbt_get32be(p + 1);
    return 1 + extra;
}

/* 16-bit form of a UUID element; 0 if it isn't one we could have */
static uint32_t uuid_short(const uint8_t *data, uint32_t len)
{
    static const uint8_t base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                      0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
    if (len == 2)
        return lbt_get16be(data);
    if (len == 4)
        return lbt_get32be(data);
    if (len == 16 && memcmp(data + 4, base, 12) == 0)
        return lbt_get32be(data);
    return 0xffffffff;   /* a 128-bit UUID that isn't one of ours */
}

/* Does the encoded value contain this UUID anywhere? */
static bool value_has_uuid(const uint8_t *p, int len, uint32_t uuid)
{
    while (len > 0) {
        uint8_t type;
        uint32_t dlen;
        int h = de_header(p, len, &type, &dlen);
        if (!h || h + (int)dlen > len)
            return false;
        if (type == 3 && uuid_short(p + h, dlen) == uuid)
            return true;
        if ((type == 6 || type == 7) && value_has_uuid(p + h, (int)dlen, uuid))
            return true;
        p += h + dlen;
        len -= h + (int)dlen;
    }
    return false;
}

static bool record_has_uuid(const struct sdp_record *r, uint32_t uuid)
{
    for (int i = 0; i < r->nattrs; i++)
        if (value_has_uuid(r->attrs[i].value, r->attrs[i].len, uuid))
            return true;
    return false;
}

/* Read a service search pattern (a sequence of UUIDs) */
static int read_pattern(const uint8_t *p, int len, uint32_t *uuids, int *n)
{
    uint8_t type;
    uint32_t dlen;
    int h = de_header(p, len, &type, &dlen);
    if (!h || type != 6 || h + (int)dlen > len)
        return -1;
    *n = 0;
    for (int off = h; off < h + (int)dlen; ) {
        uint8_t t;
        uint32_t l;
        int hh = de_header(p + off, h + (int)dlen - off, &t, &l);
        if (!hh || t != 3 || *n >= 12)
            return -1;
        uuids[(*n)++] = uuid_short(p + off + hh, l);
        off += hh + (int)l;
    }
    return h + (int)dlen;
}

/* Read an attribute id list (ids and id ranges) into [lo, hi] pairs */
static int read_attr_ids(const uint8_t *p, int len, uint16_t ranges[][2], int *n)
{
    uint8_t type;
    uint32_t dlen;
    int h = de_header(p, len, &type, &dlen);
    if (!h || type != 6 || h + (int)dlen > len)
        return -1;
    *n = 0;
    for (int off = h; off < h + (int)dlen; ) {
        uint8_t t;
        uint32_t l;
        int hh = de_header(p + off, h + (int)dlen - off, &t, &l);
        if (!hh || t != 1 || *n >= 16)
            return -1;
        if (l == 2) {
            ranges[*n][0] = ranges[*n][1] = lbt_get16be(p + off + hh);
        } else if (l == 4) {
            ranges[*n][0] = lbt_get16be(p + off + hh);
            ranges[*n][1] = lbt_get16be(p + off + hh + 2);
        } else {
            return -1;
        }
        (*n)++;
        off += hh + (int)l;
    }
    return h + (int)dlen;
}

/* ---- building answers ------------------------------------------------------------------ */

static bool wanted(uint16_t id, uint16_t ranges[][2], int n)
{
    for (int i = 0; i < n; i++)
        if (id >= ranges[i][0] && id <= ranges[i][1])
            return true;
    return false;
}

/* One record's attribute list as a sequence (2-byte length form) */
static void attr_list(builder *b, const struct sdp_record *r, uint16_t ranges[][2], int n)
{
    int start = b->n;
    put(b, 0x36); put(b, 0); put(b, 0);
    for (int i = 0; i < r->nattrs; i++) {
        const struct sdp_attr *a = &r->attrs[i];
        if (!wanted(a->id, ranges, n))
            continue;
        de_u16(b, a->id);
        for (int k = 0; k < a->len; k++)
            put(b, a->value[k]);
    }
    int len = b->n - start - 3;
    if (start + 2 < b->max) {
        b->p[start + 1] = (uint8_t)(len >> 8);
        b->p[start + 2] = (uint8_t)len;
    }
}

static bool matches(const struct sdp_record *r, const uint32_t *uuids, int n)
{
    for (int i = 0; i < n; i++)
        if (!record_has_uuid(r, uuids[i]))
            return false;
    return n > 0;
}

static uint16_t error_rsp(uint8_t *rsp, uint16_t tid, uint16_t code)
{
    rsp[0] = SDP_ERROR_RSP;
    lbt_put16be(rsp + 1, tid);
    lbt_put16be(rsp + 3, 2);
    lbt_put16be(rsp + 5, code);
    return 7;
}

/* Send part of a long answer, from the offset in the continuation state */
/* What they look for (a car finding what we offer), in the log */
static void log_search(const char *what, const uint32_t *uuids, int n)
{
    char text[80];
    int at = 0;
    for (int i = 0; i < n && at < 60; i++) {
        int found = 0;
        for (int r = 0; r < 3; r++)
            found += matches(&records[r], &uuids[i], 1);
        at += snprintf(text + at, sizeof(text) - at, "%s0x%04x%s", i ? ", " : "",
                       (unsigned)uuids[i], found ? "" : " (none)");
    }
    text[at < (int)sizeof(text) ? at : (int)sizeof(text) - 1] = 0;
    lbt_platform_log("sdp: they look for %s: %s", what, n ? text : "nothing");
}

static uint16_t sliced_rsp(uint8_t *rsp, uint8_t pdu, uint16_t tid,
                           const uint8_t *all, int total, uint16_t max_count,
                           const uint8_t *cont, int cont_len, uint16_t max_out)
{
    int offset = 0;
    if (cont_len == 2)
        offset = lbt_get16be(cont);
    else if (cont_len != 0)
        return error_rsp(rsp, tid, SDP_ERR_INVALID_CONTINUATION);
    if (offset > total)
        return error_rsp(rsp, tid, SDP_ERR_INVALID_CONTINUATION);

    int room = max_out - (5 + 2 + 3);
    if (room > max_count)
        room = max_count;
    if (room < 1)
        room = 1;
    int n = total - offset < room ? total - offset : room;

    rsp[0] = pdu;
    lbt_put16be(rsp + 1, tid);
    lbt_put16be(rsp + 5, (uint16_t)n);
    memcpy(rsp + 7, all + offset, n);
    int pos = 7 + n;
    if (offset + n < total) {
        rsp[pos++] = 2;
        lbt_put16be(rsp + pos, (uint16_t)(offset + n));
        pos += 2;
    } else {
        rsp[pos++] = 0;
    }
    lbt_put16be(rsp + 3, (uint16_t)(pos - 5));
    return (uint16_t)pos;
}

uint16_t sdp_handle(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t max_out)
{
    static uint8_t all[1024];
    if (len < 5)
        return 0;
    uint8_t pdu = req[0];
    uint16_t tid = lbt_get16be(req + 1);
    uint16_t plen = lbt_get16be(req + 3);
    const uint8_t *p = req + 5;
    if (plen + 5 > len)
        return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);

    uint32_t uuids[12];
    int nuuids = 0, nranges = 0, used;
    uint16_t ranges[16][2];
    builder b = { all, 0, sizeof(all) };

    switch (pdu) {
    case SDP_SERVICE_SEARCH_REQ: {
        used = read_pattern(p, plen, uuids, &nuuids);
        if (used < 0 || used + 3 > plen)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
        uint16_t max_records = lbt_get16be(p + used);
        int count = 0;
        log_search("services", uuids, nuuids);
        rsp[0] = SDP_SERVICE_SEARCH_RSP;
        lbt_put16be(rsp + 1, tid);
        for (int i = 0; i < 3 && count < max_records; i++)
            if (matches(&records[i], uuids, nuuids)) {
                lbt_put32be(rsp + 9 + 4 * count, records[i].handle);
                count++;
            }
        lbt_put16be(rsp + 5, (uint16_t)count);   /* total */
        lbt_put16be(rsp + 7, (uint16_t)count);   /* in this response */
        rsp[9 + 4 * count] = 0;                  /* no continuation */
        lbt_put16be(rsp + 3, (uint16_t)(5 + 4 * count));
        return (uint16_t)(10 + 4 * count);
    }

    case SDP_SERVICE_ATTR_REQ: {
        if (plen < 7)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
        uint32_t handle = lbt_get32be(p);
        uint16_t max_count = lbt_get16be(p + 4);
        used = read_attr_ids(p + 6, plen - 6, ranges, &nranges);
        if (used < 0 || 6 + used >= plen)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
        const struct sdp_record *r = NULL;
        for (int i = 0; i < 3; i++)
            if (records[i].handle == handle)
                r = &records[i];
        if (!r)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_HANDLE);
        attr_list(&b, r, ranges, nranges);
        const uint8_t *cont = p + 6 + used;
        return sliced_rsp(rsp, SDP_SERVICE_ATTR_RSP, tid, all, b.n, max_count,
                          cont + 1, cont[0], max_out);
    }

    case SDP_SERVICE_SEARCH_ATTR_REQ: {
        used = read_pattern(p, plen, uuids, &nuuids);
        if (used < 0 || used + 2 > plen)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
        uint16_t max_count = lbt_get16be(p + used);
        int used2 = read_attr_ids(p + used + 2, plen - used - 2, ranges, &nranges);
        if (used2 < 0 || used + 2 + used2 >= plen)
            return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
        log_search("services and attributes", uuids, nuuids);
        /* a sequence of attribute lists, one per matching record */
        put(&b, 0x36); put(&b, 0); put(&b, 0);
        for (int i = 0; i < 3; i++)
            if (matches(&records[i], uuids, nuuids))
                attr_list(&b, &records[i], ranges, nranges);
        all[1] = (uint8_t)((b.n - 3) >> 8);
        all[2] = (uint8_t)(b.n - 3);
        const uint8_t *cont = p + used + 2 + used2;
        return sliced_rsp(rsp, SDP_SERVICE_SEARCH_ATTR_RSP, tid, all, b.n, max_count,
                          cont + 1, cont[0], max_out);
    }

    default:
        return error_rsp(rsp, tid, SDP_ERR_INVALID_SYNTAX);
    }
}

/* ---- the server ---------------------------------------------------------------------- */

static void sdp_opened(l2cap_chan *c, uint16_t result) { (void)c; (void)result; }
static void sdp_closed(l2cap_chan *c) { (void)c; }

static void sdp_data(l2cap_chan *c, const uint8_t *data, uint16_t len)
{
    static uint8_t rsp[L2CAP_TX_MAX];
    uint16_t max = l2cap_max_send(c);
    if (max > sizeof(rsp))
        max = sizeof(rsp);
    uint16_t n = sdp_handle(data, len, rsp, max);
    if (n)
        l2cap_send(c, rsp, n);
}

static const l2cap_ops sdp_ops = { sdp_opened, sdp_data, sdp_closed };

void sdp_init(void)
{
    build_records();
    l2cap_listen(L2CAP_PSM_SDP, &sdp_ops, 672);
}
