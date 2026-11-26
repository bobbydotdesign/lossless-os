/*
 * lbt host tests: L2CAP plumbing for profile tests (one connection).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_TEST_CHAN_H
#define LBT_TEST_CHAN_H

#include "fake.h"

extern uint16_t chan_handle;

/* A signaling command from the other side */
void chan_signal_in(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len);
/* The next ACL packet's L2CAP part (and the controller frees its buffer) */
struct sent_packet *chan_next(void);
/* They accept the channel we're opening (MTU 1023); returns our CID */
uint16_t chan_accept_ours(uint16_t their_cid);
/* They open a channel to us on psm; returns our CID */
uint16_t chan_open_theirs(uint16_t psm, uint16_t their_cid);
/* Data from them on our CID */
void chan_in(uint16_t our_cid, const uint8_t *d, uint16_t len);
/* The payload of the next data packet we sent (after the L2CAP header) */
struct sent_packet *chan_next_data(void);

#endif
