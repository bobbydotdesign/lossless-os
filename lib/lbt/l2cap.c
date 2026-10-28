/*
 * lbt L2CAP: signaling, configuration and data in basic mode, and in
 * Enhanced Retransmission Mode for the services that ask for it (OBEX for
 * cover art: GOEP 2.0 runs OBEX over L2CAP in ERTM)
 * (Bluetooth Core, Vol 3 Part A, sections 3 to 5 and 8).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "l2cap.h"
#include "hci.h"
#include "lbt_platform.h"

#define CID_SIGNALING 0x0001
#define CID_DYNAMIC   0x0040

/* signaling command codes */
#define SIG_COMMAND_REJECT  0x01
#define SIG_CONN_REQ        0x02
#define SIG_CONN_RSP        0x03
#define SIG_CONFIG_REQ      0x04
#define SIG_CONFIG_RSP      0x05
#define SIG_DISCONN_REQ     0x06
#define SIG_DISCONN_RSP     0x07
#define SIG_ECHO_REQ        0x08
#define SIG_ECHO_RSP        0x09
#define SIG_INFO_REQ        0x0a
#define SIG_INFO_RSP        0x0b

/* configuration options and results */
#define OPT_MTU             0x01
#define OPT_FLUSH_TIMEOUT   0x02
#define OPT_QOS             0x03
#define OPT_RETRANSMISSION  0x04
#define OPT_FCS             0x05
#define OPT_HINT            0x80
#define CONF_SUCCESS        0x0000
#define CONF_UNACCEPTABLE   0x0001
#define CONF_REJECTED       0x0002
#define CONF_UNKNOWN        0x0003

#define MODE_BASIC          0x00
#define MODE_ERTM           0x03

/* ERTM (Vol 3 Part A 3.3.2, 5.4, 8): one channel at a time is plenty */
#define ERTM_CHANNELS       1
#define ERTM_WIN            8      /* I-frames we take unacknowledged */
#define ERTM_MPS            1004   /* most payload in one I-frame we take */
#define ERTM_SDU            8192   /* largest SDU either way */
#define ERTM_RETRANS_MS     2000
#define ERTM_MONITOR_MS     12000
#define S_RR                0
#define S_REJ               1
#define S_RNR               2
#define S_SREJ              3
#define SAR_ONE             0
#define SAR_START           1
#define SAR_END             2
#define SAR_CONT            3

struct ertm {
    bool used;
    uint8_t next_tx_seq;    /* the next new I-frame of ours */
    uint8_t expected_ack;   /* our oldest unacknowledged */
    uint8_t expected_tx;    /* their next we want */
    uint8_t remote_win, max_transmit;
    uint16_t remote_mps;
    bool remote_busy;       /* RNR from them */
    bool rej_sent;          /* REJ out, until the frame comes */
    bool ack_due;           /* theirs taken, not acknowledged yet */
    bool polling;           /* our poll out, waiting for their final */
    bool resend;            /* frames from resend_from to send again */
    uint8_t resend_from;
    uint8_t polls;
    uint32_t timer_at;      /* retransmission or monitor time-out; 0 off */
    /* the SDU we're sending (one at a time): our frames are parts of it */
    uint8_t sdu[ERTM_SDU];
    uint16_t sdu_len, sdu_next;
    struct { uint16_t off, len; uint8_t sar; } frame[64];   /* by TxSeq */
    /* theirs, put back together */
    uint8_t rx[ERTM_SDU];
    uint16_t rx_len, rx_total;
};

static struct ertm ertms[ERTM_CHANNELS];

enum chan_state {
    CHAN_FREE = 0,
    CHAN_WAIT_CONN_RSP,   /* we asked to open it */
    CHAN_CONFIG,          /* both sides configuring */
    CHAN_OPEN,
    CHAN_WAIT_DISCONN_RSP,
};

struct l2cap_chan {
    enum chan_state state;
    uint16_t handle;
    uint16_t psm;
    uint16_t local_cid, remote_cid;
    uint16_t local_mtu;     /* most we take */
    uint16_t remote_mtu;    /* most they take */
    bool local_config_done; /* they accepted our configuration */
    bool remote_config_done;/* we accepted theirs */
    bool initiator;
    bool was_open;          /* told the profile it opened */
    bool want_ertm;         /* its service asks for ERTM */
    uint8_t mode;           /* MODE_BASIC or MODE_ERTM, once configured */
    bool fcs;               /* frame check sequence on ERTM frames */
    uint8_t remote_win, remote_max_transmit;
    uint16_t remote_mps;
    struct ertm *e;
    uint8_t pending_id;     /* id of our request awaiting an answer */
    const l2cap_ops *ops;
    void *user;
};

static struct l2cap_chan chans[L2CAP_MAX_CHANNELS];

static struct {
    uint16_t psm;
    const l2cap_ops *ops;
    uint16_t mtu;
    bool ertm;
} services[L2CAP_MAX_SERVICES];

static uint8_t next_id = 1;
static uint16_t next_cid = CID_DYNAMIC;
static uint8_t pdu[L2CAP_TX_MAX];

/* ---- helpers ------------------------------------------------------------------- */

static uint8_t new_id(void)
{
    uint8_t id = next_id++;
    if (next_id == 0)
        next_id = 1;
    return id;
}

static struct l2cap_chan *chan_by_local(uint16_t handle, uint16_t cid)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++)
        if (chans[i].state != CHAN_FREE && chans[i].handle == handle &&
            chans[i].local_cid == cid)
            return &chans[i];
    return NULL;
}

static struct l2cap_chan *chan_by_pending(uint16_t handle, uint8_t id)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++)
        if (chans[i].state != CHAN_FREE && chans[i].handle == handle &&
            chans[i].pending_id == id)
            return &chans[i];
    return NULL;
}

static struct l2cap_chan *chan_alloc(uint16_t handle)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (chans[i].state == CHAN_FREE) {
            struct l2cap_chan *c = &chans[i];
            memset(c, 0, sizeof(*c));
            c->handle = handle;
            /* a local CID not in use on this connection */
            do {
                c->local_cid = next_cid++;
                if (next_cid < CID_DYNAMIC)
                    next_cid = CID_DYNAMIC;
            } while (chan_by_local(handle, c->local_cid));
            c->remote_mtu = L2CAP_DEFAULT_MTU;
            return c;
        }
    }
    return NULL;
}

/* Send one signaling command */
static void send_signal(uint16_t handle, uint8_t code, uint8_t id,
                        const uint8_t *data, uint16_t len)
{
    uint8_t buf[4 + 4 + 64];
    if (len > 64)
        len = 64;
    lbt_put16(buf, 4 + len);
    lbt_put16(buf + 2, CID_SIGNALING);
    buf[4] = code;
    buf[5] = id;
    lbt_put16(buf + 6, len);
    memcpy(buf + 8, data, len);
    if (!hci_send_pdu(handle, buf, 8 + len))
        lbt_platform_log("l2cap: no room to send signal 0x%02x", code);
}

static void send_config_request(struct l2cap_chan *c)
{
    uint8_t d[8 + 11 + 3];
    uint16_t n = 8;
    lbt_put16(d, c->remote_cid);
    lbt_put16(d + 2, 0);          /* flags: no continuation */
    d[4] = OPT_MTU;
    d[5] = 2;
    lbt_put16(d + 6, c->local_mtu);
    if (c->want_ertm) {
        /* what we take: window, retries (theirs), time-outs 0 here, MPS */
        d[n++] = OPT_RETRANSMISSION;
        d[n++] = 9;
        d[n++] = MODE_ERTM;
        d[n++] = ERTM_WIN;
        d[n++] = 3;
        lbt_put16(d + n, 0);
        lbt_put16(d + n + 2, 0);
        lbt_put16(d + n + 4, ERTM_MPS);
        n += 6;
        d[n++] = OPT_FCS;
        d[n++] = 1;
        d[n++] = 0x01;            /* 16-bit FCS */
    }
    c->pending_id = new_id();
    send_signal(c->handle, SIG_CONFIG_REQ, c->pending_id, d, n);
}

static void chan_free(struct l2cap_chan *c)
{
    if (c->e)
        c->e->used = false;
    c->e = NULL;
    c->state = CHAN_FREE;
}

/* Opening failed: tell the profile and forget the channel */
static void open_failed(struct l2cap_chan *c, uint16_t result)
{
    const l2cap_ops *ops = c->ops;
    chan_free(c);
    if (ops && ops->opened)
        ops->opened(c, result);
}

static void check_open(struct l2cap_chan *c)
{
    if (c->state == CHAN_CONFIG && c->local_config_done && c->remote_config_done &&
        c->mode == MODE_ERTM && !c->e) {
        for (int i = 0; i < ERTM_CHANNELS && !c->e; i++)
            if (!ertms[i].used) {
                c->e = &ertms[i];
                memset(c->e, 0, sizeof(*c->e));
                c->e->used = true;
                c->e->remote_win = c->remote_win ? c->remote_win : 1;
                c->e->max_transmit = c->remote_max_transmit;
                c->e->remote_mps = c->remote_mps;
            }
        if (!c->e) {
            lbt_platform_log("l2cap: no room for another ERTM channel");
            l2cap_disconnect(c);
            return;
        }
    }
    if (c->state == CHAN_CONFIG && c->local_config_done && c->remote_config_done) {
        c->state = CHAN_OPEN;
        c->was_open = true;
        if (c->ops && c->ops->opened)
            c->ops->opened(c, L2CAP_RESULT_SUCCESS);
    }
}

/* An open (or closing) channel is gone */
static void chan_closed(struct l2cap_chan *c)
{
    const l2cap_ops *ops = c->ops;
    if (!c->was_open) {
        open_failed(c, L2CAP_RESULT_LINK_LOST);
        return;
    }
    chan_free(c);
    if (ops && ops->closed)
        ops->closed(c);
}

/* ---- signaling in ------------------------------------------------------------------ */

static void handle_conn_req(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 4)
        return;
    uint16_t psm = lbt_get16(d);
    uint16_t scid = lbt_get16(d + 2);
    uint8_t rsp[8];
    lbt_put16(rsp + 2, scid);
    lbt_put16(rsp + 6, 0);   /* status: no further information */

    int s;
    for (s = 0; s < L2CAP_MAX_SERVICES; s++)
        if (services[s].ops && services[s].psm == psm)
            break;
    if (s == L2CAP_MAX_SERVICES) {
        lbt_put16(rsp, 0);
        lbt_put16(rsp + 4, L2CAP_RESULT_PSM_NOT_SUPPORTED);
        send_signal(handle, SIG_CONN_RSP, id, rsp, sizeof(rsp));
        return;
    }
    struct l2cap_chan *c = chan_alloc(handle);
    if (!c) {
        lbt_put16(rsp, 0);
        lbt_put16(rsp + 4, L2CAP_RESULT_NO_RESOURCES);
        send_signal(handle, SIG_CONN_RSP, id, rsp, sizeof(rsp));
        return;
    }
    c->state = CHAN_CONFIG;
    c->psm = psm;
    c->remote_cid = scid;
    c->local_mtu = services[s].mtu;
    c->ops = services[s].ops;
    c->want_ertm = services[s].ertm;
    lbt_put16(rsp, c->local_cid);
    lbt_put16(rsp + 4, L2CAP_RESULT_SUCCESS);
    send_signal(handle, SIG_CONN_RSP, id, rsp, sizeof(rsp));
    send_config_request(c);
}

static void handle_conn_rsp(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 8)
        return;
    uint16_t dcid = lbt_get16(d);
    uint16_t scid = lbt_get16(d + 2);
    uint16_t result = lbt_get16(d + 4);
    struct l2cap_chan *c = chan_by_local(handle, scid);
    if (!c || c->state != CHAN_WAIT_CONN_RSP || c->pending_id != id)
        return;
    if (result == L2CAP_RESULT_PENDING)
        return;   /* e.g. waiting for authentication: another response follows */
    c->pending_id = 0;
    if (result != L2CAP_RESULT_SUCCESS) {
        open_failed(c, result);
        return;
    }
    c->remote_cid = dcid;
    c->state = CHAN_CONFIG;
    send_config_request(c);
}

static void handle_config_req(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 4)
        return;
    uint16_t dcid = lbt_get16(d);
    uint16_t flags = lbt_get16(d + 2);
    struct l2cap_chan *c = chan_by_local(handle, dcid);
    if (!c || (c->state != CHAN_CONFIG && c->state != CHAN_OPEN)) {
        /* invalid CID: reject with reason 0x0002 and the CIDs */
        uint8_t rej[6] = { 0x02, 0x00 };
        lbt_put16(rej + 2, dcid);
        lbt_put16(rej + 4, 0);
        send_signal(handle, SIG_COMMAND_REJECT, id, rej, sizeof(rej));
        return;
    }

    uint8_t rsp[6 + 32];
    uint16_t rlen = 6;
    uint16_t result = CONF_SUCCESS;
    bool ertm_ok = false;
    uint8_t ertm_opt[9];
    uint8_t unknown[16];
    int n_unknown = 0;

    for (uint16_t off = 4; off + 2 <= len; ) {
        uint8_t type = d[off];
        uint8_t olen = d[off + 1];
        const uint8_t *val = d + off + 2;
        if (off + 2 + olen > len)
            break;
        switch (type & ~OPT_HINT) {
        case OPT_MTU:
            if (olen >= 2) {
                uint16_t mtu = lbt_get16(val);
                c->remote_mtu = mtu < L2CAP_MIN_MTU ? L2CAP_MIN_MTU : mtu;
            }
            break;
        case OPT_FLUSH_TIMEOUT:
        case OPT_QOS:
        case OPT_FCS:
            break;   /* fine as offered (we always send an FCS in ERTM, so it's on) */
        case OPT_RETRANSMISSION:
            if (olen >= 9 && val[0] == MODE_ERTM && c->want_ertm) {
                /* what they take: our window, retries and MPS towards them */
                c->mode = MODE_ERTM;
                c->fcs = true;
                c->remote_win = val[1] > 63 ? 63 : val[1];
                c->remote_max_transmit = val[2];
                uint16_t mps = lbt_get16(val + 7);
                c->remote_mps = mps < ERTM_MPS ? mps : ERTM_MPS;
                ertm_ok = true;
                memcpy(ertm_opt, val, 9);
                lbt_put16(ertm_opt + 3, ERTM_RETRANS_MS);   /* ours, in the answer */
                lbt_put16(ertm_opt + 5, ERTM_MONITOR_MS);
                lbt_put16(ertm_opt + 7, c->remote_mps);
            } else if (olen >= 1 && val[0] != 0x00 && result == CONF_SUCCESS) {
                /* only basic mode: offer that instead */
                result = CONF_UNACCEPTABLE;
                rsp[rlen++] = OPT_RETRANSMISSION;
                rsp[rlen++] = 9;
                memset(rsp + rlen, 0, 9);
                rlen += 9;
            }
            break;
        default:
            if (!(type & OPT_HINT) && n_unknown < (int)sizeof(unknown))
                unknown[n_unknown++] = type;
            break;
        }
        off += 2 + olen;
    }

    if (n_unknown) {
        result = CONF_UNKNOWN;
        rlen = 6;
        memcpy(rsp + rlen, unknown, n_unknown);
        rlen += n_unknown;
    }

    if (result == CONF_SUCCESS && ertm_ok) {
        rsp[rlen++] = OPT_RETRANSMISSION;
        rsp[rlen++] = 9;
        memcpy(rsp + rlen, ertm_opt, 9);
        rlen += 9;
    }
    lbt_put16(rsp, c->remote_cid);
    lbt_put16(rsp + 2, flags & 0x0001);   /* echo continuation */
    lbt_put16(rsp + 4, result);
    send_signal(handle, SIG_CONFIG_RSP, id, rsp, rlen);

    if (result == CONF_SUCCESS && !(flags & 0x0001)) {
        c->remote_config_done = true;
        check_open(c);
    }
}

static void handle_config_rsp(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 6)
        return;
    uint16_t scid = lbt_get16(d);
    uint16_t flags = lbt_get16(d + 2);
    uint16_t result = lbt_get16(d + 4);
    struct l2cap_chan *c = chan_by_local(handle, scid);
    if (!c || c->state != CHAN_CONFIG || c->pending_id != id)
        return;
    c->pending_id = 0;

    if (result == CONF_SUCCESS) {
        if (flags & 0x0001)
            return;   /* more to come */
        c->local_config_done = true;
        check_open(c);
        return;
    }
    if (result == CONF_UNACCEPTABLE && c->want_ertm) {
        /* no ERTM with them: basic mode, then */
        lbt_platform_log("l2cap: psm 0x%04x: they won't take ERTM; basic mode", c->psm);
        c->want_ertm = false;
        c->mode = MODE_BASIC;
        send_config_request(c);
        return;
    }
    if (result == CONF_UNACCEPTABLE) {
        /* take their MTU suggestion if that's all it is, and try again */
        for (uint16_t off = 6; off + 2 <= len; off += 2 + d[off + 1]) {
            if ((d[off] & ~OPT_HINT) == OPT_MTU && d[off + 1] >= 2 &&
                off + 4 <= len) {
                uint16_t mtu = lbt_get16(d + off + 2);
                if (mtu >= L2CAP_MIN_MTU) {
                    c->local_mtu = mtu;
                    send_config_request(c);
                    return;
                }
            }
        }
    }
    lbt_platform_log("l2cap: configuration of psm 0x%04x refused (0x%04x)",
                     c->psm, result);
    l2cap_disconnect(c);
}

static void handle_disconn_req(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 4)
        return;
    uint16_t dcid = lbt_get16(d);
    uint16_t scid = lbt_get16(d + 2);
    send_signal(handle, SIG_DISCONN_RSP, id, d, 4);
    struct l2cap_chan *c = chan_by_local(handle, dcid);
    if (c && c->remote_cid == scid)
        chan_closed(c);
}

static void handle_disconn_rsp(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    (void)id;
    if (len < 4)
        return;
    struct l2cap_chan *c = chan_by_local(handle, lbt_get16(d + 2));
    if (c && c->state == CHAN_WAIT_DISCONN_RSP)
        chan_closed(c);
}

static void handle_info_req(uint16_t handle, uint8_t id, const uint8_t *d, uint16_t len)
{
    if (len < 2)
        return;
    uint16_t type = lbt_get16(d);
    uint8_t rsp[4 + 8];
    uint16_t rlen = 4;
    lbt_put16(rsp, type);
    lbt_put16(rsp + 2, 0x0000);   /* success */
    switch (type) {
    case 0x0001:   /* connectionless MTU */
        lbt_put16(rsp + 4, L2CAP_DEFAULT_MTU);
        rlen += 2;
        break;
    case 0x0002:   /* extended features: ERTM, the FCS option, fixed channels */
        lbt_put32(rsp + 4, 0x00000008 | 0x00000020 | 0x00000080);
        rlen += 4;
        break;
    case 0x0003:   /* fixed channels: signaling only */
        memset(rsp + 4, 0, 8);
        rsp[4] = 0x02;
        rlen += 8;
        break;
    default:
        lbt_put16(rsp + 2, 0x0001);   /* not supported */
        break;
    }
    send_signal(handle, SIG_INFO_RSP, id, rsp, rlen);
}

static void handle_reject(uint16_t handle, uint8_t id)
{
    struct l2cap_chan *c = chan_by_pending(handle, id);
    if (!c)
        return;
    c->pending_id = 0;
    if (c->state == CHAN_WAIT_DISCONN_RSP)
        chan_closed(c);
    else if (c->state != CHAN_OPEN)
        open_failed(c, L2CAP_RESULT_REJECTED);
}

static void signaling(uint16_t handle, const uint8_t *data, uint16_t len)
{
    /* one PDU can hold several commands */
    while (len >= 4) {
        uint8_t code = data[0], id = data[1];
        uint16_t clen = lbt_get16(data + 2);
        const uint8_t *d = data + 4;
        if (4 + clen > len)
            return;
        switch (code) {
        case SIG_COMMAND_REJECT: handle_reject(handle, id); break;
        case SIG_CONN_REQ:       handle_conn_req(handle, id, d, clen); break;
        case SIG_CONN_RSP:       handle_conn_rsp(handle, id, d, clen); break;
        case SIG_CONFIG_REQ:     handle_config_req(handle, id, d, clen); break;
        case SIG_CONFIG_RSP:     handle_config_rsp(handle, id, d, clen); break;
        case SIG_DISCONN_REQ:    handle_disconn_req(handle, id, d, clen); break;
        case SIG_DISCONN_RSP:    handle_disconn_rsp(handle, id, d, clen); break;
        case SIG_ECHO_REQ:       send_signal(handle, SIG_ECHO_RSP, id, d, clen); break;
        case SIG_INFO_REQ:       handle_info_req(handle, id, d, clen); break;
        case SIG_ECHO_RSP:
        case SIG_INFO_RSP:
            break;
        default: {
            uint8_t rej[2] = { 0x00, 0x00 };   /* command not understood */
            send_signal(handle, SIG_COMMAND_REJECT, id, rej, 2);
            break;
        }
        }
        data += 4 + clen;
        len -= 4 + clen;
    }
}

/* ---- ERTM ---------------------------------------------------------------------------- */

/* The FCS: CRC-16, polynomial x^16 + x^15 + x^2 + 1, from 0, least
 * significant bit first (Vol 3 Part A 3.3.5) */
static uint16_t fcs16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint8_t unacked(const struct ertm *e)
{
    return (uint8_t)((e->next_tx_seq - e->expected_ack) & 63);
}

static uint32_t later(uint32_t ms)
{
    uint32_t t = lbt_platform_ms() + ms;
    return t ? t : 1;
}

/* Finish a PDU in 'pdu' (control and payload from byte 4): length, CID, FCS */
static bool ertm_out(struct l2cap_chan *c, uint16_t body)
{
    uint16_t plen = (uint16_t)(body + (c->fcs ? 2 : 0));
    lbt_put16(pdu, plen);
    lbt_put16(pdu + 2, c->remote_cid);
    if (c->fcs)
        lbt_put16(pdu + 4 + body, fcs16(pdu, (uint16_t)(4 + body)));
    return hci_send_pdu(c->handle, pdu, (uint16_t)(4 + plen));
}

static bool send_sframe(struct l2cap_chan *c, uint8_t type, bool poll, bool final)
{
    uint16_t ctrl = (uint16_t)(0x0001 | type << 2 | (poll ? 0x0010 : 0) | (final ? 0x0080 : 0) |
                               c->e->expected_tx << 8);
    lbt_put16(pdu + 4, ctrl);
    if (!ertm_out(c, 2))
        return false;
    c->e->ack_due = false;
    return true;
}

static bool send_iframe(struct l2cap_chan *c, uint8_t seq, bool final)
{
    struct ertm *e = c->e;
    uint8_t sar = e->frame[seq].sar;
    uint16_t n = e->frame[seq].len, at = 6;
    uint16_t ctrl = (uint16_t)(seq << 1 | (final ? 0x0080 : 0) | e->expected_tx << 8 | sar << 14);
    lbt_put16(pdu + 4, ctrl);
    if (sar == SAR_START) {
        lbt_put16(pdu + at, e->sdu_len);
        at += 2;
    }
    memcpy(pdu + at, e->sdu + e->frame[seq].off, n);
    if (!ertm_out(c, (uint16_t)(at - 4 + n)))
        return false;
    e->ack_due = false;
    return true;
}

/* Send what can go: frames to send again, then new ones in the window */
static void ertm_push(struct l2cap_chan *c)
{
    struct ertm *e = c->e;
    if (e->resend) {
        while (e->resend_from != e->next_tx_seq) {
            if (!send_iframe(c, e->resend_from, false))
                return;
            e->resend_from = (uint8_t)((e->resend_from + 1) & 63);
        }
        e->resend = false;
        if (!e->polling)
            e->timer_at = later(ERTM_RETRANS_MS);
    }
    while (!e->remote_busy && !e->polling && e->sdu_next < e->sdu_len &&
           unacked(e) < e->remote_win) {
        uint16_t left = (uint16_t)(e->sdu_len - e->sdu_next), room = e->remote_mps;
        uint8_t sar;
        if (e->sdu_next == 0) {
            if (left <= room) {
                sar = SAR_ONE;
            } else {
                sar = SAR_START;
                room -= 2;                      /* the SDU length goes in too */
            }
        } else {
            sar = left <= room ? SAR_END : SAR_CONT;
        }
        uint16_t n = left < room ? left : room;
        uint8_t seq = e->next_tx_seq;
        e->frame[seq].off = e->sdu_next;
        e->frame[seq].len = n;
        e->frame[seq].sar = sar;
        if (!send_iframe(c, seq, false))
            return;                             /* HCI is full: the tick tries again */
        e->sdu_next = (uint16_t)(e->sdu_next + n);
        e->next_tx_seq = (uint8_t)((seq + 1) & 63);
        if (!e->timer_at)
            e->timer_at = later(ERTM_RETRANS_MS);
    }
}

/* Their ReqSeq: ours before it arrived */
static void ertm_ack(struct l2cap_chan *c, uint8_t req)
{
    struct ertm *e = c->e;
    uint8_t n = (uint8_t)((req - e->expected_ack) & 63);
    if (n > unacked(e))
        return;                                 /* not one of ours */
    e->expected_ack = req;
    if (n && !e->polling) {
        e->polls = 0;
        e->timer_at = unacked(e) ? later(ERTM_RETRANS_MS) : 0;
    }
    if (!unacked(e) && e->sdu_next == e->sdu_len)
        e->sdu_len = e->sdu_next = 0;           /* the SDU is through */
}

/* Their final answer to our poll: everything from ReqSeq again */
static void ertm_final(struct l2cap_chan *c, uint8_t req)
{
    struct ertm *e = c->e;
    if (!e->polling)
        return;
    e->polling = false;
    e->polls = 0;
    if (unacked(e)) {
        e->resend = true;
        e->resend_from = req;
        e->timer_at = later(ERTM_RETRANS_MS);
    } else {
        e->timer_at = 0;
    }
}

static void ertm_deliver(struct l2cap_chan *c, const uint8_t *d, uint16_t n)
{
    if (c->ops && c->ops->data)
        c->ops->data(c, d, n);
}

static void ertm_rx(struct l2cap_chan *c, const uint8_t *whole, uint16_t total)
{
    struct ertm *e = c->e;
    if (c->fcs) {
        if (total < 4 + 2 + 2 || fcs16(whole, (uint16_t)(total - 2)) != lbt_get16(whole + total - 2))
            return;                             /* damaged: as if lost */
        total -= 2;
    }
    if (total < 6)
        return;
    const uint8_t *p = whole + 6;
    uint16_t len = (uint16_t)(total - 6);
    uint16_t ctrl = lbt_get16(whole + 4);
    uint8_t req = (ctrl >> 8) & 63;
    bool final = ctrl & 0x0080;

    if (ctrl & 0x0001) {                        /* S-frame */
        uint8_t type = (ctrl >> 2) & 3;
        bool poll = ctrl & 0x0010;
        if (type == S_SREJ) {                   /* that one again (not an acknowledgement) */
            if (((req - e->expected_ack) & 63) < unacked(e))
                send_iframe(c, req, poll);
            return;
        }
        ertm_ack(c, req);
        e->remote_busy = type == S_RNR;
        if (type == S_REJ) {
            e->resend = true;
            e->resend_from = req;
        }
        if (final)
            ertm_final(c, req);
        if (poll)
            send_sframe(c, S_RR, false, true);
        ertm_push(c);
        return;
    }

    /* I-frame */
    uint8_t txseq = (ctrl >> 1) & 63, sar = (uint8_t)(ctrl >> 14);
    ertm_ack(c, req);
    if (final)
        ertm_final(c, req);
    if (txseq != e->expected_tx) {
        if (((e->expected_tx - txseq) & 63) <= ERTM_WIN) {
            e->ack_due = true;                  /* one we have: say so again */
        } else if (!e->rej_sent) {
            e->rej_sent = true;                 /* a gap: from expected_tx again */
            send_sframe(c, S_REJ, false, false);
        }
        ertm_push(c);
        if (e->ack_due)
            send_sframe(c, S_RR, false, false);
        return;
    }
    e->rej_sent = false;
    e->expected_tx = (uint8_t)((txseq + 1) & 63);
    e->ack_due = true;
    switch (sar) {
    case SAR_ONE:
        e->rx_len = e->rx_total = 0;
        ertm_deliver(c, p, len);
        break;
    case SAR_START:
        e->rx_len = 0;
        e->rx_total = len >= 2 ? lbt_get16(p) : 0;
        if (len < 2 || e->rx_total > ERTM_SDU || len - 2 > e->rx_total) {
            e->rx_total = 0;
            break;
        }
        memcpy(e->rx, p + 2, len - 2);
        e->rx_len = (uint16_t)(len - 2);
        break;
    default:                                    /* continuation, end */
        if (!e->rx_total || e->rx_len + len > e->rx_total) {
            e->rx_len = e->rx_total = 0;        /* not part of anything */
            break;
        }
        memcpy(e->rx + e->rx_len, p, len);
        e->rx_len = (uint16_t)(e->rx_len + len);
        if (sar == SAR_END) {
            uint16_t n = e->rx_len;
            e->rx_len = e->rx_total = 0;
            ertm_deliver(c, e->rx, n);
        }
        break;
    }
    if (c->state != CHAN_OPEN || !c->e)
        return;                                 /* the profile closed it */
    ertm_push(c);
    if (e->ack_due)
        send_sframe(c, S_RR, false, false);
}

/* ---- from HCI ------------------------------------------------------------------------ */

static void pdu_received(uint16_t handle, const uint8_t *data, uint16_t len)
{
    if (len < 4)
        return;
    uint16_t plen = lbt_get16(data);
    uint16_t cid = lbt_get16(data + 2);
    if (plen + 4 != len)
        return;
    if (cid == CID_SIGNALING) {
        signaling(handle, data + 4, plen);
        return;
    }
    struct l2cap_chan *c = chan_by_local(handle, cid);
    if (c && c->state == CHAN_OPEN && c->e) {
        ertm_rx(c, data, len);
        return;
    }
    if (c && c->state == CHAN_OPEN && c->ops && c->ops->data)
        c->ops->data(c, data + 4, plen);
}

/* For the replay test, which sits between HCI and L2CAP */
void l2cap_pdu_received_for_tests(uint16_t handle, const uint8_t *data, uint16_t len);
void l2cap_pdu_received_for_tests(uint16_t handle, const uint8_t *data, uint16_t len)
{
    pdu_received(handle, data, len);
}

static void event(uint8_t code, const uint8_t *p, uint8_t len)
{
    if (code == HCI_EV_DISCONNECTION_COMPLETE && len >= 4 && p[0] == 0) {
        uint16_t handle = lbt_get16(p + 1) & 0x0fff;
        for (int i = 0; i < L2CAP_MAX_CHANNELS; i++)
            if (chans[i].state != CHAN_FREE && chans[i].handle == handle)
                chan_closed(&chans[i]);
    }
}

/* ---- API ------------------------------------------------------------------------------ */

void l2cap_init(void)
{
    memset(chans, 0, sizeof(chans));
    memset(services, 0, sizeof(services));
    memset(ertms, 0, sizeof(ertms));
    next_id = 1;
    next_cid = CID_DYNAMIC;
    hci_set_pdu_handler(pdu_received);
    hci_add_event_handler(event);
}

bool l2cap_listen(uint16_t psm, const l2cap_ops *ops, uint16_t mtu)
{
    for (int i = 0; i < L2CAP_MAX_SERVICES; i++) {
        if (!services[i].ops) {
            services[i].psm = psm;
            services[i].ops = ops;
            services[i].mtu = mtu < L2CAP_MIN_MTU ? L2CAP_MIN_MTU : mtu;
            services[i].ertm = false;
            return true;
        }
    }
    return false;
}

bool l2cap_listen_ertm(uint16_t psm, const l2cap_ops *ops, uint16_t mtu)
{
    if (!l2cap_listen(psm, ops, mtu))
        return false;
    for (int i = 0; i < L2CAP_MAX_SERVICES; i++)
        if (services[i].ops == ops && services[i].psm == psm)
            services[i].ertm = true;
    return true;
}

void l2cap_tick(void)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        struct l2cap_chan *c = &chans[i];
        if (c->state != CHAN_OPEN || !c->e)
            continue;
        struct ertm *e = c->e;
        ertm_push(c);
        if (e->ack_due)
            send_sframe(c, S_RR, false, false);
        if (e->timer_at && (int32_t)(lbt_platform_ms() - e->timer_at) >= 0) {
            /* no acknowledgement: ask where they are (poll), a few times */
            if (e->max_transmit && e->polls >= e->max_transmit) {
                lbt_platform_log("l2cap: psm 0x%04x: no answer to our polls", c->psm);
                l2cap_disconnect(c);
                continue;
            }
            e->polls++;
            e->polling = true;
            send_sframe(c, S_RR, true, false);
            e->timer_at = later(ERTM_MONITOR_MS);
        }
    }
}

l2cap_chan *l2cap_connect(uint16_t handle, uint16_t psm, const l2cap_ops *ops,
                          uint16_t mtu, void *user)
{
    struct l2cap_chan *c = chan_alloc(handle);
    if (!c)
        return NULL;
    c->state = CHAN_WAIT_CONN_RSP;
    c->psm = psm;
    c->ops = ops;
    c->user = user;
    c->initiator = true;
    c->local_mtu = mtu < L2CAP_MIN_MTU ? L2CAP_MIN_MTU : mtu;
    c->pending_id = new_id();
    uint8_t d[4];
    lbt_put16(d, psm);
    lbt_put16(d + 2, c->local_cid);
    send_signal(handle, SIG_CONN_REQ, c->pending_id, d, sizeof(d));
    return c;
}

uint16_t l2cap_max_send(const l2cap_chan *c)
{
    uint16_t m = c->remote_mtu;
    if (c->e)
        return m > ERTM_SDU ? ERTM_SDU : m;
    return m > L2CAP_TX_MAX - 4 ? L2CAP_TX_MAX - 4 : m;
}

bool l2cap_can_send(const l2cap_chan *c, uint16_t len)
{
    if (c->e)
        return c->state == CHAN_OPEN && !c->e->sdu_len && len <= l2cap_max_send(c);
    return c->state == CHAN_OPEN && len <= l2cap_max_send(c) &&
           hci_fragments_for(len + 4) <= hci_tx_free();
}

bool l2cap_send(l2cap_chan *c, const uint8_t *data, uint16_t len)
{
    if (c->state != CHAN_OPEN || len > l2cap_max_send(c))
        return false;
    if (c->e) {
        /* ERTM: one SDU at a time, in I-frames as the window allows */
        if (c->e->sdu_len || !len)
            return false;
        memcpy(c->e->sdu, data, len);
        c->e->sdu_len = len;
        c->e->sdu_next = 0;
        ertm_push(c);
        return true;
    }
    lbt_put16(pdu, len);
    lbt_put16(pdu + 2, c->remote_cid);
    memcpy(pdu + 4, data, len);
    return hci_send_pdu(c->handle, pdu, 4 + len);
}

void l2cap_disconnect(l2cap_chan *c)
{
    if (c->state == CHAN_FREE || c->state == CHAN_WAIT_DISCONN_RSP)
        return;
    if (c->state == CHAN_WAIT_CONN_RSP) {
        /* no CID from them yet: just forget it */
        open_failed(c, L2CAP_RESULT_REJECTED);
        return;
    }
    /* a channel still configuring reports a failed open when it's gone */
    c->state = CHAN_WAIT_DISCONN_RSP;
    uint8_t d[4];
    lbt_put16(d, c->remote_cid);
    lbt_put16(d + 2, c->local_cid);
    c->pending_id = new_id();
    send_signal(c->handle, SIG_DISCONN_REQ, c->pending_id, d, sizeof(d));
}

uint16_t l2cap_handle(const l2cap_chan *c) { return c->handle; }
uint16_t l2cap_psm(const l2cap_chan *c) { return c->psm; }
bool l2cap_is_open(const l2cap_chan *c) { return c->state == CHAN_OPEN; }
void *l2cap_user(const l2cap_chan *c) { return c->user; }
void l2cap_set_user(l2cap_chan *c, void *user) { c->user = user; }
