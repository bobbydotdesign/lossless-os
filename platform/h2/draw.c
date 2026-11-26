/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "draw.h"
#include "dotfont.h"

uint16_t draw_buf[DRAW_W * DRAW_H] __attribute__((aligned(32)));

uint16_t draw_rgb(uint32_t hex)
{
	return (uint16_t)(((hex >> 19) & 0x1f) << 11 | ((hex >> 10) & 0x3f) << 5 |
			  ((hex >> 3) & 0x1f));
}

void draw_fill(int x, int y, int w, int h, uint32_t hex)
{
	uint16_t c = draw_rgb(hex);
	int x1 = x + w, y1 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > DRAW_W) x1 = DRAW_W;
	if (y1 > DRAW_H) y1 = DRAW_H;
	for (int j = y; j < y1; j++) {
		for (int i = x; i < x1; i++) {
			draw_buf[j * DRAW_W + i] = c;
		}
	}
}

/* The mark, as in theme/tools/make_boot_logo.py */
#define LIGHT  0xEEECE7
#define ACCENT 0xFF5A1F

static const struct { uint8_t row, col; uint32_t colour; } tiles[] = {
	{0, 1, LIGHT}, {0, 2, LIGHT}, {0, 5, ACCENT}, {0, 6, ACCENT},
	{1, 0, LIGHT}, {1, 3, LIGHT}, {1, 4, ACCENT}, {1, 7, ACCENT},
	{2, 0, LIGHT}, {2, 3, ACCENT}, {2, 4, LIGHT}, {2, 7, ACCENT},
	{3, 1, LIGHT}, {3, 2, LIGHT}, {3, 5, ACCENT}, {3, 6, ACCENT},
};

int draw_mark_width(int tile, int gap) { return 8 * (tile + gap) - gap; }
int draw_mark_height(int tile, int gap) { return 4 * (tile + gap) - gap; }

static uint32_t mix(uint32_t a, uint32_t b, int num, int den)
{
	uint32_t out = 0;

	for (int shift = 0; shift < 24; shift += 8) {
		int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
		out |= (uint32_t)(cb + (ca - cb) * num / den) << shift;
	}
	return out;
}

/* How many of the 4 x 4 sample points of pixel (px, py) fall inside a
 * rounded square of side `side` (in 1/4 pixels, radius r). */
static int coverage(int px, int py, int side, int r)
{
	int n = 0;

	for (int sy = 0; sy < 4; sy++) {
		for (int sx = 0; sx < 4; sx++) {
			/* sample centres, in 1/8 pixels */
			int x = (px * 4 + sx) * 2 + 1, y = (py * 4 + sy) * 2 + 1;
			int s = side * 2, rr = r * 2;
			int cx = x < rr ? rr : (x > s - rr ? s - rr : x);
			int cy = y < rr ? rr : (y > s - rr ? s - rr : y);
			int dx = x - cx, dy = y - cy;

			if (x < s && y < s && dx * dx + dy * dy <= rr * rr) {
				n++;
			}
		}
	}
	return n;
}

void draw_mark(int x, int y, int tile, int gap, uint32_t bg)
{
	int pitch = tile + gap;
	int r = tile;    /* radius 0.25 tile, in 1/4 pixels */

	for (unsigned t = 0; t < sizeof(tiles) / sizeof(tiles[0]); t++) {
		int tx = x + tiles[t].col * pitch, ty = y + tiles[t].row * pitch;

		for (int j = 0; j < tile; j++) {
			for (int i = 0; i < tile; i++) {
				int n = coverage(i, j, tile * 4, r);
				int X = tx + i, Y = ty + j;

				if (n && X >= 0 && Y >= 0 && X < DRAW_W && Y < DRAW_H) {
					draw_buf[Y * DRAW_W + X] =
						draw_rgb(mix(tiles[t].colour, bg, n, 16));
				}
			}
		}
	}
}

static const los_dotglyph *glyph(char ch)
{
	const los_dotglyph *q = 0;

	for (unsigned i = 0; i < sizeof(los_dotfont) / sizeof(los_dotfont[0]); i++) {
		if (los_dotfont[i].ch == ch) {
			return &los_dotfont[i];
		}
		if (los_dotfont[i].ch == '?') {
			q = &los_dotfont[i];
		}
	}
	return q;
}

int draw_text(int x, int y, const char *s, int scale, uint32_t hex, int x_max)
{
	uint16_t c = draw_rgb(hex);

	for (; *s; s++) {
		const los_dotglyph *g = glyph(*s);
		int adv = (g->width + 1) * scale;

		if (x + g->width * scale > x_max) {
			break;
		}
		for (int row = 0; row < 8; row++) {
			for (int col = 0; col < g->width; col++) {
				if (!(g->rows[row] >> col & 1)) {
					continue;
				}
				for (int j = 0; j < scale; j++) {
					int Y = y + row * scale + j;

					for (int i = 0; i < scale; i++) {
						int X = x + col * scale + i;

						if (X >= 0 && Y >= 0 && X < DRAW_W && Y < DRAW_H) {
							draw_buf[Y * DRAW_W + X] = c;
						}
					}
				}
			}
		}
		x += adv;
	}
	return x;
}

int draw_text_width(const char *s, int scale)
{
	int w = 0;

	for (; *s; s++) {
		w += (glyph(*s)->width + 1) * scale;
	}
	return w ? w - scale : 0;
}
