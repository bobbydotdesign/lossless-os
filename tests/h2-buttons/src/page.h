/*
 * Test 3's screen: the button tester. Layout only; main.c (or host/preview.c
 * on the Mac) fills in the state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_BUTTONS_PAGE_H
#define H2_BUTTONS_PAGE_H

#include <stdbool.h>
#include <stdint.h>

struct tester {
	uint32_t held;          /* bit per button (enum h2_button) */
	uint32_t seen;          /* buttons pressed at least once */
	int32_t wheel_steps;
	const char *power;      /* "AXP2101, 3.95 V, 82%" */
	int off_hold_ms;        /* how long Power has been held, 0 if not */
	const char *message;    /* bottom line: the log, or a warning */
	bool message_bad;
};

void page_draw(const struct tester *t);

/* Full screen while switching off; `error` set if that failed */
void page_switching_off(const char *error);

#endif
