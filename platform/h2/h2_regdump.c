/*
 * Register dump: the clock, memory controller, memory interface and power
 * chip registers as the bootloader left them, in
 * /.lossless/logs/regs.txt, written at every start.
 *
 * For our own bootloader: its memory and clock set-up is worked out from
 * Ingenic's X1000 Programming Manual and the JEDEC LPDDR standard, and
 * checked against what a running player has. Only reads; each block is
 * appended as soon as it's read, so a read that hangs still leaves the
 * blocks before it on the card.
 *
 * Ranges (X1000 Programming Manual, 2016-01-13):
 *   CPM, clocks and power        0xb0000000, 0x100 bytes (11.1.2)
 *   DDR controller, AHB group    0xb34f0000: 0x00-0x7c, 0x9c-0xbc,
 *                                0x100-0x118, 0x200-0x244, 0x304-0x30c
 *                                (Table 9-1)
 *   DDR controller, APB group    0xb3012000, 0x00-0x8c (Table 9-1)
 *   DDR PHY                      0xb3011000, 0x00-0x2fc (registers not
 *                                published; the manual names a few, e.g.
 *                                0xb3011014 in 9.6)
 *   GPIO ports A-D               0xb0010000 + port * 0x100, 0x00-0x7c (ch. 19)
 *   MSC0, the card controller    0xb3450000, 0x00-0x5c
 *   AXP2101                      I2C bus 2, address 0x34, 0x00-0xff
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "h2_regdump.h"
#include "x1000_i2c.h"
#include "los_sys.h"

#include <stdint.h>
#include <stdio.h>

#define PATH "/SD:/.lossless/logs/regs.txt"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static char text[2048];

static void flush(int n)
{
	if (n > (int)sizeof(text) - 1)
		n = (int)sizeof(text) - 1;
	los_file_append(PATH, text, (size_t)n);
}

/* Words from base+from to base+to (inclusive), four to a line */
static void block(const char *name, uint32_t base, uint32_t from, uint32_t to)
{
	int n = snprintf(text, sizeof(text), "\n%s\n", name);

	for (uint32_t off = from; off <= to && n < (int)sizeof(text) - 64; off += 4) {
		if ((off - from) % 16 == 0)
			n += snprintf(text + n, sizeof(text) - n, "%08x:", base + off);
		n += snprintf(text + n, sizeof(text) - n, " %08x", REG(base + off));
		if ((off - from) % 16 == 12 || off + 4 > to)
			n += snprintf(text + n, sizeof(text) - n, "\n");
	}
	flush(n);
}

static void axp2101(void)
{
	int n = snprintf(text, sizeof(text), "\nAXP2101 (I2C 2, 0x34)\n");
	uint8_t v[16];

	for (int reg = 0; reg < 0x100; reg += 16) {
		if (x1000_i2c_read(2, 0x34, (uint8_t)reg, v, 16)) {
			n += snprintf(text + n, sizeof(text) - n, "%02x: no answer\n", reg);
			continue;
		}
		n += snprintf(text + n, sizeof(text) - n, "%02x:", reg);
		for (int i = 0; i < 16; i++)
			n += snprintf(text + n, sizeof(text) - n, " %02x", v[i]);
		n += snprintf(text + n, sizeof(text) - n, "\n");
	}
	flush(n);
}

/* The same read test our first stage runs, here under the current
 * bootloader's memory set-up: 4096 words at 8 MB (uncached, an area
 * Lossless OS doesn't use), read back four times; then the same through
 * the cache. Wrong reads, and which bits flipped. */
static uint32_t pattern(uint32_t i)
{
	return (i * 0x9e3779b9u) ^ (i << 24) ^ 0x01020304u;
}

static void memtest(void)
{
	volatile uint32_t *u = (volatile uint32_t *)0xa0800000u;
	volatile uint32_t *c = (volatile uint32_t *)0x80800000u;
	uint32_t bad = 0, flips = 0, cbad = 0, cflips = 0;

	for (uint32_t i = 0; i < 4096; i++)
		u[i] = pattern(i);
	for (int pass = 0; pass < 4; pass++)
		for (uint32_t i = 0; i < 4096; i++) {
			uint32_t got = u[i];
			if (got != pattern(i)) {
				bad++;
				flips |= got ^ pattern(i);
			}
		}
	for (int pass = 0; pass < 4; pass++) {
		/* drop the lines so each pass reads memory again */
		for (uint32_t a = 0x80800000u; a < 0x80804000u; a += 32)
			__asm__ volatile("cache 0x11, 0(%0)" : : "r"(a));    /* Hit_Invalidate_D */
		for (uint32_t i = 0; i < 4096; i++) {
			uint32_t got = c[i];
			if (got != pattern(i)) {
				cbad++;
				cflips |= got ^ pattern(i);
			}
		}
	}
	int n = snprintf(text, sizeof(text),
			 "\nRead test, 4096 words at 8 MB, four passes\n"
			 "uncached: %u wrong of 16384, bits %08x\n"
			 "cached:   %u wrong of 16384, bits %08x\n", bad, flips, cbad, cflips);
	flush(n);
}

void h2_regdump(void)
{
	los_mkdir("/SD:/.lossless");
	los_mkdir("/SD:/.lossless/logs");
	int n = snprintf(text, sizeof(text), "Register dump, as the bootloader left them\n");
	los_file_save(PATH, text, (size_t)n);

	block("CPM", 0xb0000000u, 0x00, 0xfc);
	block("DDR controller, AHB group", 0xb34f0000u, 0x00, 0x7c);
	block("DDR controller, AHB group (remap)", 0xb34f0000u, 0x9c, 0xbc);
	block("DDR controller, AHB group (performance)", 0xb34f0000u, 0x100, 0x118);
	block("DDR controller, AHB group (QoS)", 0xb34f0000u, 0x200, 0x244);
	block("DDR controller, AHB group (self refresh)", 0xb34f0000u, 0x304, 0x30c);
	block("DDR controller, APB group", 0xb3012000u, 0x00, 0x8c);
	block("DDR PHY", 0xb3011000u, 0x00, 0x1fc);
	block("DDR PHY, byte lanes 1 and up", 0xb3011000u, 0x200, 0x2fc);
	block("GPIO port A", 0xb0010000u, 0x00, 0x7c);
	block("GPIO port B", 0xb0010100u, 0x00, 0x7c);
	block("GPIO port C", 0xb0010200u, 0x00, 0x7c);
	block("GPIO port D", 0xb0010300u, 0x00, 0x7c);
	block("MSC0 (card controller)", 0xb3450000u, 0x00, 0x5c);
	axp2101();
	memtest();
}
