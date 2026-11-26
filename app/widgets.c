/*
 * Lossless OS: drawing pieces shared by the screens.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "widgets.h"
#include "theme.h"
#include "dotfont.h"
#include "covers.h"

/* ---- dot-matrix text ------------------------------------------------------------- */

typedef struct {
    char text[96];
    int dot;
    lv_color_t color;
    int max_w;      /* 0: as wide as the text; else wider text scrolls */
    int text_w;
    int offset;     /* scroll position, 0 or negative */
} dots_t;

/* Scrolling, like LVGL's labels: a second's pause, 40 px a second, round
 * and round with a gap of three dot-spaces between the end and the start */
#define DOTS_SCROLL_DELAY_MS 1000
#define DOTS_SCROLL_SPEED    40

static const los_dotglyph *glyph(char c)
{
    for (size_t i = 0; i < sizeof(los_dotfont) / sizeof(los_dotfont[0]); i++)
        if (los_dotfont[i].ch == c)
            return &los_dotfont[i];
    return NULL;
}

static int dots_width(const dots_t *d)
{
    int pitch = d->dot + 1, w = 0;
    for (const char *s = d->text; *s; s++) {
        const los_dotglyph *g = glyph(*s);
        if (g)
            w += (g->width + 1) * pitch;
    }
    return w > 0 ? w - pitch : 0;
}

static int dots_gap(const dots_t *d)
{
    return 3 * 4 * (d->dot + 1);   /* three spaces (3 dots + 1 between) */
}

static void dots_draw_text(lv_layer_t *layer, const dots_t *d, const lv_area_t *area, int x)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = d->color;
    r.bg_opa = LV_OPA_COVER;

    int pitch = d->dot + 1;
    for (const char *s = d->text; *s; s++) {
        const los_dotglyph *g = glyph(*s);
        if (!g)
            continue;
        int gw = (g->width + 1) * pitch;
        if (x + gw < area->x1 || x > area->x2) {   /* off the visible part */
            x += gw;
            continue;
        }
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < g->width; col++)
                if (g->rows[row] & (1 << col)) {
                    lv_area_t a = { x + col * pitch, area->y1 + row * pitch,
                                    x + col * pitch + d->dot - 1,
                                    area->y1 + row * pitch + d->dot - 1 };
                    lv_draw_rect(layer, &r, &a);
                }
        x += gw;
    }
}

static void dots_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    dots_t *d = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    /* The layer clips to the object, so text outside it isn't drawn */
    int x = area.x1 + d->offset;
    dots_draw_text(layer, d, &area, x);
    if (d->offset)   /* scrolling: the start again, following the end */
        dots_draw_text(layer, d, &area, x + d->text_w + dots_gap(d));
}

static void dots_scroll(void *var, int32_t v)
{
    lv_obj_t *o = var;
    dots_t *d = lv_obj_get_user_data(o);
    d->offset = v;
    lv_obj_invalidate(o);
}

/* Width, and scrolling if the text doesn't fit */
static void dots_layout(lv_obj_t *o)
{
    dots_t *d = lv_obj_get_user_data(o);
    d->text_w = dots_width(d);
    d->offset = 0;
    lv_anim_delete(o, dots_scroll);
    if (d->max_w && d->text_w > d->max_w) {
        int span = d->text_w + dots_gap(d);
        lv_obj_set_width(o, d->max_w);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, dots_scroll);
        lv_anim_set_values(&a, 0, -span);
        lv_anim_set_duration(&a, (uint32_t)span * 1000 / DOTS_SCROLL_SPEED);
        lv_anim_set_delay(&a, DOTS_SCROLL_DELAY_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, DOTS_SCROLL_DELAY_MS);
        lv_anim_start(&a);
    } else {
        lv_obj_set_width(o, d->text_w);
    }
    lv_obj_invalidate(o);
}

static void free_user_data(lv_event_t *e)
{
    lv_free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

lv_obj_t *dots_create(lv_obj_t *parent, int dot, lv_color_t color)
{
    lv_obj_t *o = theme_box(parent);
    dots_t *d = lv_malloc_zeroed(sizeof(*d));
    d->dot = dot;
    d->color = color;
    lv_obj_set_user_data(o, d);
    lv_obj_add_event_cb(o, dots_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, free_user_data, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(o, 0, 7 * (dot + 1) - 1);
    return o;
}

void dots_set_text(lv_obj_t *o, const char *text)
{
    dots_t *d = lv_obj_get_user_data(o);
    strncpy(d->text, text, sizeof(d->text) - 1);
    dots_layout(o);
}

void dots_set_max_width(lv_obj_t *o, int width)
{
    dots_t *d = lv_obj_get_user_data(o);
    d->max_w = width;
    dots_layout(o);
}

void dots_set_color(lv_obj_t *o, lv_color_t color)
{
    dots_t *d = lv_obj_get_user_data(o);
    d->color = color;
    lv_obj_invalidate(o);
}

/* ---- segmented bar ----------------------------------------------------------------- */

typedef struct {
    int count, gap, lit;
    lv_color_t on, off;
} segbar_t;

static void segbar_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    segbar_t *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int w = lv_area_get_width(&area);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.radius = 1;
    for (int i = 0; i < s->count; i++) {
        int x0 = area.x1 + (w + s->gap) * i / s->count;
        int x1 = area.x1 + (w + s->gap) * (i + 1) / s->count - s->gap - 1;
        lv_area_t a = { x0, area.y1, x1, area.y2 };
        r.bg_color = i < s->lit ? s->on : s->off;
        lv_draw_rect(layer, &r, &a);
    }
}

lv_obj_t *segbar_create(lv_obj_t *parent, int w, int h, int count, int gap,
                        lv_color_t on, lv_color_t off)
{
    lv_obj_t *o = theme_box(parent);
    segbar_t *s = lv_malloc_zeroed(sizeof(*s));
    s->count = count;
    s->gap = gap;
    s->on = on;
    s->off = off;
    lv_obj_set_user_data(o, s);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, segbar_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, free_user_data, LV_EVENT_DELETE, NULL);
    return o;
}

void segbar_set(lv_obj_t *o, int value, int max)
{
    segbar_t *s = lv_obj_get_user_data(o);
    int lit = max > 0 ? (value * s->count + max / 2) / max : 0;
    if (value > 0 && lit == 0)
        lit = 1;
    if (lit != s->lit) {
        s->lit = lit;
        lv_obj_invalidate(o);
    }
}

/* ---- panel ------------------------------------------------------------------------- */

lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = theme_box(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, theme.lcd, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LOS_RADIUS_SURFACE, 0);
    lv_obj_set_style_border_color(o, theme.lcd_edge, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    return o;
}

/* ---- the lossless mark ----------------------------------------------------------- */

/* Two linked rings of rounded tiles on an 8 x 4 grid, light and orange
 * (as on the start-up screen, theme/tools/make_boot_logo.py) */
static const struct { uint8_t row, col, accent; } mark_tiles[] = {
    {0, 1, 0}, {0, 2, 0}, {0, 5, 1}, {0, 6, 1},
    {1, 0, 0}, {1, 3, 0}, {1, 4, 1}, {1, 7, 1},
    {2, 0, 0}, {2, 3, 1}, {2, 4, 0}, {2, 7, 1},
    {3, 1, 0}, {3, 2, 0}, {3, 5, 1}, {3, 6, 1},
};

static void mark_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    int tile = (int)(intptr_t)lv_obj_get_user_data(obj), pitch = tile + 1;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.radius = tile / 4;
    for (size_t i = 0; i < sizeof(mark_tiles) / sizeof(mark_tiles[0]); i++) {
        /* the title bar's text and the accent; on a skin's own coloured bar,
         * that bar's text in two tones, so it never matches the bar */
        if (theme.header_fill)
            r.bg_color = mark_tiles[i].accent ? theme.header_fg : theme.header_muted;
        else
            r.bg_color = mark_tiles[i].accent ? theme.accent : theme.header_fg;
        lv_area_t t = { a.x1 + mark_tiles[i].col * pitch, a.y1 + mark_tiles[i].row * pitch, 0, 0 };
        t.x2 = t.x1 + tile - 1;
        t.y2 = t.y1 + tile - 1;
        lv_draw_rect(lv_event_get_layer(e), &r, &t);
    }
}

lv_obj_t *mark_create(lv_obj_t *parent, int tile)
{
    lv_obj_t *o = theme_box(parent);
    lv_obj_set_size(o, 8 * (tile + 1) - 1, 4 * (tile + 1) - 1);
    lv_obj_set_user_data(o, (void *)(intptr_t)tile);
    lv_obj_add_event_cb(o, mark_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    return o;
}

/* ---- play / pause icon ------------------------------------------------------------ */

/* A play triangle or pause bars in the box (x, y, w, h); nothing when stopped */
static void draw_play_state(lv_layer_t *layer, int x, int y, int w, int h, int state)
{
    if (state == 1) {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = theme.accent;
        t.opa = LV_OPA_COVER;
        t.p[0].x = x;          t.p[0].y = y;
        t.p[1].x = x + w - 1;  t.p[1].y = y + h / 2;
        t.p[2].x = x;          t.p[2].y = y + h - 1;
        lv_draw_triangle(layer, &t);
    } else if (state == 2) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = theme.accent;
        r.bg_opa = LV_OPA_COVER;
        int bw = (w + 1) * 2 / 7;
        lv_area_t b1 = { x, y, x + bw - 1, y + h - 1 };
        lv_area_t b2 = { x + w - bw, y, x + w - 1, y + h - 1 };
        lv_draw_rect(layer, &r, &b1);
        lv_draw_rect(layer, &r, &b2);
    }
}

static void state_icon_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    draw_play_state(lv_event_get_layer(e), a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a),
                    (int)(intptr_t)lv_obj_get_user_data(obj));
}

lv_obj_t *state_icon_create(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = theme_box(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, state_icon_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    return o;
}

void state_icon_set(lv_obj_t *icon, int state)
{
    if ((int)(intptr_t)lv_obj_get_user_data(icon) != state) {
        lv_obj_set_user_data(icon, (void *)(intptr_t)state);
        lv_obj_invalidate(icon);
    }
}

/* ---- header ------------------------------------------------------------------------ */

typedef struct {
    int playing;      /* 0 stop, 1 play, 2 pause */
    bool play_icon;   /* the play state at the left (the lock screen) instead of the time */
    lv_obj_t *time;   /* the time, at the left */
    lv_obj_t *title;  /* the screen's title, or "lossless" */
    lv_obj_t *level;  /* "-32 dB", shown in its place while the volume changes */
    int shown_db;     /* the dB the level text was set to */
} header_t;

/* The volume, the same on every screen (header_set_volume) */
static struct {
    bool known, flash;
    int db, min_db, max_db;
} volume;
/* Every header that exists (a screen's, the lock screen's over it): the
 * volume, padlock and Bluetooth state go to all of them */
#define HEADERS 4
static lv_obj_t *live_headers[HEADERS];
static bool keys_locked;        /* padlock shown (header_set_locked) */
static char clock_text[12];     /* the time at the left of headers ("" if unknown) */
static int bt_level;            /* rune: 0 off, 1 on, 2 headphones connected */
static int battery = -1;        /* percent, -1 unknown */
static bool charging;
static bool plugged;        /* USB power: the bolt */
static bool pct_always;     /* the percent beside the battery always (or while plugged in) */

static void header_show_volume(lv_obj_t *h)
{
    header_t *d = lv_obj_get_user_data(h);
    bool flash = volume.known && volume.flash && !theme.classic;   /* Classic: a line under the bar */
    if (flash && d->shown_db != volume.db) {
        char text[16];
        snprintf(text, sizeof(text), "%d dB", volume.db);
        dots_set_text(d->level, text);
        lv_obj_align(d->level, LV_ALIGN_CENTER, 0, 1 - LOS_VOLUME_H / 2);
        d->shown_db = volume.db;
    }
    if (flash) {
        lv_obj_add_flag(d->title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(d->level, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(d->title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->level, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(h);
}

/* Classic's title bar (theme.classic): the play state left, the battery
 * right with a green level that turns red when low, and a 1px edge under
 * it; the volume shows as a thin line only while it changes */
static void header_classic_draw(lv_event_t *e, header_t *h)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int x = a.x1, y = a.y1, H = lv_area_get_height(&a);

    draw_play_state(layer, x + 9, y + (H - 10) / 2 - 1, 9, 10, h->playing);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    /* the battery: an outline, a nub, and a glossy level */
    int bx2 = x + LOS_W - 9, bx1 = bx2 - 22, by1 = y + (H - 11) / 2 - 1, by2 = by1 + 10;
    r.bg_opa = LV_OPA_COVER;
    r.bg_color = lv_color_hex(0xF4F4F4);
    r.border_color = lv_color_hex(0x7A7A7A);
    r.border_width = 1;
    r.radius = 2;
    lv_area_t bat = { bx1, by1, bx2, by2 };
    lv_draw_rect(layer, &r, &bat);
    r.border_width = 0;
    r.radius = 0;
    r.bg_color = lv_color_hex(0x7A7A7A);
    lv_area_t nub = { bx2 + 1, by1 + 3, bx2 + 2, by2 - 3 };
    lv_draw_rect(layer, &r, &nub);
    int inner = (bx2 - 2) - (bx1 + 2) + 1;
    int w = battery < 0 ? inner : (inner * battery + 50) / 100;
    if (battery >= 0 && w < 2)
        w = 2;
    bool low = battery >= 0 && battery <= 20 && !charging;
    r.bg_color = low ? lv_color_hex(0xF2675A) : lv_color_hex(0xA8E27E);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0].color = r.bg_color;
    r.bg_grad.stops[0].opa = LV_OPA_COVER;
    r.bg_grad.stops[0].frac = 0;
    r.bg_grad.stops[1].color = low ? lv_color_hex(0xC8281C) : lv_color_hex(0x46A82A);
    r.bg_grad.stops[1].opa = LV_OPA_COVER;
    r.bg_grad.stops[1].frac = 255;
    lv_area_t fill = { bx1 + 2, by1 + 2, bx1 + 1 + w, by2 - 2 };
    lv_draw_rect(layer, &r, &fill);
    r.bg_grad.dir = LV_GRAD_DIR_NONE;

    /* left of the battery: a bolt while plugged in, the percent, Bluetooth, the padlock */
    int at = bx1 - 4;
    if (plugged) {
        static const char *const bolt[] = { "..##", ".##.", "####", ".##.", "##..", "#..." };
        r.bg_color = theme.header_fg;
        for (int row = 0; row < 6; row++)
            for (int col = 0; col < 4; col++)
                if (bolt[row][col] == '#') {
                    lv_area_t px = { at - 4 + col, by1 + 2 + row, at - 4 + col, by1 + 2 + row };
                    lv_draw_rect(layer, &r, &px);
                }
        at -= 8;
    }
    if (battery >= 0 && (plugged || pct_always)) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", battery);
        lv_point_t ts;
        lv_text_get_size(&ts, pct, theme.label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_draw_label_dsc_t t;
        lv_draw_label_dsc_init(&t);
        t.text = pct;
        t.font = theme.label;
        t.color = theme.header_fg;
        lv_area_t ta = { at - ts.x, y + (H - ts.y) / 2 - 1, at, y + (H - ts.y) / 2 - 1 + ts.y };
        lv_draw_label(layer, &t, &ta);
        at -= ts.x + 5;
    }
    if (bt_level > 0) {
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color = bt_level == 2 ? theme.accent : theme.header_muted;
        l.width = 1;
        static const int rune[][4] = {
            { 0, 2, 5, 7 }, { 5, 7, 2, 9 }, { 2, 9, 2, 0 }, { 2, 0, 5, 2 }, { 5, 2, 0, 7 },
        };
        int rx = at - 6, ry = y + (H - 10) / 2 - 1;
        for (size_t i = 0; i < sizeof(rune) / sizeof(rune[0]); i++) {
            l.p1.x = rx + rune[i][0]; l.p1.y = ry + rune[i][1];
            l.p2.x = rx + rune[i][2]; l.p2.y = ry + rune[i][3];
            lv_draw_line(layer, &l);
        }
        at -= 11;
    }
    if (keys_locked) {
        int px = at - 9, py = y + (H - 11) / 2 - 1;
        lv_draw_rect_dsc_t s;
        lv_draw_rect_dsc_init(&s);
        s.bg_opa = LV_OPA_TRANSP;
        s.border_color = theme.header_fg;
        s.border_width = 1;
        s.radius = 3;
        lv_area_t shackle = { px + 1, py, px + 7, py + 7 };
        lv_draw_rect(layer, &s, &shackle);
        s.bg_opa = LV_OPA_COVER;
        s.bg_color = theme.header_fg;
        s.border_width = 0;
        s.radius = 1;
        lv_area_t body = { px, py + 4, px + 8, py + 10 };
        lv_draw_rect(layer, &s, &body);
    }

    /* the edge under the bar, and the volume over it while it changes */
    r.bg_color = lv_color_hex(0x9C9C9C);
    lv_area_t edge = { a.x1, a.y2, a.x2, a.y2 };
    lv_draw_rect(layer, &r, &edge);
    if (volume.flash && volume.known && volume.max_db > volume.min_db) {
        int span = volume.max_db - volume.min_db;
        int vw = (int)((int64_t)LOS_W * (volume.db - volume.min_db) / span);
        r.bg_color = theme.accent;
        lv_area_t level = { a.x1, a.y2 - 1, a.x1 + vw - 1, a.y2 };
        if (vw > 0)
            lv_draw_rect(layer, &r, &level);
    }
}

static void header_icons_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    header_t *h = lv_obj_get_user_data(obj);
    if (theme.classic) {
        header_classic_draw(e, h);
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int x = a.x1, y = a.y1;

    /* play state, left (the lock screen; elsewhere the time is there) */
    if (h->play_icon)
        draw_play_state(layer, x + 14, y + 10, 11, 12, h->playing);

    /* the percent, left of the bolt (or the battery), while plugged in or
     * always (Settings); the rune and padlock move over for it */
    char pct[16] = "";
    int shift = 0;
    if (battery >= 0 && (plugged || pct_always)) {
        snprintf(pct, sizeof(pct), "%d%%", battery);
        lv_point_t ts;
        lv_text_get_size(&ts, pct, theme.label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int right = x + LOS_W - (plugged ? 45 : 36) - 4;
        lv_draw_label_dsc_t t;
        lv_draw_label_dsc_init(&t);
        t.text = pct;
        t.font = theme.label;
        t.color = theme.header_fg;
        lv_area_t ta = { right - ts.x + 1, y + 15 - ts.y / 2, right, y + 15 - ts.y / 2 + ts.y - 1 };
        lv_draw_label(layer, &t, &ta);
        shift = ts.x + 4;
    }

    /* Bluetooth rune, then battery, right */
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = bt_level == 2 ? theme.accent : bt_level == 1 ? theme.header_fg : theme.header_faint;
    l.width = 1;
    int bx = x + LOS_W - (plugged ? 60 : 52) - shift, by = y + 9;
    static const int rune[][4] = {
        { 0, 3, 7, 10 }, { 7, 10, 3, 13 }, { 3, 13, 3, 0 }, { 3, 0, 7, 3 }, { 7, 3, 0, 10 },
    };
    for (size_t i = 0; i < sizeof(rune) / sizeof(rune[0]); i++) {
        l.p1.x = bx + rune[i][0]; l.p1.y = by + rune[i][1];
        l.p2.x = bx + rune[i][2]; l.p2.y = by + rune[i][3];
        lv_draw_line(layer, &l);
    }

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_TRANSP;
    r.border_color = theme.header_fg;
    r.border_width = 1;
    r.radius = 2;
    lv_area_t bat = { x + LOS_W - 36, y + 9, x + LOS_W - 14, y + 20 };
    lv_draw_rect(layer, &r, &bat);
    r.bg_opa = LV_OPA_COVER;
    r.bg_color = charging ? theme.accent : theme.header_fg;
    r.border_width = 0;
    r.radius = 1;
    /* filled to the level (orange while charging; a sliver when nearly empty) */
    int inner = (bat.x2 - 2) - (bat.x1 + 2) + 1;
    int w = battery < 0 ? inner : (inner * battery + 50) / 100;
    if (battery >= 0 && w < 1)
        w = 1;
    lv_area_t fill = { bat.x1 + 2, bat.y1 + 2, bat.x1 + 1 + w, bat.y2 - 2 };
    lv_draw_rect(layer, &r, &fill);
    r.bg_color = theme.header_fg;
    lv_area_t nub = { bat.x2 + 1, bat.y1 + 3, bat.x2 + 2, bat.y2 - 3 };
    lv_draw_rect(layer, &r, &nub);

    /* the cable is in: a pixel lightning bolt left of the battery (the
     * Bluetooth rune and padlock move over for it), orange while
     * charging, white once full */
    if (plugged) {
        static const char *const bolt[] = {
            "...###", "...##.", "..##..", "..##..", ".##...", "######",
            "######", "...##.", "..##..", "..##..", ".##...", ".#....",
        };
        lv_draw_rect_dsc_t z;
        lv_draw_rect_dsc_init(&z);
        z.bg_opa = LV_OPA_COVER;
        z.bg_color = charging ? theme.accent : theme.header_fg;
        int zx = x + LOS_W - 45, zy = y + 9;
        for (int row = 0; row < 12; row++)
            for (int col = 0; col < 6; col++)
                if (bolt[row][col] == '#') {
                    lv_area_t px = { zx + col, zy + row, zx + col, zy + row };
                    lv_draw_rect(layer, &z, &px);
                }
    }

    /* padlock, left of the Bluetooth rune, while the keys are locked
     * (not on Gallery: its lock screen says so already) */
    if (keys_locked && !theme.gallery) {
        int px = x + LOS_W - (plugged ? 78 : 70) - shift, py = y + 8;
        lv_draw_rect_dsc_t s;
        lv_draw_rect_dsc_init(&s);
        s.bg_opa = LV_OPA_TRANSP;
        s.border_color = theme.header_fg;
        s.border_width = 2;
        s.radius = 4;
        lv_area_t shackle = { px + 1, py, px + 9, py + 9 };
        lv_draw_rect(layer, &s, &shackle);
        s.bg_opa = LV_OPA_COVER;
        s.bg_color = theme.header_fg;
        s.border_width = 0;
        s.radius = 1;
        lv_area_t body = { px, py + 6, px + 10, py + 14 };
        lv_draw_rect(layer, &s, &body);
    }

    /* volume bar, the bottom edge: a track, filled to the level (Gallery:
     * only while the volume changes, a thin line) */
    if (theme.gallery && !volume.flash)
        return;
    lv_area_t track = { a.x1, a.y2 - (theme.gallery ? 1 : LOS_VOLUME_H - 1), a.x2, a.y2 };
    r.radius = 0;
    r.bg_color = theme.header_line;
    lv_draw_rect(layer, &r, &track);
    if (volume.known && volume.max_db > volume.min_db) {
        int span = volume.max_db - volume.min_db;
        int w = (int)((int64_t)LOS_W * (volume.db - volume.min_db) / span);
        if (w > 0) {
            lv_area_t level = track;
            level.x2 = level.x1 + w - 1;
            /* gray: orange is for what's playing (the progress bar);
             * brighter while the volume is being changed */
            r.bg_color = volume.flash ? theme.header_fg : theme.header_muted;
            lv_draw_rect(layer, &r, &level);
        }
    }
}

static void header_deleted(lv_event_t *e)
{
    lv_obj_t *h = lv_event_get_target_obj(e);
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i] == h)
            live_headers[i] = NULL;
}

void header_set_bluetooth(int level)
{
    if (bt_level == level)
        return;
    bt_level = level;
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i])
            lv_obj_invalidate(live_headers[i]);
}

void header_set_battery(int percent, bool is_charging, bool is_plugged)
{
    if (percent == battery && is_charging == charging && is_plugged == plugged)
        return;
    battery = percent;
    charging = is_charging;
    plugged = is_plugged;
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i])
            lv_obj_invalidate(live_headers[i]);
}

void header_set_percent_always(bool on)
{
    if (pct_always == on)
        return;
    pct_always = on;
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i])
            lv_obj_invalidate(live_headers[i]);
}

void header_set_locked(bool locked)
{
    if (keys_locked == locked)
        return;
    keys_locked = locked;
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i])
            lv_obj_invalidate(live_headers[i]);
}

void header_set_volume(bool known, int db, int min_db, int max_db, bool flash)
{
    if (volume.known == known && volume.db == db && volume.min_db == min_db &&
        volume.max_db == max_db && volume.flash == flash)
        return;
    volume.known = known;
    volume.db = db;
    volume.min_db = min_db;
    volume.max_db = max_db;
    volume.flash = flash;
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i])
            header_show_volume(live_headers[i]);
}

lv_obj_t *header_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *h = theme_box(parent);
    header_t *d = lv_malloc_zeroed(sizeof(*d));
    lv_obj_set_user_data(h, d);
    lv_obj_add_event_cb(h, free_user_data, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(h, header_icons_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(h, header_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(h, LOS_W, LOS_HEADER_H);
    if (theme.header_fill) {           /* a skin's title bar: a band of its own */
        lv_obj_set_style_bg_color(h, theme.header_bg, 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        if (theme.header_grad) {
            lv_obj_set_style_bg_grad_color(h, theme.header_bottom, 0);
            lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_VER, 0);
        }
    }

    if (theme.classic) {
        /* the name in bold, centred; the play state at the left, no clock */
        d->title = lv_label_create(h);
        lv_label_set_text(d->title, title ? title : "Lossless");
        lv_obj_set_style_text_font(d->title, theme.bar_title, 0);
        lv_obj_set_style_text_color(d->title, theme.header_fg, 0);
        lv_obj_set_style_text_align(d->title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(d->title, LV_LABEL_LONG_MODE_DOTS);   /* clear of the icons */
        lv_obj_set_size(d->title, LOS_W - 2 * 72, lv_font_get_line_height(theme.bar_title));   /* one line */
        lv_obj_align(d->title, LV_ALIGN_CENTER, 0, -1);
        d->play_icon = true;
    } else if (title) {         /* centred in the 32 px above the volume bar */
        /* on a skin's own bar, its text colour: the softer one fades into it */
        d->title = theme_tag(h, title, theme.header_fill ? theme.header_fg : theme.header_muted);
        lv_obj_update_layout(d->title);
        if (lv_obj_get_width(d->title) > LOS_W - 2 * 98) {   /* clear of the clock and the icons */
            lv_label_set_long_mode(d->title, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_size(d->title, LOS_W - 2 * 98, lv_obj_get_height(d->title));
        }
        lv_obj_align(d->title, LV_ALIGN_CENTER, 0, -LOS_VOLUME_H / 2);
    } else {
        d->title = mark_create(h, 4);
        lv_obj_align(d->title, LV_ALIGN_CENTER, 0, -LOS_VOLUME_H / 2);
    }
    /* small dots, about the height of the screen titles, in the colour
     * the volume bar takes while it changes */
    d->level = dots_create(h, 1, theme.header_fg);
    /* the time, left, on the same line as the title */
    d->time = lv_label_create(h);
    lv_label_set_text(d->time, clock_text);
    lv_obj_set_style_text_font(d->time, theme.label, 0);
    lv_obj_set_style_text_color(d->time, theme.header_fg, 0);
    lv_obj_align(d->time, LV_ALIGN_LEFT_MID, LOS_EDGE, -LOS_VOLUME_H / 2);
    if (theme.classic)
        lv_obj_add_flag(d->time, LV_OBJ_FLAG_HIDDEN);
    d->shown_db = 1;   /* no dB is positive: the text gets set when first shown */
    for (int i = 0; i < HEADERS; i++)
        if (!live_headers[i]) {
            live_headers[i] = h;
            break;
        }
    header_show_volume(h);
    return h;
}

void header_show_play_state(lv_obj_t *h)
{
    header_t *d = lv_obj_get_user_data(h);
    d->play_icon = true;
    lv_obj_add_flag(d->time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(h);
}

void header_show_mark_left(lv_obj_t *h)
{
    header_t *d = lv_obj_get_user_data(h);
    lv_obj_add_flag(d->time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *m = mark_create(h, 4);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, LOS_EDGE, -LOS_VOLUME_H / 2);
}

void header_set_time(const char *text)
{
    if (!strcmp(text, clock_text))
        return;
    snprintf(clock_text, sizeof(clock_text), "%s", text);
    for (int i = 0; i < HEADERS; i++)
        if (live_headers[i]) {
            header_t *d = lv_obj_get_user_data(live_headers[i]);
            lv_label_set_text(d->time, clock_text);
        }
}

void header_set_playing(lv_obj_t *h, int state)
{
    header_t *d = lv_obj_get_user_data(h);
    if (d->playing != state) {
        d->playing = state;
        lv_obj_invalidate(h);
    }
}

/* ---- cover ------------------------------------------------------------------------- */

lv_obj_t *cover_create(lv_obj_t *parent, const char *path, uint32_t offset, int size,
                       int radius)
{
    lv_obj_t *frame = theme_box(parent);
    lv_obj_set_size(frame, size, size);
    lv_obj_set_style_radius(frame, radius, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, theme.lcd, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    const lv_draw_buf_t *thumb = cover_thumb(path, offset, size);
    if (thumb) {
        lv_obj_t *img = lv_image_create(frame);
        lv_image_set_src(img, thumb);
    } else if (size < 40) {
        /* no cover, small (song rows: many at once): one orange dot */
        lv_obj_t *d = theme_box(frame);
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, theme.accent, 0);
        lv_obj_center(d);
    } else {
        /* no cover: a grid of unlit dots with one orange dot */
        int n = size >= 80 ? 7 : 5;
        int pitch = size / (n + 1);
        for (int gy = 0; gy < n; gy++)
            for (int gx = 0; gx < n; gx++) {
                lv_obj_t *d = theme_box(frame);
                int ds = size >= 80 ? 4 : 3;
                lv_obj_set_size(d, ds, ds);
                lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                bool lit = gx == n / 2 && gy == n / 2;
                lv_obj_set_style_bg_color(d, lit ? theme.accent : theme.lcd_off, 0);
                lv_obj_set_pos(d, (size - (n - 1) * pitch) / 2 + gx * pitch - ds / 2,
                               (size - (n - 1) * pitch) / 2 + gy * pitch - ds / 2);
            }
    }
    return frame;
}
