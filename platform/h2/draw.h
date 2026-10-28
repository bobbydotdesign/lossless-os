/*
 * Drawing into a 320 x 240 RGB565 buffer: rectangles, the lossless mark and
 * text in the dot-matrix font. Plain C, no Zephyr, so the page can also be
 * previewed on the Mac (host/preview.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_DRAW_H
#define H2_DRAW_H

#include <stdint.h>

#define DRAW_W 320
#define DRAW_H 240

extern uint16_t draw_buf[DRAW_W * DRAW_H];

/* 0xRRGGBB to RGB565 */
uint16_t draw_rgb(uint32_t hex);

void draw_fill(int x, int y, int w, int h, uint32_t hex);

/* The lossless mark (two rings of rounded tiles on a 4 x 8 grid) with its
 * top-left corner at x, y over the background colour bg. */
void draw_mark(int x, int y, int tile, int gap, uint32_t bg);

/* The mark's size at start-up and switching off (centred on the screen) */
#define BOOT_MARK_TILE 11
#define BOOT_MARK_GAP  2
int draw_mark_width(int tile, int gap);
int draw_mark_height(int tile, int gap);

/* Text in the dot-matrix font, each dot scale x scale pixels, cut off at
 * x_max. Lines are 8 dots tall (the last is for descenders). Returns the x
 * after the text. Characters the font lacks show as '?'. */
int draw_text(int x, int y, const char *s, int scale, uint32_t hex, int x_max);
int draw_text_width(const char *s, int scale);

#endif
