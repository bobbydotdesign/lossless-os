/*
 * MP4 / M4A (mp4.h). The file is a tree of boxes: a 32-bit size (1: a
 * 64-bit size follows; 0: to the end of the file) and a four-letter type.
 *
 *   moov > trak > mdia > minf > stbl > stsd   the codec, and ALAC's cookie
 *                                      stts   packet durations
 *                                      stsz   packet sizes
 *                                      stsc   packets per chunk
 *                                      stco   chunk offsets (co64: 64-bit)
 *   moov > udta > meta > ilst > "©nam" > data ...   tags
 *
 * Source: ISO/IEC 14496-12 and Apple's QuickTime File Format documentation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "mp4.h"
#include "los_sys.h"

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint64_t be64(const uint8_t *b) { return (uint64_t)be32(b) << 32 | be32(b + 4); }

static bool at(void *f, uint64_t pos, void *buf, size_t n)
{
    return los_file_seek(f, pos) && los_file_read(f, buf, n) == n;
}

/* A box at 'pos' (before 'end'): its type, where its contents start, its end */
static bool box(void *f, uint64_t pos, uint64_t end, char type[4], uint64_t *body, uint64_t *next)
{
    uint8_t h[16];
    if (pos + 8 > end || !at(f, pos, h, 8))
        return false;
    uint64_t size = be32(h);
    memcpy(type, h + 4, 4);
    *body = pos + 8;
    if (size == 1) {
        if (!at(f, pos + 8, h + 8, 8))
            return false;
        size = be64(h + 8);
        *body = pos + 16;
    } else if (size == 0) {
        size = end - pos;
    }
    if (size < *body - pos || pos + size > end)
        return false;
    *next = pos + size;
    return true;
}

/* The track being read; kept if it's ALAC */
typedef struct {
    bool alac;
    uint8_t cookie[24];
    uint64_t frames;
    uint32_t packets, fixed_size;
    mp4_table sizes, chunks_runs, chunks;
    bool chunks64;
} track;

/* stsd: the first sample description; for "alac", the cookie in its own
 * "alac" box after the 28-byte audio sample entry */
static void read_stsd(void *f, uint64_t body, uint64_t end, track *t)
{
    uint8_t h[8];
    char type[4];
    uint64_t entry, entry_end;
    if (!box(f, body + 8, end, type, &entry, &entry_end) || memcmp(type, "alac", 4))
        return;
    /* QuickTime sound description versions 1 and 2 are longer */
    uint64_t pos = entry + 28, sub, sub_end;
    if (at(f, entry + 8, h, 2))
        pos += h[1] == 1 ? 16 : h[1] == 2 ? 36 : 0;
    while (box(f, pos, entry_end, type, &sub, &sub_end)) {
        if (!memcmp(type, "alac", 4) && sub_end - sub >= 28 && at(f, sub, h, 4) &&
            at(f, sub + 4, t->cookie, 24)) {
            t->alac = true;
            return;
        }
        pos = sub_end;
    }
}

static void read_table(void *f, uint64_t body, track *t, const char *type)
{
    uint8_t h[12];
    if (!at(f, body, h, 12))
        return;
    if (!strcmp(type, "stsz")) {
        if (!at(f, body + 4, h, 8))
            return;
        t->fixed_size = be32(h);
        t->packets = be32(h + 4);
        t->sizes = (mp4_table){ body + 12, t->packets };
        return;
    }
    uint32_t count = be32(h + 4);
    mp4_table tab = { body + 8, count };
    if (!strcmp(type, "stts")) {
        uint64_t frames = 0;
        uint8_t e[8];
        for (uint32_t i = 0; i < count && i < 1024; i++)
            if (at(f, tab.pos + 8ull * i, e, 8))
                frames += (uint64_t)be32(e) * be32(e + 4);
        t->frames = frames;
    } else if (!strcmp(type, "stsc")) {
        t->chunks_runs = tab;
    } else if (!strcmp(type, "stco") || !strcmp(type, "co64")) {
        t->chunks = tab;
        t->chunks64 = type[1] == 'o';
    }
}

static void read_tag(void *f, uint64_t body, uint64_t end, const char item[4], mp4_info *m)
{
    char type[4];
    uint64_t data, data_end;
    uint8_t h[8];
    if (!box(f, body, end, type, &data, &data_end) || memcmp(type, "data", 4) ||
        data_end - data < 8 || !at(f, data, h, 8))
        return;
    uint32_t kind = be32(h) & 0xffffff;          /* 1 UTF-8, 13 JPEG, 0 or 21 numbers */
    uint64_t value = data + 8, len = data_end - value;
    char *dst = NULL;
    if (!memcmp(item, "\xa9nam", 4))
        dst = m->title;
    else if (!memcmp(item, "\xa9" "ART", 4))
        dst = m->artist;
    else if (!memcmp(item, "aART", 4))
        dst = m->album_artist;
    else if (!memcmp(item, "\xa9" "alb", 4))
        dst = m->album;
    if (dst && kind == 1) {
        size_t n = len < sizeof(m->title) - 1 ? (size_t)len : sizeof(m->title) - 1;
        if (at(f, value, dst, n))
            dst[n] = '\0';
        else
            dst[0] = '\0';
    } else if (!memcmp(item, "\xa9" "day", 4) && kind == 1 && len >= 4) {
        char y[5] = "";
        if (at(f, value, y, 4))
            sscanf(y, "%4d", &m->year);
    } else if (!memcmp(item, "trkn", 4) && len >= 4 && at(f, value, h, 4)) {
        m->track = h[2] << 8 | h[3];
    } else if (!memcmp(item, "covr", 4) && kind == 13 && !m->cover) {
        m->cover = value;
    }
}

static void walk(void *f, uint64_t pos, uint64_t end, int depth, mp4_info *m, track *t, bool tags)
{
    char type[4];
    uint64_t body, next;
    while (depth < 8 && box(f, pos, end, type, &body, &next)) {
        char name[5] = { type[0], type[1], type[2], type[3], 0 };
        if (!strcmp(name, "trak")) {
            track tr;
            memset(&tr, 0, sizeof(tr));
            walk(f, body, next, depth + 1, m, &tr, false);
            if (tr.alac && !m->packets && tr.packets && tr.chunks.count && tr.chunks_runs.count) {
                memcpy(m->cookie, tr.cookie, sizeof(m->cookie));
                m->frames = tr.frames;
                m->packets = tr.packets;
                m->fixed_size = tr.fixed_size;
                m->sizes = tr.sizes;
                m->chunks_runs = tr.chunks_runs;
                m->chunks = tr.chunks;
                m->chunks64 = tr.chunks64;
            }
        } else if (!strcmp(name, "moov") || !strcmp(name, "mdia") || !strcmp(name, "minf") ||
                   !strcmp(name, "stbl") || !strcmp(name, "udta")) {
            walk(f, body, next, depth + 1, m, t, false);
        } else if (!strcmp(name, "ilst")) {
            walk(f, body, next, depth + 1, m, t, !t);
        } else if (!strcmp(name, "meta")) {
            /* usually a "full box" (4 bytes of version and flags first);
             * QuickTime's isn't */
            uint8_t h[4];
            uint64_t start = body;
            if (at(f, body, h, 4) && be32(h) == 0)
                start += 4;
            walk(f, start, next, depth + 1, m, t, false);
        } else if (t && !strcmp(name, "stsd")) {
            read_stsd(f, body, next, t);
        } else if (t && (!strcmp(name, "stts") || !strcmp(name, "stsz") || !strcmp(name, "stsc") ||
                         !strcmp(name, "stco") || !strcmp(name, "co64"))) {
            read_table(f, body, t, name);
        } else if (tags) {
            read_tag(f, body, next, type, m);     /* an item in ilst */
        }
        pos = next;
    }
}

int mp4_open(void *f, mp4_info *m)
{
    memset(m, 0, sizeof(*m));
    uint64_t size = los_file_size(f);
    uint8_t h[8];
    if (!size || !at(f, 0, h, 8) || memcmp(h + 4, "ftyp", 4))
        return -1;
    walk(f, 0, size, 0, m, NULL, false);
    return m->packets ? 0 : -2;
}

/* ---- packets ---- */

enum { T_SIZES, T_RUNS, T_CHUNKS };

void mp4_cursor_reset(mp4_cursor *c)
{
    memset(c->n, 0, sizeof(c->n));
}

/* Entry i of table 'which' (fields of 4 bytes each), through the cursor */
static bool entry(void *f, const mp4_info *m, mp4_cursor *c, int which, uint32_t i, uint32_t *out)
{
    const mp4_table *t = which == T_SIZES ? &m->sizes : which == T_RUNS ? &m->chunks_runs : &m->chunks;
    unsigned fields = which == T_RUNS ? 3 : which == T_CHUNKS && m->chunks64 ? 2 : 1;
    unsigned per = 3 * 64 / fields;
    if (i >= t->count)
        return false;
    if (i < c->first[which] || i >= c->first[which] + c->n[which]) {
        uint32_t first = i, n = t->count - i < per ? t->count - i : per;
        uint8_t buf[3 * 64 * 4];
        if (!at(f, t->pos + (uint64_t)first * fields * 4, buf, (size_t)n * fields * 4))
            return false;
        for (uint32_t k = 0; k < n * fields; k++)
            c->v[which][k] = be32(buf + 4 * k);
        c->first[which] = first;
        c->n[which] = n;
    }
    const uint32_t *e = &c->v[which][(i - c->first[which]) * fields];
    for (unsigned k = 0; k < fields; k++)
        out[k] = e[k];
    return true;
}

static bool packet_size(void *f, const mp4_info *m, mp4_cursor *c, uint32_t i, uint32_t *size)
{
    if (m->fixed_size) {
        *size = m->fixed_size;
        return true;
    }
    return entry(f, m, c, T_SIZES, i, size);
}

bool mp4_packet(void *f, const mp4_info *m, mp4_cursor *c, uint32_t i, uint64_t *offset,
                uint32_t *size)
{
    if (i >= m->packets)
        return false;
    /* the chunk holding packet i: runs of chunks with the same packet count */
    uint32_t done = 0, chunk = 0, in_chunk = 0;
    bool found = false;
    for (uint32_t r = 0; r < m->chunks_runs.count && !found; r++) {
        uint32_t run[3], after[3];
        if (!entry(f, m, c, T_RUNS, r, run) || !run[0] || !run[1])
            return false;
        uint32_t last = r + 1 < m->chunks_runs.count && entry(f, m, c, T_RUNS, r + 1, after) ?
                        after[0] - 1 : m->chunks.count;
        if (last < run[0])
            return false;
        uint64_t in_run = (uint64_t)(last - run[0] + 1) * run[1];
        if (i - done < in_run) {
            chunk = run[0] - 1 + (i - done) / run[1];
            in_chunk = (i - done) % run[1];
            found = true;
        } else {
            done += (uint32_t)in_run;
        }
    }
    uint32_t co[2];
    if (!found || !entry(f, m, c, T_CHUNKS, chunk, co))
        return false;
    uint64_t pos = m->chunks64 ? (uint64_t)co[0] << 32 | co[1] : co[0];
    for (uint32_t k = i - in_chunk; k < i; k++) {
        uint32_t s;
        if (!packet_size(f, m, c, k, &s))
            return false;
        pos += s;
    }
    *offset = pos;
    return packet_size(f, m, c, i, size);
}
