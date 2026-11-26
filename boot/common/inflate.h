/*
 * gzip and DEFLATE decompression (inflate.c), RFC 1952 and RFC 1951.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_INFLATE_H
#define BOOT_INFLATE_H

#include <stdint.h>

enum {
	INFLATE_SHORT = -1,         /* input ended early */
	INFLATE_FULL = -2,          /* output doesn't fit */
	INFLATE_BAD_BLOCK = -3,
	INFLATE_BAD_CODE = -4,
	INFLATE_BAD_DISTANCE = -5,  /* refers back before the start */
	INFLATE_NOT_GZIP = -6,
	INFLATE_BAD_SIZE = -7,      /* gzip trailer: wrong length */
	INFLATE_BAD_CRC = -8,       /* gzip trailer: wrong CRC-32 */
};

/* A gzip stream (header, DEFLATE data, CRC-32 and length checked) into
 * out. 0, or one of the errors above. */
int gunzip(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax, uint32_t *outlen);

/* Raw DEFLATE data; *used: input bytes consumed. 0 or an error. */
int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax,
		uint32_t *outlen, uint32_t *used);

/* The usual CRC-32 (as zlib.crc32) */
uint32_t inflate_crc32(const uint8_t *p, uint32_t n);

#endif
