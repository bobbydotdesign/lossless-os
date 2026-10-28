/*
 * X1000 pins and clocks, for drivers written after the first few (which
 * still carry their own copies). Facts: docs/h2-hardware.md, "GPIO" and
 * "Clocks (CPM)".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef X1000_SOC_H
#define X1000_SOC_H

#include <stdint.h>

#define X1000_REG(a)   (*(volatile uint32_t *)(uintptr_t)(a))

enum { X1000_PA, X1000_PB, X1000_PC, X1000_PD };

#define X1000_GPIO(port, off) X1000_REG(0xb0010000u + (port) * 0x100u + (off))

/* A plain output at 'level': INT 0, MSK 1, PAT1 0, PAT0 = level */
static inline void x1000_pin_out(int port, int pin, int level)
{
	uint32_t bit = 1u << pin;

	X1000_GPIO(port, level ? 0x44 : 0x48) = bit;
	X1000_GPIO(port, 0x18) = bit;
	X1000_GPIO(port, 0x24) = bit;
	X1000_GPIO(port, 0x38) = bit;
}

/* An input: INT 0, MSK 1, PAT1 1 */
static inline void x1000_pin_in(int port, int pin)
{
	uint32_t bit = 1u << pin;

	X1000_GPIO(port, 0x18) = bit;
	X1000_GPIO(port, 0x24) = bit;
	X1000_GPIO(port, 0x34) = bit;
}

/* Given to a peripheral: INT 0, MSK 0, PAT1/PAT0 = function */
static inline void x1000_pin_function(int port, int pin, int fn)
{
	uint32_t bit = 1u << pin;

	X1000_GPIO(port, 0x18) = bit;
	X1000_GPIO(port, 0x28) = bit;
	X1000_GPIO(port, (fn & 2) ? 0x34 : 0x38) = bit;
	X1000_GPIO(port, (fn & 1) ? 0x44 : 0x48) = bit;
}

#define X1000_CPM_CCR   0xb0000000u
#define X1000_CPM_APCR  0xb0000010u
#define X1000_CPM_MPCR  0xb0000014u
#define X1000_CPM_CLKGR 0xb0000020u

static inline uint32_t x1000_pll_hz(uint32_t reg, uint32_t on_bit)
{
	uint32_t v = X1000_REG(reg);

	if (!(v & on_bit)) {
		return 0;
	}
	uint64_t hz = 24000000ull * ((v >> 24 & 0x7f) + 1) / ((v >> 18 & 0x1f) + 1);
	return (uint32_t)(hz >> (v >> 16 & 3));
}

/* The peripheral clock: CCR bits 25:24 source (1 SCLK_A, 2 MPLL) / (bits 19:16 + 1) */
static inline uint32_t x1000_pclk_hz(void)
{
	uint32_t ccr = X1000_REG(X1000_CPM_CCR), src;

	switch (ccr >> 24 & 3) {
	case 1:
		src = (ccr >> 30) == 1 ? 24000000u :
		      (ccr >> 30) == 2 ? x1000_pll_hz(X1000_CPM_APCR, 1u << 10) : 0;
		break;
	case 2:
		src = x1000_pll_hz(X1000_CPM_MPCR, 1u << 0);
		break;
	default:
		src = 0;
	}
	return src / ((ccr >> 16 & 0xf) + 1);
}

#endif
