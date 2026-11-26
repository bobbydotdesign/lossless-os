/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui.h"
#include "draw.h"

void ui_header(const char *title)
{
	draw_fill(0, 0, DRAW_W, DRAW_H, UI->bg);
	draw_mark(UI_MARGIN, 10, 6, 2, UI->bg);
	draw_text(92, 10, "Lossless OS", UI_TEXT, UI->fg, DRAW_W);
	draw_text(92, 28, title, UI_TEXT, UI->muted, DRAW_W);
	draw_fill(0, 47, DRAW_W, 1, UI->line);
}

void ui_stopped(const char *why, const char *const *lines, int count)
{
	draw_fill(0, 0, DRAW_W, DRAW_H, 0x2A0F0C);
	draw_text(UI_MARGIN, 12, "Stopped", 3, UI->danger, DRAW_W);
	draw_text(UI_MARGIN, 44, why, UI_TEXT, UI->fg, DRAW_W - 4);
	for (int i = 0; i < count && i < 9; i++) {
		draw_text(UI_MARGIN, 68 + i * 19, lines[i], UI_TEXT, UI->muted, DRAW_W - 4);
	}
}
