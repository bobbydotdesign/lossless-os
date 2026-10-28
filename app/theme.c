/*
 * Lossless OS: the look.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "los.h"
#include "theme.h"

los_theme theme;

lv_color_t theme_hex(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static lv_color_t mix(uint32_t a, uint32_t b, uint8_t weight_a)
{
    /* lv_color_mix(c1, c2, ratio): ratio 255 = all c1 */
    return lv_color_mix(lv_color_hex(a), lv_color_hex(b), weight_a);
}

/* A font file read into memory once and shared by every size of it: on a
 * player, drawing text from a file on the card would read it a few bytes at
 * a time for every glyph. */
static const void *font_data(const char *file, uint32_t *size)
{
    static struct { char name[32]; void *data; uint32_t size; } loaded[8];
    static int nloaded;
    for (int i = 0; i < nloaded; i++)
        if (!strcmp(loaded[i].name, file)) {
            *size = loaded[i].size;
            return loaded[i].data;
        }
    if (nloaded == (int)LV_ARRAYLEN(loaded))
        return NULL;

    char path[256];
    snprintf(path, sizeof(path), "A:%s/%s", los_assets_path(), file);
    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return NULL;
    uint32_t len = 0, got = 0;
    lv_fs_seek(&f, 0, LV_FS_SEEK_END);
    lv_fs_tell(&f, &len);
    lv_fs_seek(&f, 0, LV_FS_SEEK_SET);
    void *data = len ? lv_malloc(len) : NULL;
    if (data && (lv_fs_read(&f, data, len, &got) != LV_FS_RES_OK || got != len)) {
        lv_free(data);
        data = NULL;
    }
    lv_fs_close(&f);
    if (!data)
        return NULL;
    snprintf(loaded[nloaded].name, sizeof(loaded[0].name), "%s", file);
    loaded[nloaded].data = data;
    loaded[nloaded].size = len;
    nloaded++;
    *size = len;
    return data;
}

/* Korean for any size: Noto Sans CJK cut down to Hangul (assets/fonts),
 * used for the characters Geist doesn't have. NULL if it isn't there. */
#define FALLBACK_FONT "NotoSansCJK-Hangul.otf"

static const lv_font_t *fallback(int px)
{
    static struct { int px; lv_font_t *font; } made[12];
    static int nmade;
    for (int i = 0; i < nmade; i++)
        if (made[i].px == px)
            return made[i].font;
    uint32_t size = 0;
    const void *data = font_data(FALLBACK_FONT, &size);
    lv_font_t *f = data ? lv_tiny_ttf_create_data(data, size, px) : NULL;
    if (f && nmade < (int)LV_ARRAYLEN(made)) {
        made[nmade].px = px;
        made[nmade].font = f;
        nmade++;
    }
    return f;
}

static const lv_font_t *load(const char *file, int px)
{
    uint32_t size = 0;
    const void *data = font_data(file, &size);
    lv_font_t *f = data ? lv_tiny_ttf_create_data(data, size, px) : NULL;
    if (!f) {
        LV_LOG_WARN("font %s didn't load; using the built-in one", file);
        return &lv_font_montserrat_14;
    }
    f->fallback = fallback(px);
    return f;
}

void theme_init(bool dark)
{
    const los_palette *p = dark ? &los_graphite : &los_aluminium;
    theme.p = p;
    theme.dark = dark;
    theme.bg = lv_color_hex(p->bg);
    theme.fg = lv_color_hex(p->fg);
    theme.muted = lv_color_hex(p->muted);
    theme.faint = lv_color_hex(p->faint);
    theme.line = lv_color_hex(p->line);
    theme.raised = lv_color_hex(p->raised);
    theme.accent = lv_color_hex(p->accent);
    theme.select = mix(p->fg, p->bg, 52);       /* about a fifth of the way to fg */
    theme.accent_fg = lv_color_hex(p->accentFg);
    theme.ink = lv_color_hex(p->accentInk);
    theme.second = mix(p->fg, p->muted, 128);
    theme.dim = dark ? lv_color_hex(p->lcdDim) : lv_color_hex(p->faint);
    theme.seg_off = dark ? mix(p->line, p->bg, 191) : lv_color_hex(p->line);
    /* the black display panel looks the same in both modes */
    theme.lcd = lv_color_hex(p->lcd);
    theme.lcd_text = lv_color_hex(0xEEECE7);
    theme.lcd_muted = lv_color_hex(0x8D8A83);
    theme.lcd_off = lv_color_hex(0x2C2B29);
    theme.lcd_edge = dark ? lv_color_hex(p->line) : lv_color_hex(p->lcd);

    if (!theme.list) {
        /* everything in Geist Mono, the player's face (the dot-matrix
         * text is drawn, not a font); SemiBold only for song titles */
        theme.title = load("GeistMono-SemiBold.ttf", 20);
        theme.title_small = load("GeistMono-SemiBold.ttf", 17);
        theme.list = theme.title;
        theme.body = load("GeistMono-Medium.ttf", 16);
        theme.small = load("GeistMono-Medium.ttf", 14);
        theme.row = load("GeistMono-Medium.ttf", 19);
        theme.row_sub = load("GeistMono-Medium.ttf", 15);
        theme.track = load("GeistMono-Medium.ttf", 16);
        theme.label = load("GeistMono-Medium.ttf", 12);
        theme.mono = load("GeistMono-Medium.ttf", 12);
    }
}

lv_obj_t *theme_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *theme_tag(lv_obj_t *parent, const char *text, lv_color_t color)
{
    char caps[96];
    size_t i;
    for (i = 0; text[i] && i < sizeof(caps) - 1; i++)
        caps[i] = (char)toupper((unsigned char)text[i]);
    caps[i] = '\0';
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, caps);
    lv_obj_set_style_text_font(l, theme.label, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}
