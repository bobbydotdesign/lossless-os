/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lvgl.h"
#include "playlists.h"
#include "library.h"
#include "los_sys.h"

static los_playlist lists[PLAYLISTS_MAX];
static int nlists;

/* The last 'n' parts of a path ("Album/01 Song.flac" for 2), or the path */
static const char *last_parts(const char *p, int n)
{
    const char *end = p + strlen(p);
    for (const char *c = end - 1; c >= p; c--)
        if (*c == '/' && --n == 0)
            return c + 1;
    return p;
}

/* The library's song for one playlist line, or -1 */
static int match(const char *line, int *track)
{
    char e[512];
    size_t n = 0;
    for (const char *c = line; *c && n < sizeof(e) - 1; c++)
        e[n++] = *c == '\\' ? '/' : *c;
    e[n] = '\0';
    const char *file = last_parts(e, 1);
    if (file == e)
        return -1;
    /* in a disc folder ("Album/CD1/01 x.flac"): the album is the folder above */
    char *slash = (char *)file - 1;
    *slash = '\0';
    const char *parent = last_parts(e, 1);
    bool disc = parent != e && library_disc_folder(parent);
    *slash = '/';
    if (disc) {
        /* drop the disc folder from the path: "Album/01 x.flac" */
        size_t pl = strlen(parent) + 1;
        memmove((char *)parent, file, strlen(file) + 1);
        file = parent;
        (void)pl;
    }
    /* the album's folder: the part before the file name */
    char album[200];
    const char *af = last_parts(e, 2);
    size_t alen = (size_t)(file - af - 1);
    if (alen == 0 || alen >= sizeof(album))
        return -1;
    memcpy(album, af, alen);
    album[alen] = '\0';
    char artist[200] = "";
    const char *rf = last_parts(e, 3);
    if (rf != af && (size_t)(af - rf - 1) < sizeof(artist)) {
        memcpy(artist, rf, (size_t)(af - rf - 1));
        artist[af - rf - 1] = '\0';
    }
    int found = -1;
    for (int i = 0; i < library_albums(); i++) {
        const los_album *a = library_album(i);
        const char *ad = last_parts(a->dir, 1), *ar = last_parts(a->dir, 2);
        if (strcasecmp(ad, album))
            continue;
        for (int t = 0; t < a->ntracks; t++) {
            if (strcasecmp(last_parts(a->tracks[t].file, 1), file))
                continue;
            /* the artist folder settles it when there are several */
            bool artist_ok = artist[0] && !strncasecmp(ar, artist, strlen(artist)) &&
                             ar[strlen(artist)] == '/';
            if (found < 0 || artist_ok) {
                found = i;
                *track = t;
            }
            if (artist_ok)
                return found;
        }
    }
    return found;
}

/* Calls fn for each song line of the file */
static int each_line(const char *path, void (*fn)(int album, int track, void *ctx), void *ctx)
{
    void *f = los_file_open(path);
    if (!f)
        return 0;
    char buf[1024], line[512];
    size_t len = 0, got;
    int songs = 0;
    bool done = false;
    while (!done) {
        got = los_file_read(f, buf, sizeof(buf));
        if (!got) {
            done = true;
            buf[0] = '\n';
            got = 1;
        }
        for (size_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c != '\n' && c != '\r') {
                if (len < sizeof(line) - 1)
                    line[len++] = c;
                continue;
            }
            line[len] = '\0';
            len = 0;
            /* skip the UTF-8 byte order mark, comments, blank lines */
            char *s = line;
            if ((unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf)
                s += 3;
            while (*s == ' ' || *s == '\t')
                s++;
            if (!*s || *s == '#')
                continue;
            int track, album = match(s, &track);
            if (album >= 0) {
                songs++;
                if (fn)
                    fn(album, track, ctx);
            }
        }
    }
    los_file_close(f);
    return songs;
}

static int compare(const void *a, const void *b)
{
    return strcasecmp(((const los_playlist *)a)->name, ((const los_playlist *)b)->name);
}

/* The playlists in one folder ('sub' under the card, "" for its top) */
static void scan_folder(const char *sub)
{
    char dir[200], drive[210], entry[256];
    snprintf(dir, sizeof(dir), "%s%s%s", los_card_path(), sub[0] ? "/" : "", sub);
    snprintf(drive, sizeof(drive), "A:%s", dir);
    lv_fs_dir_t d;
    if (lv_fs_dir_open(&d, drive) != LV_FS_RES_OK)
        return;
    while (nlists < PLAYLISTS_MAX && lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        if (entry[0] == '/' || entry[0] == '.')
            continue;                        /* folders, hidden files */
        const char *dot = strrchr(entry, '.');
        if (!dot || (strcasecmp(dot, ".m3u") && strcasecmp(dot, ".m3u8")))
            continue;
        los_playlist *p = &lists[nlists];
        size_t n = (size_t)(dot - entry) < sizeof(p->name) - 1 ? (size_t)(dot - entry) : sizeof(p->name) - 1;
        memcpy(p->name, entry, n);
        p->name[n] = '\0';
        size_t dl = strlen(dir), el = strlen(entry);
        if (dl + 1 + el >= sizeof(p->path))
            continue;                          /* a path too long to keep */
        memcpy(p->path, dir, dl);
        p->path[dl] = '/';
        memcpy(p->path + dl + 1, entry, el + 1);
        p->songs = each_line(p->path, NULL, NULL);
        nlists++;
    }
    lv_fs_dir_close(&d);
}

int playlists_scan(void)
{
    nlists = 0;
    scan_folder("Playlists");
    scan_folder("");
    qsort(lists, (size_t)nlists, sizeof(lists[0]), compare);
    return nlists;
}

int playlists_count(void) { return nlists; }

const los_playlist *playlist_get(int i)
{
    return i >= 0 && i < nlists ? &lists[i] : NULL;
}

int playlist_find(const char *path)
{
    for (int i = 0; i < nlists; i++)
        if (!strcmp(lists[i].path, path))
            return i;
    return -1;
}

struct load_ctx {
    int *album, *track, n, max;
};

static void add(int album, int track, void *ctx)
{
    struct load_ctx *c = ctx;
    if (c->n < c->max) {
        c->album[c->n] = album;
        c->track[c->n] = track;
        c->n++;
    }
}

int playlist_load(int i, int *album, int *track, int max)
{
    if (i < 0 || i >= nlists)
        return 0;
    struct load_ctx c = { album, track, 0, max };
    each_line(lists[i].path, add, &c);
    return c.n;
}
