/*
 * lbt: what the platform provides. Lossless OS implements these on the
 * player; the host tests implement them with a fake controller.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_PLATFORM_H
#define LBT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* H4 packet types (the first byte of every packet on the UART) */
#define LBT_H4_COMMAND 0x01
#define LBT_H4_ACL     0x02
#define LBT_H4_SCO     0x03
#define LBT_H4_EVENT   0x04

/* Send one whole packet to the controller: the H4 type, then 'len' bytes
 * of packet (header and payload). May block until written. */
void lbt_platform_send(uint8_t h4_type, const uint8_t *packet, size_t len);

/* Milliseconds from any fixed point; wraps. */
uint32_t lbt_platform_ms(void);

/* A line for the log (no newline needed). */
void lbt_platform_log(const char *fmt, ...);

#endif
