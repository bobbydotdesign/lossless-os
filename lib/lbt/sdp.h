/*
 * lbt SDP server: tells the other side what we offer
 * (Bluetooth Core, Vol 3 Part B).
 *
 * Three records, built in: A2DP source, AVRCP target and AVRCP controller.
 * Answers service searches, attribute requests and the combined search,
 * splitting long answers with continuation states.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_SDP_H
#define LBT_SDP_H

#include "lbt_util.h"

/* Record handles */
#define SDP_HANDLE_A2DP_SOURCE  0x00010001
#define SDP_HANDLE_AVRCP_TARGET 0x00010002
#define SDP_HANDLE_AVRCP_CTRL   0x00010003

/* Start answering on PSM 1. Needs l2cap_init() first. */
void sdp_init(void);

/* Handle one request PDU and write the response; returns its length
 * (0: no response). 'max_out' is the most the channel can carry.
 * Exposed for tests; the server calls it for each request. */
uint16_t sdp_handle(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t max_out);

#endif
