/*
 * Covers (coverflow.h).
 *
 * Each album sits at d = its index - pos. The facing one (d = 0) is drawn
 * flat and full size; from d = 0 to 1 a cover turns: it slides out, gets
 * narrower and its inner edge shorter; past 1 the rest stack up behind,
 * closer together and a little darker. The farthest are drawn first, so
 * the ones nearer the middle cover them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "coverflow.h"
#include "misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop */

#define C       COVERFLOW_COVER
#define TOP     6       /* above the covers */
#define FIRST   98      /* the middle to the centre of the first turned cover */
#define STEP    26      /* between the turned covers after that */
#define SIDE_W  44      /* a turned cover's width */
#define INNER_H 90      /* a turned cover's edge towards the middle */
#define REFLECT 34      /* the reflection under a facing cover, in rows */
#define REACH   5       /* covers drawn on each side */

static uint16_t pixels[COVERFLOW_W * COVERFLOW_H] __attribute__((aligned(4)));
static lv_draw_buf_t frame;

lv_obj_t *coverflow_create(lv_obj_t *parent)
{
    lv_draw_buf_init(&frame, COVERFLOW_W, COVERFLOW_H, LV_COLOR_FORMAT_RGB565, COVERFLOW_W * 2,
                     pixels, sizeof(pixels));
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &frame);
    return img;
}

/* RGB565 'p' at k/256 of its brightness */
static inline uint16_t shade(uint16_t p, unsigned k)
{
    unsigned r = (p >> 11) * k >> 8, g = ((p >> 5) & 63) * k >> 8, b = (p & 31) * k >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* a/256 of 'p' over 'q' */
static inline uint16_t blend(uint16_t p, uint16_t q, unsigned a)
{
    unsigned na = 256 - a;
    unsigned r = ((p >> 11) * a + (q >> 11) * na) >> 8;
    unsigned g = (((p >> 5) & 63) * a + ((q >> 5) & 63) * na) >> 8;
    unsigned b = ((p & 31) * a + (q & 31) * na) >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* One album at d from the middle */
static void draw_one(float d, const lv_draw_buf_t *src, uint16_t bg, uint16_t blank, uint16_t dot)
{
    float ad = fabsf(d), turn = ad < 1 ? ad : 1;
    int side = d < 0 ? -1 : 1;
    float cx = COVERFLOW_W / 2.0f + side * (turn * FIRST + (ad > 1 ? (ad - 1) * STEP : 0));
    float w = lerp(C, SIDE_W, turn);
    float inner = lerp(C, INNER_H, turn);
    /* darker as it turns and the farther it is */
    float k = lerp(1.0f, 0.62f, turn) * (ad > 1 ? fmaxf(0.35f, 1.0f - 0.14f * (ad - 1)) : 1.0f);
    unsigned light = (unsigned)(k * 256);
    float left = cx - w / 2;
    int x0 = (int)floorf(left), x1 = (int)ceilf(left + w);
    if (x0 < 0)
        x0 = 0;
    if (x1 > COVERFLOW_W)
        x1 = COVERFLOW_W;
    const uint8_t *data = src ? src->data : NULL;
    uint32_t stride = src ? src->header.stride : 0;
    int mid = TOP + C / 2;
    for (int x = x0; x < x1; x++) {
        float t = (x + 0.5f - left) / w;          /* 0 at the cover's left edge, 1 at its right */
        if (t < 0 || t >= 1)
            continue;
        /* the outer edge stays full height; the edge towards the middle is shorter */
        float tin = side < 0 ? t : 1 - t;          /* 0 outer, 1 inner (a turned cover) */
        int h = (int)lerp(C, inner, tin);
        if (h < 2)
            continue;
        int sx = (int)(t * C);
        int y0 = mid - h / 2;
        int rows = h * REFLECT / C;
        for (int y = 0; y < h + rows; y++) {
            int dy = y0 + y;
            if (dy < 0 || dy >= COVERFLOW_H)
                continue;
            bool mirror = y >= h;
            int sy = mirror ? C - 1 - (y - h) * C / h : y * C / h;
            uint16_t p;
            if (data)
                p = ((const uint16_t *)(data + (uint32_t)sy * stride))[sx];
            else    /* no cover: a plain square with one dot in the middle */
                p = sx >= C / 2 - 5 && sx < C / 2 + 5 && sy >= C / 2 - 5 && sy < C / 2 + 5 ? dot : blank;
            p = shade(p, light);
            if (mirror) {
                unsigned a = 110u * (unsigned)(rows - (y - h)) / (unsigned)rows;   /* fades out */
                p = blend(p, bg, a);
            }
            pixels[dy * COVERFLOW_W + x] = p;
        }
    }
}

/* Flat (coverflow_draw_flat): at d from the middle, a square of side
 * 'size' centred 'off' from the middle, at 'k' of its brightness. From the
 * facing cover (d = 0) to the first beside it (1), to the next (2), then
 * off the edge. */
static void flat_place(float ad, float *size, float *off, float *k)
{
    if (ad <= 1) {
        *size = lerp(C, 84, ad);
        *off = lerp(0, 82, ad);
        *k = lerp(1.0f, 0.62f, ad);
    } else if (ad <= 2) {
        *size = lerp(84, 64, ad - 1);
        *off = lerp(82, 134, ad - 1);
        *k = lerp(0.62f, 0.38f, ad - 1);
    } else {
        float t = ad - 2 < 1 ? ad - 2 : 1;
        *size = lerp(64, 52, t);
        *off = lerp(134, 178, t);
        *k = lerp(0.38f, 0.25f, t);
    }
}

/* A soft pixel for the covers set back: nine samples around (sx, sy),
 * 'r' source pixels apart, averaged */
static uint16_t soft(const uint8_t *data, uint32_t stride, int sx, int sy, int r)
{
    unsigned rs = 0, gs = 0, bs = 0;
    for (int dy = -r; dy <= r; dy += r)
        for (int dx = -r; dx <= r; dx += r) {
            int x = sx + dx, y = sy + dy;
            x = x < 0 ? 0 : x >= C ? C - 1 : x;
            y = y < 0 ? 0 : y >= C ? C - 1 : y;
            uint16_t q = ((const uint16_t *)(data + (uint32_t)y * stride))[x];
            rs += q >> 11;
            gs += (q >> 5) & 63;
            bs += q & 31;
        }
    return (uint16_t)((rs / 9) << 11 | (gs / 9) << 5 | (bs / 9));
}

static void draw_flat(float d, const lv_draw_buf_t *src, uint16_t blank, uint16_t dot, bool round)
{
    float size, off, k;
    float ad = fabsf(d);
    flat_place(ad, &size, &off, &k);
    /* depth: the facing cover sharp, the ones beside it a little soft, the
     * farther ones softer */
    int blur = ad < 0.5f ? 0 : ad < 1.5f ? 2 : 3;
    float corner = size * 0.06f;      /* rounded corners: 7 px on the facing cover */
    unsigned light = (unsigned)(k * 256);
    float cx = COVERFLOW_W / 2.0f + (d < 0 ? -off : off), cy = TOP + C / 2.0f;
    float left = cx - size / 2, top = cy - size / 2, r = size / 2;
    int x0 = (int)floorf(left), x1 = (int)ceilf(left + size);
    int y0 = (int)floorf(top), y1 = (int)ceilf(top + size);
    if (x0 < 0)
        x0 = 0;
    if (x1 > COVERFLOW_W)
        x1 = COVERFLOW_W;
    if (y0 < 0)
        y0 = 0;
    if (y1 > COVERFLOW_H)
        y1 = COVERFLOW_H;
    const uint8_t *data = src ? src->data : NULL;
    uint32_t stride = src ? src->header.stride : 0;
    float scale = C / size;
    for (int y = y0; y < y1; y++) {
        float fy = y + 0.5f - top;
        if (fy < 0 || fy >= size)
            continue;
        int sy = (int)(fy * scale);
        for (int x = x0; x < x1; x++) {
            float fx = x + 0.5f - left;
            if (fx < 0 || fx >= size)
                continue;
            unsigned edge = 256;          /* a soft edge on a round cover, or its rounded corners */
            float rr = round ? r : corner, cx0 = fx < rr ? rr - fx : fx > size - rr ? fx - (size - rr) : 0;
            float cy0 = fy < rr ? rr - fy : fy > size - rr ? fy - (size - rr) : 0;
            if (cx0 > 0 && cy0 > 0) {
                float dist = sqrtf(cx0 * cx0 + cy0 * cy0);
                if (dist >= rr)
                    continue;
                if (dist > rr - 1)
                    edge = (unsigned)((rr - dist) * 256);
            }
            int sx = (int)(fx * scale);
            uint16_t p;
            if (data && blur)
                p = soft(data, stride, sx, sy, blur);
            else if (data)
                p = ((const uint16_t *)(data + (uint32_t)sy * stride))[sx];
            else
                p = sx >= C / 2 - 5 && sx < C / 2 + 5 && sy >= C / 2 - 5 && sy < C / 2 + 5 ? dot : blank;
            p = shade(p, light);
            if (edge < 256)
                p = blend(p, pixels[y * COVERFLOW_W + x], edge);
            pixels[y * COVERFLOW_W + x] = p;
        }
    }
}

/* The albums in reach, farthest from the middle first */
static int draw_order(float pos, int count, int *order)
{
    int centre = (int)lroundf(pos), n = 0;
    for (int i = centre - REACH; i <= centre + REACH; i++)
        if (i >= 0 && i < count)
            order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && fabsf(order[j] - pos) > fabsf(order[j - 1] - pos); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    return n;
}

static int skip = -1;

void coverflow_skip(int i)
{
    skip = i;
}

void coverflow_draw_flat(lv_obj_t *img, float pos, int count, coverflow_cover_fn cover, lv_color_t bg,
                         lv_color_t blank, lv_color_t dot, bool round)
{
    uint16_t b = lv_color_to_u16(bg), none = lv_color_to_u16(blank), d = lv_color_to_u16(dot);
    for (int i = 0; i < COVERFLOW_W * COVERFLOW_H; i++)
        pixels[i] = b;
    int order[2 * REACH + 1];
    int n = draw_order(pos, count, order);
    for (int i = 0; i < n; i++)
        if (order[i] != skip)
            draw_flat(order[i] - pos, cover(order[i]), none, d, round);
    lv_image_cache_drop(&frame);
    lv_obj_invalidate(img);
}

void coverflow_draw(lv_obj_t *img, float pos, int count, coverflow_cover_fn cover, lv_color_t bg,
                    lv_color_t blank, lv_color_t dot)
{
    uint16_t b = lv_color_to_u16(bg), none = lv_color_to_u16(blank), d = lv_color_to_u16(dot);
    for (int i = 0; i < COVERFLOW_W * COVERFLOW_H; i++)
        pixels[i] = b;
    /* the albums in reach, farthest from the middle first */
    int centre = (int)lroundf(pos), order[2 * REACH + 1], n = 0;
    for (int i = centre - REACH; i <= centre + REACH; i++)
        if (i >= 0 && i < count)
            order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && fabsf(order[j] - pos) > fabsf(order[j - 1] - pos); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int i = 0; i < n; i++)
        draw_one(order[i] - pos, cover(order[i]), b, none, d);
    lv_image_cache_drop(&frame);
    lv_obj_invalidate(img);
}
