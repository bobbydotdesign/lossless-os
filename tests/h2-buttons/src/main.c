/*
 * Test 3 on the H2: buttons, the wheel, and switching off (docs/h2-hardware.md).
 *
 * After choosing "Linux" in the bootloader menu, a tester:
 *   - each button's box lights orange while held and keeps a green border
 *     once it has worked; "Seen" counts them;
 *   - turning the wheel walks a dot along a track and counts steps;
 *   - the power chip's name and the battery;
 *   - holding Power for 2 seconds switches the player off through the power
 *     chip, so the long hardware hold is no longer needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <stdio.h>

#include "draw.h"
#include "screen.h"
#include "h2_buttons.h"
#include "h2_power.h"
#include "page.h"

#define OFF_HOLD_MS 2000

static char power_text[40];
static char error_text[40];

static void read_power(enum h2_pmu pmu, uint8_t id)
{
	switch (pmu) {
	case PMU_AXP2101: {
		int mv = h2_power_battery_mv(), pct = h2_power_battery_percent();

		if (mv < 0) {
			snprintf(power_text, sizeof(power_text), "AXP2101, no battery reading");
		} else if (pct < 0) {
			snprintf(power_text, sizeof(power_text), "AXP2101, %d.%02d V",
				 mv / 1000, mv % 1000 / 10);
		} else {
			snprintf(power_text, sizeof(power_text), "AXP2101, %d.%02d V, %d%%",
				 mv / 1000, mv % 1000 / 10, pct);
		}
		break;
	}
	case PMU_AXP192:
		snprintf(power_text, sizeof(power_text), "AXP192");
		break;
	case PMU_UNKNOWN:
		snprintf(power_text, sizeof(power_text), "unknown chip, id %02x", id);
		break;
	default:
		snprintf(power_text, sizeof(power_text), "no answer on I2C");
	}
}

int main(void)
{
	printk("Lossless OS test 3\n");
	if (!screen_frame()) {
		screen_backlight_blink(10, 8000000);
		for (;;) {
		}
	}

	uint8_t id;
	enum h2_pmu pmu = h2_power_init(&id);

	printk("power chip id %02x\n", id);
	read_power(pmu, id);
	h2_buttons_start();

	struct tester t = { .power = power_text };
	int64_t power_down_at = 0, next_battery = k_uptime_get() + 1000;
	bool armed = false;    /* Power seen released at least once */

	for (;;) {
		int64_t now = k_uptime_get();

		t.held = h2_buttons_held();
		t.seen |= t.held;
		t.wheel_steps = h2_wheel_steps();

		if (!(t.held & (1u << BTN_POWER))) {
			armed = true;
		}
		if (armed && (t.held & (1u << BTN_POWER))) {
			if (!power_down_at) {
				power_down_at = now;
			}
			t.off_hold_ms = (int)(now - power_down_at);
		} else {
			power_down_at = 0;
			t.off_hold_ms = 0;
		}

		if (now >= next_battery) {
			read_power(pmu, id);
			next_battery = now + 1000;
		}

		const char *older, *newest;

		screen_log_tail(&older, &newest);
		t.message = newest;
		page_draw(&t);
		screen_show();

		if (t.off_hold_ms >= OFF_HOLD_MS) {
			page_switching_off(NULL);
			screen_show();
			printk("switching off\n");
			k_msleep(300);
			int err = h2_power_off();

			snprintf(error_text, sizeof(error_text), "Couldn't switch off (%d)", err);
			page_switching_off(error_text);
			screen_show();
			/* Wait for Power to be let go, then back to the tester */
			while (h2_buttons_held() & (1u << BTN_POWER)) {
				k_msleep(20);
			}
			k_msleep(3000);
			power_down_at = 0;
		}
		k_msleep(30);
	}
	return 0;
}
