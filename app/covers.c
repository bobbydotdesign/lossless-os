/*
 * Lossless OS: album covers, shrunk to the size a screen shows them at.
 *
 * The JPEG is decoded block by block (TJpgDec, bundled with LVGL) and each
 * block is averaged straight into the small image, so the full-size picture
 * never sits in memory: what a 32 MB player needs. Each cover is decoded
 * once, at the largest size the screens use (COVER_MASTER); smaller sizes
 * are shrunk from that in memory, so the card is read once per album.
 * Results are kept, most recently used first. The JPEG can sit inside
 * another file (a FLAC's embedded picture): 'offset' says where.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "libs/tjpgd/tjpgd.h"
#include "covers.h"

/* Enough for every size of every album's cover at once (a cover on screen
 * must never be dropped); for big libraries this wants a thumbnail file on
 * the card instead (later). */
#define COVER_CACHE  256
#define COVER_MASTER 120
#define WORK_SIZE    4096

typedef struct {
    char path[256];
    uint32_t offset;
    int size;
    uint32_t used;
    lv_draw_buf_t *buf;    /* NULL: tried, no usable cover */
    bool valid;
} cover_entry;

static cover_entry cache[COVER_CACHE];
static uint32_t use_counter;

/* ---- decoding -------------------------------------------------------------------- */

typedef struct {
    lv_fs_file_t file;
    int size;               /* output side */
    int crop_x, crop_y;     /* top-left of the centred square in the source */
    int side;               /* square side in the source */
    uint32_t *sum;          /* size * size * 4: r, g, b, count */
} decode_ctx;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
    decode_ctx *d = jd->device;
    uint32_t got = 0;
    if (buf) {
        lv_fs_read(&d->file, buf, (uint32_t)n, &got);
        return got;
    }
    uint32_t pos = 0;
    lv_fs_tell(&d->file, &pos);
    return lv_fs_seek(&d->file, pos + (uint32_t)n, LV_FS_SEEK_SET) == LV_FS_RES_OK ? n : 0;
}

static int out_func(JDEC *jd, void *bitmap, JRECT *r)
{
    decode_ctx *d = jd->device;
    const uint8_t *px = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        int sy = y - d->crop_y;
        for (int x = r->left; x <= r->right; x++, px += 3) {
            int sx = x - d->crop_x;
            if (sx < 0 || sy < 0 || sx >= d->side || sy >= d->side)
                continue;
            int ox = sx * d->size / d->side, oy = sy * d->size / d->side;
            uint32_t *s = &d->sum[(oy * d->size + ox) * 4];
            s[0] += px[0];   /* TJpgDec gives R, G, B */
            s[1] += px[1];
            s[2] += px[2];
            s[3]++;
        }
    }
    return 1;
}

static lv_draw_buf_t *decode(const char *path, uint32_t offset, int size)
{
    char src[300];
    lv_snprintf(src, sizeof(src), "A:%s", path);
    decode_ctx d;
    memset(&d, 0, sizeof(d));
    if (lv_fs_open(&d.file, src, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return NULL;
    if (offset && lv_fs_seek(&d.file, offset, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
        lv_fs_close(&d.file);
        return NULL;
    }

    lv_draw_buf_t *out = NULL;
    void *work = lv_malloc(WORK_SIZE);
    JDEC jd;
    if (work && jd_prepare(&jd, in_func, work, WORK_SIZE, &d) == JDR_OK) {
        d.size = size;
        d.side = jd.width < jd.height ? jd.width : jd.height;
        d.crop_x = (jd.width - d.side) / 2;
        d.crop_y = (jd.height - d.side) / 2;
        d.sum = lv_malloc_zeroed((size_t)size * size * 4 * sizeof(uint32_t));
        if (d.sum && jd_decomp(&jd, out_func, 0) == JDR_OK) {
            out = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, 0);
            if (out) {
                for (int y = 0; y < size; y++) {
                    uint16_t *row = (uint16_t *)(out->data + y * out->header.stride);
                    for (int x = 0; x < size; x++) {
                        uint32_t *s = &d.sum[(y * size + x) * 4];
                        uint32_t n = s[3] ? s[3] : 1;
                        lv_color_t c = lv_color_make(s[0] / n, s[1] / n, s[2] / n);
                        row[x] = lv_color_to_u16(c);
                    }
                }
            }
        }
        lv_free(d.sum);
    }
    lv_free(work);
    lv_fs_close(&d.file);
    return out;
}

/* A smaller copy of a decoded cover: each output pixel averages the
 * source pixels under it */
static lv_draw_buf_t *shrink(const lv_draw_buf_t *src, int size)
{
    int from = src->header.w;
    lv_draw_buf_t *out = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, 0);
    if (!out)
        return NULL;
    for (int y = 0; y < size; y++) {
        uint16_t *row = (uint16_t *)(out->data + y * out->header.stride);
        int y0 = y * from / size, y1 = (y + 1) * from / size;
        for (int x = 0; x < size; x++) {
            int x0 = x * from / size, x1 = (x + 1) * from / size;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint16_t *s = (const uint16_t *)(src->data + sy * src->header.stride);
                for (int sx = x0; sx < x1; sx++, n++) {
                    r += s[sx] >> 11;
                    g += (s[sx] >> 5) & 0x3f;
                    b += s[sx] & 0x1f;
                }
            }
            if (!n)
                n = 1;
            row[x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
        }
    }
    return out;
}

/* ---- cache ----------------------------------------------------------------------- */

static cover_entry *lookup(const char *path, uint32_t offset, int size)
{
    for (int i = 0; i < COVER_CACHE; i++) {
        cover_entry *e = &cache[i];
        if (e->valid && e->size == size && e->offset == offset && !strcmp(e->path, path)) {
            e->used = ++use_counter;
            return e;
        }
    }
    return NULL;
}

static cover_entry *make_room(void)
{
    cover_entry *oldest = &cache[0];
    for (int i = 0; i < COVER_CACHE; i++) {
        cover_entry *e = &cache[i];
        if (!e->valid)
            return e;
        if (e->used < oldest->used)
            oldest = e;
    }
    if (oldest->buf)
        lv_draw_buf_destroy(oldest->buf);
    oldest->valid = false;
    return oldest;
}

const lv_draw_buf_t *cover_thumb(const char *path, uint32_t offset, int size)
{
    if (!path || !*path || size <= 0)
        return NULL;
    cover_entry *e = lookup(path, offset, size);
    if (e)
        return e->buf;

    lv_draw_buf_t *buf = NULL;
    if (size == COVER_MASTER || size > COVER_MASTER) {
        buf = decode(path, offset, size);
    } else {
        const lv_draw_buf_t *master = cover_thumb(path, offset, COVER_MASTER);
        buf = master ? shrink(master, size) : NULL;
    }

    e = make_room();
    e->valid = true;
    e->size = size;
    e->offset = offset;
    e->used = ++use_counter;
    lv_snprintf(e->path, sizeof(e->path), "%s", path);
    e->buf = buf;
    return buf;
}
