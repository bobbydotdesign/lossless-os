/*
 * Lossless OS: album covers at the size they're shown (see covers.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_COVERS_H
#define LOS_COVERS_H

#include "lvgl.h"

/* The cover at 'path' (a JPEG starting 'offset' bytes in, path without the
 * LVGL drive letter) as a size x size image, centre-cropped to a square.
 * NULL if it can't be read. Kept and shared: don't free it. */
const lv_draw_buf_t *cover_thumb(const char *path, uint32_t offset, int size);

/* Lets go of the cover at that size (a big one shown once, such as a
 * slideshow's); nothing may still be showing it */
void cover_forget(const char *path, uint32_t offset, int size);

#endif
