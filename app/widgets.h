/*
 * Lossless OS: drawing pieces shared by the screens.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_WIDGETS_H
#define LOS_WIDGETS_H

#include "lvgl.h"

/* Dot-matrix text: each character on a 5 x 7 grid of square dots, like the
 * app's display face, crisp at any size because the dots are drawn, not
 * scaled. 'dot' is the dot size in pixels, with a 1px gap. */
lv_obj_t *dots_create(lv_obj_t *parent, int dot, lv_color_t color);
void dots_set_text(lv_obj_t *obj, const char *text);
void dots_set_color(lv_obj_t *obj, lv_color_t color);
/* Text wider than 'width' scrolls (after a second's pause), like a label */
void dots_set_max_width(lv_obj_t *obj, int width);

/* A bar of segments, 'lit' of 'count' in the accent colour */
lv_obj_t *segbar_create(lv_obj_t *parent, int w, int h, int count, int gap,
                        lv_color_t on, lv_color_t off);
void segbar_set(lv_obj_t *obj, int value, int max);

/* The black display panel */
lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h);

/* Header: play state, title in spaced capitals (or the dot wordmark when
 * title is NULL), Bluetooth and battery */
lv_obj_t *header_create(lv_obj_t *parent, const char *title);
void header_set_playing(lv_obj_t *header, int state);   /* 0 stop, 1 play, 2 pause */
/* The time at the left of every header ("" for none) */
void header_set_time(const char *text);
/* This header shows the play state at the left instead of the time (the lock screen) */
void header_show_play_state(lv_obj_t *header);
/* This header shows the mark at the left instead of the time (the lock screen) */
void header_show_mark_left(lv_obj_t *header);

/* The lossless mark: tiles of 'tile' px, 1 px apart (8 x 4 of them) */
lv_obj_t *mark_create(lv_obj_t *parent, int tile);

/* A small play / pause icon (nothing while stopped) */
lv_obj_t *state_icon_create(lv_obj_t *parent, int w, int h);
void state_icon_set(lv_obj_t *icon, int state);   /* 0 stop, 1 play, 2 pause */
/* The volume bar on every header; 'flash' shows the dB in place of the title */
/* The padlock on every header while the keys are locked */
void header_set_locked(bool locked);
/* The battery icon: percent (-1 unknown), orange while charging */
void header_set_battery(int percent, bool charging);
/* The Bluetooth rune: 0 off (faint), 1 on, 2 headphones connected (orange) */
void header_set_bluetooth(int level);
void header_set_volume(bool known, int db, int min_db, int max_db, bool flash);

/* Square cover image fitted to size, with rounded corners */
lv_obj_t *cover_create(lv_obj_t *parent, const char *path, uint32_t offset, int size,
                       int radius);

#endif
