/*
 * The screens of test 2: the mark and the diagnostics page. Layout only; the values come from main.c (or
 * from host/preview.c on the Mac).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_PAGE_H
#define H2_PAGE_H

enum page_row {
	ROW_CHIP, ROW_CACHES, ROW_CPU, ROW_COUNT, ROW_MEMORY,
	ROW_TIMER, ROW_TICKS, ROW_SLEEP, PAGE_ROWS
};

enum page_tone { TONE_PLAIN, TONE_LIVE, TONE_OK, TONE_BAD, TONE_WAIT };

void page_splash(void);
void page_frame(void);
void page_row(enum page_row row, const char *value, enum page_tone tone);
void page_log(const char *older, const char *newest);

#endif
