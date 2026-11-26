/*
 * MP3 files (mp3file.h). Frames are found here and handed to Android's
 * fixed-point decoder (lib/pvmp3, Apache-2.0) one at a time.
 *
 * A frame: a 4-byte header (sync, MPEG version, layer, bit rate, sample
 * rate, padding, channels), side information, then its data, which may
 * start in earlier frames (the "bit reservoir"). The first frame is often
 * not music but a Xing / Info (LAME) or VBRI header: the number of frames,
 * a table for seeking, and from LAME the encoder's delay and padding, so
 * the music can be cut exactly (gapless). The decoder adds 529 samples of
 * its own delay.
 *
 * Tags: ID3v2 (2.2 to 2.4) at the start, else ID3v1 at the end.
 *
 * Sources: ISO/IEC 11172-3 and 13818-3 (frame header), the Xing and LAME
 * header descriptions (Gabriel Bouvigne, LAME project), id3.org (ID3v2.3,
 * 2.4 and ID3v1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "mp3file.h"
#include "pvmp3decoder_api.h"
#include "los_sys.h"

#define DECODER_DELAY 529
#define FRAME_MAX 2900                  /* 320 kb/s at 8 kHz, padded */

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

static bool at(void *f, uint64_t pos, void *buf, size_t n)
{
    return los_file_seek(f, pos) && los_file_read(f, buf, n) == n;
}

/* ---- frame headers ---- */

typedef struct {
    unsigned version;                   /* 1 MPEG-1, 2 MPEG-2, 3 MPEG-2.5 */
    unsigned rate, channels, bitrate;   /* bitrate in bit/s */
    unsigned len, spf;                  /* frame bytes, samples per frame */
    bool crc;
} frame_hdr;

static const uint16_t kbps[2][15] = {
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },    /* MPEG-1 */
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },        /* MPEG-2, 2.5 */
};
static const uint16_t rates[3][3] = {
    { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 },
};

/* A Layer III header; false if it isn't one we play (free format isn't) */
static bool parse_hdr(const uint8_t *b, frame_hdr *h)
{
    if (b[0] != 0xff || (b[1] & 0xe0) != 0xe0)
        return false;
    unsigned v = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3;
    unsigned br = b[2] >> 4, sr = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
    if (v == 1 || layer != 1 || br == 0 || br == 15 || sr == 3)
        return false;
    h->version = v == 3 ? 1 : v == 2 ? 2 : 3;
    h->rate = rates[h->version - 1][sr];
    h->bitrate = kbps[h->version == 1 ? 0 : 1][br] * 1000u;
    h->channels = (b[3] >> 6) == 3 ? 1 : 2;
    h->spf = h->version == 1 ? 1152 : 576;
    h->len = (h->version == 1 ? 144 : 72) * h->bitrate / h->rate + pad;
    h->crc = !(b[1] & 1);
    return true;
}

static bool same_stream(const frame_hdr *a, const frame_hdr *b)
{
    return a->version == b->version && a->rate == b->rate;
}

/* The first frame at or after 'pos' (before 'end') that the next one
 * follows: where it is, and its header */
static bool find_frame(void *f, uint64_t pos, uint64_t end, uint64_t *found, frame_hdr *h)
{
    uint8_t buf[4096 + 4];
    for (uint64_t scanned = 0; scanned < 256 * 1024 && pos + 4 <= end; ) {
        size_t want = end - pos < sizeof(buf) ? (size_t)(end - pos) : sizeof(buf);
        if (!los_file_seek(f, pos))
            return false;
        size_t got = los_file_read(f, buf, want);
        if (got < 4)
            return false;
        for (size_t i = 0; i + 4 <= got; i++) {
            frame_hdr a, b;
            uint8_t next[4];
            if (buf[i] != 0xff || !parse_hdr(buf + i, &a))
                continue;
            uint64_t p = pos + i;
            if (p + a.len + 4 <= end &&
                (!at(f, p + a.len, next, 4) || !parse_hdr(next, &b) || !same_stream(&a, &b)))
                continue;
            *found = p;
            *h = a;
            return true;
        }
        pos += got - 3;
        scanned += got;
    }
    return false;
}

/* ---- the stream's layout ---- */

typedef struct {
    uint64_t first;                     /* the first frame (perhaps a Xing header) */
    uint64_t audio;                     /* the first frame of music */
    uint64_t end;                       /* before an ID3v1 tag */
    frame_hdr h;                        /* the first frame's */
    uint32_t frames, bytes;             /* from Xing / VBRI, 0 if unknown */
    uint8_t toc[100];
    bool have_toc;
    bool cbr;                           /* an "Info" header: one bit rate */
    int delay, padding;                 /* from LAME, -1 if unknown */
} layout;

static uint64_t id3v2_size(void *f, uint64_t pos)
{
    uint8_t h[10];
    if (!at(f, pos, h, 10) || memcmp(h, "ID3", 3))
        return 0;
    uint32_t n = (uint32_t)(h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f);
    return 10 + n + ((h[5] & 0x10) ? 10 : 0);   /* a footer too */
}

static bool read_layout(void *f, layout *L)
{
    memset(L, 0, sizeof(*L));
    L->delay = L->padding = -1;
    uint64_t size = los_file_size(f), pos = 0, n;
    uint8_t b[200];
    L->end = size;
    if (size > 128 && at(f, size - 128, b, 3) && !memcmp(b, "TAG", 3))
        L->end = size - 128;
    while ((n = id3v2_size(f, pos)) != 0)
        pos += n;
    if (!find_frame(f, pos, L->end, &L->first, &L->h))
        return false;
    L->audio = L->first;
    /* Xing / Info after the side information; VBRI at 36 */
    unsigned side = L->h.version == 1 ? (L->h.channels == 1 ? 17 : 32) : (L->h.channels == 1 ? 9 : 17);
    unsigned x = 4 + (L->h.crc ? 2 : 0) + side;
    size_t got = L->h.len < sizeof(b) ? L->h.len : sizeof(b);
    if (!at(f, L->first, b, got))
        return true;
    if (x + 8 <= got && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4))) {
        uint32_t flags = be32(b + x + 4);
        L->cbr = !memcmp(b + x, "Info", 4);
        unsigned p = x + 8;
        if ((flags & 1) && p + 4 <= got) {
            L->frames = be32(b + p);
            p += 4;
        }
        if ((flags & 2) && p + 4 <= got) {
            L->bytes = be32(b + p);
            p += 4;
        }
        if ((flags & 4) && p + 100 <= got) {
            memcpy(L->toc, b + p, 100);
            L->have_toc = true;
            p += 100;
        }
        if (flags & 8)
            p += 4;
        /* LAME: its tag, then delay and padding (12 bits each) 21 bytes on */
        uint8_t lame[24];
        /* written by LAME, and by ffmpeg (as Lavf or Lavc) */
        if (at(f, L->first + p, lame, 24) && (!memcmp(lame, "LAME", 4) ||
            !memcmp(lame, "Lavf", 4) || !memcmp(lame, "Lavc", 4))) {
            L->delay = lame[21] << 4 | lame[22] >> 4;
            L->padding = (lame[22] & 15) << 8 | lame[23];
        }
        L->audio = L->first + L->h.len;
    } else if (36 + 18 <= got && !memcmp(b + 36, "VBRI", 4)) {
        L->bytes = be32(b + 36 + 10);
        L->frames = be32(b + 36 + 14);
        L->audio = L->first + L->h.len;
    }
    return true;
}

/* Samples the music lasts, 0 if unknown */
static uint64_t music_frames(const layout *L)
{
    if (L->frames) {
        uint64_t all = (uint64_t)L->frames * L->h.spf;
        if (L->delay >= 0)
            all = all > (uint64_t)(L->delay + L->padding) ? all - (uint64_t)(L->delay + L->padding) : 0;
        return all;
    }
    /* one bit rate, it's to be hoped */
    return L->h.bitrate ? (L->end - L->audio) * 8 * L->h.rate / L->h.bitrate : 0;
}

/* ---- playing ---- */

struct mp3_state {
    uint64_t pvmem[(30 * 1024) / 8];
    tPVMP3DecoderExternal ext;
    uint8_t frame[FRAME_MAX + 8];
    int16_t pcm[1152 * 2];
    layout L;
    uint64_t pos;                       /* the next frame */
    int pcm_n, pcm_used;
    int64_t at;                         /* decoder output index of pcm[0] */
    int64_t skip_until, end;            /* decoder output indexes; end -1 unknown */
    int64_t start_skip;                 /* decoder output before the music */
};
_Static_assert(sizeof(struct mp3_state) <= DECODER_MEM_BYTES, "MP3 fits");
#define mp (*(struct mp3_state *)decoder_mem)

static void decoder_start(void)
{
    memset(&mp.ext, 0, sizeof(mp.ext));
    pvmp3_InitDecoder(&mp.ext, mp.pvmem);
    mp.ext.crcEnabled = 0;
    mp.ext.equalizerType = flat;
}

int mp3file_open(los_source *s)
{
    if (pvmp3_decoderMemRequirements() > sizeof(mp.pvmem) || !read_layout(s->file, &mp.L))
        return -2;
    decoder_start();
    mp.pos = mp.L.audio;
    mp.at = 0;
    mp.pcm_n = mp.pcm_used = 0;
    uint64_t frames = music_frames(&mp.L);
    if (mp.L.delay >= 0) {
        mp.start_skip = mp.L.delay + DECODER_DELAY;
        mp.end = mp.start_skip + (int64_t)frames;
    } else {
        mp.start_skip = 0;
        mp.end = mp.L.frames ? (int64_t)frames : -1;
    }
    mp.skip_until = mp.start_skip;
    s->rate = mp.L.h.rate;
    s->bits = 16;
    s->channels = mp.L.h.channels;
    s->frames = frames;
    s->format = "MP3";
    return 0;
}

/* The next frame decoded into pcm, trimmed; false at the end */
static bool next_frame(los_source *s)
{
    for (;;) {
        mp.at += mp.pcm_n;
        mp.pcm_n = mp.pcm_used = 0;
        if (mp.end >= 0 && mp.at >= mp.end)
            return false;
        frame_hdr h;
        uint8_t b[4];
        if (mp.pos + 4 > mp.L.end || !at(s->file, mp.pos, b, 4))
            return false;
        if (!parse_hdr(b, &h) || !same_stream(&h, &mp.L.h) || h.len > FRAME_MAX) {
            /* lost: find the next frame */
            uint64_t p;
            if (!find_frame(s->file, mp.pos + 1, mp.L.end, &p, &h) || h.len > FRAME_MAX)
                return false;
            mp.pos = p;
        }
        if (mp.pos + h.len > mp.L.end || !at(s->file, mp.pos, mp.frame, h.len))
            return false;
        mp.pos += h.len;
        memset(mp.frame + h.len, 0, 8);
        mp.ext.pInputBuffer = mp.frame;
        mp.ext.inputBufferCurrentLength = (int32)h.len;
        mp.ext.inputBufferMaxLength = (int32)h.len;
        mp.ext.inputBufferUsedLength = 0;
        mp.ext.pOutputBuffer = mp.pcm;
        mp.ext.outputFrameSize = 1152 * 2;
        ERROR_CODE err = pvmp3_framedecoder(&mp.ext, mp.pvmem);
        int n = (int)h.spf;
        if (err != NO_DECODING_ERROR || mp.ext.num_channels != (int)s->channels ||
            mp.ext.outputFrameSize != n * (int)s->channels) {
            /* no data yet (after a seek), or damaged: silence keeps time */
            memset(mp.pcm, 0, sizeof(int16_t) * (size_t)n * s->channels);
        }
        mp.pcm_n = n;
        if (mp.end >= 0 && mp.at + n > mp.end)
            mp.pcm_n = (int)(mp.end - mp.at);
        if (mp.at + mp.pcm_n <= mp.skip_until)
            continue;
        mp.pcm_used = mp.at < mp.skip_until ? (int)(mp.skip_until - mp.at) : 0;
        return true;
    }
}

int mp3file_read(los_source *s, int32_t *out, int frames)
{
    int done = 0, ch = (int)s->channels;
    while (done < frames) {
        if (mp.pcm_used >= mp.pcm_n && !next_frame(s))
            break;
        int take = mp.pcm_n - mp.pcm_used;
        if (take > frames - done)
            take = frames - done;
        const int16_t *p = mp.pcm + mp.pcm_used * ch;
        for (int i = 0; i < take * ch; i++)
            out[done * ch + i] = p[i];
        mp.pcm_used += take;
        done += take;
    }
    return done;
}

bool mp3file_seek(los_source *s, uint64_t frame)
{
    const layout *L = &mp.L;
    int64_t d = (int64_t)frame + mp.start_skip;
    /* frames before it to decode first: a frame's data can begin up to
     * 511 bytes back (the bit reservoir), and each frame overlaps the one
     * before */
    unsigned avg = L->h.len ? L->h.len : 1;
    int64_t prime = 2 + (511 + avg - 1) / avg;
    int64_t k = d / L->h.spf, k0 = k > prime ? k - prime : 0;
    uint64_t pos;
    if (k0 == 0) {
        pos = L->audio;
    } else if (!L->cbr && L->have_toc && L->frames && L->bytes) {
        /* the table: per cent of the frames -> 1/256ths of the bytes */
        uint64_t x = (uint64_t)k0 * 25600 / L->frames;  /* per cent x 256 */
        unsigned i = x / 256 < 99 ? (unsigned)(x / 256) : 99, frac = (unsigned)(x % 256);
        unsigned a = L->toc[i], b = i < 99 ? L->toc[i + 1] : 256;
        uint64_t t = (uint64_t)a * 256 + (uint64_t)(b > a ? b - a : 0) * frac;   /* x 256 */
        pos = L->first + t * L->bytes / 65536;
    } else {
        /* one bit rate: frame k starts k average frames in (the padding
         * keeps the average); a little before, so rounding can't skip it */
        pos = L->audio + (uint64_t)k0 * L->h.spf * (L->h.bitrate / 8) / L->h.rate;
        pos = pos > L->audio + 2 ? pos - 2 : L->audio;
    }
    frame_hdr h;
    uint64_t p;
    if (!find_frame(s->file, pos, L->end, &p, &h))
        return false;
    decoder_start();
    mp.pos = p;
    mp.at = k0 * (int64_t)L->h.spf;
    mp.pcm_n = mp.pcm_used = 0;
    mp.skip_until = d;
    return true;
}

void mp3file_close(los_source *s)
{
    (void)s;
}

/* ---- tags, for the library ---- */

/* Text as UTF-8 into out: ID3's encodings 0 Latin-1, 1 UTF-16 with a byte
 * order mark, 2 UTF-16 big-endian, 3 UTF-8. Stops at the first nul. */
static void to_utf8(unsigned enc, const uint8_t *p, size_t n, char *out, size_t max)
{
    size_t o = 0;
    bool be = enc == 2;
    if (enc == 1 && n >= 2) {
        be = p[0] == 0xfe && p[1] == 0xff;
        p += 2;
        n -= 2;
    }
    for (size_t i = 0; i < n && o + 4 < max; ) {
        uint32_t c;
        if (enc == 0) {
            c = p[i++];
        } else if (enc == 3) {
            if (!p[i])
                break;
            out[o++] = (char)p[i++];
            continue;
        } else {
            if (i + 1 >= n)
                break;
            c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i + 1] << 8 | p[i]);
            i += 2;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {      /* a surrogate pair */
                uint32_t lo = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i + 1] << 8 | p[i]);
                i += 2;
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
            }
        }
        if (!c)
            break;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xc0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xe0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | c >> 18);
            out[o++] = (char)(0x80 | (c >> 12 & 0x3f));
            out[o++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = '\0';
}

static void emit(const char *key, const char *value, void (*tag)(const char *, void *), void *ctx)
{
    char kv[300];
    if (value[0]) {
        snprintf(kv, sizeof(kv), "%s=%s", key, value);
        tag(kv, ctx);
    }
}

/* ID3v2 at the start: its text frames, and the front cover's JPEG */
static bool read_id3v2(void *f, void (*tag)(const char *, void *), void *ctx, uint32_t *cover)
{
    uint8_t h[10];
    if (!at(f, 0, h, 10) || memcmp(h, "ID3", 3))
        return false;
    unsigned ver = h[3];
    if (ver < 2 || ver > 4 || (h[5] & 0x80))
        return false;                   /* unsynchronised: rare, left alone */
    uint32_t size = (uint32_t)(h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f);
    uint64_t pos = 10, end = 10 + (uint64_t)size;
    if (h[5] & 0x40) {                  /* an extended header */
        uint8_t e[4];
        if (!at(f, pos, e, 4))
            return false;
        pos += ver == 4 ? ((uint32_t)(e[0] & 0x7f) << 21 | (e[1] & 0x7f) << 14 | (e[2] & 0x7f) << 7 | (e[3] & 0x7f))
                        : be32(e) + 4;
    }
    unsigned idlen = ver == 2 ? 3 : 4, hlen = ver == 2 ? 6 : 10;
    bool cover_front = false;
    while (pos + hlen <= end) {
        uint8_t fh[10];
        if (!at(f, pos, fh, hlen) || !fh[0])
            break;                      /* padding */
        uint32_t n = ver == 2 ? (uint32_t)(fh[3] << 16 | fh[4] << 8 | fh[5]) :
                     ver == 4 ? ((uint32_t)(fh[4] & 0x7f) << 21 | (fh[5] & 0x7f) << 14 | (fh[6] & 0x7f) << 7 | (fh[7] & 0x7f)) :
                     be32(fh + 4);
        uint64_t body = pos + hlen;
        if (body + n > end)
            break;
        char id[5] = { 0 };
        memcpy(id, fh, idlen);
        bool compressed = ver > 2 && (fh[9] & (ver == 4 ? 0x0c : 0xc0));
        const char *key = NULL;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
            key = "TITLE";
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1"))
            key = "ARTIST";
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2"))
            key = "ALBUMARTIST";
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL"))
            key = "ALBUM";
        else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK"))
            key = "TRACKNUMBER";
        else if (!strcmp(id, "TYER") || !strcmp(id, "TDRC") || !strcmp(id, "TYE"))
            key = "DATE";
        if (key && tag && !compressed && n >= 2) {
            uint8_t t[256];
            char v[256];
            size_t m = n < sizeof(t) ? n : sizeof(t);
            if (at(f, body, t, m)) {
                to_utf8(t[0], t + 1, m - 1, v, sizeof(v));
                emit(key, v, tag, ctx);
            }
        } else if (cover && !compressed && !cover_front &&
                   (!strcmp(id, "APIC") || !strcmp(id, "PIC")) && n > 16) {
            uint8_t t[160];
            size_t m = n < sizeof(t) ? n : sizeof(t), i = 1;
            if (at(f, body, t, m)) {
                bool jpeg;
                if (ver == 2) {
                    jpeg = !memcmp(t + 1, "JPG", 3);
                    i = 4;
                } else {
                    jpeg = !memcmp(t + 1, "image/jp", 8);
                    while (i < m && t[i])
                        i++;                    /* the type, nul-ended */
                    i++;
                }
                unsigned type = i < m ? t[i] : 0;
                i++;
                /* the description, nul-ended (two nuls in UTF-16) */
                bool wide = t[0] == 1 || t[0] == 2;
                while (i + (wide ? 1 : 0) < m && (t[i] || (wide && t[i + 1])))
                    i += wide ? 2 : 1;
                i += wide ? 2 : 1;
                if (jpeg && i < m && (!*cover || type == 3)) {
                    *cover = (uint32_t)(body + i);
                    cover_front = type == 3;
                }
            }
        }
        pos = body + n;
    }
    return true;
}

/* ID3v1: 128 bytes at the end, Latin-1, space or nul padded */
static void read_id3v1(void *f, void (*tag)(const char *, void *), void *ctx)
{
    uint64_t size = los_file_size(f);
    uint8_t t[128];
    if (size < 128 || !at(f, size - 128, t, 128) || memcmp(t, "TAG", 3))
        return;
    static const struct { const char *key; int at, len; } fields[] = {
        { "TITLE", 3, 30 }, { "ARTIST", 33, 30 }, { "ALBUM", 63, 30 }, { "DATE", 93, 4 },
    };
    for (unsigned k = 0; k < 4; k++) {
        uint8_t raw[31];
        int n = fields[k].len;
        memcpy(raw, t + fields[k].at, (size_t)n);
        while (n && (raw[n - 1] == ' ' || raw[n - 1] == 0))
            n--;
        char v[100];
        to_utf8(0, raw, (size_t)n, v, sizeof(v));
        emit(fields[k].key, v, tag, ctx);
    }
    if (t[125] == 0 && t[126]) {        /* ID3v1.1: the track number */
        char v[8];
        snprintf(v, sizeof(v), "%u", t[126]);
        emit("TRACKNUMBER", v, tag, ctx);
    }
}

bool mp3file_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                  void (*tag)(const char *comment, void *ctx), void *ctx, uint32_t *cover)
{
    static layout L;                    /* the UI thread's */
    void *f = los_file_open(path);
    if (!f)
        return false;
    bool ok = read_layout(f, &L);
    if (ok) {
        *rate = L.h.rate;
        *channels = L.h.channels;
        *frames = music_frames(&L);
        if (cover)
            *cover = 0;
        if (!read_id3v2(f, tag, ctx, cover) && tag)
            read_id3v1(f, tag, ctx);
    }
    los_file_close(f);
    return ok;
}
