/*
 * Clocks: the APLL at 1008 MHz and the bus dividers, as the player runs
 * today (measured: research notes, step 2), so the memory runs at
 * 1008 / 5 = 201.6 MHz.
 *
 * X1000 Programming Manual 11.1:
 *   CPCCR  0xb0000000  31:30 SCLK_A source (01 crystal, 10 APLL); 29:28,
 *                      27:26, 25:24 CPU, AHB0, AHB2 source (01 SCLK_A);
 *                      22/21/20 change enables for CPU+L2, AHB0, AHB2 and
 *                      APB; 19:16 APB, 15:12 AHB2, 11:8 AHB0, 7:4 L2,
 *                      3:0 CPU divider - 1
 *   CPCSR  0xb00000d4  31 SCLK_A switch settled; 2:0 AHB2, AHB0, CPU
 *                      divider change busy
 *   CPAPCR 0xb0000010  31 high band; 30:24 M, 22:18 N, 17:16 OD; 10 on and
 *                      stable; 8 enable; 7:0 settle time (32 kHz cycles)
 *   FOUT = 24 MHz x (M + 1) / (N + 1) / 2^OD (11.1.3.1)
 *   CPU = 1, 2, 3 or 4 x L2; AHB2 = 1 or 2 x APB (11.1.4)
 *
 * The manual gives 1000 MHz as the PLL's top; 1008 is what the player has
 * always run. Changing it means changing the drivers' dividers too, so
 * it's one constant here and a decision for later. Over USB boot the ROM
 * leaves SCLK_A on the APLL (576 MHz); started from flash it may not, so
 * SCLK_A is moved onto the APLL if needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "stage1.h"

#define CPCCR  0xb0000000u
#define CPAPCR 0xb0000010u
#define CPMPCR 0xb0000014u
#define CPCSR  0xb00000d4u

#define APLL_M 41   /* x42: 1008 MHz */
#define MPLL_M 24   /* x25: 600 MHz, the stand-in while the APLL changes */

#define SRC_APLL (2u << 30)
#define SEL_SCLKA ((1u << 28) | (1u << 26) | (1u << 24))
#define SEL_MPLL ((2u << 28) | (2u << 26) | (2u << 24))
#define SEL_MASK ((3u << 28) | (3u << 26) | (3u << 24))
#define CE_ALL ((1u << 22) | (1u << 21) | (1u << 20))
#define DIV_MASK 0x000fffffu
#define DIVS_SAFE ((7u << 16) | (3u << 12) | (3u << 8) | (1u << 4) | 0u)  /* /8 /4 /4 /2 /1 */
#define DIVS ((9u << 16) | (4u << 12) | (4u << 8) | (1u << 4) | 0u)       /* /10 /5 /5 /2 /1 */
#define MUX_SETTLED ((1u << 30) | (1u << 29) | (1u << 28))  /* CPU, AHB0, AHB2 */

static int wait_set(uint32_t reg, uint32_t mask, uint32_t want, uint32_t tries)
{
	while (tries--) {
		if ((REG(reg) & mask) == want)
			return 0;
	}
	return -1;
}

/* New dividers, with the change enables, then wait for them */
static int set_divs(uint32_t divs)
{
	uint32_t keep = REG(CPCCR) & ~(CE_ALL | DIV_MASK);

	REG(CPCCR) = keep | CE_ALL | divs;
	if (wait_set(CPCSR, 7u, 0, 1000000))
		return -1;
	REG(CPCCR) = keep | divs;
	return 0;
}

static int set_mux(uint32_t sel)
{
	REG(CPCCR) = (REG(CPCCR) & ~(SEL_MASK | CE_ALL)) | sel;
	return wait_set(CPCSR, MUX_SETTLED, MUX_SETTLED, 1000000);
}

/*
 * The CPU and buses never run slow, so USB boot's connection to the
 * computer keeps going: they move to the MPLL (600 MHz) while the APLL
 * changes, then back. The buses stay at 120 MHz or more throughout.
 */
int clocks_init(void)
{
	note(REG(CPCCR));
	note(REG(CPAPCR));
	note(REG(CPMPCR));

	/* Already set (a later run over USB boot): leave them */
	if ((REG(CPCCR) & ~CE_ALL) == (SRC_APLL | SEL_SCLKA | DIVS) &&
	    (REG(CPAPCR) & 0xffff0500u) == ((1u << 31) | (APLL_M << 24) | (1u << 10) | (1u << 8)))
		return 0;

	/* 1. The MPLL at 600 MHz */
	REG(CPMPCR) = (1u << 31) | (MPLL_M << 24) | (1u << 7);
	if (wait_set(CPMPCR, 1u, 1u, 10000000))
		return 1;

	/* 2. Dividers safe for 600 MHz, then CPU and buses onto the MPLL */
	if (set_divs(DIVS_SAFE))
		return 2;
	if (set_mux(SEL_MPLL))
		return 3;

	/* 3. The APLL at 1008 MHz (SCLK_A stays on it) */
	REG(CPAPCR) = (1u << 31) | (APLL_M << 24) | (1u << 8) | 0x20u;
	if (wait_set(CPAPCR, 1u << 10, 1u << 10, 10000000))
		return 4;
	/* SCLK_A onto the APLL if it isn't (USB boot leaves it there; a start
	 * from flash may leave it on the crystal). Nothing runs on SCLK_A
	 * now: the CPU and buses are on the MPLL. */
#ifdef FLASH_BOOT
	if ((REG(CPCCR) & (3u << 30)) != SRC_APLL) {
		REG(CPCCR) = (REG(CPCCR) & ~((3u << 30) | CE_ALL)) | SRC_APLL;
		for (volatile uint32_t i = 0; i < 100000; i++)
			;
		if ((REG(CPCCR) & (3u << 30)) != SRC_APLL)
			return 5;
	}
#else
	if ((REG(CPCCR) & (3u << 30)) != SRC_APLL)
		return 5;
#endif

	/* 4. The final dividers (on 600 MHz for now: CPU 600, buses 120),
	 * then back onto the APLL */
	if (set_divs(DIVS))
		return 6;
	if (set_mux(SEL_SCLKA))
		return 7;

	/* 5. The MPLL off again */
	REG(CPMPCR) &= ~(1u << 7);

	note(REG(CPCCR));
	note(REG(CPAPCR));
	note(REG(CPMPCR));
	note(REG(CPCSR));
	return 0;
}
