/*
 * lbt AVRCP over AVCTP: buttons, play status, the song and absolute volume
 * (AVCTP 1.4 section 6; AVRCP 1.6 sections 4, 6.6 (element attributes,
 * 26 for their ids), 6.7, 6.13, 25; AV/C panel subunit for pass-through).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "avrcp.h"
#include "l2cap.h"
#include "lbt_platform.h"

#define PID_AV_REMOTE  0x110e

/* AV/C command types and responses */
#define CT_CONTROL         0x00
#define CT_STATUS          0x01
#define CT_NOTIFY          0x03
#define RSP_NOT_IMPLEMENTED 0x08
#define RSP_ACCEPTED       0x09
#define RSP_REJECTED       0x0a
#define RSP_STABLE         0x0c
#define RSP_CHANGED        0x0d
#define RSP_INTERIM        0x0f

#define SUBUNIT_PANEL      0x48   /* panel (9) << 3, id 0 */
#define SUBUNIT_UNIT       0xff

#define OP_VENDOR          0x00
#define OP_UNIT_INFO       0x30
#define OP_SUBUNIT_INFO    0x31
#define OP_PASS_THROUGH    0x7c

/* AVRCP PDUs (vendor dependent, Bluetooth SIG company id 00 19 58) */
#define PDU_GET_CAPABILITIES      0x10
#define PDU_GET_ELEMENT_ATTRIBUTES 0x20
#define PDU_GET_PLAY_STATUS       0x30
#define PDU_REGISTER_NOTIFICATION 0x31
#define PDU_SET_ABSOLUTE_VOLUME   0x50

#define CAP_COMPANY_ID     0x02
#define CAP_EVENTS         0x03

#define EVENT_PLAYBACK_STATUS 0x01
#define EVENT_TRACK_CHANGED   0x02
#define EVENT_POS_CHANGED     0x05
#define EVENT_VOLUME_CHANGED  0x0d

/* Element attributes (AVRCP 1.6 section 26) */
#define ATTR_TITLE   1
#define ATTR_ARTIST  2
#define ATTR_ALBUM   3
#define ATTR_NUMBER  4
#define ATTR_TOTAL   5
#define ATTR_TIME    7   /* playing time, ms, as text */
#define ATTR_COVER   8   /* default cover art: an image handle */
#define CHARSET_UTF8 0x006a

#define AVC_MAX      512 /* an AV/C frame (AV/C digital interface 1.0) */

#define ERR_INVALID_COMMAND   0x00
#define ERR_INVALID_PARAMETER 0x01

static const avrcp_ops *ops;

static struct {
    l2cap_chan *chan;
    uint16_t handle;
    /* our commands */
    uint8_t next_label;
    int8_t pending_label;         /* -1: none */
    uint8_t pending_pdu;
    uint32_t sent_ms;
    bool abs_volume;              /* they report and take absolute volume */
    int8_t volume_label;          /* our volume registration, -1 none */
    int16_t queued_volume;        /* to send when the last one is answered */
    /* their registrations with us */
    int8_t status_label, track_label, pos_label;
    l2cap_chan *replaced;         /* ours, closing: theirs is the one */
    int logged;                   /* their commands written to the log */
    uint8_t status;
    uint32_t serial;              /* the song they last heard of */
    bool have_serial;
    uint32_t pos_every_ms, pos_sent_ms;   /* their position registration */
} s;

static void get_track(avrcp_track *t)
{
    memset(t, 0, sizeof(*t));
    if (ops->track)
        ops->track(t);
}

/* ---- sending -------------------------------------------------------------------- */

static void send_frame(uint8_t label, bool response, const uint8_t *avc, uint16_t len)
{
    uint8_t buf[3 + AVC_MAX];
    if (!s.chan || len > AVC_MAX || 3 + len > l2cap_max_send(s.chan))
        return;
    buf[0] = (uint8_t)((label << 4) | (response ? 0x02 : 0x00));
    lbt_put16be(buf + 1, PID_AV_REMOTE);
    memcpy(buf + 3, avc, len);
    l2cap_send(s.chan, buf, 3 + len);
}

/* A vendor-dependent AVRCP PDU: ctype, panel, company id, pdu, params */
static uint16_t vendor(uint8_t *avc, uint8_t ctype, uint8_t pdu,
                       const uint8_t *params, uint16_t plen)
{
    avc[0] = ctype;
    avc[1] = SUBUNIT_PANEL;
    avc[2] = OP_VENDOR;
    avc[3] = 0x00; avc[4] = 0x19; avc[5] = 0x58;
    avc[6] = pdu;
    avc[7] = 0;                   /* single packet */
    lbt_put16be(avc + 8, plen);
    memcpy(avc + 10, params, plen);
    return 10 + plen;
}

static uint8_t take_label(void)
{
    uint8_t l;
    do {
        l = s.next_label;
        s.next_label = (s.next_label + 1) & 0x0f;
    } while (l == s.volume_label);   /* that one stays with the registration */
    return l;
}

static void command(uint8_t ctype, uint8_t pdu, const uint8_t *params, uint16_t plen)
{
    uint8_t avc[64];
    uint8_t label = take_label();
    s.pending_label = (int8_t)label;
    s.pending_pdu = pdu;
    s.sent_ms = lbt_platform_ms();
    send_frame(label, false, avc, vendor(avc, ctype, pdu, params, plen));
}

static void register_volume(void)
{
    uint8_t avc[64];
    uint8_t p[5] = { EVENT_VOLUME_CHANGED, 0, 0, 0, 0 };
    s.volume_label = -1;
    uint8_t label = take_label();
    s.volume_label = (int8_t)label;
    send_frame(label, false, avc, vendor(avc, CT_NOTIFY, PDU_REGISTER_NOTIFICATION, p, 5));
}

static void send_volume(uint8_t v)
{
    uint8_t p = v & 0x7f;
    command(CT_CONTROL, PDU_SET_ABSOLUTE_VOLUME, &p, 1);
}

/* ---- their commands --------------------------------------------------------------- */

static void reject(uint8_t label, uint8_t pdu, uint8_t error)
{
    uint8_t avc[64];
    send_frame(label, true, avc, vendor(avc, RSP_REJECTED, pdu, &error, 1));
}

/* Up to n bytes of UTF-8 text, not cutting a character */
static uint16_t utf8_fit(const char *text, uint16_t n)
{
    size_t len = strlen(text);
    if (len <= n)
        return (uint16_t)len;
    while (n > 0 && ((uint8_t)text[n] & 0xc0) == 0x80)
        n--;
    return n;
}

/* One attribute: its id, UTF-8, its length, the text (cut to fit) */
static uint16_t put_attr(uint8_t *out, uint16_t room, uint32_t id, const char *text)
{
    if (room < 9 || !text[0])
        return 0;
    uint16_t n = utf8_fit(text, room - 8);
    lbt_put32be(out, id);
    lbt_put16be(out + 4, CHARSET_UTF8);
    lbt_put16be(out + 6, n);
    memcpy(out + 8, text, n);
    return 8 + n;
}

/* GetElementAttributes: the song's title, artist, album, number, count
 * and length, the ones asked for (count 0: all we have), in one frame */
static void element_attributes(uint8_t label, uint8_t count, const uint8_t *ids, uint16_t len)
{
    static uint8_t avc[AVC_MAX], p[AVC_MAX];
    static const uint32_t all[] = { ATTR_TITLE, ATTR_ARTIST, ATTR_ALBUM, ATTR_NUMBER,
                                    ATTR_TOTAL, ATTR_TIME, ATTR_COVER };
    avrcp_track t;
    get_track(&t);
    uint16_t max = AVC_MAX - 10;
    if (s.chan && l2cap_max_send(s.chan) < 3 + AVC_MAX)
        max = l2cap_max_send(s.chan) > 3 + 10 ? l2cap_max_send(s.chan) - 3 - 10 : 0;
    uint16_t at = 1;
    uint8_t n = 0;
    int want = count ? count : (int)(sizeof(all) / sizeof(all[0]));
    if (count && len < 4u * count)
        want = len / 4;
    for (int i = 0; i < want && t.loaded; i++) {
        uint32_t id = count ? lbt_get32be(ids + 4 * i) : all[i];
        char num[12] = "";
        const char *text = "";
        if (id == ATTR_TITLE) text = t.title;
        else if (id == ATTR_ARTIST) text = t.artist;
        else if (id == ATTR_ALBUM) text = t.album;
        else if (id == ATTR_NUMBER && t.number) { snprintf(num, sizeof(num), "%u", (unsigned)t.number); text = num; }
        else if (id == ATTR_TOTAL && t.total) { snprintf(num, sizeof(num), "%u", (unsigned)t.total); text = num; }
        else if (id == ATTR_TIME && t.length_ms) { snprintf(num, sizeof(num), "%u", (unsigned)t.length_ms); text = num; }
        else if (id == ATTR_COVER) text = t.cover;
        uint16_t w = put_attr(p + at, max > at ? (uint16_t)(max - at) : 0, id, text);
        if (w) {
            at += w;
            n++;
        }
    }
    p[0] = n;
    send_frame(label, true, avc, vendor(avc, RSP_STABLE, PDU_GET_ELEMENT_ATTRIBUTES, p, at));
}

static void their_vendor(uint8_t label, uint8_t ctype, const uint8_t *f, uint16_t len)
{
    uint8_t avc[64], p[16];
    (void)ctype;
    if (len < 10 || f[3] != 0x00 || f[4] != 0x19 || f[5] != 0x58)
        return;
    uint8_t pdu = f[6];
    const uint8_t *params = f + 10;
    uint16_t plen = lbt_get16be(f + 8);
    if (10 + plen > len)
        plen = len - 10;

    switch (pdu) {
    case PDU_GET_CAPABILITIES:
        if (plen >= 1 && params[0] == CAP_COMPANY_ID) {
            p[0] = CAP_COMPANY_ID; p[1] = 1;
            p[2] = 0x00; p[3] = 0x19; p[4] = 0x58;
            send_frame(label, true, avc, vendor(avc, RSP_STABLE, pdu, p, 5));
        } else if (plen >= 1 && params[0] == CAP_EVENTS) {
            p[0] = CAP_EVENTS; p[1] = 3;
            p[2] = EVENT_PLAYBACK_STATUS; p[3] = EVENT_TRACK_CHANGED; p[4] = EVENT_POS_CHANGED;
            send_frame(label, true, avc, vendor(avc, RSP_STABLE, pdu, p, 5));
        } else {
            reject(label, pdu, ERR_INVALID_PARAMETER);
        }
        break;

    case PDU_GET_PLAY_STATUS: {
        avrcp_track t;
        get_track(&t);
        lbt_put32be(p, t.loaded && t.length_ms ? t.length_ms : 0xffffffff);
        lbt_put32be(p + 4, t.loaded ? t.position_ms : 0xffffffff);
        p[8] = ops->play_status ? ops->play_status() : s.status;
        send_frame(label, true, avc, vendor(avc, RSP_STABLE, pdu, p, 9));
        break;
    }

    case PDU_GET_ELEMENT_ATTRIBUTES:
        /* identifier (8 bytes: 0, the song playing), then the attributes
         * they want (none: all) */
        if (plen < 9) {
            reject(label, pdu, ERR_INVALID_PARAMETER);
            break;
        }
        {
            /* what they ask for says what they show: 8 is the cover */
            static bool told;
            if (!told) {
                char ids[64] = "all";
                int n = params[8], at = 0;
                for (int i = 0; i < n && 9 + 4 * i + 4 <= plen && at < 56; i++)
                    at += snprintf(ids + at, sizeof(ids) - at, "%s%u", i ? "," : "",
                                   (unsigned)lbt_get32be(params + 9 + 4 * i));
                lbt_platform_log("avrcp: they ask for the song's attributes: %s", ids);
                told = true;
            }
        }
        element_attributes(label, params[8], params + 9, plen - 9);
        break;

    case PDU_REGISTER_NOTIFICATION:
        if (plen < 1) {
            reject(label, pdu, ERR_INVALID_PARAMETER);
            break;
        }
        if (params[0] == EVENT_PLAYBACK_STATUS) {
            s.status_label = (int8_t)label;
            if (ops->play_status)
                s.status = ops->play_status();
            p[0] = EVENT_PLAYBACK_STATUS;
            p[1] = s.status;
            send_frame(label, true, avc, vendor(avc, RSP_INTERIM, pdu, p, 2));
        } else if (params[0] == EVENT_TRACK_CHANGED) {
            avrcp_track t;
            get_track(&t);
            s.track_label = (int8_t)label;
            s.serial = t.serial;
            s.have_serial = true;
            p[0] = EVENT_TRACK_CHANGED;
            /* no browsing: 0 while a song is selected, all ones if none */
            memset(p + 1, t.loaded ? 0x00 : 0xff, 8);
            send_frame(label, true, avc, vendor(avc, RSP_INTERIM, pdu, p, 9));
        } else if (params[0] == EVENT_POS_CHANGED && plen >= 5) {
            avrcp_track t;
            get_track(&t);
            s.pos_label = (int8_t)label;
            s.pos_every_ms = lbt_get32be(params + 1) * 1000;
            s.pos_sent_ms = lbt_platform_ms();
            p[0] = EVENT_POS_CHANGED;
            lbt_put32be(p + 1, t.loaded ? t.position_ms : 0xffffffff);
            send_frame(label, true, avc, vendor(avc, RSP_INTERIM, pdu, p, 5));
        } else {
            reject(label, pdu, ERR_INVALID_PARAMETER);
        }
        break;

    default:
        reject(label, pdu, ERR_INVALID_COMMAND);
        break;
    }
}

static void their_command(uint8_t label, const uint8_t *f, uint16_t len)
{
    uint8_t avc[8];
    if (len < 3)
        return;
    uint8_t ctype = f[0] & 0x0f, opcode = f[2];
    if (s.logged < 24) {
        /* what a car asks of us, in the log: vendor PDUs with their
         * first parameter (an event, a capability), or the opcode */
        s.logged++;
        if (opcode == OP_VENDOR && len >= 10)
            lbt_platform_log("avrcp: they ask: pdu 0x%02x%s%02x", f[6],
                             len > 10 ? " param 0x" : "", len > 10 ? f[10] : 0);
        else if (opcode != OP_PASS_THROUGH)
            lbt_platform_log("avrcp: they ask: opcode 0x%02x", opcode);
    }

    if (opcode == OP_PASS_THROUGH && len >= 5) {
        uint8_t op = f[3] & 0x7f;
        bool released = f[3] & 0x80;
        bool known = op == AVRCP_OP_PLAY || op == AVRCP_OP_PAUSE || op == AVRCP_OP_STOP ||
                     op == AVRCP_OP_FORWARD || op == AVRCP_OP_BACKWARD ||
                     op == AVRCP_OP_VOLUME_UP || op == AVRCP_OP_VOLUME_DOWN ||
                     op == AVRCP_OP_FAST_FORWARD || op == AVRCP_OP_REWIND || op == AVRCP_OP_MUTE;
        /* answer with the same operation, pressed or released */
        avc[0] = known ? RSP_ACCEPTED : RSP_NOT_IMPLEMENTED;
        avc[1] = f[1];
        avc[2] = OP_PASS_THROUGH;
        avc[3] = f[3];
        avc[4] = 0;
        send_frame(label, true, avc, 5);
        if (known && ops->button)
            ops->button(op, !released);
    } else if (opcode == OP_UNIT_INFO) {
        uint8_t r[8] = { RSP_STABLE, SUBUNIT_UNIT, OP_UNIT_INFO, 0x07, SUBUNIT_PANEL,
                         0x00, 0x19, 0x58 };
        send_frame(label, true, r, 8);
    } else if (opcode == OP_SUBUNIT_INFO) {
        uint8_t r[8] = { RSP_STABLE, SUBUNIT_UNIT, OP_SUBUNIT_INFO, 0x07, SUBUNIT_PANEL,
                         0xff, 0xff, 0xff };
        send_frame(label, true, r, 8);
    } else if (opcode == OP_VENDOR) {
        their_vendor(label, ctype, f, len);
    } else {
        avc[0] = RSP_NOT_IMPLEMENTED;
        memcpy(avc + 1, f + 1, 2);
        send_frame(label, true, avc, 3);
    }
}

/* ---- answers to ours ------------------------------------------------------------------ */

static void our_answer(uint8_t label, const uint8_t *f, uint16_t len)
{
    if (len < 10 || f[2] != OP_VENDOR)
        return;
    uint8_t rsp = f[0] & 0x0f, pdu = f[6];
    const uint8_t *params = f + 10;
    uint16_t plen = lbt_get16be(f + 8);
    if (10 + plen > len)
        plen = len - 10;

    /* our volume registration: interim now, changed later */
    if (pdu == PDU_REGISTER_NOTIFICATION && (int8_t)label == s.volume_label) {
        if ((rsp == RSP_INTERIM || rsp == RSP_CHANGED) && plen >= 2 &&
            params[0] == EVENT_VOLUME_CHANGED) {
            if (ops->volume)
                ops->volume(params[1] & 0x7f);
            if (rsp == RSP_CHANGED)
                register_volume();   /* notifications are one-shot */
        } else if (rsp == RSP_REJECTED || rsp == RSP_NOT_IMPLEMENTED) {
            s.volume_label = -1;
            s.abs_volume = false;
        }
        return;
    }

    if ((int8_t)label != s.pending_label || pdu != s.pending_pdu)
        return;
    s.pending_label = -1;

    if (pdu == PDU_GET_CAPABILITIES && rsp == RSP_STABLE && plen >= 2 &&
        params[0] == CAP_EVENTS) {
        for (int i = 0; i < params[1] && 2 + i < plen; i++)
            if (params[2 + i] == EVENT_VOLUME_CHANGED)
                s.abs_volume = true;
        if (s.abs_volume)
            register_volume();
    }
    /* a volume change waiting for this answer goes now */
    if (s.queued_volume >= 0 && s.abs_volume && s.pending_label < 0) {
        uint8_t v = (uint8_t)s.queued_volume;
        s.queued_volume = -1;
        send_volume(v);
    }
}

/* ---- L2CAP ------------------------------------------------------------------------- */

static void chan_opened(l2cap_chan *c, uint16_t result)
{
    if (s.chan && c != s.chan) {
        if (result)
            return;
        /* they opened their own: there's one control channel, and a car
         * may use only the one it opened, so theirs it is */
        lbt_platform_log("avrcp: they opened their own control channel: using theirs");
        l2cap_chan *ours = s.chan;
        s.chan = c;
        s.handle = l2cap_handle(c);
        s.replaced = ours;
        s.pending_label = s.volume_label = s.status_label = s.track_label = s.pos_label = -1;
        s.abs_volume = false;
        s.have_serial = false;
        l2cap_disconnect(ours);
        uint8_t p = CAP_EVENTS;
        command(CT_STATUS, PDU_GET_CAPABILITIES, &p, 1);
        return;
    }
    if (result) {
        s.chan = NULL;
        if (ops->connected)
            ops->connected(false);
        return;
    }
    s.chan = c;
    s.handle = l2cap_handle(c);
    if (ops->connected)
        ops->connected(true);
    /* do they take absolute volume? */
    uint8_t p = CAP_EVENTS;
    command(CT_STATUS, PDU_GET_CAPABILITIES, &p, 1);
}

static void chan_data(l2cap_chan *c, const uint8_t *d, uint16_t len)
{
    if (c != s.chan || len < 3)
        return;
    uint8_t label = d[0] >> 4;
    uint8_t ptype = (d[0] >> 2) & 3;
    bool response = d[0] & 0x02;
    uint16_t pid = lbt_get16be(d + 1);
    if (ptype != 0)
        return;   /* fragmented: not used by anything we talk to */
    if (pid != PID_AV_REMOTE) {
        if (!response) {   /* not our profile: say so (IPID) */
            uint8_t r[3] = { (uint8_t)((label << 4) | 0x03), d[1], d[2] };
            l2cap_send(s.chan, r, 3);
        }
        return;
    }
    if (response)
        our_answer(label, d + 3, len - 3);
    else
        their_command(label, d + 3, len - 3);
}

static void chan_closed(l2cap_chan *c)
{
    if (c == s.replaced) {
        s.replaced = NULL;
        return;
    }
    if (c != s.chan)
        return;
    memset(&s, 0, sizeof(s));
    s.pending_label = s.volume_label = s.status_label = s.track_label = s.pos_label = -1;
    s.queued_volume = -1;
    if (ops->disconnected)
        ops->disconnected();
}

static const l2cap_ops avrcp_l2cap_ops = { chan_opened, chan_data, chan_closed };

/* ---- API ------------------------------------------------------------------------------ */

void avrcp_init(const avrcp_ops *o)
{
    ops = o;
    memset(&s, 0, sizeof(s));
    s.pending_label = s.volume_label = s.status_label = s.track_label = s.pos_label = -1;
    s.queued_volume = -1;
    l2cap_listen(L2CAP_PSM_AVCTP, &avrcp_l2cap_ops, 672);
}

bool avrcp_connect(uint16_t handle)
{
    if (s.chan)
        return false;
    s.chan = l2cap_connect(handle, L2CAP_PSM_AVCTP, &avrcp_l2cap_ops, 672, NULL);
    return s.chan != NULL;
}

void avrcp_disconnect(void)
{
    if (s.chan)
        l2cap_disconnect(s.chan);
}

void avrcp_play_status_changed(uint8_t status)
{
    uint8_t avc[64], p[2] = { EVENT_PLAYBACK_STATUS, status };
    if (status == s.status)
        return;
    s.status = status;
    if (s.status_label >= 0) {
        send_frame((uint8_t)s.status_label, true, avc,
                   vendor(avc, RSP_CHANGED, PDU_REGISTER_NOTIFICATION, p, 2));
        s.status_label = -1;   /* they register again */
    }
}

static void send_position(uint32_t ms)
{
    uint8_t avc[64], p[5] = { EVENT_POS_CHANGED };
    if (s.pos_label < 0)
        return;
    lbt_put32be(p + 1, ms);
    send_frame((uint8_t)s.pos_label, true, avc,
               vendor(avc, RSP_CHANGED, PDU_REGISTER_NOTIFICATION, p, 5));
    s.pos_label = -1;          /* they register again */
}

void avrcp_track_changed(void)
{
    uint8_t avc[64], p[9] = { EVENT_TRACK_CHANGED };
    avrcp_track t;
    get_track(&t);
    s.serial = t.serial;
    s.have_serial = true;
    send_position(t.loaded ? t.position_ms : 0xffffffff);
    if (s.track_label >= 0) {
        memset(p + 1, t.loaded ? 0x00 : 0xff, 8);
        send_frame((uint8_t)s.track_label, true, avc,
                   vendor(avc, RSP_CHANGED, PDU_REGISTER_NOTIFICATION, p, 9));
        s.track_label = -1;
    }
}

bool avrcp_set_volume(uint8_t volume)
{
    if (!s.chan || !s.abs_volume)
        return false;
    if (volume > 127)
        volume = 127;
    if (s.pending_label >= 0) {
        s.queued_volume = volume & 0x7f;   /* send the latest when free */
        return true;
    }
    send_volume(volume);
    return true;
}

bool avrcp_has_absolute_volume(void)
{
    return s.abs_volume;
}

void avrcp_tick(void)
{
    if (s.chan && (s.track_label >= 0 || s.pos_label >= 0)) {
        avrcp_track t;
        get_track(&t);
        if (s.have_serial && t.serial != s.serial)
            avrcp_track_changed();               /* a new song */
        else if (s.pos_label >= 0 && s.pos_every_ms &&
                 lbt_platform_ms() - s.pos_sent_ms >= s.pos_every_ms &&
                 s.status == AVRCP_PLAYING)
            send_position(t.loaded ? t.position_ms : 0xffffffff);
    }
    if (s.pending_label >= 0 && lbt_platform_ms() - s.sent_ms > AVRCP_TIMEOUT_MS) {
        lbt_platform_log("avrcp: no answer to pdu 0x%02x", s.pending_pdu);
        s.pending_label = -1;
        if (s.queued_volume >= 0) {
            uint8_t v = (uint8_t)s.queued_volume;
            s.queued_volume = -1;
            send_volume(v);
        }
    }
}
