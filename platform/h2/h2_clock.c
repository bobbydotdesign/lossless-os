/*
 * The H2's clock (the X1000's RTC): local time, kept while off.
 * Facts: docs/h2-hardware.md, "Clock (RTC)".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include "los_sys.h"
#include "x1000_soc.h"

#define RTC(off)   X1000_REG(0xb0003000u + (off))
#define CR         0x00
#define SR         0x04
#define GR         0x0c
#define HSPR       0x34
#define WENR       0x3c
#define CR_ON      (1u << 0)
#define CR_READY   (1u << 7)
#define WENR_OPEN  (1u << 31)
#define SET_UP     0x52544356u    /* "RTCV": set up, and still powered */
#define DIVIDER    32767u

static bool wait(uint32_t off, uint32_t bit)
{
	for (int i = 0; i < 100000; i++) {
		if (RTC(off) & bit) {
			return true;
		}
	}
	return false;
}

static bool rtc_write(uint32_t off, uint32_t value)
{
	if (!wait(CR, CR_READY)) {
		return false;
	}
	RTC(WENR) = 0xa55a;
	if (!wait(WENR, WENR_OPEN) || !wait(CR, CR_READY)) {
		return false;
	}
	RTC(off) = value;
	return wait(CR, CR_READY);
}

static bool set_up(void)
{
	return RTC(HSPR) == SET_UP && (RTC(GR) & 0xffff) == DIVIDER && (RTC(CR) & CR_ON);
}

bool los_clock_get(int64_t *seconds)
{
	if (!set_up()) {
		return false;
	}
	*seconds = RTC(SR);
	return true;
}

bool los_clock_set(int64_t seconds)
{
	if (!set_up()) {
		/* the clock lost power: set it up again first */
		if (!rtc_write(GR, DIVIDER) || !rtc_write(CR, CR_ON) || !rtc_write(HSPR, SET_UP)) {
			return false;
		}
	}
	return rtc_write(SR, (uint32_t)seconds);
}
