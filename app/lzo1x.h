/*
 * LZO1X decompression (lzo1x.c), for the player's original firmware: its
 * system partition compresses files with it (stockfw.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_LZO1X_H
#define LOS_LZO1X_H

#include <stddef.h>
#include <stdint.h>

/* in (n bytes) into out (at most cap bytes); *out_len: what it wrote.
 * 0, or < 0 for data that runs past its input or its output, or refers
 * back before the start. Every read and write is checked. */
int lzo1x_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);

#endif
