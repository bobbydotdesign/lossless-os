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
    const char *x = ((const los_playlist *)a)->name, *y = ((const los_playlist *)b)->name;
    bool fx = !strcasecmp(x, FAVOURITES), fy = !strcasecmp(y, FAVOURITES);
    if (fx != fy)
        return fx ? -1 : 1;                  /* Favourites first */
    return library_name_cmp(x, y);
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

/* ---- made and changed on the player (playlists.h) ---- */

static void playlists_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/Playlists", los_card_path());
}

bool playlist_editable(int i)
{
    char dir[220];
    playlists_dir(dir, sizeof(dir));
    size_t n = strlen(dir);
    return i >= 0 && i < nlists && !strncmp(lists[i].path, dir, n) && lists[i].path[n] == '/';
}

/* Makes /Playlists/<name>.m3u8 if it isn't there; its index after a rescan, or -1 */
static int make(const char *name)
{
    char dir[220], path[300];
    playlists_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.m3u8", dir, name);
    int i = playlist_find(path);
    if (i >= 0)
        return i;
    if (!los_writes_ok() || !los_mkdir(dir))
        return -1;
    static const char head[] = "#EXTM3U\n";
    if (!los_file_save(path, head, sizeof(head) - 1))
        return -1;
    playlists_scan();
    return playlist_find(path);
}

int playlist_favourites(bool make_it)
{
    for (int i = 0; i < nlists; i++)
        if (!strcasecmp(lists[i].name, FAVOURITES) && playlist_editable(i))
            return i;
    return make_it ? make(FAVOURITES) : -1;
}

int playlist_new(void)
{
    char name[32];
    for (int k = 1; k < 1000; k++) {
        snprintf(name, sizeof(name), "Playlist %d", k);
        bool taken = false;
        for (int i = 0; i < nlists; i++)
            taken |= !strcasecmp(lists[i].name, name);
        if (!taken)
            return make(name);
    }
    return -1;
}

/* The song's path from /Playlists: "../Artist/Album/01 x.flac" */
static bool line_for(int album, int track, char *out, size_t n)
{
    const los_album *a = library_album(album);
    if (!a || track < 0 || track >= a->ntracks)
        return false;
    const char *d = a->dir + 2, *card = los_card_path();      /* the drive letter off */
    size_t cl = strlen(card);
    if (!strncmp(d, card, cl))
        d += cl;
    while (*d == '/')
        d++;
    return snprintf(out, n, "../%s/%s\n", d, a->tracks[track].file) < (int)n;
}

bool playlist_add(int i, int album, int track)
{
    char line[600];
    if (!playlist_editable(i) || !line_for(album, track, line, sizeof(line)))
        return false;
    /* a file that doesn't end in a new line gets one first */
    void *f = los_file_open(lists[i].path);
    if (f) {
        uint64_t size = los_file_size(f);
        char last = '\n';
        if (size && los_file_seek(f, size - 1))
            los_file_read(f, &last, 1);
        los_file_close(f);
        if (last != '\n' && !los_file_append(lists[i].path, "\n", 1))
            return false;
    }
    if (!los_file_append(lists[i].path, line, strlen(line)))
        return false;
    lists[i].songs++;
    return true;
}

struct has_ctx {
    int album, track;
    bool found;
};

static void has_one(int album, int track, void *ctx)
{
    struct has_ctx *c = ctx;
    c->found |= album == c->album && track == c->track;
}

bool playlist_has(int i, int album, int track)
{
    if (i < 0 || i >= nlists)
        return false;
    struct has_ctx c = { album, track, false };
    each_line(lists[i].path, has_one, &c);
    return c.found;
}

/* The whole file, rewritten without the song's first line */
static char edit_in[64 * 1024], edit_out[64 * 1024];

bool playlist_remove(int i, int album, int track)
{
    if (!playlist_editable(i))
        return false;
    void *f = los_file_open(lists[i].path);
    if (!f)
        return false;
    uint64_t size = los_file_size(f);
    size_t got = size < sizeof(edit_in) ? los_file_read(f, edit_in, (size_t)size) : 0;
    los_file_close(f);
    if (!got || got != size)
        return false;                        /* empty, or too big to change here */
    size_t out = 0;
    bool gone = false;
    for (size_t at = 0; at < got;) {
        size_t end = at;
        while (end < got && edit_in[end] != '\n')
            end++;
        size_t len = end - at, next = end < got ? end + 1 : end;
        bool drop = false;
        if (!gone && len) {
            char line[512];
            size_t l = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
            memcpy(line, edit_in + at, l);
            line[l] = '\0';
            if (l && line[l - 1] == '\r')
                line[--l] = '\0';
            int t, a = line[0] && line[0] != '#' ? match(line, &t) : -1;
            drop = a == album && t == track;
        }
        if (drop)
            gone = true;
        else {
            memcpy(edit_out + out, edit_in + at, next - at);
            out += next - at;
        }
        at = next;
    }
    if (!gone || !los_file_save(lists[i].path, edit_out, out))
        return false;
    lists[i].songs--;
    return true;
}

bool playlist_delete(int i)
{
    if (!playlist_editable(i) || !los_file_remove(lists[i].path))
        return false;
    playlists_scan();
    return true;
}
