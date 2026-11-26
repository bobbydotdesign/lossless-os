/*
 * The H2's screen, started by us: the X1000's LCD controller in "smart"
 * (8080, 8-bit) mode and the panel's own start-up, for when nothing has
 * started it (a normal power-on: the bootloader only starts the screen for
 * its menu). Then frames go out the same way as when the bootloader left
 * the screen running: two DMA descriptors in a loop, a "write memory"
 * command and the frame, and each DMA_START sends one frame
 * (screen.c, screen_show).
 *
 * Hardware revision 4 (the H2): the panel needs no power pin, and takes
 * the table below. Revisions 1-2 have a different panel (not handled).
 *
 * Facts: docs/h2-hardware.md, "Screen, started by us".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include "x1000_soc.h"

#define LCD(off)          X1000_REG(0xb3050000u + (off))
#define DAH               0x10
#define DAV               0x14
#define CTRL              0x30
#define STATE             0x34
#define DA                0x40
#define MCFG              0xa0
#define MCTRL             0xa4
#define MSTATE            0xa8
#define MDATA             0xac
#define WTIME             0xb0
#define TASH              0xb4
#define MCFG_NEW          0xb8
#define SMWT              0xbc

#define CTRL_BURST_64     (4u << 28)
#define CTRL_BURST_MASK   (7u << 28)
#define CTRL_ENABLE       (1u << 3)
/* frame and interrupt bits in CTRL we keep off: EOFM, SOFM, IFUM, QDM, BEDN, PEDN */
#define CTRL_OFF_MASK     ((1u << 13) | (1u << 12) | (1u << 10) | (1u << 7) | (1u << 6) | (1u << 5))
#define MCTRL_DEFINED     0x7dfu            /* bits 10:6 and 4:0 */
#define MCTRL_NO_TE       (1u << 8)
#define MCTRL_FAST        (1u << 4)
#define MCTRL_DMA_MODE    (1u << 2)
#define MCTRL_DMA_START   (1u << 1)
#define MCTRL_DMA_TX      (1u << 0)
#define MSTATE_BUSY       (1u << 0)
#define MDATA_COMMAND     (1u << 30)
#define MCFG_8BIT_CMD     0x100u
#define MCFG_NEW_FRAMES   ((1u << 8) | (1u << 0))   /* 2 bus cycles a pixel, format conversion */

#define CPM_LPCDR         0xb0000064u
#define LPCDR_MPLL        (1u << 31)
#define LPCDR_CE          (1u << 28)
#define LPCDR_BUSY        (1u << 27)
#define CLKGR_LCD         (1u << 23)
#define PIXEL_HZ          20000000u

#define W 320
#define H 240

/* port B pins */
#define PIN_RESET  13
#define PIN_RD     16
#define PIN_WR     17
#define PIN_CS     18
#define PIN_TE     19
#define PIN_DC     20

struct desc {
	uint32_t next, source, id, command, offset, page, count, size;
};

static struct desc descs[2] __aligned(32);
static uint8_t write_cmd[64] __aligned(64);
static uint16_t frame[W * H] __aligned(64);

#define PHYS(p)      ((uint32_t)(uintptr_t)(p) & 0x1fffffffu)
#define UNCACHED(p)  ((void *)(uintptr_t)(0xa0000000u | PHYS(p)))

/* ---- time, before Zephyr's timer runs: OST counter 2 ---- */

#define OST_CTRL       0xb2000000u
#define OST_ENABLE     0xb2000004u
#define OST_ENABLE_SET 0xb2000034u
#define OST_2CNTL      0xb2000020u

static void delay_us(uint32_t us)
{
	if (!(X1000_REG(OST_ENABLE) & 2u)) {
		X1000_REG(X1000_CPM_CLKGR) &= ~(1u << 20);
		X1000_REG(OST_CTRL) = (X1000_REG(OST_CTRL) & ~(3u << 2)) | (1u << 2);  /* /4 */
		X1000_REG(OST_ENABLE_SET) = 2u;
	}
	static const uint32_t div[4] = { 1, 4, 16, 16 };
	uint32_t per_us = 24 / div[(X1000_REG(OST_CTRL) >> 2) & 3];
	uint32_t start = X1000_REG(OST_2CNTL), want = us * (per_us ? per_us : 1);

	while (X1000_REG(OST_2CNTL) - start < want) {
	}
}

/* ---- talking to the panel ---- */

static void wait_idle(void)
{
	for (int i = 0; i < 65500 && (LCD(MSTATE) & MSTATE_BUSY); i++) {
	}
}

static void command(uint8_t c)
{
	wait_idle();
	LCD(MDATA) = MDATA_COMMAND | c;
}

static void param(uint8_t p)
{
	wait_idle();
	LCD(MDATA) = p;
}

/* Revision 3-4 panel: each entry is a command, its parameter count, then
 * the parameters; a delay after 0x11 and 0x29 */
static const uint8_t panel_start[] = {
	0xfe, 0,
	0xef, 0,
	0x36, 1, 0x90,
	0x3a, 1, 0x55,
	0x84, 1, 0x04,
	0x86, 1, 0xfb,
	0x87, 1, 0x79,
	0x89, 1, 0x0b,
	0x8a, 1, 0x20,
	0x8b, 1, 0x80,
	0x8d, 1, 0x3b,
	0x8e, 1, 0xcf,
	0xec, 3, 0x33, 0x02, 0x4c,
	0x98, 1, 0x3e,
	0x9c, 1, 0x4b,
	0x99, 1, 0x3e,
	0x9d, 1, 0x4b,
	0x9b, 1, 0x55,
	0xe8, 2, 0x11, 0x00,
	0xff, 1, 0x62,
	0xc3, 1, 0x20,
	0xc4, 1, 0x03,
	0xc9, 1, 0x2a,
	0xf0, 6, 0x4a, 0x10, 0x0a, 0x0a, 0x26, 0x39,
	0xf2, 6, 0x4a, 0x10, 0x0a, 0x0a, 0x26, 0x39,
	0xf1, 6, 0x50, 0x8f, 0xaf, 0x3b, 0x3f, 0x7f,
	0xf3, 6, 0x50, 0x8f, 0xaf, 0x3b, 0x3f, 0x7f,
	0xba, 1, 0x0a,
	0x21, 0,
	0xfe, 0,
	0xee, 0,
};

static void send_table(const uint8_t *t, size_t len)
{
	for (size_t i = 0; i < len;) {
		uint8_t c = t[i++], n = t[i++];

		command(c);
		while (n--) {
			param(t[i++]);
		}
	}
	wait_idle();
}

/* ---- clock ---- */

static uint32_t sclk_a_hz(void)
{
	uint32_t ccr = X1000_REG(X1000_CPM_CCR);

	if ((ccr >> 30) == 1) {
		return 24000000u;
	}
	return x1000_pll_hz(X1000_CPM_APCR, 1u << 10);
}

static bool pixel_clock(void)
{
	uint32_t src = sclk_a_hz();

	if (!src) {
		return false;
	}
	uint32_t div = (src + PIXEL_HZ - 1) / PIXEL_HZ;

	if (div < 1) {
		div = 1;
	}
	if (div > 256) {
		div = 256;
	}
	X1000_REG(CPM_LPCDR) = (X1000_REG(CPM_LPCDR) & ~(LPCDR_MPLL | 0xffu)) | LPCDR_CE | (div - 1);
	for (int i = 0; i < 1000000 && (X1000_REG(CPM_LPCDR) & LPCDR_BUSY); i++) {
	}
	X1000_REG(CPM_LPCDR) &= ~LPCDR_CE;
	return true;
}

/* ---- start ---- */

bool h2_lcd_start(void)
{
	/* pins: data, write, DC, TE to the controller; reset, RD, CS ours */
	for (int p = 0; p < 16; p++) {
		x1000_pin_function(X1000_PA, p, 1);
	}
	x1000_pin_function(X1000_PB, PIN_WR, 1);
	x1000_pin_function(X1000_PB, PIN_TE, 1);
	x1000_pin_function(X1000_PB, PIN_DC, 1);
	x1000_pin_out(X1000_PB, PIN_RD, 1);
	x1000_pin_out(X1000_PB, PIN_CS, 1);
	x1000_pin_out(X1000_PB, PIN_RESET, 0);

	/* the controller, idle, set up for commands */
	X1000_REG(X1000_CPM_CLKGR) &= ~CLKGR_LCD;
	LCD(CTRL) &= ~CTRL_ENABLE;
	LCD(MCFG) = MCFG_8BIT_CMD;
	LCD(MCFG_NEW) = 0;
	LCD(MCTRL) = (LCD(MCTRL) & ~MCTRL_DEFINED) | MCTRL_NO_TE | MCTRL_FAST | MCTRL_DMA_MODE;
	LCD(WTIME) = 0;
	LCD(TASH) = 0;
	LCD(SMWT) = 0;
	LCD(CTRL) = (LCD(CTRL) & ~(CTRL_BURST_MASK | CTRL_OFF_MASK | CTRL_ENABLE)) | CTRL_BURST_64;
	LCD(DAH) = W;
	LCD(DAV) = H;

	/* the two descriptors and the "write memory" command, in memory */
	uint8_t *cmd = UNCACHED(write_cmd);
	for (int i = 0; i < 4; i++) {
		cmd[i] = 0x2c;
	}
	struct desc *d = UNCACHED(descs);
	d[0] = (struct desc){ PHYS(&descs[1]), PHYS(write_cmd), 0xc0, 0x20000001u, 0, 0, 4, 0 };
	d[1] = (struct desc){ PHYS(&descs[0]), PHYS(frame), 0xf0,
			      (1u << 30) | (1u << 26) | (W * H * 2 / 4), 0, 0, 0x20000000u,
			      (W - 1) | ((H - 1) << 12) };
	uint32_t *px = UNCACHED(frame);
	for (int i = 0; i < W * H / 2; i++) {
		px[i] = 0;
	}

	/* the panel: out of reset, clock, select it, its start-up table */
	delay_us(10000);                 /* a clean reset pulse */
	delay_us(20000);
	x1000_pin_out(X1000_PB, PIN_RESET, 1);
	delay_us(12000);
	if (!pixel_clock()) {
		return false;
	}
	x1000_pin_out(X1000_PB, PIN_RD, 1);
	x1000_pin_out(X1000_PB, PIN_CS, 1);
	delay_us(5000);
	x1000_pin_out(X1000_PB, PIN_CS, 0);
	send_table(panel_start, sizeof(panel_start));
	command(0x11);                   /* out of sleep */
	wait_idle();
	delay_us(120000);
	/* black into the panel's memory before it shows it (it powers up
	 * holding noise) */
	command(0x2c);
	for (int i = 0; i < W * H * 2; i++) {
		param(0);
	}
	wait_idle();
	command(0x29);                   /* display on */
	wait_idle();
	delay_us(20000);

	/* frames by DMA: two bus cycles a pixel, then a first (black) frame */
	LCD(MCFG_NEW) = MCFG_NEW_FRAMES;
	LCD(MCTRL) |= MCTRL_DMA_MODE | MCTRL_DMA_START;
	wait_idle();
	LCD(MCTRL) |= MCTRL_DMA_TX;
	LCD(STATE) = 0;
	LCD(DA) = PHYS(&descs[0]);
	LCD(CTRL) |= CTRL_ENABLE;
	return true;
}
