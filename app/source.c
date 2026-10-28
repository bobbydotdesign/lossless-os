/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "source.h"
#include "opusfile.h"
#include "oggvorbis.h"
#include "mp3file.h"
#include "los_sys.h"

/* One file plays at a time: the FLAC decoder's frame buffer is fixed.
 * 2 channels x 16384 samples covers every common FLAC (4096 or 4608). */
#define FLAC_STORE_SAMPLES (2 * 16384)
uint64_t decoder_mem[DECODER_MEM_BYTES / 8];
#define flac_store ((int32_t *)decoder_mem)
_Static_assert(FLAC_STORE_SAMPLES * 4 <= DECODER_MEM_BYTES, "FLAC fits");

/* ALAC: one packet, read and decoded (one file at a time, as for FLAC) */
#define ALAC_PACKET_MAX (ALAC_MAX_FRAMES * 2 * 4 + 64)
#define alac_out ((int32_t *)decoder_mem)
#define alac_packet ((uint8_t *)decoder_mem + ALAC_MAX_FRAMES * 2 * 4)
_Static_assert(ALAC_MAX_FRAMES * 2 * 4 + ALAC_PACKET_MAX + 8 <= DECODER_MEM_BYTES, "ALAC fits");
static mp4_cursor alac_cursor;

static void *flac_alloc(size_t n)
{
    return n <= FLAC_STORE_SAMPLES * sizeof(int32_t) ? flac_store : NULL;
}

static void flac_free(void *p)
{
    (void)p;
}

static size_t file_read(void *ctx, uint8_t *buf, size_t n)
{
    los_source *s = ctx;
    if (!buf) {                                      /* skip n bytes */
        s->offset += n;
        return los_file_seek(s->file, s->offset) ? n : 0;
    }
    size_t got = los_file_read(s->file, buf, n);
    s->offset += got;
    return got;
}

static bool read_exact(los_source *s, void *buf, size_t n)
{
    return file_read(s, buf, n) == n;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint16_t be16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }

/* WAV: RIFF chunks; "fmt " says PCM (1, or extensible with a PCM GUID) */
static int open_wav(los_source *s)
{
    uint8_t h[40];
    bool have_fmt = false;
    for (;;) {
        if (!read_exact(s, h, 8))
            return -1;
        uint32_t len = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            if (len < 16 || len > sizeof(h) || !read_exact(s, h, len))
                return -1;
            uint16_t tag = le16(h);
            if (tag == 0xfffe && len >= 26)
                tag = le16(h + 24);                  /* the sub-format GUID's first part */
            if (tag != 1)
                return -2;                           /* float, compressed: not yet */
            s->channels = le16(h + 2);
            s->rate = le32(h + 4);
            s->bits = le16(h + 14);
            have_fmt = true;
            if (len & 1)
                file_read(s, NULL, 1);
        } else if (!memcmp(h, "data", 4)) {
            if (!have_fmt)
                return -1;
            s->pcm_left = len;
            s->pcm_big_endian = false;
            break;
        } else {
            file_read(s, NULL, len + (len & 1));
        }
    }
    s->format = "WAV";
    return 0;
}

/* AIFF / AIFF-C: "COMM" (channels, frames, bits, rate as an 80-bit float),
 * "SSND"; AIFF-C only uncompressed ("NONE", or "sowt" = little-endian) */
static int open_aiff(los_source *s, bool aifc)
{
    uint8_t h[26];
    bool have_comm = false;
    for (;;) {
        if (!read_exact(s, h, 8))
            return -1;
        uint32_t len = be32(h + 4);
        if (!memcmp(h, "COMM", 4)) {
            uint32_t want = aifc ? 22 : 18;
            if (len < want || !read_exact(s, h, want))
                return -1;
            s->channels = be16(h);
            s->frames = be32(h + 2);
            s->bits = be16(h + 6);
            int exp = ((h[8] & 0x7f) << 8 | h[9]) - 16383 - 63;
            uint64_t mant = (uint64_t)be32(h + 10) << 32 | be32(h + 14);
            s->rate = exp < 0 ? (unsigned)(mant >> -exp) : (unsigned)(mant << exp);
            s->pcm_big_endian = true;
            if (aifc) {
                /* uncompressed: big-endian "NONE" "twos" "in24" "in32", or
                 * little-endian "sowt" "42ni" "23ni" */
                const uint8_t *c = h + 18;
                if (!memcmp(c, "sowt", 4) || !memcmp(c, "42ni", 4) || !memcmp(c, "23ni", 4))
                    s->pcm_big_endian = false;
                else if (memcmp(c, "NONE", 4) && memcmp(c, "twos", 4) &&
                         memcmp(c, "in24", 4) && memcmp(c, "in32", 4))
                    return -2;
            }
            file_read(s, NULL, len - want + (len & 1));
            have_comm = true;
        } else if (!memcmp(h, "SSND", 4)) {
            if (!have_comm || !read_exact(s, h, 8))
                return -1;
            uint32_t skip = be32(h);
            file_read(s, NULL, skip);
            s->pcm_left = len - 8 - skip;
            break;
        } else {
            file_read(s, NULL, len + (len & 1));
        }
    }
    s->format = "AIFF";
    return 0;
}

int source_open(los_source *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->file = los_file_open(path);
    if (!s->file)
        return -1;

    uint8_t h[12];
    int err = -2;
    if (!read_exact(s, h, 4)) {
        err = -1;
    } else if (memcmp(h, "fLaC", 4) && !read_exact(s, h + 4, 8)) {
        err = -1;
    } else if (!memcmp(h, "fLaC", 4)) {
        s->offset = 0;
        los_file_seek(s->file, 0);
        if (flac_open(&s->flac, file_read, s, flac_alloc) == 0 && s->flac.channels <= 2) {
            s->kind = SRC_FLAC;
            s->rate = s->flac.rate;
            s->bits = s->flac.bits;
            s->channels = s->flac.channels;
            s->frames = s->flac.total_samples;
            s->format = "FLAC";
            s->audio_start = s->offset - flac_buffered(&s->flac);
            return 0;
        }
    } else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        err = open_wav(s);
        s->kind = SRC_PCM;
    } else if (!memcmp(h, "OggS", 4)) {
        err = opusfile_open(s);
        if (!err) {
            s->kind = SRC_OPUS;
            return 0;
        }
        if (err == -2) {
            err = oggvorbis_open(s);
            if (!err) {
                s->kind = SRC_VORBIS;
                return 0;
            }
            oggvorbis_close(s);
        }
    } else if (!memcmp(h, "ID3", 3) || (h[0] == 0xff && (h[1] & 0xe0) == 0xe0)) {
        err = mp3file_open(s);
        if (!err) {
            s->kind = SRC_MP3;
            return 0;
        }
    } else if (!memcmp(h + 4, "ftyp", 4)) {
        err = mp4_open(s->file, &s->mp4);
        if (!err)
            err = alac_config_read(&s->alac, s->mp4.cookie, sizeof(s->mp4.cookie));
        if (!err) {
            s->kind = SRC_ALAC;
            s->rate = s->alac.rate;
            s->bits = s->alac.bits;
            s->channels = s->alac.channels;
            s->frames = s->mp4.frames;
            s->format = "ALAC";
            s->packet = 0;
            s->alac_frames = s->alac_used = 0;
            mp4_cursor_reset(&alac_cursor);
            return 0;
        }
    } else if (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) {
        err = open_aiff(s, !memcmp(h + 8, "AIFC", 4));
        s->kind = SRC_PCM;
    }
    if (s->kind == SRC_PCM && !err) {
        s->audio_start = s->offset;
        s->data_bytes = s->pcm_left;
        s->pcm_bytes = (s->bits + 7) / 8;
        if (s->bits < 8 || s->bits > 32 || s->channels < 1 || s->channels > 2 || !s->rate)
            err = -2;
        else if (!s->frames)
            s->frames = s->pcm_left / (s->pcm_bytes * s->channels);
        if (!err)
            return 0;
    }
    source_close(s);
    return err;
}

static int read_pcm(los_source *s, int32_t *out, int frames)
{
    uint8_t buf[1536];
    unsigned frame_bytes = s->pcm_bytes * s->channels;
    int done = 0;
    while (done < frames && s->pcm_left >= frame_bytes) {
        size_t want = (size_t)(frames - done) * frame_bytes;
        if (want > sizeof(buf) / frame_bytes * frame_bytes)
            want = sizeof(buf) / frame_bytes * frame_bytes;
        if (want > s->pcm_left)
            want = s->pcm_left / frame_bytes * frame_bytes;
        size_t got = file_read(s, buf, want);
        got -= got % frame_bytes;
        if (got == 0)
            break;
        s->pcm_left -= got;
        const uint8_t *p = buf;
        for (size_t i = 0; i < got / s->pcm_bytes; i++, p += s->pcm_bytes) {
            uint32_t v = 0;
            for (unsigned b = 0; b < s->pcm_bytes; b++)
                v |= (uint32_t)p[s->pcm_big_endian ? b : s->pcm_bytes - 1 - b]
                     << (8 * (s->pcm_bytes - 1 - b));
            /* 8-bit WAV is unsigned; everything else signed */
            if (s->pcm_bytes == 1 && !s->pcm_big_endian)
                v ^= 0x80;
            unsigned width = s->pcm_bytes * 8;
            int32_t x = (int32_t)(v << (32 - width)) >> (32 - width);
            out[(size_t)done * s->channels + i] = x >> (width - s->bits); /* bits < width: padded low */
        }
        done += (int)(got / frame_bytes);
    }
    return done;
}

static int read_flac(los_source *s, int32_t *out, int frames)
{
    int done = 0;
    while (done < frames) {
        if (s->flac_used >= s->flac.block) {
            int n = flac_next(&s->flac);
            if (n == 0)
                break;
            if (n < 0)
                continue;                            /* a damaged frame: skip to the next */
            s->flac_used = 0;
        }
        unsigned take = s->flac.block - s->flac_used;
        if (take > (unsigned)(frames - done))
            take = (unsigned)(frames - done);
        for (unsigned i = 0; i < take; i++)
            for (unsigned c = 0; c < s->channels; c++)
                out[(size_t)(done + (int)i) * s->channels + c] = s->flac.samples[c][s->flac_used + i];
        s->flac_used += take;
        done += (int)take;
    }
    return done;
}

/* The next packet decoded into alac_out; false at the end. A damaged
 * packet becomes silence, so the time stays right. */
static bool alac_next(los_source *s)
{
    uint64_t at;
    uint32_t size;
    if (!mp4_packet(s->file, &s->mp4, &alac_cursor, s->packet, &at, &size))
        return false;
    s->packet++;
    int n = -1;
    if (size <= ALAC_PACKET_MAX && los_file_seek(s->file, at) &&
        los_file_read(s->file, alac_packet, size) == size) {
        memset(alac_packet + size, 0, 8);
        n = alac_decode(&s->alac, alac_packet, size, alac_out);
    }
    if (n < 0) {
        n = (int)s->alac.frame_length;
        memset(alac_out, 0, (size_t)n * s->channels * sizeof(int32_t));
    }
    s->alac_frames = (unsigned)n;
    s->alac_used = 0;
    return true;
}

static int read_alac(los_source *s, int32_t *out, int frames)
{
    int done = 0;
    while (done < frames) {
        if (s->alac_used >= s->alac_frames && !alac_next(s))
            break;
        unsigned take = s->alac_frames - s->alac_used;
        if (take > (unsigned)(frames - done))
            take = (unsigned)(frames - done);
        memcpy(out + (size_t)done * s->channels, alac_out + (size_t)s->alac_used * s->channels,
               (size_t)take * s->channels * sizeof(int32_t));
        s->alac_used += take;
        done += (int)take;
    }
    return done;
}

/* ---- seeking ---- */

/* From byte 'at', the first real frame: its first sample, or UINT64_MAX if
 * none before the end. Real means the frame after it carries on from it
 * (a frame header can turn up by chance inside audio data). */
static uint64_t flac_frame_from(los_source *s, uint64_t at)
{
    for (int tries = 0; tries < 64; tries++) {
        s->offset = at;
        if (!los_file_seek(s->file, at))
            return UINT64_MAX;
        flac_reset(&s->flac);
        int n;
        do {
            n = flac_next(&s->flac);
        } while (n < 0 && tries++ < 64);
        if (n <= 0)
            return UINT64_MAX;
        uint64_t first = s->flac.position, after = first + s->flac.block;
        /* where the next search starts if this one was a fluke */
        uint64_t next_at = s->offset - flac_buffered(&s->flac);
        do {
            n = flac_next(&s->flac);
        } while (n < 0 && tries++ < 64);
        if (n == 0 || s->flac.position == after)
            return first;                         /* the last frame, or confirmed */
        at = next_at;
    }
    return UINT64_MAX;
}

static bool seek_flac(los_source *s, uint64_t target)
{
    uint64_t size = los_file_size(s->file);
    if (!size || size <= s->audio_start)
        return false;
    /* lo: frames found from here start at or before the target */
    uint64_t lo = s->audio_start, hi = size;
    while (hi - lo > 32 * 1024) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t p = flac_frame_from(s, mid);
        if (p <= target)
            lo = mid;
        else
            hi = mid;
    }
    /* from lo, frame by frame to the one holding the target */
    if (flac_frame_from(s, lo) == UINT64_MAX)
        return false;
    s->offset = lo;
    los_file_seek(s->file, lo);
    flac_reset(&s->flac);
    for (;;) {
        int n = flac_next(&s->flac);
        if (n == 0)
            return false;
        if (n < 0)
            continue;
        if (s->flac.position + s->flac.block > target) {
            s->flac_used = target > s->flac.position ? (unsigned)(target - s->flac.position) : 0;
            return true;
        }
    }
}

bool source_seek(los_source *s, uint64_t frame)
{
    if (s->frames && frame >= s->frames)
        frame = s->frames - 1;
    if (s->kind == SRC_PCM) {
        uint64_t frame_bytes = (uint64_t)s->pcm_bytes * s->channels, at = frame * frame_bytes;
        if (at > s->data_bytes)
            at = s->data_bytes;
        s->offset = s->audio_start + at;
        s->pcm_left = s->data_bytes - at;
        return los_file_seek(s->file, s->offset);
    }
    if (s->kind == SRC_FLAC)
        return seek_flac(s, frame);
    if (s->kind == SRC_OPUS)
        return opusfile_seek(s, frame);
    if (s->kind == SRC_VORBIS)
        return oggvorbis_seek(s, frame);
    if (s->kind == SRC_MP3)
        return mp3file_seek(s, frame);
    if (s->kind == SRC_ALAC) {
        /* every packet but the last is frame_length long */
        s->packet = (uint32_t)(frame / s->alac.frame_length);
        if (!alac_next(s))
            return false;
        s->alac_used = (unsigned)(frame - (uint64_t)(s->packet - 1) * s->alac.frame_length);
        return true;
    }
    return false;
}

int source_read(los_source *s, int32_t *out, int frames)
{
    switch (s->kind) {
    case SRC_FLAC: return read_flac(s, out, frames);
    case SRC_PCM:  return read_pcm(s, out, frames);
    case SRC_ALAC: return read_alac(s, out, frames);
    case SRC_OPUS: return opusfile_read(s, out, frames);
    case SRC_VORBIS: return oggvorbis_read(s, out, frames);
    case SRC_MP3: return mp3file_read(s, out, frames);
    default:       return 0;
    }
}

void source_close(los_source *s)
{
    if (s->kind == SRC_FLAC)
        flac_close(&s->flac, flac_free);
    if (s->kind == SRC_OPUS)
        opusfile_close(s);
    if (s->kind == SRC_VORBIS)
        oggvorbis_close(s);
    if (s->kind == SRC_MP3)
        mp3file_close(s);
    if (s->file)
        los_file_close(s->file);
    s->file = NULL;
    s->kind = SRC_NONE;
}
