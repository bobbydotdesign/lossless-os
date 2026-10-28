/*
 * Test 2's screens on the Mac, with made-up values, to check the layout:
 * writes splash.ppm, page.ppm and stopped.ppm (320 x 240). See preview.sh.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "draw.h"
#include "page.h"
#include "ui.h"

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
	page_splash();
	save("splash.ppm");

	page_frame();
	page_row(ROW_CHIP, "X1000, id 2ed1024f", TONE_PLAIN);
	page_row(ROW_CACHES, "16 KB + 16 KB", TONE_PLAIN);
	page_row(ROW_CPU, "1008 MHz, RAM 200", TONE_PLAIN);
	page_row(ROW_COUNT, "1008 MHz", TONE_PLAIN);
	page_row(ROW_MEMORY, "15.8 MB free, ok", TONE_OK);
	page_row(ROW_TIMER, "123.4 s", TONE_LIVE);
	page_row(ROW_TICKS, "120342, 1000/s", TONE_LIVE);
	page_row(ROW_SLEEP, "200 ms took 200 ms", TONE_OK);
	page_log("boot status 00400004", "boot cause 00800000");
	save("page.ppm");

	page_frame();
	page_row(ROW_CHIP, "X1000, id 2ed1024f", TONE_PLAIN);
	page_row(ROW_TICKS, "none arrived", TONE_BAD);
	page_row(ROW_SLEEP, "skipped, no ticks", TONE_WAIT);
	page_log("boot status 00400004", "boot cause 00800000");
	save("page-bad.ppm");

	const char *lines[] = { "at   81002a10", "ra   810029f0", "code 7 (cause 0000001c)",
				"addr 00000000", "stat 0000ff03", "", "ticks: 500 in 0.5 s",
				">>> ZEPHYR FATAL ERROR 0" };
	ui_stopped("CPU exception", lines, 8);
	save("stopped.ppm");
	return 0;
}
