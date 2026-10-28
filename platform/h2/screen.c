/*
 * Register facts: docs/h2-hardware.md
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "screen.h"
#include "draw.h"
#include "ui.h"

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include "los_sys.h"
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>
#include <string.h>

#define REG(a)          (*(volatile uint32_t *)(a))
#define UNCACHED(pa)    (0xa0000000u | (uint32_t)(pa))
#define RAM_BYTES       (32u * 1024 * 1024)

#define LCD_DA          0xb3050040u
#define LCD_MCTRL       0xb30500a4u
#define LCD_MSTATE      0xb30500a8u
#define MSTATE_BUSY     (1u << 0)
#define MCTRL_DMA_START (1u << 1)
#define MCTRL_DMA_MODE  (1u << 2)
#define FRAME_SIZE      ((DRAW_W - 1) | ((DRAW_H - 1) << 12))

#define GPIO_C          0xb0010200u
#define GPIO_INT_CLR    (GPIO_C + 0x18)
#define GPIO_MSK_SET    (GPIO_C + 0x24)
#define GPIO_PAT1_CLR   (GPIO_C + 0x38)
#define GPIO_PAT0_SET   (GPIO_C + 0x44)
#define GPIO_PAT0_CLR   (GPIO_C + 0x48)
#define BACKLIGHT_PIN   (1u << 25)

static uint32_t frame_pa;

bool h2_lcd_start(void);       /* h2_lcd.c */

uint32_t screen_frame(void)
{
	return frame_pa;
}

static bool in_ram(uint32_t pa, uint32_t bytes)
{
	return pa < RAM_BYTES && bytes <= RAM_BYTES - pa;
}

/* Follow the LCD controller's descriptor chain to the frame it sends. */
static uint32_t find_frame(void)
{
	uint32_t pa = REG(LCD_DA);

	for (int i = 0; i < 4; i++) {
		if ((pa & 3) || !in_ram(pa, 32)) {
			return 0;
		}
		volatile uint32_t *desc = (volatile uint32_t *)UNCACHED(pa);
		uint32_t source = desc[1];

		if (desc[7] == FRAME_SIZE && !(source & 3) &&
		    in_ram(source, DRAW_W * DRAW_H * 2)) {
			return source;
		}
		pa = desc[0];
	}
	return 0;
}

void screen_show(void)
{
	if (!frame_pa) {
		return;
	}
	/* Uncached stores go straight to memory, where the LCD's DMA reads. */
	volatile uint32_t *dst = (volatile uint32_t *)UNCACHED(frame_pa);
	const uint32_t *src = (const uint32_t *)draw_buf;

	for (int i = 0; i < DRAW_W * DRAW_H / 2; i++) {
		dst[i] = src[i];
	}
	/* Each start sends the frame once: let a running transfer finish
	 * (it may have sent old pixels), then send it again. Counted wait, as
	 * this also runs with interrupts off (the "Stopped" screen). */
	for (int i = 0; i < 20000000 && (REG(LCD_MSTATE) & MSTATE_BUSY); i++) {
	}
	REG(LCD_MCTRL) |= MCTRL_DMA_START | MCTRL_DMA_MODE;
}

void screen_backlight_blink(int times, uint32_t loops)
{
	REG(GPIO_PAT0_SET) = BACKLIGHT_PIN;
	REG(GPIO_INT_CLR) = BACKLIGHT_PIN;
	REG(GPIO_MSK_SET) = BACKLIGHT_PIN;
	REG(GPIO_PAT1_CLR) = BACKLIGHT_PIN;
	for (int t = 0; t < times; t++) {
		REG(GPIO_PAT0_CLR) = BACKLIGHT_PIN;
		for (volatile uint32_t i = 0; i < loops; i++) {
		}
		REG(GPIO_PAT0_SET) = BACKLIGHT_PIN;
		for (volatile uint32_t i = 0; i < loops; i++) {
		}
	}
}

/* The screen log: what Zephyr and we print, kept as lines. */

#define LOG_LINES 16
#define LOG_CHARS 60

static char log_lines[LOG_LINES][LOG_CHARS];
static unsigned log_count;     /* complete lines so far */
static char log_part[LOG_CHARS];
static unsigned log_len;

static int log_char(int c)
{
	unsigned key = irq_lock();

	if (c == '\n' || log_len == LOG_CHARS - 1) {
		log_part[log_len] = '\0';
		memcpy(log_lines[log_count % LOG_LINES], log_part, log_len + 1);
		log_count++;
		log_len = 0;
	}
	if (c != '\n' && c != '\r') {
		log_part[log_len++] = (char)c;
	}
	irq_unlock(key);
	return c;
}

static const char *log_line(unsigned back)
{
	if (back >= log_count || back >= LOG_LINES) {
		return "";
	}
	return log_lines[(log_count - 1 - back) % LOG_LINES];
}

void screen_log_tail(const char **older, const char **newest)
{
	*older = log_line(1);
	*newest = log_line(0);
}

static int screen_init(void)
{
	/* The bootloader switches the backlight off before starting the file
	 * it starts at power-on (not before the menu's Linux item): light it */
	/* a running screen only if the LCD's clock is on (reading a stopped
	 * peripheral's registers is not something to rely on) */
	frame_pa = (REG(0xb0000020u) & (1u << 23)) ? 0 : find_frame();
	if (!frame_pa && h2_lcd_start()) {
		/* nothing had started the screen (a normal power-on): ours */
		frame_pa = find_frame();
	}
	/* our mark on black, sent to the panel, then the light: the first
	 * thing seen, until the app draws */
	if (frame_pa) {
		int tile = BOOT_MARK_TILE, gap = BOOT_MARK_GAP;
		int w = draw_mark_width(tile, gap), h = draw_mark_height(tile, gap);

		draw_fill(0, 0, DRAW_W, DRAW_H, 0x000000);
		draw_mark((DRAW_W - w) / 2, (DRAW_H - h) / 2, tile, gap, 0x000000);
		screen_show();
		for (int i = 0; i < 20000000 && (REG(LCD_MSTATE) & MSTATE_BUSY); i++) {
		}
	}
	los_backlight(100);
	__printk_hook_install(log_char);
	return 0;
}

/* Right after the exception entry is in place, before the timer starts:
 * from here on any fatal error ends on the "Stopped" screen. */
SYS_INIT(screen_init, PRE_KERNEL_1, 1);

static const char *reason_text(unsigned int reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION: return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:  return "Unexpected interrupt";
	case K_ERR_STACK_CHK_FAIL: return "Stack overflow";
	case K_ERR_KERNEL_OOPS:   return "Kernel oops";
	case K_ERR_KERNEL_PANIC:  return "Kernel panic";
	default:                  return "Fatal error";
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	static char text[7][32];
	const char *lines[9];
	int n = 0;

	(void)irq_lock();
	if (esf) {
		snprintk(text[0], sizeof(text[0]), "at   %08lx", esf->epc);
		snprintk(text[1], sizeof(text[1]), "ra   %08lx", esf->ra);
		snprintk(text[2], sizeof(text[2]), "code %lu (cause %08lx)",
			 (esf->cause >> 2) & 0x1f, esf->cause);
		snprintk(text[3], sizeof(text[3]), "addr %08lx", esf->badvaddr);
		snprintk(text[4], sizeof(text[4]), "stat %08lx", esf->status);
		for (int i = 0; i < 5; i++) {
			lines[n++] = text[i];
		}
	}
	lines[n++] = "";
	lines[n++] = log_line(1);
	lines[n++] = log_line(0);
	ui_stopped(reason_text(reason), lines, n);
	screen_show();
	if (!frame_pa) {
		screen_backlight_blink(1000000, 2000000);
	}
	for (;;) {
		/* hold Power to switch off */
	}
}
