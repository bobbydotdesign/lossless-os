/*
 * Covers: albums as a row of covers, the chosen one facing you and the
 * rest turned towards it on either side, each with a soft reflection.
 * Drawn by hand into one image: the graphics library can't tilt pictures,
 * so each turned cover is drawn as thin strips that get shorter towards
 * the middle.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_COVERFLOW_H
#define LOS_COVERFLOW_H

#include "lvgl.h"

#define COVERFLOW_W     320
#define COVERFLOW_H     164
#define COVERFLOW_COVER 120   /* the facing cover's side, the covers' master size */

/* The cover of album 'i' at COVERFLOW_COVER, or NULL for none */
typedef const lv_draw_buf_t *(*coverflow_cover_fn)(int i);

/* The image Covers draws into (one at a time) */
lv_obj_t *coverflow_create(lv_obj_t *parent);

/* Draws 'count' albums with album 'pos' facing (between two while
 * moving), on 'bg'; an album without a cover is 'blank' with a 'dot' */
void coverflow_draw(lv_obj_t *img, float pos, int count, coverflow_cover_fn cover, lv_color_t bg,
                    lv_color_t blank, lv_color_t dot);

/* The same, flat: the facing cover full size and the rest smaller and
 * darker beside it, face on, with no reflection (Gallery); 'round' cuts
 * each into a circle (artists) */
/* Leaves cover i out of the next draws (-1: none): another copy of it is
 * on its way there */
void coverflow_skip(int i);

void coverflow_draw_flat(lv_obj_t *img, float pos, int count, coverflow_cover_fn cover, lv_color_t bg,
                         lv_color_t blank, lv_color_t dot, bool round);

#endif
