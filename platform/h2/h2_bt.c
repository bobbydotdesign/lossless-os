/*
 * The H2's Bluetooth chip (CYW43438) for lbt: power, the 32 kHz sleep
 * clock, the firmware patch, 3 Mbaud; then bytes both ways.
 *
 * Bringing it up is done here with plain polled HCI commands, before lbt
 * starts: until patched, the chip answers only Reset and Broadcom's
 * download commands. Then lbt (app/bluetooth.c) takes over at 3 Mbaud.
 *
 * Facts: docs/h2-hardware.md, "Bluetooth".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "los_sys.h"
#include "stockfw.h"
#include "x1000_soc.h"
#include "x1000_uart.h"
#include "../../lib/lbt/lbt_platform.h"
#include "../../lib/lbt/hci.h"
#include "../../lib/lbt/source.h"
#include "startlog.h"

#define UART       0
#define SLOW_BAUD  115200
#define FAST_BAUD  3000000

/* pins, port C */
#define PIN_RXD       10
#define PIN_TXD       11
#define PIN_CTS       12
#define PIN_RTS       13
#define PIN_REG_ON    18     /* hardware revision 4 */
#define PIN_HOST_WAKE 19     /* host wakes the chip: held low */
#define PIN_CHIP_WAKE 20     /* chip wakes the host: unused */
#define PIN_POWER     22     /* not touched by the original firmware; kept as it was */
#define PIN_RTC32K    26     /* port B, function 0: the X1000's 32.768 kHz out (rtc32k) */

#define HCD_PATH          LOS_BT_PATCH

static void (*log_fn)(const char *line);
static bool lpo_on;

static void say(const char *fmt, ...)
{
	char line[120];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (log_fn) {
		log_fn(line);
	}
}

/* ---- the sleep clock ------------------------------------------------------------- */

/* The sleep clock: PB26 as the X1000's own 32.768 kHz output (function
 * 0, rtc32k: X1000 data sheet, pin list), from the RTC's crystal, as the
 * original firmware gives it to the chip. Left as a plain input, the
 * chip's sleep clock input picked up the USB cable: with a cable in, it
 * didn't start at all (H2, 2026-11-01/02). */
static void lpo(bool on)
{
	if (on == lpo_on) {
		return;
	}
	if (on) {
		x1000_pin_function(X1000_PB, PIN_RTC32K, 0);
	} else {
		x1000_pin_in(X1000_PB, PIN_RTC32K);
	}
	lpo_on = on;
}

/* ---- power ------------------------------------------------------------------------ */

static void power(bool on)
{
	if (on) {
		x1000_pin_function(X1000_PC, PIN_RXD, 0);
		x1000_pin_function(X1000_PC, PIN_TXD, 0);
		x1000_pin_function(X1000_PC, PIN_CTS, 0);
		x1000_pin_function(X1000_PC, PIN_RTS, 0);
		x1000_pin_out(X1000_PC, PIN_REG_ON, 0);
		x1000_pin_out(X1000_PC, PIN_POWER, 1);
		x1000_pin_out(X1000_PC, PIN_HOST_WAKE, 0);
		x1000_pin_in(X1000_PC, PIN_CHIP_WAKE);
		k_msleep(20);
		x1000_pin_out(X1000_PC, PIN_REG_ON, 1);   /* low to high starts it */
		k_msleep(200);
	} else {
		x1000_pin_out(X1000_PC, PIN_REG_ON, 0);
		x1000_pin_out(X1000_PC, PIN_POWER, 0);
		x1000_pin_out(X1000_PC, PIN_HOST_WAKE, 0);
		x1000_pin_in(X1000_PC, PIN_RXD);
		x1000_pin_in(X1000_PC, PIN_TXD);
		x1000_pin_in(X1000_PC, PIN_CTS);
		x1000_pin_in(X1000_PC, PIN_RTS);
	}
}

/* ---- plain HCI, polled (before lbt) ----------------------------------------------- */

static bool read_bytes(uint8_t *buf, size_t n, int64_t deadline)
{
	size_t got = 0;

	while (got < n) {
		got += x1000_uart_read(UART, buf + got, n - got);
		if (got < n) {
			if (k_uptime_get() > deadline) {
				return false;
			}
			k_msleep(1);
		}
	}
	return true;
}

/* Send a command and wait for its Command Complete; the return
 * parameters (status first) go to 'ret'. Returns their length, or -1. */
static int command(uint16_t op, const uint8_t *params, uint8_t len, uint8_t *ret, int ret_max,
		   int timeout_ms)
{
	uint8_t pkt[4 + 255];
	int64_t deadline = k_uptime_get() + timeout_ms;

	pkt[0] = 0x01;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = len;
	if (len) {
		memcpy(pkt + 4, params, len);
	}
	if (!x1000_uart_write(UART, pkt, 4u + len, timeout_ms)) {
		say("command %04x: chip not taking data (CTS)", op);
		return -1;
	}
	for (;;) {
		uint8_t h[3], p[255];

		if (!read_bytes(h, 1, deadline)) {
			say("command %04x: no answer", op);
			return -1;
		}
		if (h[0] != 0x04) {
			continue;           /* not an event: skip a byte */
		}
		if (!read_bytes(h + 1, 2, deadline) || !read_bytes(p, h[2], deadline)) {
			say("command %04x: answer cut short", op);
			return -1;
		}
		/* Command Complete: credits, opcode, then the return parameters */
		if (h[1] == 0x0e && h[2] >= 3 && (p[1] | p[2] << 8) == op) {
			int n = h[2] - 3;

			n = n < ret_max ? n : ret_max;
			if (ret && n > 0) {
				memcpy(ret, p + 3, (size_t)n);
			}
			return h[2] - 3;
		}
		/* anything else (Command Status, vendor events) is skipped */
	}
}

static uint8_t hcd[64 * 1024];

static bool patch(void)
{
	char path[96];
	uint8_t r[8];

	/* the installer copies the player's own here (install.c, stockfw.c) */
	snprintf(path, sizeof(path), "%s%s", los_card_path(), HCD_PATH);
	void *f = los_file_open(path);
	if (!f) {
		say("no patch file (%s)", HCD_PATH);
		return false;
	}
	size_t size = los_file_read(f, hcd, sizeof(hcd));
	los_file_close(f);
	if (size == 0 || size == sizeof(hcd) || !stockfw_hcd_ok(hcd, size)) {
		say("patch file: %u bytes, not a patch", (unsigned)size);
		return false;
	}
	if (command(0xfc2e, NULL, 0, r, sizeof(r), 1000) < 1 || r[0] != 0) {
		say("Download Minidriver refused");
		return false;
	}
	k_msleep(50);
	int records = 0;
	for (size_t at = 0; at + 3 <= size;) {
		uint16_t op = hcd[at] | hcd[at + 1] << 8;
		uint8_t len = hcd[at + 2];

		if (at + 3 + len > size) {
			break;
		}
		if (command(op, &hcd[at + 3], len, r, sizeof(r), 1000) < 1 || r[0] != 0) {
			say("patch record %d (%04x) refused", records, op);
			return false;
		}
		at += 3u + len;
		records++;
	}
	say("patch loaded: %d records, %u bytes", records, (unsigned)size);
	startlog_mark("bluetooth: patch sent (%u bytes at 115200)", (unsigned)size);
	k_msleep(250);                 /* the chip restarts into it */
	return true;
}

/* Power on, reset; false if it doesn't answer */
static bool start_chip(bool with_lpo)
{
	uint8_t r[16];

	power(false);
	lpo(false);
	k_msleep(200);
	x1000_uart_init(UART, SLOW_BAUD);  /* RTS up before the chip wakes */
	k_msleep(50);
	if (with_lpo) {
		lpo(true);
	}
	power(true);
	x1000_uart_flush_rx(UART);
	return command(0x0c03, NULL, 0, r, sizeof(r), 2000) >= 1 && r[0] == 0;
}

/* As start_chip, with a longer wait after the start pin rises */
static bool start_chip_slow(int settle_ms)
{
	uint8_t r[16];

	power(false);
	lpo(false);
	k_msleep(300);
	x1000_uart_init(UART, SLOW_BAUD);
	k_msleep(50);
	lpo(true);
	k_msleep(100);                     /* the sleep clock steady first */
	power(true);
	k_msleep(settle_ms > 200 ? settle_ms - 200 : 0);
	x1000_uart_flush_rx(UART);
	return command(0x0c03, NULL, 0, r, sizeof(r), 3000) >= 1 && r[0] == 0;
}

/* When the chip stays silent: what the pins and the sleep clock look
 * like, then slower starts, then a listen at 3 Mbaud (a chip still
 * running from before). Each step says what it found; true if one got
 * an answer (at 115200, ready for the patch). */
static bool diagnose(void)
{
	uint32_t pin = X1000_GPIO(X1000_PC, 0x00);

	say("port C: level %08x int %08x mask %08x pat1 %08x pat0 %08x pull-off %08x", pin,
	    X1000_GPIO(X1000_PC, 0x10), X1000_GPIO(X1000_PC, 0x20), X1000_GPIO(X1000_PC, 0x30),
	    X1000_GPIO(X1000_PC, 0x40), X1000_GPIO(X1000_PC, 0x70));
	say("CTS %u, power %u, start %u, host wake %u, chip wake %u", pin >> PIN_CTS & 1,
	    pin >> PIN_POWER & 1, pin >> PIN_REG_ON & 1, pin >> PIN_HOST_WAKE & 1,
	    pin >> PIN_CHIP_WAKE & 1);
	static const int waits[] = { 600, 1500 };

	for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
		bool ok = start_chip_slow(waits[i]);

		pin = X1000_GPIO(X1000_PC, 0x00);
		say("slow start (%d ms): %s; CTS %u", waits[i], ok ? "answered" : "no answer",
		    pin >> PIN_CTS & 1);
		if (ok) {
			return true;
		}
	}
	uint8_t r[16];

	x1000_uart_set_baud(UART, FAST_BAUD);
	x1000_uart_flush_rx(UART);
	bool fast = command(0x0c03, NULL, 0, r, sizeof(r), 1000) >= 1 && r[0] == 0;
	say(fast ? "answered at 3 Mbaud (still running from before)" : "no answer at 3 Mbaud either");
	x1000_uart_set_baud(UART, SLOW_BAUD);
	return false;
}

static bool bt_up(void)
{
	uint8_t r[16];

	/* the sleep clock on before the start pin rises, as the original
	 * firmware does; without it only if that fails */
	bool ok = start_chip(true);
	say(ok ? "chip answered" : "no answer with the sleep clock");
	startlog_mark(ok ? "bluetooth: chip answered" : "bluetooth: no answer with the sleep clock");
	if (!ok) {
		ok = start_chip(false);
		say(ok ? "chip answered without the sleep clock" : "no answer without the sleep clock either");
	}
	if (!ok) {
		ok = diagnose();
	}
	if (!ok || !patch()) {
		los_bt_down();
		return false;
	}
	x1000_uart_flush_rx(UART);
	if (command(0x0c03, NULL, 0, r, sizeof(r), 2000) < 1 || r[0] != 0) {
		say("no answer to reset after the patch");
		los_bt_down();
		return false;
	}
	int n = command(0x1001, NULL, 0, r, sizeof(r), 1000);
	if (n >= 9 && r[0] == 0) {
		say("chip ready: HCI %u, LMP subversion %04x, maker %04x", r[1],
		    r[7] | r[8] << 8, r[5] | r[6] << 8);
	}
	/* Broadcom Update UART Baud Rate: two zero bytes, then the rate */
	static const uint8_t baud[6] = { 0, 0, FAST_BAUD & 0xff, FAST_BAUD >> 8 & 0xff,
					 FAST_BAUD >> 16 & 0xff, FAST_BAUD >> 24 };
	if (command(0xfc18, baud, sizeof(baud), r, sizeof(r), 1000) < 1 || r[0] != 0) {
		say("baud rate change refused");
		los_bt_down();
		return false;
	}
	k_msleep(20);
	x1000_uart_set_baud(UART, FAST_BAUD);
	x1000_uart_flush_rx(UART);
	say("UART at 3 Mbaud");
	return true;
}

bool los_bt_up(void (*log)(const char *line))
{
	/* The patch goes at 115200 baud, where the send loop waits in a spin
	 * (16 bytes drain in 1.4 ms, under its 2 ms nap). Above the UI, that
	 * spin held the start-up logo for the whole ~3 s patch: below the UI
	 * until the link is at 3 Mbaud, and back up for the audio after. */
	k_tid_t self = k_current_get();
	int prio = k_thread_priority_get(self);

	log_fn = log;
	k_thread_priority_set(self, CONFIG_MAIN_THREAD_PRIORITY + 1);
	bool ok = bt_up();
	k_thread_priority_set(self, prio);
	return ok;
}

void los_bt_down(void)
{
	power(false);
	x1000_uart_off(UART);
	lpo(false);
}

void los_bt_poll(void)
{
	uint8_t buf[256];
	size_t n;

	while ((n = x1000_uart_read(UART, buf, sizeof(buf))) > 0) {
		hci_receive(buf, n);
	}
	source_poll();

	/* bytes lost coming in (the receive FIFO overflowed): worth knowing */
	static uint32_t lost_seen;
	uint32_t lost = x1000_uart_overruns();
	if (lost != lost_seen) {
		say("UART: bytes lost coming in (%u times so far)", lost);
		lost_seen = lost;
	}
}

/* ---- lbt_platform.h ------------------------------------------------------------------ */

void lbt_platform_send(uint8_t h4_type, const uint8_t *packet, size_t len)
{
	static uint8_t tx[1 + 4 + 1100];

	if (len + 1 > sizeof(tx)) {
		return;
	}
	tx[0] = h4_type;
	memcpy(tx + 1, packet, len);
	if (!x1000_uart_write(UART, tx, len + 1, 500)) {
		say("send of %u bytes timed out (CTS)", (unsigned)(len + 1));
	}
}

/* Straight from the crystal-driven counter (32 bits, wrapping every
 * 12 minutes; lbt asks far more often): audio is paced by this. Only the
 * Bluetooth thread calls it. */
uint32_t lbt_platform_ms(void)
{
	static uint32_t last;
	static uint64_t cycles;
	uint32_t now = k_cycle_get_32();

	cycles += now - last;
	last = now;
	return (uint32_t)(cycles / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000));
}

void lbt_platform_log(const char *fmt, ...)
{
	char line[120];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (log_fn) {
		log_fn(line);
	}
}
