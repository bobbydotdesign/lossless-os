/*
 * lbt host tests: simulated controller and headphones (see sim.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include "sim.h"
#include "../lbt_platform.h"
#include "../hci.h"
#include "../source.h"

int test_failures;
const char *sim_only;
uint32_t sim_now;
uint32_t sim_poll_ms = 10;
struct sim_headphones hp;
int sim_flow_errors;
int sim_pages;
bool sim_page_scan;

static const uint8_t player_addr[6] = { 0x02, 0x00, 0x00, 0xcc, 0xbb, 0xaa };

#define ACL_BUFFERS   8
#define ACL_LEN       1021
#define PAGE_MS       15360   /* the page timeout the stack sets (0x6000) */
#define SUPERVISION_MS 5000
#define PREBUFFER_MS  150

/* ---- the UART towards the stack: packets in order, each with a time ---------------- */

struct pending {
    uint32_t due;
    uint16_t len;
    uint8_t data[1 + 4 + 1100];
};
#define QUEUE 512
static struct pending queue[QUEUE];
static int q_head, q_tail;
static uint32_t q_last_due;

static void to_host(uint32_t delay, uint8_t type, const uint8_t *pkt, uint16_t len)
{
    int next = (q_tail + 1) % QUEUE;
    if (next == q_head) {
        printf("  FAIL: sim queue full\n");
        test_failures++;
        return;
    }
    struct pending *p = &queue[q_tail];
    uint32_t due = sim_now + delay;
    if ((int32_t)(due - q_last_due) < 0)
        due = q_last_due;   /* one wire: keep the order */
    q_last_due = due;
    p->due = due;
    p->data[0] = type;
    memcpy(p->data + 1, pkt, len);
    p->len = (uint16_t)(len + 1);
    q_tail = next;
}

static void event(uint32_t delay, uint8_t code, const uint8_t *params, uint8_t len)
{
    uint8_t b[2 + 255];
    b[0] = code;
    b[1] = len;
    memcpy(b + 2, params, len);
    to_host(delay, LBT_H4_EVENT, b, (uint16_t)(2 + len));
}

static void command_complete(uint16_t op, const uint8_t *ret, uint8_t len)
{
    uint8_t p[3 + 255];
    p[0] = 1;
    lbt_put16(p + 1, op);
    memcpy(p + 3, ret, len);
    event(1, HCI_EV_COMMAND_COMPLETE, p, (uint8_t)(3 + len));
}

static void command_ok(uint16_t op)
{
    command_complete(op, (const uint8_t[]){ 0 }, 1);
}

static void command_status(uint16_t op, uint8_t status)
{
    uint8_t p[4] = { status, 1, 0, 0 };
    lbt_put16(p + 2, op);
    event(1, HCI_EV_COMMAND_STATUS, p, 4);
}

/* ---- timers (things that happen later) -------------------------------------------------- */

enum timer_kind { T_NONE, T_CONNECT_DONE, T_PAGE_TRY, T_PAGE_FAILED, T_INQUIRY_RESULT, T_INQUIRY_DONE,
                  T_LINK_LOST, T_RELEASE, T_PAGE_PLAYER, T_OPEN_AVDTP };
struct timer { enum timer_kind kind; uint32_t at; uint32_t arg; };
#define TIMERS 16
static struct timer timers[TIMERS];

static void after_ms(uint32_t ms, enum timer_kind k, uint32_t arg)
{
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].kind == T_NONE) {
            timers[i] = (struct timer){ k, sim_now + ms, arg };
            return;
        }
    printf("  FAIL: sim out of timers\n");
    test_failures++;
}

static bool cancel(enum timer_kind k)   /* true if one was set */
{
    bool any = false;
    for (int i = 0; i < TIMERS; i++)
        if (timers[i].kind == k) {
            timers[i].kind = T_NONE;
            any = true;
        }
    return any;
}

static lbt_addr paged;              /* the address the player is paging */

/* ---- the link ----------------------------------------------------------------------------- */

static uint16_t link_handle;        /* 0: no link */
static bool we_paged;               /* the headphones made this link */
static uint16_t next_handle = 0x000c;
static int acl_free = ACL_BUFFERS;
/* sent ACL packets on the air, freed in order */
static uint32_t air_done[64];
static int air_n;
static uint32_t air_busy_until;

static void headphones_reset_link(void);
static void l2cap_in(const uint8_t *frame, uint16_t len);

static void link_down(uint8_t reason, uint32_t delay)
{
    if (!link_handle)
        return;
    uint8_t p[4] = { 0, 0, 0, reason };
    lbt_put16(p + 1, link_handle);
    event(delay, HCI_EV_DISCONNECTION_COMPLETE, p, 4);
    link_handle = 0;
    air_n = 0;
    acl_free = ACL_BUFFERS;
    cancel(T_LINK_LOST);
    headphones_reset_link();
}

static void link_up(uint32_t delay)
{
    uint8_t p[11] = { 0 };
    link_handle = next_handle++;
    lbt_put16(p + 1, link_handle);
    memcpy(p + 3, hp.addr.b, 6);
    p[9] = 0x01;
    event(delay, HCI_EV_CONNECTION_COMPLETE, p, 11);
    hp.connected = true;
    hp.connections++;
    we_paged = false;
}

/* ---- headphones: L2CAP --------------------------------------------------------------------- */

struct hchan {
    bool used;
    uint16_t psm, mine, theirs;     /* mine: the headphones' CID */
    bool their_conf, my_conf, open;
    bool media;
    bool ours;                      /* the headphones opened it */
};
static struct hchan ch[8];
static uint16_t next_cid;
static uint8_t sig_id = 0x80;
static uint8_t rx_buf[2048];
static uint16_t rx_have, rx_want;

static void send_frame(uint16_t cid, const uint8_t *d, uint16_t len)
{
    if (!link_handle || !hp.in_range)
        return;
    uint8_t b[4 + 4 + 1100];
    lbt_put16(b, (link_handle & 0x0fff) | 0x2000);
    lbt_put16(b + 2, (uint16_t)(4 + len));
    lbt_put16(b + 4, len);
    lbt_put16(b + 6, cid);
    memcpy(b + 8, d, len);
    to_host(3, LBT_H4_ACL, b, (uint16_t)(8 + len));
}

static void l2sig(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t b[4 + 64];
    b[0] = code;
    b[1] = id;
    lbt_put16(b + 2, len);
    memcpy(b + 4, d, len);
    send_frame(0x0001, b, (uint16_t)(4 + len));
}

static struct hchan *chan_mine(uint16_t cid)
{
    for (int i = 0; i < 8; i++)
        if (ch[i].used && ch[i].mine == cid)
            return &ch[i];
    return NULL;
}

static struct hchan *chan_psm(uint16_t psm, bool media)
{
    for (int i = 0; i < 8; i++)
        if (ch[i].used && ch[i].open && ch[i].psm == psm && ch[i].media == media)
            return &ch[i];
    return NULL;
}

static struct hchan *chan_new(uint16_t psm)
{
    for (int i = 0; i < 8; i++)
        if (!ch[i].used) {
            memset(&ch[i], 0, sizeof(ch[i]));
            ch[i].used = true;
            ch[i].psm = psm;
            ch[i].mine = next_cid++;
            if (psm == 0x0019) {
                /* the first AVDTP channel signals, the next carries media */
                ch[i].media = chan_psm(0x0019, false) != NULL;
            }
            return &ch[i];
        }
    return NULL;
}

static void send_config(struct hchan *c)
{
    uint8_t d[8] = { 0, 0, 0, 0, 0x01, 0x02, 0, 0 };
    lbt_put16(d, c->theirs);
    lbt_put16(d + 6, c->media ? 1023 : 672);
    l2sig(0x04, sig_id++, d, 8);
}

static void avdtp_opened(struct hchan *c);
static void avrcp_opened(struct hchan *c);

static void maybe_open(struct hchan *c)
{
    if (c->open || !c->their_conf || !c->my_conf)
        return;
    c->open = true;
    if (c->psm == 0x0019)
        avdtp_opened(c);
    else if (c->psm == 0x0017)
        avrcp_opened(c);
}

/* the headphones open a channel to the player */
static struct hchan *open_channel(uint16_t psm)
{
    struct hchan *c = chan_new(psm);
    if (!c)
        return NULL;
    c->ours = true;
    uint8_t d[4];
    lbt_put16(d, psm);
    lbt_put16(d + 2, c->mine);
    l2sig(0x02, sig_id++, d, 4);
    return c;
}

static void avdtp_in(struct hchan *c, const uint8_t *d, uint16_t len);
static void avrcp_in(const uint8_t *d, uint16_t len);
static void media_in(const uint8_t *d, uint16_t len);

static void signaling_in(const uint8_t *d, uint16_t len)
{
    while (len >= 4) {
        uint8_t code = d[0], id = d[1];
        uint16_t clen = lbt_get16(d + 2);
        const uint8_t *p = d + 4;
        if (4 + clen > len)
            break;
        switch (code) {
        case 0x02: {   /* connection request: psm, their cid */
            struct hchan *c = chan_new(lbt_get16(p));
            uint8_t r[8] = { 0 };
            if (!c) {
                lbt_put16(r + 2, lbt_get16(p + 2));
                lbt_put16(r + 4, 0x0004);
                l2sig(0x03, id, r, 8);
                break;
            }
            c->theirs = lbt_get16(p + 2);
            lbt_put16(r, c->mine);
            lbt_put16(r + 2, c->theirs);
            l2sig(0x03, id, r, 8);
            if (c->psm == 0x0019 && !c->media)
                hp.sig_channels++;
            send_config(c);
            break;
        }
        case 0x03: {   /* connection response to ours: their cid, our cid, result */
            struct hchan *c = chan_mine(lbt_get16(p + 2));
            if (!c)
                break;
            if (lbt_get16(p + 4) != 0) {
                if (lbt_get16(p + 4) != 0x0001)   /* not "pending" */
                    c->used = false;
                break;
            }
            c->theirs = lbt_get16(p);
            if (c->psm == 0x0019 && !c->media)
                hp.sig_channels++;
            send_config(c);
            break;
        }
        case 0x04: {   /* configuration request: our cid */
            struct hchan *c = chan_mine(lbt_get16(p));
            uint8_t r[6] = { 0 };
            if (c)
                lbt_put16(r, c->theirs);
            l2sig(0x05, id, r, 6);
            if (c) {
                c->their_conf = true;
                maybe_open(c);
            }
            break;
        }
        case 0x05: {   /* configuration response: our cid */
            struct hchan *c = chan_mine(lbt_get16(p));
            if (c && lbt_get16(p + 4) == 0) {
                c->my_conf = true;
                maybe_open(c);
            }
            break;
        }
        case 0x06: {   /* disconnection request: our cid, their cid */
            struct hchan *c = chan_mine(lbt_get16(p));
            l2sig(0x07, id, p, 4);
            if (c) {
                if (c->media)
                    hp.open = hp.streaming = false;
                else if (c->psm == 0x0019)
                    hp.configured = hp.open = hp.streaming = false;
                c->used = false;
            }
            break;
        }
        case 0x07: {   /* disconnection response */
            struct hchan *c = chan_mine(lbt_get16(p + 2));
            if (c)
                c->used = false;
            break;
        }
        case 0x0a: {   /* information request: not supported */
            uint8_t r[4] = { p[0], p[1], 0x01, 0x00 };
            l2sig(0x0b, id, r, 4);
            break;
        }
        default:
            break;
        }
        d += 4 + clen;
        len = (uint16_t)(len - 4 - clen);
    }
}

static void l2cap_in(const uint8_t *frame, uint16_t len)
{
    uint16_t cid = lbt_get16(frame + 2);
    const uint8_t *d = frame + 4;
    uint16_t dl = (uint16_t)(len - 4);
    if (cid == 0x0001) {
        signaling_in(d, dl);
        return;
    }
    struct hchan *c = chan_mine(cid);
    if (!c || !c->open)
        return;
    if (c->psm == 0x0019 && c->media)
        media_in(d, dl);
    else if (c->psm == 0x0019)
        avdtp_in(c, d, dl);
    else if (c->psm == 0x0017)
        avrcp_in(d, dl);
}

/* ---- headphones: AVDTP sink ---------------------------------------------------------- */

static uint8_t avdtp_label;
static uint8_t player_seid;   /* the player's source endpoint (when we lead) */
static int lead_step;
static uint16_t last_seq;
static uint32_t last_ts;
static bool have_seq;

static void avdtp_send(const uint8_t *d, uint16_t len)
{
    struct hchan *c = chan_psm(0x0019, false);
    if (c)
        send_frame(c->theirs, d, len);
}

static void avdtp_reply(uint8_t label, uint8_t sig, uint8_t type, const uint8_t *p, uint16_t len)
{
    uint8_t b[64];
    b[0] = (uint8_t)((label << 4) | type);
    b[1] = sig;
    memcpy(b + 2, p, len);
    avdtp_send(b, (uint16_t)(2 + len));
}

static void avdtp_command(uint8_t sig, const uint8_t *p, uint16_t len)
{
    uint8_t b[64];
    b[0] = (uint8_t)(avdtp_label++ << 4);
    avdtp_label &= 0x0f;
    b[1] = sig;
    memcpy(b + 2, p, len);
    avdtp_send(b, (uint16_t)(2 + len));
}

static const uint8_t sbc_caps[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xff, 0x02, 0x35 };

static void lead_next(void)
{
    switch (lead_step) {
    case 1: avdtp_command(0x01, NULL, 0); break;                        /* DISCOVER */
    case 2: { uint8_t s = (uint8_t)(player_seid << 2); avdtp_command(0x02, &s, 1); break; }
    case 3: {                                                           /* SET_CONFIGURATION */
        uint8_t p[] = { (uint8_t)(player_seid << 2), 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
                        0x21, 0x15, 0x02, 0x35 };
        avdtp_command(0x03, p, sizeof(p));
        break;
    }
    case 4: { uint8_t s = (uint8_t)(player_seid << 2); avdtp_command(0x06, &s, 1); break; }
    case 5: open_channel(0x0019); break;                                /* media channel */
    default: break;
    }
}

static void avdtp_opened(struct hchan *c)
{
    if (c->media) {
        hp.open = true;
        return;
    }
    if (hp.lead_audio && c->ours && link_handle) {
        lead_step = 1;
        lead_next();
    }
}

static void avdtp_in(struct hchan *c, const uint8_t *d, uint16_t len)
{
    (void)c;
    if (len < 2)
        return;
    uint8_t label = d[0] >> 4, type = d[0] & 3, sig = d[1] & 0x3f;
    if (type == 2 || type == 3) {   /* an answer to us */
        if (type == 2 && lead_step) {
            if (sig == 0x01 && len >= 4)
                player_seid = d[2] >> 2;
            if (sig == 0x03) {
                hp.configured = true;
                hp.rate = 44100;
            }
            lead_step++;
            lead_next();
            if (lead_step >= 5)
                lead_step = 0;
        }
        if (type == 2 && sig == 0x09)
            hp.streaming = false;
        return;
    }
    if (type != 0)
        return;
    switch (sig) {
    case 0x01: {   /* DISCOVER: one SBC sink, seid 1 */
        uint8_t p[2] = { 0x01 << 2, 0x08 };
        avdtp_reply(label, sig, 2, p, 2);
        break;
    }
    case 0x02:     /* GET_CAPABILITIES */
        avdtp_reply(label, sig, 2, sbc_caps, sizeof(sbc_caps));
        break;
    case 0x0c: {   /* GET_ALL_CAPABILITIES: and delay reporting */
        uint8_t p[sizeof(sbc_caps) + 2];
        memcpy(p, sbc_caps, sizeof(sbc_caps));
        p[sizeof(sbc_caps)] = 0x08;
        p[sizeof(sbc_caps) + 1] = 0x00;
        avdtp_reply(label, sig, 2, p, sizeof(p));
        break;
    }
    case 0x03:     /* SET_CONFIGURATION */
        if (hp.busy_drops > 0) {
            /* what the AirPods did on the H2 while a phone had them */
            hp.busy_drops--;
            link_down(0x14, 20);   /* "low resources" */
            break;
        }
        if (hp.configured) {
            uint8_t r[2] = { 0x00, 0x13 };   /* in use */
            avdtp_reply(label, sig, 3, r, 2);
            break;
        }
        hp.configured = true;
        hp.rate = 44100;
        for (uint16_t i = 4; i + 1 < len; i += (uint16_t)(2 + d[i + 1]))
            if (d[i] == 0x07 && i + 4 < len)
                hp.rate = (d[i + 4] & 0x10) ? 48000 : 44100;
        avdtp_reply(label, sig, 2, NULL, 0);
        break;
    case 0x06:     /* OPEN */
    case 0x0a:     /* ABORT */
        avdtp_reply(label, sig, 2, NULL, 0);
        break;
    case 0x07:     /* START */
        hp.streaming = true;
        have_seq = false;
        avdtp_reply(label, sig, 2, NULL, 0);
        break;
    case 0x09:     /* SUSPEND */
        hp.streaming = false;
        avdtp_reply(label, sig, 2, NULL, 0);
        break;
    case 0x08:     /* CLOSE */
        hp.streaming = false;
        hp.configured = false;
        avdtp_reply(label, sig, 2, NULL, 0);
        break;
    default: {
        avdtp_reply(label, sig, 1, NULL, 0);   /* general reject */
        break;
    }
    }
}

static void media_in(const uint8_t *d, uint16_t len)
{
    if (len < 13) {
        hp.bad_frames++;
        return;
    }
    uint16_t seq = lbt_get16be(d + 2);
    uint32_t ts = lbt_get32be(d + 4);
    int n = d[12] & 0x0f;
    const uint8_t *f = d + 13;
    uint16_t left = (uint16_t)(len - 13);
    uint32_t samples = 0;
    for (int i = 0; i < n; i++) {
        if (left < 4 || f[0] != 0x9c) {
            hp.bad_frames++;
            return;
        }
        int blocks = 4 * (1 + ((f[1] >> 4) & 3));
        int subbands = (f[1] & 1) ? 8 : 4;
        int mode = (f[1] >> 2) & 3;
        int bitpool = f[2];
        int ch = mode == 0 ? 1 : 2;
        int flen = 4 + (4 * subbands * ch) / 8;
        if (mode == 0 || mode == 1)
            flen += (blocks * ch * bitpool + 7) / 8;
        else
            flen += ((mode == 3 ? subbands : 0) + blocks * bitpool + 7) / 8;
        if (flen > left) {
            hp.bad_frames++;
            return;
        }
        samples += (uint32_t)(blocks * subbands);
        f += flen;
        left = (uint16_t)(left - flen);
    }
    if (!hp.streaming) {
        hp.bad_frames++;   /* media outside a started stream */
        return;
    }
    if (have_seq) {
        if (seq != (uint16_t)(last_seq + 1))
            hp.seq_errors++;
        if (ts != last_ts)
            hp.ts_errors++;
    }
    have_seq = true;
    last_seq = seq;
    last_ts = ts + samples;
    hp.frames += (uint32_t)n;
    hp.samples += samples;
    hp.buffered += (int32_t)samples;
    if (hp.buffered > hp.buffered_max)
        hp.buffered_max = hp.buffered;
}

/* ---- headphones: AVRCP ---------------------------------------------------------------- */

static uint8_t avrcp_label;
static int volume_label = -1;     /* the player's volume registration */
static bool status_registered;

static void avctp_send(uint8_t label, bool response, const uint8_t *avc, uint16_t len)
{
    struct hchan *c = chan_psm(0x0017, false);
    if (!c)
        return;
    uint8_t b[3 + 64];
    b[0] = (uint8_t)((label << 4) | (response ? 0x02 : 0));
    lbt_put16be(b + 1, 0x110e);
    memcpy(b + 3, avc, len);
    send_frame(c->theirs, b, (uint16_t)(3 + len));
}

static void vendor_send(uint8_t label, bool response, uint8_t ctype, uint8_t pdu,
                        const uint8_t *p, uint16_t plen)
{
    uint8_t avc[64] = { ctype, 0x48, 0x00, 0x00, 0x19, 0x58, pdu, 0 };
    lbt_put16be(avc + 8, plen);
    memcpy(avc + 10, p, plen);
    avctp_send(label, response, avc, (uint16_t)(10 + plen));
}

static void register_status(void)
{
    uint8_t p[5] = { 0x01, 0, 0, 0, 0 };
    vendor_send(avrcp_label++ & 0x0f, false, 0x03, 0x31, p, 5);
}

static void avrcp_opened(struct hchan *c)
{
    (void)c;
    status_registered = false;
    volume_label = -1;
    /* like the AirPods: ask what the player can tell them */
    uint8_t p = 0x03;
    vendor_send(avrcp_label++ & 0x0f, false, 0x01, 0x10, &p, 1);
}

static void avrcp_in(const uint8_t *d, uint16_t len)
{
    if (len < 6)
        return;
    uint8_t label = d[0] >> 4;
    bool response = d[0] & 0x02;
    const uint8_t *avc = d + 3;
    uint16_t alen = (uint16_t)(len - 3);
    uint8_t ctype = avc[0] & 0x0f;

    if (response) {
        if (avc[2] == 0x00 && alen >= 10) {
            uint8_t pdu = avc[6];
            const uint8_t *p = avc + 10;
            if (pdu == 0x10 && !status_registered) {   /* their capabilities */
                status_registered = true;
                register_status();
            } else if (pdu == 0x31 && alen >= 12 && p[0] == 0x01) {
                hp.player_status = p[1];
                if (ctype == 0x0d)                     /* changed: again */
                    register_status();
            }
        }
        return;
    }

    if (avc[2] == 0x7c) {   /* pass-through to us: not expected, accept */
        uint8_t r[64];
        memcpy(r, avc, alen);
        r[0] = 0x09;
        avctp_send(label, true, r, alen);
        return;
    }
    if (avc[2] != 0x00 || alen < 10) {
        uint8_t r[64];
        memcpy(r, avc, alen < 64 ? alen : 64);
        r[0] = 0x08;
        avctp_send(label, true, r, alen < 64 ? alen : 64);
        return;
    }
    uint8_t pdu = avc[6];
    const uint8_t *p = avc + 10;
    switch (pdu) {
    case 0x10: {   /* capabilities: events we report */
        uint8_t r[] = { 0x03, 0x03, 0x01, 0x02, 0x0d };
        vendor_send(label, true, 0x0c, pdu, r, sizeof(r));
        break;
    }
    case 0x31:     /* register: volume */
        if (alen >= 11 && p[0] == 0x0d) {
            uint8_t r[2] = { 0x0d, hp.volume };
            volume_label = label;
            vendor_send(label, true, 0x0f, pdu, r, 2);
        } else {
            uint8_t r = 0x01;
            vendor_send(label, true, 0x0a, pdu, &r, 1);
        }
        break;
    case 0x50:     /* set absolute volume */
        if (alen >= 11) {
            hp.volume = p[0] & 0x7f;
            hp.volume_sets++;
            vendor_send(label, true, 0x09, pdu, &hp.volume, 1);
        }
        break;
    default: {
        uint8_t r = 0x00;
        vendor_send(label, true, 0x0a, pdu, &r, 1);
        break;
    }
    }
}

static void headphones_reset_link(void)
{
    memset(ch, 0, sizeof(ch));
    next_cid = 0x0040;
    rx_have = rx_want = 0;
    hp.connected = hp.encrypted = hp.configured = hp.open = hp.streaming = false;
    hp.playing = false;
    hp.buffered = 0;
    lead_step = 0;
    volume_label = -1;
    status_registered = false;
    cancel(T_RELEASE);
}

/* ---- controller: commands ------------------------------------------------------------- */

static uint8_t pending_key[16];

static void command(const uint8_t *pkt, size_t len)
{
    uint16_t op = lbt_get16(pkt);
    const uint8_t *p = pkt + 3;
    (void)len;
    switch (op) {
    case HCI_READ_LOCAL_VERSION:
        command_complete(op, (const uint8_t[]){ 0, 8, 0x1a, 0x02, 8, 0x0f, 0, 0xa1, 0x08 }, 9);
        break;
    case HCI_READ_BD_ADDR: {
        uint8_t r[7] = { 0 };
        memcpy(r + 1, player_addr, 6);
        command_complete(op, r, 7);
        break;
    }
    case HCI_READ_BUFFER_SIZE: {
        uint8_t r[8] = { 0 };
        lbt_put16(r + 1, ACL_LEN);
        r[3] = 64;
        lbt_put16(r + 4, ACL_BUFFERS);
        command_complete(op, r, 8);
        break;
    }
    case HCI_WRITE_SCAN_ENABLE:
        sim_page_scan = (p[0] & 0x02) != 0;
        command_ok(op);
        break;
    case HCI_INQUIRY:
        command_status(op, 0);
        if (hp.in_range && hp.awake && hp.pairing_mode)
            after_ms(1500, T_INQUIRY_RESULT, 0);
        after_ms((uint32_t)p[3] * 1280, T_INQUIRY_DONE, 0);
        break;
    case HCI_INQUIRY_CANCEL:
        cancel(T_INQUIRY_RESULT);
        cancel(T_INQUIRY_DONE);
        command_ok(op);
        break;
    case HCI_CREATE_CONNECTION:
        sim_pages++;
        if (link_handle) {
            command_status(op, 0x0b);   /* already connected */
            break;
        }
        command_status(op, 0);
        memcpy(paged.b, p, 6);
        if (memcmp(p, hp.addr.b, 6))
            after_ms(PAGE_MS, T_PAGE_FAILED, 0);
        else   /* the page train runs until they answer or time runs out */
            after_ms(400, T_PAGE_TRY, PAGE_MS - 400);
        break;
    case HCI_CREATE_CONNECTION_CANCEL: {
        bool timed = cancel(T_PAGE_FAILED), trying = cancel(T_PAGE_TRY);
        bool was_paging = timed || trying;
        uint8_t r[7] = { was_paging ? 0 : 0x02 };
        memcpy(r + 1, p, 6);
        command_complete(op, r, 7);
        if (was_paging) {
            uint8_t c[11] = { 0x02 };   /* the attempt ends: "unknown connection" */
            memcpy(c + 3, p, 6);
            c[9] = 1;
            event(5, HCI_EV_CONNECTION_COMPLETE, c, 11);
        }
        break;
    }
    case HCI_ACCEPT_CONNECTION_REQUEST:
        command_status(op, 0);
        after_ms(100, T_CONNECT_DONE, 1);
        break;
    case HCI_DISCONNECT:
        command_status(op, 0);
        if (link_handle == (lbt_get16(p) & 0x0fff))
            link_down(0x16, 30);   /* "terminated by local host" */
        break;
    case HCI_AUTHENTICATION_REQUESTED:
        command_status(op, 0);
        event(20, HCI_EV_LINK_KEY_REQUEST, hp.addr.b, 6);
        break;
    case HCI_LINK_KEY_REQUEST_REPLY: {
        uint8_t r[7] = { 0 };
        memcpy(r + 1, p, 6);
        command_complete(op, r, 7);
        uint8_t ac[3] = { 0, 0, 0 };
        lbt_put16(ac + 1, link_handle);
        if (!(hp.has_key && !memcmp(p + 6, hp.key, 16)))
            ac[0] = 0x06;   /* they don't know this key */
        event(80, HCI_EV_AUTHENTICATION_COMPLETE, ac, 3);
        break;
    }
    case HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY: {
        uint8_t r[7] = { 0 };
        memcpy(r + 1, p, 6);
        command_complete(op, r, 7);
        if (hp.pairing_mode || !hp.has_key) {
            event(20, HCI_EV_IO_CAPABILITY_REQUEST, hp.addr.b, 6);
        } else {
            uint8_t ac[3] = { 0x05, 0, 0 };   /* they won't pair now */
            lbt_put16(ac + 1, link_handle);
            event(80, HCI_EV_AUTHENTICATION_COMPLETE, ac, 3);
        }
        break;
    }
    case HCI_IO_CAPABILITY_REQUEST_REPLY: {
        uint8_t r[7] = { 0 };
        memcpy(r + 1, p, 6);
        command_complete(op, r, 7);
        uint8_t io[9];
        memcpy(io, hp.addr.b, 6);
        io[6] = 0x03; io[7] = 0; io[8] = 0x02;
        event(20, HCI_EV_IO_CAPABILITY_RESPONSE, io, 9);
        uint8_t uc[10];
        memcpy(uc, hp.addr.b, 6);
        lbt_put32(uc + 6, 123456);
        event(100, HCI_EV_USER_CONFIRMATION_REQUEST, uc, 10);
        break;
    }
    case HCI_USER_CONFIRMATION_REQUEST_REPLY: {
        uint8_t r[7] = { 0 };
        memcpy(r + 1, p, 6);
        command_complete(op, r, 7);
        uint8_t spc[7] = { 0 };
        memcpy(spc + 1, hp.addr.b, 6);
        event(100, HCI_EV_SIMPLE_PAIRING_COMPLETE, spc, 7);
        for (int i = 0; i < 16; i++)
            pending_key[i] = (uint8_t)(rand() & 0xff);
        memcpy(hp.key, pending_key, 16);
        hp.has_key = true;
        hp.pairings++;
        uint8_t note[23];
        memcpy(note, hp.addr.b, 6);
        memcpy(note + 6, pending_key, 16);
        note[22] = 0x07;   /* unauthenticated P-256 */
        event(1, HCI_EV_LINK_KEY_NOTIFICATION, note, 23);
        uint8_t ac[3] = { 0, 0, 0 };
        lbt_put16(ac + 1, link_handle);
        event(1, HCI_EV_AUTHENTICATION_COMPLETE, ac, 3);
        break;
    }
    case HCI_SET_CONNECTION_ENCRYPTION: {
        command_status(op, 0);
        uint8_t ec[4] = { 0, 0, 0, p[2] ? 1 : 0 };
        lbt_put16(ec + 1, link_handle);
        event(50, HCI_EV_ENCRYPTION_CHANGE, ec, 4);
        hp.encrypted = p[2] != 0;
        if (hp.encrypted && hp.lead_audio && we_paged)
            after_ms(300, T_OPEN_AVDTP, 0);   /* they connected: they set up audio */
        break;
    }
    case HCI_REMOTE_NAME_REQUEST: {
        command_status(op, 0);
        uint8_t rn[255] = { 0 };
        memcpy(rn + 1, hp.addr.b, 6);
        memcpy(rn + 7, hp.name, strlen(hp.name));
        event(200, HCI_EV_REMOTE_NAME_REQUEST_COMPLETE, rn, 255);
        break;
    }
    default:
        command_ok(op);
        break;
    }
}

/* ---- controller: ACL from the stack ---------------------------------------------------- */

static void acl_from_host(const uint8_t *pkt, size_t len)
{
    if (acl_free <= 0) {
        sim_flow_errors++;
        return;
    }
    uint16_t hdr = lbt_get16(pkt);
    uint16_t dlen = lbt_get16(pkt + 2);
    if ((hdr & 0x0fff) != link_handle || dlen + 4 != len || dlen > ACL_LEN) {
        sim_flow_errors++;
        return;
    }
    acl_free--;
    /* on the air: about 1.4 Mbit/s of payload, one packet after another */
    uint32_t start = air_busy_until > sim_now ? air_busy_until : sim_now;
    uint32_t done = start + 1 + dlen / 180;
    air_busy_until = done;
    if (air_n < 64)
        air_done[air_n++] = done;

    /* the headphones get it (when it's sent) */
    uint8_t pb = (hdr >> 12) & 3;
    const uint8_t *d = pkt + 4;
    if (pb != 1) {   /* start of a PDU */
        rx_have = 0;
        rx_want = 0;
    }
    if (rx_have + dlen > sizeof(rx_buf)) {
        rx_have = 0;
        return;
    }
    memcpy(rx_buf + rx_have, d, dlen);
    rx_have = (uint16_t)(rx_have + dlen);
    if (rx_have >= 4 && !rx_want)
        rx_want = (uint16_t)(4 + lbt_get16(rx_buf));
    if (rx_want && rx_have >= rx_want) {
        if (hp.in_range)
            l2cap_in(rx_buf, rx_want);
        rx_have = rx_want = 0;
    }
}

/* the stack sends a packet */
void lbt_platform_send(uint8_t type, const uint8_t *packet, size_t len)
{
    if (type == LBT_H4_COMMAND)
        command(packet, len);
    else if (type == LBT_H4_ACL)
        acl_from_host(packet, len);
}

uint32_t lbt_platform_ms(void)
{
    return sim_now - sim_now % 10;   /* the H2's 10 ms tick */
}

void lbt_platform_log(const char *fmt, ...)
{
    if (!getenv("LBT_LOG"))
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("    %7.2f ", sim_now / 1000.0);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- time ---------------------------------------------------------------------------- */

static void fire(struct timer *t)
{
    enum timer_kind k = t->kind;
    t->kind = T_NONE;
    switch (k) {
    case T_CONNECT_DONE:
        link_up(0);
        we_paged = t->arg == 1;
        break;
    case T_PAGE_TRY:
        if (hp.in_range && hp.awake)
            link_up(0);
        else if (t->arg > 500)
            after_ms(500, T_PAGE_TRY, t->arg - 500);
        else
            after_ms(t->arg, T_PAGE_FAILED, 0);
        break;
    case T_PAGE_FAILED: {
        uint8_t p[11] = { 0x04 };   /* page timeout */
        memcpy(p + 3, paged.b, 6);
        p[9] = 1;
        event(0, HCI_EV_CONNECTION_COMPLETE, p, 11);
        break;
    }
    case T_INQUIRY_RESULT: {
        uint8_t r[255] = { 0 };
        r[0] = 1;
        memcpy(r + 1, hp.addr.b, 6);
        r[7] = 1;
        lbt_put24(r + 9, 0x240418);   /* headphones */
        r[14] = (uint8_t)-50;
        size_t n = strlen(hp.name);
        r[15] = (uint8_t)(n + 1);
        r[16] = 0x09;
        memcpy(r + 17, hp.name, n);
        event(0, HCI_EV_EXTENDED_INQUIRY_RESULT, r, 255);
        break;
    }
    case T_INQUIRY_DONE:
        event(0, HCI_EV_INQUIRY_COMPLETE, (const uint8_t[]){ 0 }, 1);
        break;
    case T_LINK_LOST:
        link_down(0x08, 0);
        break;
    case T_RELEASE: {
        uint8_t avc[5] = { 0x00, 0x48, 0x7c, (uint8_t)(t->arg | 0x80), 0x00 };
        avctp_send(avrcp_label++ & 0x0f, false, avc, 5);
        break;
    }
    case T_OPEN_AVDTP:
        if (link_handle && !chan_psm(0x0019, false))
            open_channel(0x0019);
        break;
    case T_PAGE_PLAYER:
        if (hp.in_range && hp.awake && sim_page_scan && !link_handle) {
            uint8_t req[10];
            memcpy(req, hp.addr.b, 6);
            lbt_put24(req + 6, 0x240418);
            req[9] = 0x01;
            event(0, HCI_EV_CONNECTION_REQUEST, req, 10);
        }
        break;
    default:
        break;
    }
}

static uint32_t stall_until;
static uint32_t next_poll;
static uint32_t play_acc;

static void headphones_play_1ms(void)
{
    if (!hp.streaming || !hp.rate) {
        hp.playing = false;
        return;
    }
    if (!hp.playing) {
        if (hp.buffered >= hp.rate * PREBUFFER_MS / 1000) {
            hp.playing = true;
            hp.buffered_min = hp.buffered;
        }
        return;
    }
    play_acc += (uint32_t)hp.rate;
    int32_t take = (int32_t)(play_acc / 1000);
    play_acc %= 1000;
    if (hp.buffered < take) {
        if (hp.buffered >= 0)
            hp.underruns++;
        hp.underrun_ms++;
        hp.buffered = 0;
        hp.playing = false;   /* refill before playing again */
        return;
    }
    hp.buffered -= take;
    if (hp.buffered < hp.buffered_min)
        hp.buffered_min = hp.buffered;
}

void sim_run(uint32_t ms)
{
    uint32_t end = sim_now + ms;
    while (sim_now != end) {
        sim_now++;
        /* the radio frees buffers */
        int freed = 0;
        while (air_n && (int32_t)(sim_now - air_done[0]) >= 0) {
            memmove(air_done, air_done + 1, sizeof(air_done[0]) * (size_t)(--air_n));
            freed++;
        }
        if (freed && link_handle && hp.in_range) {
            acl_free += freed;
            uint8_t p[5] = { 1 };
            lbt_put16(p + 1, link_handle);
            lbt_put16(p + 3, (uint16_t)freed);
            event(0, HCI_EV_NUMBER_OF_COMPLETED_PACKETS, p, 5);
        } else if (freed && !hp.in_range) {
            /* out of range: nothing gets through, buffers stay taken */
            for (int i = 0; i < freed && air_n < 64; i++)
                air_done[air_n++] = sim_now + 10;
        }
        for (int i = 0; i < TIMERS; i++)
            if (timers[i].kind != T_NONE && (int32_t)(sim_now - timers[i].at) >= 0)
                fire(&timers[i]);
        headphones_play_1ms();

        if ((int32_t)(sim_now - stall_until) < 0)
            continue;   /* the player is busy */
        /* bytes reach the stack */
        while (q_head != q_tail && (int32_t)(sim_now - queue[q_head].due) >= 0) {
            struct pending p = queue[q_head];
            q_head = (q_head + 1) % QUEUE;
            hci_receive(p.data, p.len);
        }
        if ((int32_t)(sim_now - next_poll) >= 0) {
            next_poll = sim_now + sim_poll_ms;
            source_poll();
        }
    }
}

void sim_stall(uint32_t ms)
{
    stall_until = sim_now + ms;
}

void sim_reset(void)
{
    q_head = q_tail = 0;
    q_last_due = 0;
    memset(timers, 0, sizeof(timers));
    link_handle = 0;
    air_n = 0;
    air_busy_until = 0;
    acl_free = ACL_BUFFERS;
    sim_flow_errors = 0;
    sim_pages = 0;
    sim_page_scan = false;
    stall_until = 0;
    next_poll = 0;
    play_acc = 0;
    memset(&hp, 0, sizeof(hp));
    headphones_reset_link();
    static const uint8_t airpods[6] = { 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa };
    memcpy(hp.addr.b, airpods, 6);
    strcpy(hp.name, "AirPods Pro");
    hp.in_range = hp.awake = true;
    hp.volume = 85;
}

/* ---- things the headphones do -------------------------------------------------------- */

void sim_press(uint8_t op)
{
    uint8_t avc[5] = { 0x00, 0x48, 0x7c, op, 0x00 };
    avctp_send(avrcp_label++ & 0x0f, false, avc, 5);
    after_ms(60, T_RELEASE, op);
}

void sim_swipe_volume(uint8_t v)
{
    hp.volume = v;
    if (volume_label >= 0) {
        uint8_t r[2] = { 0x0d, v };
        uint8_t l = (uint8_t)volume_label;
        volume_label = -1;   /* until they register again */
        vendor_send(l, true, 0x0d, 0x31, r, 2);
    }
}

void sim_suspend(void)
{
    uint8_t s = 0x01 << 2;   /* the player's seid, as it told us (1) */
    avdtp_command(0x09, &s, 1);
}

void sim_out_of_range(void)
{
    hp.in_range = false;
    if (link_handle)
        after_ms(SUPERVISION_MS, T_LINK_LOST, 0);
}

void sim_back_in_range(void)
{
    hp.in_range = true;
    if (hp.page_player)
        after_ms(500, T_PAGE_PLAYER, 0);
}

void sim_case_close(void)
{
    hp.awake = false;
    link_down(0x13, 5);
}

void sim_case_open(void)
{
    hp.awake = true;
    if (hp.page_player)
        after_ms(800, T_PAGE_PLAYER, 0);
}
