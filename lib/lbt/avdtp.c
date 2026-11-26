/*
 * lbt AVDTP: signaling (both roles) and the media channel, for one A2DP
 * source stream with SBC (AVDTP 1.3 sections 8 and 9; A2DP 1.3 section 4.3
 * for SBC; RTP per RFC 3550).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "avdtp.h"
#include "l2cap.h"
#include "lbt_platform.h"

/* signal ids */
#define SIG_DISCOVER             0x01
#define SIG_GET_CAPABILITIES     0x02
#define SIG_SET_CONFIGURATION    0x03
#define SIG_GET_CONFIGURATION    0x04
#define SIG_RECONFIGURE          0x05
#define SIG_OPEN                 0x06
#define SIG_START                0x07
#define SIG_CLOSE                0x08
#define SIG_SUSPEND              0x09
#define SIG_ABORT                0x0a
#define SIG_SECURITY_CONTROL     0x0b
#define SIG_GET_ALL_CAPABILITIES 0x0c
#define SIG_DELAYREPORT          0x0d

/* message types */
#define MSG_CMD            0
#define MSG_GENERAL_REJECT 1
#define MSG_ACCEPT         2
#define MSG_REJECT         3

/* service categories */
#define CAT_MEDIA_TRANSPORT 0x01
#define CAT_MEDIA_CODEC     0x07
#define CAT_DELAY_REPORTING 0x08

/* errors */
#define ERR_BAD_ACP_SEID          0x12
#define ERR_SEP_IN_USE            0x13
#define ERR_BAD_PAYLOAD_FORMAT    0x18
#define ERR_NOT_SUPPORTED_COMMAND 0x19
#define ERR_INVALID_CAPABILITIES  0x1a
#define ERR_BAD_STATE             0x31

#define MEDIA_AUDIO   0x00
#define CODEC_SBC     0x00
#define TSEP_SOURCE   0
#define TSEP_SINK     1

#define LOCAL_SEID    1
#define MAX_SEPS      6

/* What our endpoint can encode: 44.1 and 48 kHz, every channel mode,
 * every block length, 4 or 8 subbands, both allocations, bitpool 2..53 */
static const uint8_t local_sbc[4] = { 0x30 | 0x0f, 0xff, 2, 53 };

enum state {
    ST_IDLE,           /* no stream */
    ST_CONFIGURED,     /* codec agreed */
    ST_OPEN,           /* media channel up, not streaming */
    ST_STREAMING,
};

enum step {            /* what we're waiting for while we lead */
    STEP_NONE,
    STEP_SIG_CHANNEL,
    STEP_SIG_THEIRS,   /* ours was refused: theirs may be on its way */
    STEP_DISCOVER,
    STEP_CAPS,
    STEP_CONFIGURE,
    STEP_OPEN,
    STEP_MEDIA_CHANNEL,
    STEP_START,
    STEP_SUSPEND,
    STEP_CLOSE,
};

static const avdtp_ops *ops;
extern const l2cap_ops avdtp_l2cap_ops;

static struct {
    uint16_t handle;
    l2cap_chan *sig, *media;
    l2cap_chan *sig_dropped; /* ours, still opening, after theirs won */
    uint32_t lead_at;        /* theirs is open: we lead from then if they haven't */
    uint32_t give_up_at;     /* STEP_SIG_THEIRS: no channel of theirs by then */
    uint16_t refused;        /* what they said to ours */
    enum state state;
    enum step step;
    bool media_expected;     /* they opened us: their media channel comes next */
    uint8_t label;           /* of our command awaiting an answer */
    uint8_t pending;         /* its signal id, 0 if none */
    uint32_t sent_ms;
    struct {
        uint8_t seid;
        bool in_use;
        uint8_t media, tsep;
        bool has_sbc;
        uint8_t sbc[4];
        bool asked;
    } seps[MAX_SEPS];
    int nseps;
    int asking;              /* endpoint whose capabilities we asked for */
    bool use_get_all;        /* GET_ALL_CAPABILITIES, else plain */
    uint8_t remote_seid;
    avdtp_sbc_config cfg;
    uint16_t seq;
    uint32_t timestamp;
} s;

static uint8_t next_label;

/* ---- SBC settings ------------------------------------------------------------------- */

static bool decode_sbc(const uint8_t b[4], avdtp_sbc_config *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->raw, b, 4);
    c->rate = (b[0] & 0x20) ? 44100 : (b[0] & 0x10) ? 48000 :
              (b[0] & 0x40) ? 32000 : (b[0] & 0x80) ? 16000 : 0;
    c->mode = b[0] & 0x0f;
    c->channels = c->mode == 0x08 ? 1 : 2;
    c->blocks = (b[1] & 0x10) ? 16 : (b[1] & 0x20) ? 12 : (b[1] & 0x40) ? 8 : 4;
    c->subbands = (b[1] & 0x04) ? 8 : 4;
    c->loudness = b[1] & 0x01;
    c->min_bitpool = b[2];
    c->max_bitpool = b[3];
    /* a configuration has exactly one bit per field */
    return c->rate && (c->mode == 1 || c->mode == 2 || c->mode == 4 || c->mode == 8) &&
           c->min_bitpool >= 2 && c->min_bitpool <= c->max_bitpool;
}

static uint8_t first_bit(uint8_t have, const uint8_t *order, int n)
{
    for (int i = 0; i < n; i++)
        if (have & order[i])
            return order[i];
    return 0;
}

bool avdtp_choose_sbc(const uint8_t remote[4], avdtp_sbc_config *cfg)
{
    static const uint8_t rates[] = { 0x20, 0x10 };              /* 44.1, 48 */
    static const uint8_t modes[] = { 0x01, 0x02, 0x04, 0x08 };  /* joint first */
    static const uint8_t blocks[] = { 0x10, 0x20, 0x40, 0x80 }; /* 16 first */
    static const uint8_t subbands[] = { 0x04, 0x08 };           /* 8 first */
    static const uint8_t allocs[] = { 0x01, 0x02 };             /* loudness */
    uint8_t b[4];

    uint8_t r = first_bit(remote[0] & local_sbc[0] & 0xf0, rates, 2);
    uint8_t m = first_bit(remote[0] & local_sbc[0] & 0x0f, modes, 4);
    uint8_t bl = first_bit(remote[1] & local_sbc[1] & 0xf0, blocks, 4);
    uint8_t sb = first_bit(remote[1] & local_sbc[1] & 0x0c, subbands, 2);
    uint8_t al = first_bit(remote[1] & local_sbc[1] & 0x03, allocs, 2);
    if (!r || !m || !bl || !sb || !al)
        return false;
    b[0] = r | m;
    b[1] = bl | sb | al;
    b[2] = remote[2] > local_sbc[2] ? remote[2] : local_sbc[2];
    b[3] = remote[3] < local_sbc[3] ? remote[3] : local_sbc[3];
    return decode_sbc(b, cfg);
}

/* ---- sending ------------------------------------------------------------------------- */

static void send_sig(uint8_t label, uint8_t msg, uint8_t sig, const uint8_t *p, uint16_t len)
{
    uint8_t buf[2 + 32];
    if (!s.sig || len > 32)
        return;
    buf[0] = (uint8_t)((label << 4) | msg);   /* single packet */
    buf[1] = sig;
    memcpy(buf + 2, p, len);
    if (!l2cap_send(s.sig, buf, 2 + len))
        lbt_platform_log("avdtp: couldn't send signal 0x%02x", sig);
}

static void command(uint8_t sig, const uint8_t *p, uint16_t len)
{
    s.label = next_label;
    next_label = (next_label + 1) & 0x0f;
    s.pending = sig;
    s.sent_ms = lbt_platform_ms();
    send_sig(s.label, MSG_CMD, sig, p, len);
}

static void accept(uint8_t label, uint8_t sig, const uint8_t *p, uint16_t len)
{
    send_sig(label, MSG_ACCEPT, sig, p, len);
}

static void reject(uint8_t label, uint8_t sig, const uint8_t *p, uint16_t len)
{
    send_sig(label, MSG_REJECT, sig, p, len);
}

/* Our capabilities or configuration: media transport, then SBC */
static uint16_t codec_caps(uint8_t *p, const uint8_t sbc[4])
{
    p[0] = CAT_MEDIA_TRANSPORT;
    p[1] = 0;
    p[2] = CAT_MEDIA_CODEC;
    p[3] = 6;
    p[4] = MEDIA_AUDIO << 4;
    p[5] = CODEC_SBC;
    memcpy(p + 6, sbc, 4);
    return 10;
}

/* ---- stream life ---------------------------------------------------------------------- */

static void reset(void)
{
    memset(&s, 0, sizeof(s));
}

static void stream_gone(void)
{
    bool had_stream = s.state != ST_IDLE || s.step != STEP_NONE;
    enum step step = s.step;
    s.state = ST_IDLE;
    s.step = STEP_NONE;
    s.pending = 0;
    if (s.media) {
        l2cap_disconnect(s.media);
        s.media = NULL;
    }
    if (step != STEP_NONE && step < STEP_START && ops->open)
        ops->open(false);
    else if (had_stream && ops->closed)
        ops->closed();
}

static void setup_failed(const char *why)
{
    lbt_platform_log("avdtp: setup failed: %s", why);
    if (s.sig)
        l2cap_disconnect(s.sig);
    stream_gone();
}

/* Ask for the next unasked audio sink's capabilities, or configure */
static void next_caps(void)
{
    for (int i = 0; i < s.nseps; i++) {
        if (!s.seps[i].asked && !s.seps[i].in_use &&
            s.seps[i].media == MEDIA_AUDIO && s.seps[i].tsep == TSEP_SINK) {
            uint8_t p = (uint8_t)(s.seps[i].seid << 2);
            s.seps[i].asked = true;
            s.asking = i;
            s.step = STEP_CAPS;
            command(s.use_get_all ? SIG_GET_ALL_CAPABILITIES : SIG_GET_CAPABILITIES, &p, 1);
            return;
        }
    }
    /* all asked: configure the first sink that takes SBC */
    for (int i = 0; i < s.nseps; i++) {
        if (s.seps[i].has_sbc && avdtp_choose_sbc(s.seps[i].sbc, &s.cfg)) {
            uint8_t p[2 + 10];
            s.remote_seid = s.seps[i].seid;
            p[0] = (uint8_t)(s.remote_seid << 2);
            p[1] = LOCAL_SEID << 2;
            codec_caps(p + 2, s.cfg.raw);
            s.step = STEP_CONFIGURE;
            command(SIG_SET_CONFIGURATION, p, sizeof(p));
            return;
        }
    }
    setup_failed("no endpoint takes SBC");
}

/* Remember an endpoint's SBC capabilities from a capabilities answer */
static void read_caps(int sep, const uint8_t *p, uint16_t len)
{
    for (uint16_t off = 0; off + 2 <= len; ) {
        uint8_t cat = p[off], l = p[off + 1];
        if (off + 2 + l > len)
            break;
        if (cat == CAT_MEDIA_CODEC && l >= 6 && (p[off + 2] >> 4) == MEDIA_AUDIO &&
            p[off + 3] == CODEC_SBC) {
            s.seps[sep].has_sbc = true;
            memcpy(s.seps[sep].sbc, p + off + 4, 4);
        }
        off += 2 + l;
    }
}

/* ---- answers to our commands ------------------------------------------------------------ */

static void answered(uint8_t msg, uint8_t sig, const uint8_t *p, uint16_t len)
{
    bool ok = msg == MSG_ACCEPT;
    s.pending = 0;

    switch (sig) {
    case SIG_DISCOVER:
        if (!ok) {
            setup_failed("discover refused");
            return;
        }
        s.nseps = 0;
        for (uint16_t i = 0; i + 1 < len && s.nseps < MAX_SEPS; i += 2) {
            s.seps[s.nseps].seid = p[i] >> 2;
            s.seps[s.nseps].in_use = (p[i] >> 1) & 1;
            s.seps[s.nseps].media = p[i + 1] >> 4;
            s.seps[s.nseps].tsep = (p[i + 1] >> 3) & 1;
            s.nseps++;
        }
        next_caps();
        break;

    case SIG_GET_ALL_CAPABILITIES:
    case SIG_GET_CAPABILITIES:
        if (!ok && sig == SIG_GET_ALL_CAPABILITIES) {
            /* an older device: ask again the AVDTP 1.0 way */
            s.use_get_all = false;
            for (int i = 0; i < s.nseps; i++)
                s.seps[i].asked = false;
            next_caps();
            return;
        }
        if (ok)
            read_caps(s.asking, p, len);
        next_caps();
        break;

    case SIG_SET_CONFIGURATION:
        if (!ok) {
            setup_failed("configuration refused");
            return;
        }
        s.state = ST_CONFIGURED;
        if (ops->configured)
            ops->configured(&s.cfg);
        {
            uint8_t seid = (uint8_t)(s.remote_seid << 2);
            s.step = STEP_OPEN;
            command(SIG_OPEN, &seid, 1);
        }
        break;

    case SIG_OPEN:
        if (!ok) {
            setup_failed("open refused");
            return;
        }
        s.step = STEP_MEDIA_CHANNEL;
        s.media = l2cap_connect(s.handle, L2CAP_PSM_AVDTP, &avdtp_l2cap_ops, 1000, NULL);
        if (!s.media)
            setup_failed("no channel for media");
        break;

    case SIG_START:
        s.step = STEP_NONE;
        if (ok) {
            s.state = ST_STREAMING;
            if (ops->started)
                ops->started();
        }
        break;

    case SIG_SUSPEND:
        s.step = STEP_NONE;
        if (ok || s.state == ST_STREAMING) {
            s.state = ST_OPEN;
            if (ops->suspended)
                ops->suspended();
        }
        break;

    case SIG_CLOSE:
    case SIG_ABORT:
        s.step = STEP_CLOSE;
        if (s.media)
            l2cap_disconnect(s.media);
        else
            stream_gone();
        break;

    default:
        break;
    }
}

/* ---- their commands ------------------------------------------------------------------------ */

static void their_command(uint8_t label, uint8_t sig, const uint8_t *p, uint16_t len)
{
    s.lead_at = 0;                       /* they lead */
    uint8_t rsp[16];
    uint8_t seid = len ? p[0] >> 2 : 0;

    switch (sig) {
    case SIG_DISCOVER:
        rsp[0] = (uint8_t)((LOCAL_SEID << 2) | ((s.state != ST_IDLE) << 1));
        rsp[1] = (uint8_t)((MEDIA_AUDIO << 4) | (TSEP_SOURCE << 3));
        accept(label, sig, rsp, 2);
        break;

    case SIG_GET_CAPABILITIES:
    case SIG_GET_ALL_CAPABILITIES:
        if (seid != LOCAL_SEID) {
            rsp[0] = ERR_BAD_ACP_SEID;
            reject(label, sig, rsp, 1);
            break;
        }
        accept(label, sig, rsp, codec_caps(rsp, local_sbc));
        break;

    case SIG_SET_CONFIGURATION: {
        avdtp_sbc_config c;
        bool found = false;
        if (seid != LOCAL_SEID || len < 2) {
            rsp[0] = 0; rsp[1] = ERR_BAD_ACP_SEID;
            reject(label, sig, rsp, 2);
            break;
        }
        if (s.state != ST_IDLE) {
            rsp[0] = 0; rsp[1] = ERR_SEP_IN_USE;
            reject(label, sig, rsp, 2);
            break;
        }
        for (uint16_t off = 2; off + 2 <= len; ) {
            uint8_t cat = p[off], l = p[off + 1];
            if (off + 2 + l > len)
                break;
            if (cat == CAT_MEDIA_CODEC && l >= 6 && p[off + 3] == CODEC_SBC &&
                decode_sbc(p + off + 4, &c) &&
                (c.raw[0] & ~local_sbc[0]) == 0 && (c.raw[1] & ~local_sbc[1]) == 0)
                found = true;
            off += 2 + l;
        }
        if (!found) {
            rsp[0] = CAT_MEDIA_CODEC; rsp[1] = ERR_INVALID_CAPABILITIES;
            reject(label, sig, rsp, 2);
            break;
        }
        s.cfg = c;
        s.remote_seid = p[1] >> 2;
        s.state = ST_CONFIGURED;
        accept(label, sig, NULL, 0);
        if (ops->configured)
            ops->configured(&s.cfg);
        break;
    }

    case SIG_GET_CONFIGURATION:
        if (s.state == ST_IDLE) {
            rsp[0] = ERR_BAD_STATE;
            reject(label, sig, rsp, 1);
            break;
        }
        accept(label, sig, rsp, codec_caps(rsp, s.cfg.raw));
        break;

    case SIG_OPEN:
        if (s.state != ST_CONFIGURED) {
            rsp[0] = ERR_BAD_STATE;
            reject(label, sig, rsp, 1);
            break;
        }
        s.media_expected = true;   /* their media channel comes next */
        accept(label, sig, NULL, 0);
        break;

    case SIG_START:
        if (s.state != ST_OPEN && s.state != ST_STREAMING) {
            rsp[0] = len ? p[0] : 0; rsp[1] = ERR_BAD_STATE;
            reject(label, sig, rsp, 2);
            break;
        }
        accept(label, sig, NULL, 0);
        if (s.state != ST_STREAMING) {
            s.state = ST_STREAMING;
            if (ops->started)
                ops->started();
        }
        break;

    case SIG_SUSPEND:
        accept(label, sig, NULL, 0);
        if (s.state == ST_STREAMING) {
            s.state = ST_OPEN;
            if (ops->suspended)
                ops->suspended();
        }
        break;

    case SIG_CLOSE:
    case SIG_ABORT:
        accept(label, sig, NULL, 0);
        if (s.media)
            l2cap_disconnect(s.media);
        else
            stream_gone();
        break;

    case SIG_DELAYREPORT:
        accept(label, sig, NULL, 0);
        break;

    case SIG_RECONFIGURE:
        rsp[0] = 0; rsp[1] = ERR_NOT_SUPPORTED_COMMAND;
        reject(label, sig, rsp, 2);
        break;

    case SIG_SECURITY_CONTROL:
        rsp[0] = ERR_NOT_SUPPORTED_COMMAND;
        reject(label, sig, rsp, 1);
        break;

    default:
        send_sig(label, MSG_GENERAL_REJECT, sig, NULL, 0);
        break;
    }
}

/* ---- L2CAP ------------------------------------------------------------------------------------ */

static void sig_data(const uint8_t *data, uint16_t len)
{
    if (len < 2)
        return;
    uint8_t label = data[0] >> 4;
    uint8_t ptype = (data[0] >> 2) & 3;
    uint8_t msg = data[0] & 3;
    uint8_t sig = data[1] & 0x3f;
    if (ptype != 0) {
        lbt_platform_log("avdtp: fragmented signal ignored");
        return;
    }
    if (msg == MSG_CMD)
        their_command(label, sig, data + 2, len - 2);
    else if (s.pending && label == s.label && sig == s.pending)
        answered(msg, sig, data + 2, len - 2);
}

/* We lead: what endpoints do they have */
static void lead(void)
{
    s.lead_at = 0;
    s.step = STEP_DISCOVER;
    s.use_get_all = true;
    command(SIG_DISCOVER, NULL, 0);
}

/* Both sides can open signaling at once (cars do, right after pairing),
 * and AVDTP has one signaling channel: when theirs comes while ours is
 * still opening, or after they refused ours, theirs is it. They opened
 * it, so they may lead; if they haven't within AVDTP_THEIRS_MS, we do. */
static void chan_opened(l2cap_chan *c, uint16_t result)
{
    if (c == s.sig_dropped) {
        s.sig_dropped = NULL;
        if (result == 0)
            l2cap_disconnect(c);
        return;
    }
    if (c == s.sig) {
        if (result) {
            lbt_platform_log("avdtp: they refused our signaling channel (0x%04x): waiting for theirs",
                             result);
            s.sig = NULL;
            s.refused = result;
            s.step = STEP_SIG_THEIRS;
            s.give_up_at = lbt_platform_ms() + AVDTP_THEIRS_MS;
            return;
        }
        if (s.step == STEP_SIG_CHANNEL) {
            s.step = STEP_DISCOVER;
            s.use_get_all = true;
            command(SIG_DISCOVER, NULL, 0);
        }
        return;
    }
    if (result == 0 && s.sig && s.step == STEP_SIG_CHANNEL && l2cap_handle(c) == s.handle) {
        lbt_platform_log("avdtp: their signaling channel crossed ours: using theirs");
        s.sig_dropped = s.sig;
        s.sig = c;
        s.step = STEP_NONE;
        s.lead_at = lbt_platform_ms() + AVDTP_THEIRS_MS;
        return;
    }
    if (!s.sig && result == 0) {
        /* they opened signaling to us */
        if (s.step == STEP_SIG_THEIRS) {
            lbt_platform_log("avdtp: their signaling channel came: using it");
            s.step = STEP_NONE;
            s.lead_at = lbt_platform_ms() + AVDTP_THEIRS_MS;
        }
        s.sig = c;
        s.handle = l2cap_handle(c);
        return;
    }
    if (c == s.media || (s.media_expected && result == 0 && l2cap_handle(c) == s.handle)) {
        if (result) {
            s.media = NULL;
            setup_failed("no media channel");
            return;
        }
        s.media = c;
        s.media_expected = false;
        s.state = ST_OPEN;
        s.step = STEP_NONE;
        s.seq = 0;
        s.timestamp = 0;
        if (ops->open)
            ops->open(true);
        return;
    }
    /* an unexpected extra channel */
    if (result == 0)
        l2cap_disconnect(c);
}

static void chan_data(l2cap_chan *c, const uint8_t *data, uint16_t len)
{
    if (c == s.sig)
        sig_data(data, len);
    /* nothing comes back on the media channel of a source */
}

static void chan_closed(l2cap_chan *c)
{
    if (c == s.media) {
        s.media = NULL;
        stream_gone();
    } else if (c == s.sig) {
        s.sig = NULL;
        stream_gone();
        reset();
    }
}

const l2cap_ops avdtp_l2cap_ops = { chan_opened, chan_data, chan_closed };

/* ---- API --------------------------------------------------------------------------------------- */

void avdtp_init(const avdtp_ops *o)
{
    ops = o;
    memset(&s, 0, sizeof(s));
    next_label = 0;
    l2cap_listen(L2CAP_PSM_AVDTP, &avdtp_l2cap_ops, 1000);
}

bool avdtp_connect(uint16_t handle)
{
    if (s.sig) {
        /* their signaling channel, with nothing set up on it: we lead */
        if (s.state == ST_IDLE && s.step == STEP_NONE && !s.pending &&
            l2cap_handle(s.sig) == handle) {
            lead();
            return true;
        }
        return false;
    }
    memset(&s, 0, sizeof(s));
    s.handle = handle;
    s.step = STEP_SIG_CHANNEL;
    s.sig = l2cap_connect(handle, L2CAP_PSM_AVDTP, &avdtp_l2cap_ops, 1000, NULL);
    if (!s.sig) {
        s.step = STEP_NONE;
        return false;
    }
    return true;
}

bool avdtp_start(void)
{
    if (s.state != ST_OPEN || s.pending)
        return false;
    uint8_t seid = (uint8_t)(s.remote_seid << 2);
    s.step = STEP_START;
    command(SIG_START, &seid, 1);
    return true;
}

bool avdtp_suspend(void)
{
    if (s.state != ST_STREAMING || s.pending)
        return false;
    uint8_t seid = (uint8_t)(s.remote_seid << 2);
    s.step = STEP_SUSPEND;
    command(SIG_SUSPEND, &seid, 1);
    return true;
}

void avdtp_close(void)
{
    if (!s.sig)
        return;
    if (s.state == ST_IDLE) {
        l2cap_disconnect(s.sig);
        return;
    }
    uint8_t seid = (uint8_t)(s.remote_seid << 2);
    s.step = STEP_CLOSE;
    command(SIG_CLOSE, &seid, 1);
}

void avdtp_tick(void)
{
    uint32_t t = lbt_platform_ms();
    if (s.step == STEP_SIG_THEIRS && (int32_t)(t - s.give_up_at) >= 0) {
        char why[48];
        snprintf(why, sizeof(why), "no signaling channel (refused: 0x%04x)", s.refused);
        setup_failed(why);
    }
    if (s.lead_at && (int32_t)(t - s.lead_at) >= 0) {
        s.lead_at = 0;
        if (s.sig && s.state == ST_IDLE && s.step == STEP_NONE && !s.pending)
            lead();
    }
    if (s.pending && lbt_platform_ms() - s.sent_ms > AVDTP_TIMEOUT_MS) {
        lbt_platform_log("avdtp: no answer to signal 0x%02x", s.pending);
        s.pending = 0;
        if (s.step != STEP_NONE && s.step < STEP_START)
            setup_failed("no answer");
        else
            s.step = STEP_NONE;
    }
}

uint16_t avdtp_max_media(void)
{
    if (!s.media)
        return 0;
    return l2cap_max_send(s.media) - 13;   /* RTP header and SBC header */
}

bool avdtp_send_sbc(const uint8_t *data, uint16_t len, uint8_t frames, uint32_t samples)
{
    static uint8_t pkt[L2CAP_TX_MAX];
    if (s.state != ST_STREAMING || !s.media || len > avdtp_max_media() ||
        !l2cap_can_send(s.media, len + 13))
        return false;
    pkt[0] = 0x80;                 /* RTP version 2 */
    pkt[1] = 96;                   /* dynamic payload type */
    lbt_put16be(pkt + 2, s.seq);
    lbt_put32be(pkt + 4, s.timestamp);
    lbt_put32be(pkt + 8, 1);       /* SSRC */
    pkt[12] = frames & 0x0f;       /* SBC header: frame count, not fragmented */
    memcpy(pkt + 13, data, len);
    if (!l2cap_send(s.media, pkt, len + 13))
        return false;
    s.seq++;
    s.timestamp += samples;
    return true;
}

bool avdtp_is_streaming(void)
{
    return s.state == ST_STREAMING;
}

bool avdtp_can_send(uint16_t len)
{
    return s.state == ST_STREAMING && s.media && len <= avdtp_max_media() &&
           l2cap_can_send(s.media, len + 13);
}
