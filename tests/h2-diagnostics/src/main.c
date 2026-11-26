/*
 * Test 2 on the H2: the screen becomes our log (docs/h2-hardware.md).
 *
 * After choosing "Linux" in the bootloader menu:
 *   1. the lossless mark on black for a moment,
 *   2. a page of facts, filled in one line at a time: chip, caches, CPU
 *      clock, CP0 count, memory test, then the timer counting and the tick
 *      interrupts arriving, live. The bottom two lines are the log.
 * Each line is drawn before the next check starts, so if one hangs the
 * screen shows how far it got. A fatal error shows "Stopped" in red with
 * the CPU's registers. No screen frame found: the backlight flickers fast
 * ten times, as in test 1.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include <stdbool.h>

#include "draw.h"
#include "page.h"
#include "screen.h"

#define REG(a)      (*(volatile uint32_t *)(a))
#define MFC0(reg, sel) ({ uint32_t v_; \
	__asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r"(v_)); v_; })

#define OST_2CNTL   0xb2000020u
#define OST_HZ      6000000u          /* counter 2: crystal / 4 */
#define CRYSTAL_HZ  24000000u

#define CPM_CCR     0xb0000000u
#define CPM_APCR    0xb0000010u
#define CPM_MPCR    0xb0000014u
#define CPM_DDRCDR  0xb000002cu

#define RAM_END     0x82000000u       /* 32 MB from 0x80000000 */

static bool ost_runs;
static char text[48];

static uint32_t ost(void)
{
	return REG(OST_2CNTL);
}

/* Wait without interrupts: on OST counter 2 when it runs, else a counted
 * loop (about 150 000 turns a millisecond, from test 1's blinks). */
static void wait_ms(uint32_t ms)
{
	if (ost_runs) {
		uint32_t t0 = ost();

		while (ost() - t0 < ms * (OST_HZ / 1000)) {
		}
	} else {
		for (volatile uint32_t i = 0; i < ms * 150000u; i++) {
		}
	}
}

static void show_row(enum page_row row, enum page_tone tone)
{
	const char *older, *newest;

	page_row(row, text, tone);
	screen_log_tail(&older, &newest);
	page_log(older, newest);
	screen_show();
}

/* Chip: the CPU's own identity register */
static void check_chip(void)
{
	snprintk(text, sizeof(text), "X1000, id %08x", MFC0(15, 0));
	show_row(ROW_CHIP, TONE_PLAIN);
}

/* Caches: sizes the CPU reports in Config1 */
static uint32_t cache_bytes(uint32_t sets, uint32_t line, uint32_t ways)
{
	return line ? (64u << sets) * (2u << line) * (ways + 1) : 0;
}

static void check_caches(void)
{
	uint32_t c1 = MFC0(16, 1);
	uint32_t ic = cache_bytes(c1 >> 22 & 7, c1 >> 19 & 7, c1 >> 16 & 7);
	uint32_t dc = cache_bytes(c1 >> 13 & 7, c1 >> 10 & 7, c1 >> 7 & 7);

	snprintk(text, sizeof(text), "%u KB + %u KB", ic / 1024, dc / 1024);
	show_row(ROW_CACHES, TONE_PLAIN);
}

/* CPU clock: worked out from the clock unit's settings, then the CP0 count
 * measured against OST counter 2. */
static uint32_t pll_hz(uint32_t reg, uint32_t on_bit)
{
	uint32_t v = REG(reg);

	if (!(v & on_bit)) {
		return 0;
	}
	uint64_t hz = (uint64_t)CRYSTAL_HZ * ((v >> 24 & 0x7f) + 1) / ((v >> 18 & 0x1f) + 1);
	return (uint32_t)(hz >> (v >> 16 & 3));
}

static uint32_t source_hz(uint32_t sel)
{
	uint32_t ccr = REG(CPM_CCR);

	switch (sel) {
	case 1:  /* SCLK_A: the crystal or APLL */
		switch (ccr >> 30) {
		case 1: return CRYSTAL_HZ;
		case 2: return pll_hz(CPM_APCR, 1u << 10);
		default: return 0;
		}
	case 2:  /* MPLL */
		return pll_hz(CPM_MPCR, 1u << 0);
	default:
		return 0;
	}
}

static uint32_t memory_hz;

static void check_cpu(void)
{
	uint32_t ccr = REG(CPM_CCR), ddr = REG(CPM_DDRCDR);
	uint32_t cpu_hz = source_hz(ccr >> 28 & 3) / ((ccr & 0xf) + 1);

	memory_hz = source_hz(ddr >> 30) / ((ddr & 0xf) + 1);

	snprintk(text, sizeof(text), "%u MHz, RAM %u", cpu_hz / 1000000,
		 memory_hz / 1000000);
	show_row(ROW_CPU, TONE_PLAIN);

	/* The CP0 count: whether it runs, and how fast (a candidate timer) */
	if (!ost_runs) {
		snprintk(text, sizeof(text), "not checked");
		show_row(ROW_COUNT, TONE_WAIT);
		return;
	}
	uint32_t c0 = MFC0(9, 0), t0 = ost();

	wait_ms(100);
	uint32_t c1 = MFC0(9, 0), t1 = ost();
	uint64_t count_hz = (uint64_t)(c1 - c0) * OST_HZ / (t1 - t0);

	if (c1 == c0) {
		snprintk(text, sizeof(text), "stopped");
	} else {
		snprintk(text, sizeof(text), "%u MHz", (uint32_t)((count_hz + 500000) / 1000000));
	}
	show_row(ROW_COUNT, TONE_PLAIN);
}

/* Memory: write and read back everything above our image, twice, with
 * patterns that differ per word (catches bad address lines too). */
extern char _image_ram_end[];

static uint32_t memory_test(uint32_t start, uint32_t end)
{
	volatile uint32_t *p;

	for (int pass = 0; pass < 2; pass++) {
		uint32_t flip = pass ? 0xffffffffu : 0;

		for (p = (uint32_t *)start; (uint32_t)p < end; p++) {
			*p = ((uint32_t)p * 0x9e3779b1u) ^ flip;
		}
		for (p = (uint32_t *)start; (uint32_t)p < end; p++) {
			if (*p != (((uint32_t)p * 0x9e3779b1u) ^ flip)) {
				return (uint32_t)p;
			}
		}
	}
	return 0;
}

static void check_memory(void)
{
	uint32_t start = ((uint32_t)_image_ram_end + 31) & ~31u;
	uint32_t tenths = (RAM_END - start) * 10 / (1024 * 1024);

	snprintk(text, sizeof(text), "testing %u.%u MB", tenths / 10, tenths % 10);
	show_row(ROW_MEMORY, TONE_WAIT);

	/* Leave the screen's frame alone if it happens to be up here */
	uint32_t frame = 0x80000000u | screen_frame(), frame_end = frame + DRAW_W * DRAW_H * 2;
	uint32_t bad;

	if (frame_end > start && frame < RAM_END) {
		bad = memory_test(start, frame > start ? frame : start);
		if (!bad && frame_end < RAM_END) {
			bad = memory_test(frame_end, RAM_END);
		}
	} else {
		bad = memory_test(start, RAM_END);
	}

	if (bad) {
		snprintk(text, sizeof(text), "bad at %08x", bad);
		show_row(ROW_MEMORY, TONE_BAD);
	} else {
		snprintk(text, sizeof(text), "%u.%u MB free, ok", tenths / 10,
			 tenths % 10);
		show_row(ROW_MEMORY, TONE_OK);
	}
}

/* Ticks: Zephyr's tick count comes only from the timer interrupt, so if
 * it grows, interrupts arrive. 1000 a second expected. */
static bool check_ticks(void)
{
	if (!ost_runs) {
		snprintk(text, sizeof(text), "not checked");
		show_row(ROW_TICKS, TONE_WAIT);
		return false;
	}
	uint32_t k0 = sys_clock_tick_get_32();

	wait_ms(500);
	uint32_t got = sys_clock_tick_get_32() - k0;
	uint32_t want = CONFIG_SYS_CLOCK_TICKS_PER_SEC / 2;
	bool ok = got > want - want / 20 && got < want + want / 20;

	if (got == 0) {
		snprintk(text, sizeof(text), "none arrived");
	} else {
		snprintk(text, sizeof(text), "%u in 0.5 s, want %u", got, want);
	}
	show_row(ROW_TICKS, ok ? TONE_OK : TONE_BAD);
	return ok;
}

/* Sleep: the main thread sleeps, the CPU idles until the timer wakes it. */
static void check_sleep(bool ticks_ok)
{
	if (!ticks_ok) {
		snprintk(text, sizeof(text), "skipped, no ticks");
		show_row(ROW_SLEEP, TONE_WAIT);
		return;
	}
	uint32_t t0 = ost();

	k_msleep(200);
	uint32_t ms = (ost() - t0) / (OST_HZ / 1000);
	bool ok = ms >= 199 && ms <= 205;

	snprintk(text, sizeof(text), "200 ms took %u ms", ms);
	show_row(ROW_SLEEP, ok ? TONE_OK : TONE_BAD);
}

int main(void)
{
	printk("Lossless OS test 2\n");
	if (!screen_frame()) {
		screen_backlight_blink(10, 8000000);
		for (;;) {
		}
	}

	/* Is OST counter 2 running? (it should be: the bootloader starts it,
	 * and so does our timer driver) */
	uint32_t t0 = ost();

	for (volatile uint32_t i = 0; i < 1000000; i++) {
	}
	ost_runs = ost() != t0;

	page_splash();
	screen_show();
	wait_ms(1500);

	page_frame();
	screen_show();
	printk("screen frame %08x\n", screen_frame());
	printk("boot status %08x\n", x1000_boot_cp0[0]);
	printk("boot cause %08x\n", x1000_boot_cp0[1]);

	check_chip();
	check_caches();
	check_cpu();
	check_memory();
	bool ticks_ok = check_ticks();

	check_sleep(ticks_ok);

	/* Live: the timer and the ticks, ten times a second */
	uint32_t last_ost = ost(), last_ticks = sys_clock_tick_get_32();
	uint64_t elapsed = last_ost - t0;    /* since main() started */

	for (;;) {
		uint32_t now = ost(), ticks = sys_clock_tick_get_32();

		elapsed += now - last_ost;
		if (ost_runs) {
			uint32_t tenths = (uint32_t)(elapsed / (OST_HZ / 10));

			snprintk(text, sizeof(text), "%u.%u s", tenths / 10, tenths % 10);
			show_row(ROW_TIMER, TONE_LIVE);
		} else {
			snprintk(text, sizeof(text), "counter 2 stopped");
			show_row(ROW_TIMER, TONE_BAD);
		}
		if (ticks_ok) {
			uint32_t rate = (uint32_t)((uint64_t)(ticks - last_ticks) * OST_HZ /
						   (now - last_ost));

			snprintk(text, sizeof(text), "%u, %u/s", ticks, rate);
			show_row(ROW_TICKS, TONE_LIVE);
		}
		last_ost = now;
		last_ticks = ticks;

		if (ticks_ok) {
			k_msleep(100);
		} else {
			wait_ms(100);
		}
	}
	return 0;
}
