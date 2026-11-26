/*
 * gzip decompression for the bootloader: the original firmware's kernel is
 * a gzip-compressed image.
 *
 * Written from the specifications: RFC 1952 (the gzip wrapper: header,
 * optional fields, CRC-32 and length at the end) and RFC 1951 (DEFLATE:
 * stored, fixed-Huffman and dynamic-Huffman blocks). The whole output goes
 * to one buffer, which is also the window back-references copy from.
 *
 * Huffman codes are decoded with canonical code counts: for each code
 * length, how many codes there are and which symbols they stand for, in
 * order. Simple, and fast enough for a few megabytes at boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "inflate.h"

#define MAX_BITS 15

struct in {
	const uint8_t *p, *end;
	uint32_t bits, nbits;
	int error;
};

struct out {
	uint8_t *p, *start, *end;
};

struct huffman {
	uint16_t count[MAX_BITS + 1];   /* codes of each length */
	uint16_t symbol[320];           /* symbols in code order */
};

static uint32_t bits(struct in *s, uint32_t n)
{
	while (s->nbits < n) {
		if (s->p >= s->end) {
			s->error = INFLATE_SHORT;
			return 0;
		}
		s->bits |= (uint32_t)*s->p++ << s->nbits;
		s->nbits += 8;
	}
	uint32_t v = s->bits & ((1u << n) - 1);

	s->bits >>= n;
	s->nbits -= n;
	return v;
}

/* Codes are sent most significant bit first, so build them a bit at a
 * time and compare with the first code of each length */
static int decode(struct in *s, const struct huffman *h)
{
	int code = 0, first = 0, index = 0;

	for (int len = 1; len <= MAX_BITS; len++) {
		code |= (int)bits(s, 1);
		if (s->error)
			return -1;
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	s->error = INFLATE_BAD_CODE;
	return -1;
}

/* From code lengths to counts and symbols. 0, or < 0 if the lengths
 * describe more codes than fit (an incomplete set is allowed). */
static int build(struct huffman *h, const uint8_t *length, int n)
{
	uint16_t offs[MAX_BITS + 1];

	for (int len = 0; len <= MAX_BITS; len++)
		h->count[len] = 0;
	for (int i = 0; i < n; i++)
		h->count[length[i]]++;
	int left = 1;
	for (int len = 1; len <= MAX_BITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return -1;
	}
	offs[1] = 0;
	for (int len = 1; len < MAX_BITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (int i = 0; i < n; i++)
		if (length[i])
			h->symbol[offs[length[i]]++] = (uint16_t)i;
	return 0;
}

/* RFC 1951 3.2.5: base lengths and distances, and their extra bits */
static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct in *s, struct out *o, const struct huffman *lit,
		 const struct huffman *dist)
{
	for (;;) {
		int sym = decode(s, lit);
		if (sym < 0)
			return s->error;
		if (sym < 256) {
			if (o->p >= o->end)
				return INFLATE_FULL;
			*o->p++ = (uint8_t)sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29)
				return INFLATE_BAD_CODE;
			uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
			int d = decode(s, dist);
			if (d < 0)
				return s->error;
			if (d >= 30)
				return INFLATE_BAD_CODE;
			uint32_t back = dist_base[d] + bits(s, dist_extra[d]);
			if (s->error)
				return s->error;
			if (back > (uint32_t)(o->p - o->start))
				return INFLATE_BAD_DISTANCE;
			if (len > (uint32_t)(o->end - o->p))
				return INFLATE_FULL;
			const uint8_t *from = o->p - back;
			while (len--)
				*o->p++ = *from++;
		}
	}
}

static int stored(struct in *s, struct out *o)
{
	s->bits = 0;            /* to the next byte */
	s->nbits = 0;
	if (s->end - s->p < 4)
		return INFLATE_SHORT;
	uint32_t len = s->p[0] | (uint32_t)s->p[1] << 8;
	uint32_t nlen = s->p[2] | (uint32_t)s->p[3] << 8;
	s->p += 4;
	if (len != (~nlen & 0xffff))
		return INFLATE_BAD_BLOCK;
	if ((uint32_t)(s->end - s->p) < len)
		return INFLATE_SHORT;
	if ((uint32_t)(o->end - o->p) < len)
		return INFLATE_FULL;
	while (len--)
		*o->p++ = *s->p++;
	return 0;
}

static int fixed(struct in *s, struct out *o)
{
	static struct huffman lit, dist;
	static int built;

	if (!built) {
		uint8_t length[288];
		int i = 0;
		for (; i < 144; i++) length[i] = 8;
		for (; i < 256; i++) length[i] = 9;
		for (; i < 280; i++) length[i] = 7;
		for (; i < 288; i++) length[i] = 8;
		build(&lit, length, 288);
		for (i = 0; i < 30; i++) length[i] = 5;
		build(&dist, length, 30);
		built = 1;
	}
	return codes(s, o, &lit, &dist);
}

static int dynamic(struct in *s, struct out *o)
{
	static const uint8_t order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t length[320];
	struct huffman lencode, lit, dist;

	uint32_t nlen = bits(s, 5) + 257;
	uint32_t ndist = bits(s, 5) + 1;
	uint32_t ncode = bits(s, 4) + 4;
	if (s->error)
		return s->error;
	if (nlen > 286 || ndist > 30)
		return INFLATE_BAD_BLOCK;

	uint32_t i;
	for (i = 0; i < 19; i++)
		length[order[i]] = i < ncode ? (uint8_t)bits(s, 3) : 0;
	if (s->error)
		return s->error;
	if (build(&lencode, length, 19))
		return INFLATE_BAD_BLOCK;

	for (i = 0; i < nlen + ndist;) {
		int sym = decode(s, &lencode);
		if (sym < 0)
			return s->error;
		if (sym < 16) {
			length[i++] = (uint8_t)sym;
			continue;
		}
		uint8_t repeat = 0;
		uint32_t times;
		if (sym == 16) {
			if (i == 0)
				return INFLATE_BAD_BLOCK;
			repeat = length[i - 1];
			times = 3 + bits(s, 2);
		} else if (sym == 17) {
			times = 3 + bits(s, 3);
		} else {
			times = 11 + bits(s, 7);
		}
		if (s->error)
			return s->error;
		if (i + times > nlen + ndist)
			return INFLATE_BAD_BLOCK;
		while (times--)
			length[i++] = repeat;
	}
	if (length[256] == 0)
		return INFLATE_BAD_BLOCK;     /* no end-of-block code */
	if (build(&lit, length, (int)nlen) || build(&dist, length + nlen, (int)ndist))
		return INFLATE_BAD_BLOCK;
	return codes(s, o, &lit, &dist);
}

int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax,
		uint32_t *outlen, uint32_t *used)
{
	struct in s = {in, in + inlen, 0, 0, 0};
	struct out o = {out, out, out + outmax};
	int last, err;

	do {
		last = (int)bits(&s, 1);
		uint32_t type = bits(&s, 2);
		if (s.error)
			return s.error;
		if (type == 0)
			err = stored(&s, &o);
		else if (type == 1)
			err = fixed(&s, &o);
		else if (type == 2)
			err = dynamic(&s, &o);
		else
			err = INFLATE_BAD_BLOCK;
		if (err)
			return err;
	} while (!last);
	*outlen = (uint32_t)(o.p - o.start);
	if (used)
		*used = (uint32_t)(s.p - in);     /* whole bytes; the rest of the last is padding */
	return 0;
}

uint32_t inflate_crc32(const uint8_t *p, uint32_t n)
{
	static uint32_t table[256];
	static int ready;

	if (!ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c >> 1) ^ (0xedb88320u & -(c & 1));
			table[i] = c;
		}
		ready = 1;
	}
	uint32_t c = 0xffffffffu;
	while (n--)
		c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

int gunzip(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax, uint32_t *outlen)
{
	/* RFC 1952 2.3: ID1 ID2 CM FLG MTIME(4) XFL OS, then optional fields */
	if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return INFLATE_NOT_GZIP;
	uint8_t flags = in[3];
	uint32_t at = 10;

	if (flags & 4) {                              /* FEXTRA */
		if (at + 2 > inlen)
			return INFLATE_SHORT;
		at += 2 + (in[at] | (uint32_t)in[at + 1] << 8);
	}
	if (flags & 8)                                /* FNAME */
		while (at < inlen && in[at++])
			;
	if (flags & 16)                               /* FCOMMENT */
		while (at < inlen && in[at++])
			;
	if (flags & 2)                                /* FHCRC */
		at += 2;
	if (at >= inlen)
		return INFLATE_SHORT;

	uint32_t used;
	int err = inflate_raw(in + at, inlen - at, out, outmax, outlen, &used);
	if (err)
		return err;
	at += used;
	if (at + 8 > inlen)
		return INFLATE_SHORT;
	uint32_t crc = in[at] | (uint32_t)in[at + 1] << 8 | (uint32_t)in[at + 2] << 16 |
		       (uint32_t)in[at + 3] << 24;
	uint32_t size = in[at + 4] | (uint32_t)in[at + 5] << 8 | (uint32_t)in[at + 6] << 16 |
			(uint32_t)in[at + 7] << 24;
	if (size != *outlen)
		return INFLATE_BAD_SIZE;
	if (crc != inflate_crc32(out, *outlen))
		return INFLATE_BAD_CRC;
	return 0;
}
