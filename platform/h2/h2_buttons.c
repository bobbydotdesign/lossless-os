/*
 * Every button pulls its pin low while pressed. The wheel is a quadrature
 * encoder on two pins: turning it walks them through 00, 01, 11, 10 one way
 * and the reverse the other, so each change of one pin is a step and its
 * direction follows from the previous state.
 *
 * Polled from a 1 ms timer: far faster than a hand turns the wheel, and no
 * GPIO interrupts needed yet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "h2_buttons.h"

#include <zephyr/kernel.h>

#define REG(a)      (*(volatile uint32_t *)(a))
#define PIN(port)   REG(0xb0010000u + (port) * 0x100u)   /* pin levels */

enum { PA, PB, PC, PD };

/* Hardware revision 4 (the H2): Power on PB31, Back on PA18 */
static const struct { uint8_t port, pin; } pins[BTN_COUNT] = {
	[BTN_POWER] = {PB, 31},
	[BTN_MENU] = {PB, 28},
	[BTN_BACK] = {PA, 18},
	[BTN_PLAY] = {PA, 16},
	[BTN_PREV] = {PD, 4},
	[BTN_NEXT] = {PC, 24},
	[BTN_VOL_UP] = {PA, 17},
	[BTN_VOL_DOWN] = {PA, 19},
};

#define WHEEL_A     24              /* PB24 */
#define WHEEL_B     23              /* PB23 */
#define DEBOUNCE_MS 10

static struct k_timer poll_timer;
static volatile uint32_t held;
static volatile int32_t steps;
static volatile uint32_t wheel_pins;
static uint32_t last_raw;
static uint8_t stable_ms;
static uint32_t wheel_last;

/* Step direction from (previous state << 2 | new state); 0 = no change or
 * an impossible jump (both pins changed: a missed step, ignored).
 * Clockwise (+1) walks A,B through 00, 10, 11, 01 (seen on our H2). */
static const int8_t wheel_step[16] = {
	 0, -1, +1,  0,
	+1,  0,  0, -1,
	-1,  0,  0, +1,
	 0, +1, -1,  0,
};

static void poll(struct k_timer *t)
{
	ARG_UNUSED(t);
	uint32_t port[4] = { PIN(PA), PIN(PB), PIN(PC), PIN(PD) };
	uint32_t raw = 0;

	for (int b = 0; b < BTN_COUNT; b++) {
		if (!(port[pins[b].port] & (1u << pins[b].pin))) {
			raw |= 1u << b;
		}
	}
	if (raw != last_raw) {
		last_raw = raw;
		stable_ms = 0;
	} else if (stable_ms < DEBOUNCE_MS) {
		if (++stable_ms == DEBOUNCE_MS) {
			held = raw;
		}
	}

	uint32_t w = (port[PB] >> WHEEL_A & 1) << 1 | (port[PB] >> WHEEL_B & 1);

	steps += wheel_step[wheel_last << 2 | w];
	wheel_last = w;
	wheel_pins = w;
}

void h2_buttons_start(void)
{
	uint32_t b = PIN(PB);

	wheel_last = (b >> WHEEL_A & 1) << 1 | (b >> WHEEL_B & 1);
	k_timer_init(&poll_timer, poll, NULL);
	k_timer_start(&poll_timer, K_MSEC(1), K_MSEC(1));
}

uint32_t h2_buttons_held(void) { return held; }
int32_t h2_wheel_steps(void) { return steps; }
uint32_t h2_wheel_pins(void) { return wheel_pins; }
