/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page.h"
#include "draw.h"
#include "ui.h"
#include "h2_buttons.h"

#include <stdio.h>

#define OFF_HOLD_MS 2000

/* On screen in this order, two rows of four */
static const struct { enum h2_button id; const char *label; } boxes[BTN_COUNT] = {
	{BTN_PLAY, "Play"}, {BTN_PREV, "Prev"}, {BTN_NEXT, "Next"}, {BTN_MENU, "Menu"},
	{BTN_BACK, "Back"}, {BTN_VOL_UP, "Vol +"}, {BTN_VOL_DOWN, "Vol -"}, {BTN_POWER, "Power"},
};

#define BOX_W   70
#define BOX_H   30
#define BOX_GAP 6
#define BOX_X0  ((DRAW_W - 4 * BOX_W - 3 * BOX_GAP) / 2)

static void outline(int x, int y, int w, int h, uint32_t c)
{
	draw_fill(x, y, w, 1, c);
	draw_fill(x, y + h - 1, w, 1, c);
	draw_fill(x, y, 1, h, c);
	draw_fill(x + w - 1, y, 1, h, c);
}

static void draw_box(int i, bool held, bool seen)
{
	int x = BOX_X0 + (i % 4) * (BOX_W + BOX_GAP);
	int y = UI_TOP + (i / 4) * (BOX_H + BOX_GAP);
	const char *label = boxes[i].label;
	uint32_t ink;

	if (held) {
		draw_fill(x, y, BOX_W, BOX_H, UI->accent);
		ink = UI->accentFg;
	} else {
		draw_fill(x, y, BOX_W, BOX_H, UI->raised);
		outline(x, y, BOX_W, BOX_H, seen ? UI->ok : UI->line);
		ink = seen ? UI->fg : UI->faint;
	}
	draw_text(x + (BOX_W - draw_text_width(label, UI_TEXT)) / 2,
		  y + (BOX_H - 7 * UI_TEXT) / 2, label, UI_TEXT, ink, x + BOX_W);
}

void page_draw(const struct tester *t)
{
	char line[48];
	int seen = 0;

	ui_header("test 3: buttons");
	for (int i = 0; i < BTN_COUNT; i++) {
		uint32_t bit = 1u << boxes[i].id;

		draw_box(i, t->held & bit, t->seen & bit);
		seen += !!(t->seen & bit);
	}

	/* Wheel: a dot that walks along a track, one place per click */
	int y = UI_TOP + 2 * (BOX_H + BOX_GAP) + 2;
	int dots = 16, pos = (t->wheel_steps % dots + dots) % dots;

	draw_text(UI_MARGIN, y, "Wheel", UI_TEXT, UI->muted, DRAW_W);
	for (int i = 0; i < dots; i++) {
		draw_fill(92 + i * 8, y + 5, 4, 4, i == pos ? UI->accentInk : UI->line);
	}
	snprintf(line, sizeof(line), "%+d", (int)t->wheel_steps);
	draw_text(228, y, line, UI_TEXT, UI->fg, DRAW_W - 4);

	y += 22;
	draw_text(UI_MARGIN, y, "Power", UI_TEXT, UI->muted, DRAW_W);
	draw_text(92, y, t->power, UI_TEXT, UI->fg, DRAW_W - 4);

	y += 22;
	snprintf(line, sizeof(line), "%d of %d", seen, BTN_COUNT);
	draw_text(UI_MARGIN, y, "Seen", UI_TEXT, UI->muted, DRAW_W);
	draw_text(92, y, line, UI_TEXT, seen == BTN_COUNT ? UI->ok : UI->fg, DRAW_W - 4);

	/* Switch-off hint, with a bar that fills while Power is held */
	y += 22;
	draw_text(UI_MARGIN, y, "Hold Power 2 s to switch off", UI_TEXT,
		  t->off_hold_ms ? UI->fg : UI->faint, DRAW_W - 4);
	if (t->off_hold_ms) {
		int w = (DRAW_W - 2 * UI_MARGIN) * (t->off_hold_ms > OFF_HOLD_MS ? OFF_HOLD_MS :
						     t->off_hold_ms) / OFF_HOLD_MS;
		draw_fill(UI_MARGIN, y + 18, DRAW_W - 2 * UI_MARGIN, 3, UI->line);
		draw_fill(UI_MARGIN, y + 18, w, 3, UI->accent);
	}

	draw_fill(0, 221, DRAW_W, 1, UI->line);
	draw_text(UI_MARGIN, 224, t->message, UI_TEXT,
		  t->message_bad ? UI->danger : UI->faint, DRAW_W - 4);
}

void page_switching_off(const char *error)
{
	int tile = 15, gap = 3;
	int w = draw_mark_width(tile, gap), h = draw_mark_height(tile, gap);
	const char *text = error ? error : "Switching off";

	draw_fill(0, 0, DRAW_W, DRAW_H, 0x000000);
	draw_mark((DRAW_W - w) / 2, (DRAW_H - h) / 2 - 16, tile, gap, 0x000000);
	draw_text((DRAW_W - draw_text_width(text, UI_TEXT)) / 2, (DRAW_H + h) / 2 + 8,
		  text, UI_TEXT, error ? UI->danger : UI->muted, DRAW_W);
	if (error) {
		const char *hint = "Hold Power to force it off";

		draw_text((DRAW_W - draw_text_width(hint, UI_TEXT)) / 2,
			  (DRAW_H + h) / 2 + 30, hint, UI_TEXT, UI->muted, DRAW_W);
	}
}
