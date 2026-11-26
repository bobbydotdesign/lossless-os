/*
 * lbt host tests: L2CAP plumbing for profile tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chan.h"
#include "../lbt_platform.h"

uint16_t chan_handle = 0x000c;

void chan_signal_in(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t pdu[8 + 64];
    lbt_put16(pdu, 4 + len);
    lbt_put16(pdu + 2, 0x0001);
    pdu[4] = code; pdu[5] = id;
    lbt_put16(pdu + 6, len);
    if (len)
        memcpy(pdu + 8, d, len);
    fake_acl(chan_handle, 2, pdu, 8 + len);
}

struct sent_packet *chan_next(void)
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
    fake_completed(chan_handle, 1);
    return &out;
}

uint16_t chan_accept_ours(uint16_t their_cid)
{
    struct sent_packet *p = chan_next();
    if (!p || p->data[4] != 0x02)
        return 0;
    uint8_t id = p->data[5];
    uint16_t ours = lbt_get16(p->data + 10);
    uint8_t rsp[8] = { 0 };
    lbt_put16(rsp, their_cid);
    lbt_put16(rsp + 2, ours);
    chan_signal_in(0x03, id, rsp, 8);
    p = chan_next();
    uint8_t cid_id = p ? p->data[5] : 0;
    uint8_t req[8] = { 0, 0, 0, 0, 0x01, 0x02, 0xff, 0x03 };
    lbt_put16(req, ours);
    chan_signal_in(0x04, 0x40, req, 8);
    chan_next();
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ours);
    chan_signal_in(0x05, cid_id, ok, 6);
    return ours;
}

uint16_t chan_open_theirs(uint16_t psm, uint16_t their_cid)
{
    uint8_t req[4];
    lbt_put16(req, psm);
    lbt_put16(req + 2, their_cid);
    chan_signal_in(0x02, 0x41, req, 4);
    struct sent_packet *p = chan_next();
    uint16_t ours = p ? lbt_get16(p->data + 8) : 0;
    p = chan_next();
    uint8_t cid_id = p ? p->data[5] : 0;
    uint8_t creq[8] = { 0, 0, 0, 0, 0x01, 0x02, 0xff, 0x03 };
    lbt_put16(creq, ours);
    chan_signal_in(0x04, 0x42, creq, 8);
    chan_next();
    uint8_t ok[6] = { 0 };
    lbt_put16(ok, ours);
    chan_signal_in(0x05, cid_id, ok, 6);
    return ours;
}

void chan_in(uint16_t our_cid, const uint8_t *d, uint16_t len)
{
    uint8_t pdu[4 + 1024];
    lbt_put16(pdu, len);
    lbt_put16(pdu + 2, our_cid);
    memcpy(pdu + 4, d, len);
    fake_acl(chan_handle, 2, pdu, 4 + len);
}

struct sent_packet *chan_next_data(void)
{
    static struct sent_packet out;
    struct sent_packet *p = chan_next();
    if (!p)
        return NULL;
    out.len = p->len - 4;
    memcpy(out.data, p->data + 4, out.len);
    return &out;
}
