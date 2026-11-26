/*
 * Skins (skins.h). A card skin is read when it's chosen, so a changed file
 * shows the next time it's picked; a file that can't be read gives
 * Graphite.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lvgl.h"
#include "skins.h"
#include "los_sys.h"

/* The built-in themes beyond Graphite (the design tokens' own), written as
 * theme files are (docs/themes.md), so any of them is a starting point for
 * your own. Aluminium, the tokens' light palette, isn't listed: it's what
 * a light theme's missing colours come from. */
static const char classic_text[] =
    "dark = no\n"
    "bg = #FFFFFF\n raised = #F2F2F2\n line = #ECECEC\n"
    "fg = #111111\n muted = #666666\n faint = #777777\n"
    "accent = #2F6FD6\n accentInk = #2F6FD6\n accentFg = #FFFFFF\n lcd = #111111\n"
    "select = #5C9DF2\n selectGrad = #1E5BCB\n selectFg = #FFFFFF\n"
    "header = #FAFAFA\n headerGrad = #CBCBCB\n headerFg = #111111\n"
    "chevrons = yes\n home = split\n layout = classic\n";
static const char gallery_text[] =
    "dark = yes\n"
    "bg = #0E0E0E\n raised = #1A1A1A\n line = #262626\n"
    "fg = #FFFFFF\n muted = #A3A3A3\n faint = #737373\n"
    "accent = #FF6326\n accentInk = #FFFFFF\n accentFg = #141414\n lcd = #0E0E0E\n lcdDim = #8C8C8C\n"
    "select = #262626\n selectFg = #FFFFFF\n layout = gallery\n";
static const char backlight_text[] =
    "dark = no\n"
    "bg = #B7CB9C\n raised = #C6D8AD\n line = #A3B98A\n"
    "fg = #1E2919\n muted = #3C4C35\n faint = #4A5B42\n"
    "accent = #1E2919\n accentInk = #233F1E\n accentFg = #B7CB9C\n lcd = #1E2919\n"
    "select = #1E2919\n selectFg = #C6D8AD\n header = #1E2919\n headerFg = #C6D8AD\n";
static const struct {
    const char *name;
    const los_palette *p;    /* the design tokens' colours */
    bool dark;
    const char *text;        /* or a skin, over Graphite or Aluminium */
} built_in[] = {
    { "Graphite", &los_graphite, true, NULL },
    { "Gallery", NULL, true, gallery_text },
    { "Backlight", NULL, false, backlight_text },
    { "Classic", NULL, false, classic_text },
};
#define BUILT_IN ((int)(sizeof(built_in) / sizeof(built_in[0])))

static struct {
    char name[SKIN_NAME];
    char path[200];          /* "" for a built-in one */
} skins[SKINS_MAX];
static int nskins;

/* ---- the file format ---- */

static const struct {
    const char *key;
    size_t at;
} keys[] = {
#define K(f) { #f, offsetof(los_palette, f) }
    K(bg), K(surface), K(raised), K(hover), K(fg), K(muted), K(faint), K(line),
    K(accent), K(accentInk), K(accentFg), K(accentSoft), K(danger), K(warn), K(ok),
    K(lcd), K(lcdFg), K(lcdDim), K(lcdNext), K(lcdPast), K(keyTop), K(keyBottom), K(keyEdge),
#undef K
};

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* "#RRGGBB" or "RRGGBB"; false for anything else */
static bool colour(const char *v, uint32_t *out)
{
    if (*v == '#')
        v++;
    if (strlen(v) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)v[i]))
            return false;
    *out = (uint32_t)strtoul(v, NULL, 16);
    return true;
}

int skin_parse(const char *text, los_palette *p, bool *dark, bool *said_dark, los_skin_extra *extra)
{
    char line[96];
    int set = 0;
    if (said_dark)
        *said_dark = false;
    while (*text) {
        size_t n = strcspn(text, "\n");
        size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, text, k);
        line[k] = '\0';
        text += n + (text[n] == '\n');
        char *l = trim(line);
        char *eq = strpbrk(l, "=:");
        if (!*l || *l == '#' || !eq)
            continue;
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1);
        char *hash = strstr(val, " #");      /* a comment after the value */
        if (hash)
            *hash = '\0';
        val = trim(val);
        if (!strcasecmp(key, "dark")) {
            bool d = !strcasecmp(val, "yes") || !strcasecmp(val, "true") || !strcmp(val, "1");
            if (dark)
                *dark = d;
            if (said_dark)
                *said_dark = true;
            continue;
        }
        if (extra && !strcasecmp(key, "chevrons")) {
            extra->chevrons = !strcasecmp(val, "yes") || !strcasecmp(val, "true") || !strcmp(val, "1");
            continue;
        }
        if (extra && !strcasecmp(key, "home")) {
            extra->split_home = !strcasecmp(val, "split");
            continue;
        }
        if (extra && !strcasecmp(key, "layout")) {
            extra->classic = !strcasecmp(val, "classic");
            extra->gallery = !strcasecmp(val, "gallery");
            continue;
        }
        if (extra && !strcasecmp(key, "albums")) {
            extra->albums_covers = !strcasecmp(val, "covers");
            continue;
        }
        static const char *const extra_keys[] = { "select", "selectFg", "header", "headerFg",
                                                  "selectGrad", "headerGrad" };
        uint32_t ec;
        for (int e = 0; extra && e < 6; e++)
            if (!strcasecmp(key, extra_keys[e]) && colour(val, &ec)) {
                uint32_t *where[] = { &extra->select, &extra->select_fg, &extra->header,
                                      &extra->header_fg, &extra->select_grad, &extra->header_grad };
                bool *has[] = { &extra->has_select, &extra->has_select_fg, &extra->has_header,
                                &extra->has_header_fg, &extra->has_select_grad, &extra->has_header_grad };
                *where[e] = ec;
                *has[e] = true;
                set++;
            }
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            uint32_t c;
            if (!strcasecmp(key, keys[i].key) && colour(val, &c)) {
                *(uint32_t *)((char *)p + keys[i].at) = c;
                set++;
            }
        }
    }
    return set;
}

/* ---- finding them ---- */

int skins_scan(void)
{
    nskins = 0;
    for (int i = 0; i < BUILT_IN; i++) {
        snprintf(skins[nskins].name, SKIN_NAME, "%s", built_in[i].name);
        skins[nskins].path[0] = '\0';
        nskins++;
    }
    char dir[160], drive[170], entry[256];
    snprintf(dir, sizeof(dir), "%s/Themes", los_card_path());
    snprintf(drive, sizeof(drive), "A:%s", dir);
    lv_fs_dir_t d;
    if (lv_fs_dir_open(&d, drive) != LV_FS_RES_OK)
        return nskins;
    while (nskins < SKINS_MAX && lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        if (entry[0] == '/' || entry[0] == '.')
            continue;                        /* folders, hidden files (a Mac's ._ ones too) */
        const char *dot = strrchr(entry, '.');
        if (!dot || strcasecmp(dot, ".txt") || dot == entry)
            continue;
        char name[SKIN_NAME];
        size_t n = (size_t)(dot - entry) < SKIN_NAME - 1 ? (size_t)(dot - entry) : SKIN_NAME - 1;
        memcpy(name, entry, n);
        name[n] = '\0';
        size_t dl = strlen(dir), el = strlen(entry);
        if (skin_find(name) >= 0 || dl + 1 + el >= sizeof(skins[0].path))
            continue;                        /* a built-in name, twice, or too long */
        snprintf(skins[nskins].name, SKIN_NAME, "%s", name);
        char *path = skins[nskins].path;
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1, entry, el + 1);
        nskins++;
    }
    lv_fs_dir_close(&d);
    return nskins;
}

int skins_count(void)
{
    return nskins;
}

const char *skin_name(int i)
{
    return i >= 0 && i < nskins ? skins[i].name : built_in[0].name;
}

int skin_find(const char *name)
{
    for (int i = 0; i < nskins; i++)
        if (!strcasecmp(skins[i].name, name))
            return i;
    return -1;
}

/* A skin's text over Graphite, or Aluminium if it says "dark = no" */
static void from_text(const char *text, los_palette *out, bool *dark, los_skin_extra *extra)
{
    bool d = true, said = false;
    los_palette scratch = los_graphite;
    skin_parse(text, &scratch, &d, &said, NULL);
    *out = said && !d ? los_aluminium : los_graphite;
    *dark = !said || d;
    skin_parse(text, out, NULL, NULL, extra);
}

bool skin_palette(int i, los_palette *out, bool *dark, los_skin_extra *extra)
{
    memset(extra, 0, sizeof(*extra));
    *out = los_graphite;
    *dark = true;
    if (i < 0 || i >= nskins)
        return false;
    if (i < BUILT_IN) {
        if (built_in[i].text) {
            from_text(built_in[i].text, out, dark, extra);
        } else {
            *out = *built_in[i].p;
            *dark = built_in[i].dark;
        }
        return true;
    }
    static char text[2048];
    void *f = los_file_open(skins[i].path);
    if (!f)
        return false;
    size_t n = los_file_read(f, text, sizeof(text) - 1);
    los_file_close(f);
    text[n] = '\0';
    from_text(text, out, dark, extra);
    return true;
}
