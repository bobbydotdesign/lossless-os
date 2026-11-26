/*
 * X1000 UART, polled, with hardware RTS/CTS flow control: enough for the
 * Bluetooth chip, which only ever sends a little to us (events and
 * acknowledgements) while we send it audio.
 *
 * Receiving is polled from the Bluetooth thread every millisecond or so.
 * Nothing is lost between polls: when the receive FIFO fills, the UART
 * drops RTS and the chip waits.
 *
 * Facts: docs/h2-hardware.md, "Bluetooth" > "UART".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include "x1000_soc.h"
#include "x1000_uart.h"

#define UART(n, off)  X1000_REG(0xb0030000u + (n) * 0x1000u + (off))
#define RBR 0x00
#define THR 0x00
#define DLL 0x00
#define DLH 0x04
#define IER 0x04
#define FCR 0x08
#define LCR 0x0c
#define MCR 0x10
#define LSR 0x14
#define UMR 0x24
#define UACR 0x28

#define FCR_FIFO      (1u << 0)
#define FCR_RX_RESET  (1u << 1)
#define FCR_TX_RESET  (1u << 2)
#define FCR_ON        (1u << 4)
#define FCR_RX_TRIG   (3u << 6)
#define LCR_8BIT      3u
#define LCR_DLAB      (1u << 7)
#define MCR_RTS       (1u << 1)
#define MCR_FLOW      (1u << 6)
#define MCR_MODEM     (1u << 7)
#define LSR_DATA      (1u << 0)
#define LSR_OVERRUN   (1u << 1)
#define LSR_TX_ROOM   (1u << 5)     /* the send FIFO wants data */

#define TX_CHUNK      16            /* bytes written each time LSR_TX_ROOM shows */
#define CLKGR_UART(n) (1u << (14 + (n)))

static uint32_t overruns;

bool x1000_uart_set_baud(int n, unsigned baud)
{
	uint32_t div, umr;

	switch (baud) {
	case 115200:  div = 13; umr = 16; break;   /* 24 MHz / (13 x 16) = 115385 */
	case 3000000: div = 1;  umr = 8;  break;   /* 24 MHz / 8 */
	default:
		return false;
	}
	UART(n, LCR) = LCR_8BIT | LCR_DLAB;
	UART(n, DLL) = div & 0xff;
	UART(n, DLH) = div >> 8;
	UART(n, LCR) = LCR_8BIT;
	UART(n, UMR) = umr;
	UART(n, UACR) = 0;
	return true;
}

bool x1000_uart_init(int n, unsigned baud)
{
	X1000_REG(X1000_CPM_CLKGR) &= ~CLKGR_UART(n);
	UART(n, IER) = 0;
	UART(n, LCR) = 0;
	if (!x1000_uart_set_baud(n, baud)) {
		return false;
	}
	/* flow control in hardware; it only drives RTS with the RTS bit set */
	UART(n, MCR) = MCR_MODEM | MCR_FLOW | MCR_RTS;
	UART(n, FCR) = FCR_RX_TRIG | FCR_ON | FCR_TX_RESET | FCR_RX_RESET | FCR_FIFO;
	overruns = 0;
	return true;
}

void x1000_uart_off(int n)
{
	UART(n, IER) = 0;
	UART(n, MCR) = 0;
	UART(n, FCR) = 0;
	X1000_REG(X1000_CPM_CLKGR) |= CLKGR_UART(n);
}

/* What has arrived, kept here so nothing is lost while we're busy sending
 * (the receive FIFO fills in about 200 us at 3 Mbaud). One UART uses
 * this driver at a time (Bluetooth's). */
#define RX_RING 8192
static uint8_t rx_ring[RX_RING];
static uint32_t rx_head, rx_tail;     /* counting up; same thread only */
static uint32_t ring_full;            /* bytes dropped because the ring was full */

static void rx_pump(int n)
{
	for (;;) {
		uint32_t lsr = UART(n, LSR);

		if (lsr & LSR_OVERRUN) {
			overruns++;
		}
		if (!(lsr & LSR_DATA)) {
			return;
		}
		uint8_t b = (uint8_t)UART(n, RBR);

		if (rx_head - rx_tail < RX_RING) {
			rx_ring[rx_head++ % RX_RING] = b;
		} else {
			ring_full++;
		}
	}
}

bool x1000_uart_write(int n, const uint8_t *data, size_t len, int timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;
	int64_t nap_after = k_uptime_get() + 2;

	while (len) {
		rx_pump(n);
		if (!(UART(n, LSR) & LSR_TX_ROOM)) {
			/* it empties in microseconds at 3 Mbaud; longer means the chip
			 * is holding CTS: nap rather than spin (the Bluetooth thread
			 * is above the rest of the player) */
			int64_t now = k_uptime_get();

			if (now > end) {
				return false;
			}
			if (now > nap_after) {
				k_msleep(1);
			}
			continue;
		}
		/* room for at least this many (the FIFO is 64 bytes) */
		for (int room = TX_CHUNK; room && len; room--, len--) {
			UART(n, THR) = *data++;
		}
		nap_after = k_uptime_get() + 2;
	}
	rx_pump(n);
	return true;
}

size_t x1000_uart_read(int n, uint8_t *buf, size_t max)
{
	size_t got = 0;

	rx_pump(n);
	while (got < max && rx_tail != rx_head) {
		buf[got++] = rx_ring[rx_tail++ % RX_RING];
	}
	return got;
}

void x1000_uart_flush_rx(int n)
{
	rx_pump(n);
	rx_tail = rx_head;
}

uint32_t x1000_uart_overruns(void)
{
	return overruns + ring_full;
}
