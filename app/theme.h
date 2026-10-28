/*
 * Lossless OS: the look. Colours come from the app's design tokens
 * (tokens.h); type is Geist Mono throughout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_THEME_H
#define LOS_THEME_H

#include "lvgl.h"
#include "tokens.h"

#define LOS_W        320
#define LOS_H        240
#define LOS_HEADER_H 36   /* 32 for the title and icons, then the volume bar */
#define LOS_VOLUME_H 4    /* the volume bar along the header's bottom edge */

typedef struct {
    const los_palette *p;
    bool dark;
    /* derived colours */
    lv_color_t bg, fg, muted, faint, line, raised, accent, accent_fg, ink;
    lv_color_t second;            /* between fg and muted: artist lines */
    lv_color_t select;            /* the selected row: a gray between bg and fg */
    lv_color_t dim;               /* remaining time */
    lv_color_t seg_off;           /* unlit bar segments */
    lv_color_t lcd, lcd_text, lcd_muted, lcd_off, lcd_edge;
    /* type */
    /* all Geist Mono */
    const lv_font_t *list;        /* SemiBold 20 (as title) */
    const lv_font_t *row;         /* Medium 19: list rows, album titles in lists */
    const lv_font_t *row_sub;     /* Medium 15: second lines in lists */
    const lv_font_t *track;       /* Medium 16: tracks on an album page */
    const lv_font_t *title;       /* SemiBold 20: song titles */
    const lv_font_t *title_small; /* SemiBold 17: song titles where room is short */
    const lv_font_t *body;        /* Medium 16: artists */
    const lv_font_t *small;       /* Medium 14: albums, what's next */
    const lv_font_t *label;       /* Medium 12: spaced capitals */
    const lv_font_t *mono;        /* Medium 12: small print */
} los_theme;

extern los_theme theme;

void theme_init(bool dark);

lv_color_t theme_hex(uint32_t rgb);

/* A label in small spaced capitals, like the app's tags */
lv_obj_t *theme_tag(lv_obj_t *parent, const char *text, lv_color_t color);

/* Plain object with no padding, border or scrollbars */
lv_obj_t *theme_box(lv_obj_t *parent);

#endif
