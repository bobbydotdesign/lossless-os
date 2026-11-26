/*
 * Our second stage: the card, the boot file, Lossless OS.
 *
 * Over USB boot it hands back to the ROM if it can't start Lossless OS,
 * and its status block (0xa0fff000, uncached main memory) says why.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef STAGE2_H
#define STAGE2_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* "LOS2", the last step reached, 0 or an error code, then values */
#define STATUS_ADDR 0xa0fff000u
#define STATUS_MAGIC 0x32534f4cu   /* "LOS2" */
struct status {
	uint32_t magic, step, error, n;
	uint32_t v[60];
};
#define status (*(volatile struct status *)STATUS_ADDR)

static inline void note(uint32_t v)
{
	if (status.n < 60)
		status.v[status.n++] = v;
}

void delay_us(uint32_t us);

/* sd.c */
int sd_init(void);
int sd_read(uint8_t *buf, uint32_t lba, uint32_t count);

#endif
