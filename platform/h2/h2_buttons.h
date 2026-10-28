/*
 * The H2's buttons and scroll wheel, read 1000 times a second.
 * Pins: docs/h2-hardware.md, "Buttons and wheel".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_BUTTONS_H
#define H2_BUTTONS_H

#include <stdint.h>

enum h2_button {
	BTN_POWER, BTN_MENU, BTN_BACK, BTN_PLAY,
	BTN_PREV, BTN_NEXT, BTN_VOL_UP, BTN_VOL_DOWN, BTN_COUNT
};

/* Start reading (needs the system timer) */
void h2_buttons_start(void);

/* Bit n set while button n is held (debounced over 10 ms) */
uint32_t h2_buttons_held(void);

/* Wheel: total steps so far (one per click), + clockwise, - anticlockwise */
int32_t h2_wheel_steps(void);

/* The wheel's two pins as last read (bit 1 A, bit 0 B), for checking */
uint32_t h2_wheel_pins(void);

#endif
