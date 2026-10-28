/*
 * Test 3's screens on the Mac with made-up states, to check the layout.
 * See preview.sh.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "draw.h"
#include "h2_buttons.h"
#include "page.h"

static void save(const char *path)
{
	FILE *f = fopen(path, "wb");

	fprintf(f, "P6\n%d %d\n255\n", DRAW_W, DRAW_H);
	for (int i = 0; i < DRAW_W * DRAW_H; i++) {
		unsigned c = draw_buf[i];
		unsigned char px[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 };

		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

int main(void)
{
	struct tester t = {
		.held = 0, .seen = 0, .wheel_steps = 0,
		.power = "AXP2101, 3.95 V, 82%", .message = "power chip id 4a",
	};

	page_draw(&t);
	save("buttons-start.ppm");

	t.held = 1u << BTN_PLAY;
	t.seen = (1u << BTN_PLAY) | (1u << BTN_PREV) | (1u << BTN_VOL_UP) | (1u << BTN_MENU);
	t.wheel_steps = -14;
	page_draw(&t);
	save("buttons-play.ppm");

	t.held = 1u << BTN_POWER;
	t.seen = 0xff;
	t.wheel_steps = 37;
	t.off_hold_ms = 1200;
	page_draw(&t);
	save("buttons-power.ppm");

	page_switching_off(NULL);
	save("off.ppm");
	page_switching_off("Couldn't switch off (-1)");
	save("off-failed.ppm");
	return 0;
}
