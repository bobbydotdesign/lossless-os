/*
 * lbt L2CAP: channels over a connection (Bluetooth Core, Vol 3 Part A).
 *
 * Basic mode, which is all A2DP, AVRCP and SDP need, and Enhanced
 * Retransmission Mode for a service registered with l2cap_listen_ertm (OBEX
 * for cover art). Channels are opened by us (l2cap_connect) or by the other
 * side, for a PSM a profile registered. Configuration covers the MTU, and
 * ERTM where it's wanted; other modes are turned down.
 *
 * In ERTM l2cap_send takes one SDU at a time (l2cap_can_send says when the
 * last one is through) and sends it in I-frames as their window allows;
 * l2cap_tick resends what isn't acknowledged in time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_L2CAP_H
#define LBT_L2CAP_H

#include "lbt_util.h"

#define L2CAP_MAX_CHANNELS 8
#define L2CAP_MAX_SERVICES 6
#define L2CAP_DEFAULT_MTU  672
#define L2CAP_MIN_MTU      48
#define L2CAP_TX_MAX       2048   /* largest PDU we build */

/* PSMs */
#define L2CAP_PSM_SDP      0x0001
#define L2CAP_PSM_AVCTP    0x0017
#define L2CAP_PSM_AVDTP    0x0019

/* Connection results */
#define L2CAP_RESULT_SUCCESS          0x0000
#define L2CAP_RESULT_PENDING          0x0001
#define L2CAP_RESULT_PSM_NOT_SUPPORTED 0x0002
#define L2CAP_RESULT_SECURITY_BLOCK   0x0003
#define L2CAP_RESULT_NO_RESOURCES     0x0004
#define L2CAP_RESULT_REJECTED         0xff00  /* command reject (ours) */
#define L2CAP_RESULT_CONFIG_FAILED    0xff01  /* configuration failed (ours) */
#define L2CAP_RESULT_LINK_LOST        0xff02  /* ACL link went away (ours) */

typedef struct l2cap_chan l2cap_chan;

typedef struct {
    /* the channel is open (result 0), or opening it failed */
    void (*opened)(l2cap_chan *chan, uint16_t result);
    void (*data)(l2cap_chan *chan, const uint8_t *data, uint16_t len);
    /* an open channel closed */
    void (*closed)(l2cap_chan *chan);
} l2cap_ops;

void l2cap_init(void);

/* Accept channels the other side opens for 'psm'. */
bool l2cap_listen(uint16_t psm, const l2cap_ops *ops, uint16_t mtu);
/* The same, in Enhanced Retransmission Mode (basic if they won't) */
bool l2cap_listen_ertm(uint16_t psm, const l2cap_ops *ops, uint16_t mtu);

/* Every couple of hundred ms: ERTM's sending, acknowledgements and time-outs */
void l2cap_tick(void);

/* Open a channel to 'psm' over connection 'handle'. 'mtu' is the most we
 * take in one packet. NULL if out of channels. */
l2cap_chan *l2cap_connect(uint16_t handle, uint16_t psm, const l2cap_ops *ops,
                          uint16_t mtu, void *user);

/* Send one packet on an open channel. False if not open, longer than
 * l2cap_max_send(), or HCI has no room right now. */
bool l2cap_send(l2cap_chan *chan, const uint8_t *data, uint16_t len);

/* Most bytes one l2cap_send can take (the other side's MTU, capped) */
uint16_t l2cap_max_send(const l2cap_chan *chan);

/* Can a packet of len go right now? */
bool l2cap_can_send(const l2cap_chan *chan, uint16_t len);

void l2cap_disconnect(l2cap_chan *chan);

uint16_t l2cap_handle(const l2cap_chan *chan);
uint16_t l2cap_psm(const l2cap_chan *chan);
bool l2cap_is_open(const l2cap_chan *chan);
void *l2cap_user(const l2cap_chan *chan);
void l2cap_set_user(l2cap_chan *chan, void *user);

#endif
