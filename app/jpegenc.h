/*
 * Lossless OS: a small baseline JPEG encoder (jpegenc.c), for album covers
 * a car asks for over Bluetooth (200 x 200, AVRCP cover art).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_JPEGENC_H
#define LOS_JPEGENC_H

#include <stddef.h>
#include <stdint.h>

/* RGB565 pixels (stride in pixels; width and height multiples of 8) as a
 * baseline JFIF JPEG, 4:4:4, quality 1..100. Returns its size, or 0 if
 * 'cap' bytes aren't enough or the size isn't whole 8 x 8 blocks. */
size_t jpeg_encode_rgb565(const uint16_t *px, int width, int height, int stride,
                          int quality, uint8_t *out, size_t cap);

#endif
