/*
 * The H2's backlight: PWM channel 0 (TCU timer 0) on PC25, a 30 us
 * period; brightness is the share of the period the pin is high.
 * Facts: docs/h2-hardware.md, "Backlight" and "PWM".
 *
 * "Normal" (100%) is whatever the bootloader left it at, so the screen
 * looks as it did; if it left the pin as a plain output (high, or low:
 * it switches the light off before starting the boot file on the card),
 * that's full brightness.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include "los_sys.h"
#include "x1000_soc.h"

#define PIN             25          /* PC25, function 0: PWM 0 */
#define CH              0
#define PERIOD_NS       30000

#define TCU(off)        X1000_REG(0xb0002000u + (off))
#define TCU_ENABLE      0x10
#define TCU_ENABLE_SET  0x14
#define TCU_ENABLE_CLR  0x18
#define TCU_STOP_CLR    0x3c
#define TCU_FULL(n)     (0x40 + (n) * 0x10)
#define TCU_HALF(n)     (0x44 + (n) * 0x10)
#define TCU_COUNT(n)    (0x48 + (n) * 0x10)
#define TCU_CTRL(n)     (0x4c + (n) * 0x10)
#define CTRL_PCLK       1u
#define CTRL_PWM        (1u << 7)
#define CLKGR_TCU       (1u << 18)

static bool ready;
static uint32_t full, normal_high;    /* counts per period; counts high at 100% */

/* Is PC25 given to the PWM (INT 0, MSK 0, PAT1 0, PAT0 0)? */
static bool pin_is_pwm(void)
{
	uint32_t bit = 1u << PIN;

	return !(X1000_GPIO(X1000_PC, 0x10) & bit) && !(X1000_GPIO(X1000_PC, 0x20) & bit) &&
	       !(X1000_GPIO(X1000_PC, 0x30) & bit) && !(X1000_GPIO(X1000_PC, 0x40) & bit);
}

static void setup(void)
{
	ready = true;
	if (pin_is_pwm() && (TCU(TCU_ENABLE) & (1u << CH)) && TCU(TCU_FULL(CH))) {
		/* the bootloader's PWM: keep its period and level */
		full = TCU(TCU_FULL(CH));
		uint32_t half = TCU(TCU_HALF(CH));
		normal_high = half < full ? full - half : full;
		return;
	}
	/* a plain high pin (full brightness): our own PWM at that level */
	uint32_t pclk = x1000_pclk_hz();

	full = (uint32_t)((uint64_t)pclk * PERIOD_NS / 1000000000u);
	if (full < 100 || full > 0xffff) {
		full = 0;            /* can't time it: on and off only */
		return;
	}
	normal_high = full;
	X1000_REG(X1000_CPM_CLKGR) &= ~CLKGR_TCU;
	TCU(TCU_STOP_CLR) = 1u << CH;
	TCU(TCU_ENABLE_CLR) = 1u << CH;
	TCU(TCU_CTRL(CH)) = CTRL_PWM | CTRL_PCLK;
	TCU(TCU_COUNT(CH)) = 0;
	TCU(TCU_FULL(CH)) = full;
	TCU(TCU_HALF(CH)) = 0;
	TCU(TCU_ENABLE_SET) = 1u << CH;
}

void los_backlight(int percent)
{
	if (!ready) {
		setup();
	}
	if (percent <= 0) {
		x1000_pin_out(X1000_PC, PIN, 0);
		return;
	}
	if (!full || (percent >= 100 && normal_high == full)) {
		if (full) {
			TCU(TCU_HALF(CH)) = 0;           /* high all period */
			x1000_pin_function(X1000_PC, PIN, 0);
		} else {
			x1000_pin_out(X1000_PC, PIN, 1);
		}
		return;
	}
	uint32_t high = (uint32_t)((uint64_t)normal_high * (uint32_t)(percent > 100 ? 100 : percent) / 100);

	if (high < 1) {
		high = 1;
	}
	TCU(TCU_HALF(CH)) = full - high;
	TCU(TCU_ENABLE_SET) = 1u << CH;
	x1000_pin_function(X1000_PC, PIN, 0);
}
