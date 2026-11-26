/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page.h"
#include "draw.h"
#include "ui.h"

#define C        UI
#define MARGIN   UI_MARGIN
#define VALUE_X  104
#define ROW_Y0   56
#define ROW_STEP 19
#define LOG_Y0   208
#define LOG_STEP 16
#define TEXT     2      /* dot size in pixels */
#define TEXT_H   (8 * TEXT)

static const char *const labels[PAGE_ROWS] = {
	[ROW_CHIP] = "Chip",
	[ROW_CACHES] = "Caches",
	[ROW_CPU] = "CPU",
	[ROW_COUNT] = "Count",
	[ROW_MEMORY] = "Memory",
	[ROW_TIMER] = "Timer",
	[ROW_TICKS] = "Ticks",
	[ROW_SLEEP] = "Sleep",
};

static uint32_t tone_colour(enum page_tone tone)
{
	switch (tone) {
	case TONE_LIVE: return C->accentInk;
	case TONE_OK:   return C->ok;
	case TONE_BAD:  return C->danger;
	case TONE_WAIT: return C->faint;
	default:        return C->fg;
	}
}

void page_splash(void)
{
	int tile = 15, gap = 3;
	int w = draw_mark_width(tile, gap), h = draw_mark_height(tile, gap);
	const char *name = "Lossless OS";

	draw_fill(0, 0, DRAW_W, DRAW_H, 0x000000);
	draw_mark((DRAW_W - w) / 2, (DRAW_H - h) / 2 - 16, tile, gap, 0x000000);
	draw_text((DRAW_W - draw_text_width(name, TEXT)) / 2, (DRAW_H + h) / 2 + 8,
		  name, TEXT, C->muted, DRAW_W);
}

void page_frame(void)
{
	ui_header("test 2: diagnostics");
	for (int r = 0; r < PAGE_ROWS; r++) {
		draw_text(MARGIN, ROW_Y0 + r * ROW_STEP, labels[r], TEXT, C->muted, VALUE_X);
		page_row((enum page_row)r, "...", TONE_WAIT);
	}
}

void page_row(enum page_row row, const char *value, enum page_tone tone)
{
	int y = ROW_Y0 + (int)row * ROW_STEP;

	draw_fill(VALUE_X, y, DRAW_W - VALUE_X, TEXT_H, C->bg);
	draw_text(VALUE_X, y, value, TEXT, tone_colour(tone), DRAW_W - 4);
}

void page_log(const char *older, const char *newest)
{
	draw_fill(0, LOG_Y0 - 3, DRAW_W, DRAW_H - LOG_Y0 + 3, C->bg);
	draw_fill(0, LOG_Y0 - 3, DRAW_W, 1, C->line);
	draw_text(MARGIN, LOG_Y0 + 1, older, TEXT, C->faint, DRAW_W - 4);
	draw_text(MARGIN, LOG_Y0 + 1 + LOG_STEP, newest, TEXT, C->muted, DRAW_W - 4);
}
