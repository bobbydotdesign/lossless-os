/*
 * Our first stage: clocks and memory, from on-chip memory.
 *
 * Every step writes what it did to a status block at the top of on-chip
 * memory, which the computer reads back over USB boot after the stage
 * hands back to the ROM (tools/h2-usb read 0xf4002e00 256 ...).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef STAGE1_H
#define STAGE1_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* The status block: "LOS1", the last step reached, 0 or an error code,
 * then up to 252 words of values, step by step (1 KB in all). */
#ifdef FLASH_BOOT
#define STATUS_ADDR 0xf4003c00u   /* from flash: the top 1 KB, see tcsm-flash.ld */
#else
#define STATUS_ADDR 0xf4002e00u
#endif
#define STATUS_MAGIC 0x31534f4cu  /* "LOS1" */
struct status {
	uint32_t magic;
	uint32_t step;
	uint32_t error;
	uint32_t n;
	uint32_t v[252];
};
#define status (*(volatile struct status *)STATUS_ADDR)

static inline void note(uint32_t v)
{
	if (status.n < 252)
		status.v[status.n++] = v;
}

/* Busy-wait for at least `us` microseconds at any CPU clock up to
 * 1008 MHz (a loop turn takes at least one cycle). */
void delay_us(uint32_t us);

/* clocks.c: CPU 1008 MHz, L2 504, AHB0 and AHB2 201.6, APB 100.8.
 * 0 or an error code. */
int clocks_init(void);

/* power.c: the supplies the player runs with. 0 or an error code. */
int power_init(void);

/* A breadcrumb in the power chip's data buffer (reg 0x04), which only a
 * complete loss of power clears: how far the first stage got, readable
 * after any restart. No-op before power_init or without the power step. */
#ifdef POWER_ON
void crumb(uint8_t step);
void crumbs_on(void);
void crumb4(uint8_t step, uint8_t a, uint8_t b, uint8_t c);
#else
static inline void crumb(uint8_t step) { (void)step; }
#endif

/* memory.c: LPDDR at 201.6 MHz, then a test of all 32 MB. 0 or an
 * error code. */
int memory_init(void);
int memory_test(void);

#endif
