/*
 * Lossless OS: the music library (folder scan).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lvgl.h"
#include "library.h"
#include "alac.h"
#include "mp4.h"
#include "opusfile.h"
#include "oggvorbis.h"
#include "mp3file.h"
#include "source.h"
#include "los_sys.h"
#include <stddef.h>

static los_album *albums;
static int nalbums;

static void plain_text(char *s);

static bool ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s), m = strlen(ext);
    if (n < m)
        return false;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != ext[i])
            return false;
    return true;
}

/* "The Party (2016)" -> title "The Party", year 2016 */
static void parse_album_dir(const char *name, los_album *a)
{
    strncpy(a->title, name, sizeof(a->title) - 1);
    size_t n = strlen(a->title);
    if (n > 7 && a->title[n - 1] == ')' && a->title[n - 6] == '(' &&
        isdigit((unsigned char)a->title[n - 5])) {
        a->year = atoi(&a->title[n - 5]);
        a->title[n - 7] = '\0';
    }
}

/* Untagged files: the title from the name. "01 The Magician.flac" -> 1,
 * "The Magician"; "Artist - Album - 01 The Magician.flac" too (the last
 * " - " part, when it starts with the track number). */
static void parse_track_file(const char *name, los_track *t)
{
    strncpy(t->file, name, sizeof(t->file) - 1);
    const char *s = name;
    for (const char *p = strstr(name, " - "); p; p = strstr(p + 1, " - ")) {
        const char *rest = p + 3;
        while (*rest == ' ' || *rest == '-')
            rest++;
        if (isdigit((unsigned char)*rest))
            s = rest;
    }
    t->number = 0;
    while (isdigit((unsigned char)*s))
        t->number = t->number * 10 + (*s++ - '0');
    while (*s == ' ' || *s == '-' || *s == '.')
        s++;
    strncpy(t->title, s, sizeof(t->title) - 1);
    char *dot = strrchr(t->title, '.');
    if (dot)
        *dot = '\0';
    plain_text(t->title);
}

/* An album's details from its tracks' tags (the first track that has them) */
typedef struct {
    char album[72], artist[72], album_artist[72];
    int year;
} album_tags;

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* Korean written letter by letter (decomposed, as macOS stores file names)
 * joined back into syllables: lead + vowel (+ tail) -> U+AC00 + ... */
static int utf8_get(const unsigned char *s, uint32_t *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && s[1]) { *cp = (uint32_t)(s[0] & 0x1f) << 6 | (s[1] & 0x3f); return 2; }
    if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) {
        *cp = (uint32_t)(s[0] & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 | (s[2] & 0x3f);
        return 3;
    }
    *cp = s[0];
    return 1;
}

static void join_hangul(char *s)
{
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        uint32_t l, v, t;
        int nl = utf8_get(r, &l);
        if (l >= 0x1100 && l <= 0x1112 && r[nl]) {
            int nv = utf8_get(r + nl, &v);
            if (v >= 0x1161 && v <= 0x1175) {
                uint32_t syl = 0xAC00 + ((l - 0x1100) * 21 + (v - 0x1161)) * 28;
                int used = nl + nv;
                if (r[used]) {
                    int nt = utf8_get(r + used, &t);
                    if (t >= 0x11A8 && t <= 0x11C2) {
                        syl += t - 0x11A7;
                        used += nt;
                    }
                }
                *w++ = (unsigned char)(0xe0 | syl >> 12);
                *w++ = (unsigned char)(0x80 | ((syl >> 6) & 0x3f));
                *w++ = (unsigned char)(0x80 | (syl & 0x3f));
                r += used;
                continue;
            }
        }
        for (int i = 0; i < nl; i++)
            *w++ = *r++;
    }
    *w = '\0';
}

/* Tags sometimes use typographic characters the fonts don't have:
 * U+2010/2011 hyphens and the no-break space become plain ones */
static void plain_text(char *s)
{
    char *w = s;
    for (const unsigned char *r = (const unsigned char *)s; *r;) {
        if (r[0] == 0xe2 && r[1] == 0x80 && (r[2] == 0x90 || r[2] == 0x91)) {
            *w++ = '-';
            r += 3;
        } else if (r[0] == 0xc2 && r[1] == 0xa0) {
            *w++ = ' ';
            r += 2;
        } else {
            *w++ = (char)*r++;
        }
    }
    *w = '\0';
    join_hangul(s);
}

static void copy_tag(char *dst, size_t n, const char *value)
{
    if (!dst[0])
        snprintf(dst, n, "%s", value);
}

/* One "KEY=value" from a FLAC's Vorbis comments */
static void take_tag(const char *c, los_track *t, album_tags *tags)
{
    const char *eq = strchr(c, '=');
    if (!eq || !eq[1])
        return;
    size_t k = (size_t)(eq - c);
    const char *v = eq + 1;
    if (k == 5 && !strncasecmp(c, "TITLE", 5)) {
        snprintf(t->title, sizeof(t->title), "%s", v);
    } else if (k == 11 && !strncasecmp(c, "TRACKNUMBER", 11)) {
        if (atoi(v) > 0)
            t->number = atoi(v);              /* "3" or "3/11" */
    } else if (k == 5 && !strncasecmp(c, "ALBUM", 5)) {
        copy_tag(tags->album, sizeof(tags->album), v);
    } else if (k == 6 && !strncasecmp(c, "ARTIST", 6)) {
        copy_tag(tags->artist, sizeof(tags->artist), v);
    } else if (k == 11 && !strncasecmp(c, "ALBUMARTIST", 11)) {
        copy_tag(tags->album_artist, sizeof(tags->album_artist), v);
    } else if (k == 4 && !strncasecmp(c, "DATE", 4) && !tags->year) {
        tags->year = atoi(v);                 /* "2016" or "2016-05-20" */
    }
}

/* A FLAC's header: STREAMINFO (rate, bits, length) and the Vorbis comments
 * (title, track number, album, artist, year). Tags win over the file name. */
static void read_flac(const char *path, los_track *t, album_tags *tags)
{
    lv_fs_file_t f;
    uint8_t h[42];
    uint32_t got = 0;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return;
    if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4 || memcmp(h, "fLaC", 4))
        goto done;
    for (int block = 0; block < 32; block++) {
        uint32_t start;
        if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4)
            break;
        bool last = h[0] & 0x80;
        uint32_t type = h[0] & 0x7f, len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        lv_fs_tell(&f, &start);
        if (type == 0 && len >= 18 && lv_fs_read(&f, h, 18, &got) == LV_FS_RES_OK && got == 18) {
            const uint8_t *si = h;
            uint32_t rate = ((uint32_t)si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            int bits = (((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1;
            uint64_t samples = ((uint64_t)(si[13] & 0x0f) << 32) |
                               ((uint32_t)si[14] << 24) | ((uint32_t)si[15] << 16) |
                               ((uint32_t)si[16] << 8) | si[17];
            t->rate = (int)rate;
            t->bits = bits;
            t->seconds = rate ? (int)(samples / rate) : 0;
        } else if (type == 4 && lv_fs_read(&f, h, 4, &got) == LV_FS_RES_OK && got == 4) {
            /* vendor string, then a count of "KEY=value" comments,
             * all lengths little-endian */
            uint32_t pos = start + 4 + le32(h);
            lv_fs_seek(&f, pos, LV_FS_SEEK_SET);
            if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4)
                break;
            uint32_t count = le32(h);
            pos += 4;
            char c[256];
            for (uint32_t i = 0; i < count && i < 64 && pos + 4 <= start + len; i++) {
                if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4)
                    break;
                uint32_t n = le32(h);
                pos += 4;
                if (n < sizeof(c)) {
                    if (lv_fs_read(&f, c, n, &got) != LV_FS_RES_OK || got != n)
                        break;
                    c[n] = '\0';
                    plain_text(c);
                    take_tag(c, t, tags);
                } else {
                    lv_fs_seek(&f, pos + n, LV_FS_SEEK_SET);   /* lyrics and the like */
                }
                pos += n;
            }
        }
        if (last)
            break;
        lv_fs_seek(&f, start + len, LV_FS_SEEK_SET);
    }
done:
    lv_fs_close(&f);
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* The picture embedded in a FLAC file's metadata (a PICTURE block, the
 * front cover if there are several): where its image data starts in the
 * file, or 0 if there is none. Only JPEGs, which covers.c decodes. */
static uint32_t find_flac_picture(const char *path)
{
    lv_fs_file_t f;
    uint8_t h[32];
    uint32_t got, found = 0;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return 0;
    if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4 || memcmp(h, "fLaC", 4))
        goto done;
    for (int block = 0; block < 32; block++) {
        uint32_t start;
        if (lv_fs_read(&f, h, 4, &got) != LV_FS_RES_OK || got != 4)
            break;
        bool last = h[0] & 0x80;
        uint32_t type = h[0] & 0x7f, len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        lv_fs_tell(&f, &start);
        if (type == 6 && lv_fs_read(&f, h, 8, &got) == LV_FS_RES_OK && got == 8) {
            uint32_t kind = be32(h), mime_len = be32(h + 4), pos = start + 8;
            char mime[24] = "";
            if (mime_len < sizeof(mime))
                lv_fs_read(&f, mime, mime_len, &got);
            pos += mime_len;
            lv_fs_seek(&f, pos, LV_FS_SEEK_SET);
            lv_fs_read(&f, h, 4, &got);                   /* description length */
            pos += 4 + be32(h);
            pos += 16;                                     /* width, height, depth, colours */
            lv_fs_seek(&f, pos, LV_FS_SEEK_SET);
            lv_fs_read(&f, h, 4, &got);                   /* image length */
            pos += 4;
            if (!strcmp(mime, "image/jpeg") || !strcmp(mime, "image/jpg")) {
                if (!found || kind == 3)
                    found = pos;
                if (kind == 3)
                    break;
            }
        }
        if (last)
            break;
        lv_fs_seek(&f, start + len, LV_FS_SEEK_SET);
    }
done:
    lv_fs_close(&f);
    return found;
}

/* WAV and AIFF: rate, depth and length from the header (no tags yet) */
static void read_pcm_info(const char *path, los_track *t)
{
    static los_source s;
    /* WAV and AIFF only: anything else would use the audio thread's
     * decoder memory */
    char head[4] = "";
    void *f = los_file_open(path);
    if (!f)
        return;
    size_t got = los_file_read(f, head, 4);
    los_file_close(f);
    if (got != 4 || (memcmp(head, "RIFF", 4) && memcmp(head, "FORM", 4)))
        return;
    if (source_open(&s, path) == 0) {
        t->rate = (int)s.rate;
        t->bits = (int)s.bits;
        t->seconds = s.rate ? (int)(s.frames / s.rate) : 0;
        source_close(&s);
    }
}

/* M4A: true if it's Apple Lossless (AAC isn't played), with its tags.
 * The cover's JPEG offset into *cover if asked and there is one. */
static bool read_m4a(const char *path, los_track *t, album_tags *tags, uint32_t *cover)
{
    static mp4_info m;
    static alac_config c;
    void *f = los_file_open(path);
    if (!f)
        return false;
    bool ok = mp4_open(f, &m) == 0 && alac_config_read(&c, m.cookie, sizeof(m.cookie)) == 0;
    los_file_close(f);
    if (!ok)
        return false;
    if (cover) {
        *cover = m.cover < UINT32_MAX ? (uint32_t)m.cover : 0;
        return true;
    }
    t->rate = (int)c.rate;
    t->bits = c.bits;
    t->seconds = (int)(m.frames / c.rate);
    if (m.title[0]) {
        plain_text(m.title);
        snprintf(t->title, sizeof(t->title), "%s", m.title);
    }
    if (m.track > 0)
        t->number = m.track;
    plain_text(m.album);
    plain_text(m.artist);
    plain_text(m.album_artist);
    copy_tag(tags->album, sizeof(tags->album), m.album);
    copy_tag(tags->artist, sizeof(tags->artist), m.artist);
    copy_tag(tags->album_artist, sizeof(tags->album_artist), m.album_artist);
    if (!tags->year && m.year > 0)
        tags->year = m.year;
    return true;
}

/* Opus: length and its Vorbis-style tags */
struct tag_ctx { los_track *t; album_tags *tags; };

static void opus_tag(const char *comment, void *ctx)
{
    struct tag_ctx *c = ctx;
    char v[256];
    snprintf(v, sizeof(v), "%s", comment);
    plain_text(v);
    take_tag(v, c->t, c->tags);
}

static bool read_opus(const char *path, los_track *t, album_tags *tags)
{
    unsigned rate, channels;
    uint64_t frames;
    struct tag_ctx c = { t, tags };
    if (!opusfile_info(path, &rate, &channels, &frames, opus_tag, &c) &&
        !oggvorbis_info(path, &rate, &channels, &frames, opus_tag, &c))
        return false;
    t->rate = (int)rate;
    t->bits = 16;
    t->seconds = (int)(frames / rate);
    return true;
}

/* MP3: length, ID3 tags; or only where its cover starts (cover not NULL) */
static bool read_mp3(const char *path, los_track *t, album_tags *tags, uint32_t *cover)
{
    unsigned rate, channels;
    uint64_t frames;
    struct tag_ctx c = { t, tags };
    if (!mp3file_info(path, &rate, &channels, &frames, cover ? NULL : opus_tag, &c, cover))
        return false;
    if (t) {
        t->rate = (int)rate;
        t->bits = 16;
        t->seconds = (int)(frames / rate);
    }
    return true;
}

static int cmp_track(const void *a, const void *b)
{
    const los_track *x = a, *y = b;
    if (x->disc != y->disc)
        return x->disc - y->disc;
    if (x->number != y->number)
        return x->number - y->number;
    return strcmp(x->title, y->title);
}

static int cmp_album(const void *a, const void *b)
{
    const los_album *x = a, *y = b;
    return strcasecmp(x->title, y->title);
}

/* ---- the cache: what the last scan found, in /.lossless/state/library.bin ----
 *
 * Each album is kept with a fingerprint of its folder's file names; when
 * the folder still has the same names, the album comes from the cache and
 * none of its files are opened. Laid out as this build's structs: a cache
 * from another build or machine just doesn't match (magic and sizes). */

#define CACHE_MAGIC 0x4c494232u            /* "LIB2": ALAC read since */
#define ALBUM_HEAD offsetof(los_album, tracks)

static uint8_t *cache;                     /* the old cache, during a scan */
static size_t cache_len;
static bool rescanned;                     /* something was read from files */
static bool ignore_cache;

static void cache_path(char *p, size_t n)
{
    snprintf(p, n, "%s/.lossless/state/library.bin", los_card_path());
}

static void cache_load(void)
{
    char path[200];
    cache_path(path, sizeof(path));
    cache = NULL;
    cache_len = 0;
    void *f = los_file_open(path);
    if (!f)
        return;
    uint64_t size = los_file_size(f);
    if (size >= 12 && size < 8u * 1024 * 1024 && (cache = lv_malloc((size_t)size))) {
        cache_len = los_file_read(f, cache, (size_t)size);
        uint32_t magic, head, track;
        memcpy(&magic, cache, 4);
        memcpy(&head, cache + 4, 4);
        memcpy(&track, cache + 8, 4);
        if (cache_len != size || magic != CACHE_MAGIC || head != ALBUM_HEAD || track != sizeof(los_track)) {
            lv_free(cache);
            cache = NULL;
            cache_len = 0;
        }
    }
    los_file_close(f);
}

/* The cached album for this folder and fingerprint, or NULL */
static bool cache_find(const char *dir, uint32_t sig, los_album *out)
{
    size_t at = 12;
    while (cache && at + ALBUM_HEAD <= cache_len) {
        uint32_t s;
        int n;
        memcpy(&s, cache + at + offsetof(los_album, sig), sizeof(s));
        memcpy(&n, cache + at + offsetof(los_album, ntracks), sizeof(n));
        size_t len = ALBUM_HEAD + (size_t)n * sizeof(los_track);
        if (n < 0 || n > LIB_MAX_TRACKS || at + len > cache_len)
            return false;
        if (s == sig && !strncmp((const char *)(cache + at + offsetof(los_album, dir)), dir,
                                 sizeof(out->dir))) {
            memcpy(out, cache + at, len);
            return true;
        }
        at += len;
    }
    return false;
}

static void cache_save(void)
{
    char path[200], dir[200];
    if (!los_writes_ok())
        return;
    size_t total = 12;
    for (int i = 0; i < nalbums; i++)
        total += ALBUM_HEAD + (size_t)albums[i].ntracks * sizeof(los_track);
    uint8_t *buf = lv_malloc(total);
    if (!buf)
        return;
    uint32_t hdr[3] = { CACHE_MAGIC, ALBUM_HEAD, sizeof(los_track) };
    memcpy(buf, hdr, 12);
    size_t at = 12;
    for (int i = 0; i < nalbums; i++) {
        size_t len = ALBUM_HEAD + (size_t)albums[i].ntracks * sizeof(los_track);
        memcpy(buf + at, &albums[i], len);
        at += len;
    }
    snprintf(dir, sizeof(dir), "%s/.lossless", los_card_path());
    los_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/.lossless/state", los_card_path());
    los_mkdir(dir);
    cache_path(path, sizeof(path));
    los_file_save(path, buf, total);
    lv_free(buf);
}

/* A fingerprint of a folder's file names (FNV-1a) */
/* "CD1", "Disc 2", "disk 3": a disc of the album in the folder above; its number, else 0 */
int library_disc_folder(const char *name)
{
    const char *p = name;
    if (!strncasecmp(p, "cd", 2))
        p += 2;
    else if (!strncasecmp(p, "disc", 4) || !strncasecmp(p, "disk", 4))
        p += 4;
    else
        return 0;
    while (*p == ' ' || *p == '-' || *p == '_')
        p++;
    if (!isdigit((unsigned char)*p))
        return 0;
    int n = atoi(p);
    while (isdigit((unsigned char)*p))
        p++;
    return *p == '\0' && n > 0 ? n : 0;
}

static uint32_t folder_sig(const char *path)
{
    lv_fs_dir_t d;
    char entry[160];
    uint32_t h = 2166136261u;
    if (lv_fs_dir_open(&d, path) != LV_FS_RES_OK)
        return 0;
    while (lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        for (const char *c = entry; *c; c++)
            h = (h ^ (uint8_t)*c) * 16777619u;
        h = (h ^ '\n') * 16777619u;
        if (entry[0] == '/' && library_disc_folder(entry + 1)) {
            /* a disc's songs belong to this album too */
            char sub[420];
            snprintf(sub, sizeof(sub), "%s%s", path, entry);
            h = (h ^ folder_sig(sub)) * 16777619u;
        }
    }
    lv_fs_dir_close(&d);
    return h;
}

static void scan_album_files(const char *artist, const char *path, const char *name);

static void scan_album(const char *artist, const char *path, const char *name)
{
    if (nalbums >= LIB_MAX_ALBUMS)
        return;
    uint32_t sig = folder_sig(path);
    if (!ignore_cache && cache_find(path, sig, &albums[nalbums])) {
        nalbums++;
        return;
    }
    int before = nalbums;
    scan_album_files(artist, path, name);
    if (nalbums > before) {
        albums[before].sig = sig;
        rescanned = true;           /* read from the files: the cache changes */
    }
}

/* Folders that are never music: the player's own, other systems', the
 * computer's */
static bool skip_folder(const char *name)
{
    static const char *const skip[] = { "System Volume Information", "LOST.DIR", "$RECYCLE.BIN",
                                        "RECYCLER", "Android", "Playlists" };
    if (name[0] == '.')
        return true;                            /* .lossless, .Trashes, the bootloader's... */
    for (unsigned i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcasecmp(name, skip[i]))
            return true;
    return false;
}

/* Folder names that hold music but aren't an artist ("Music/Album") */
static const char *artist_hint(const char *name)
{
    /* a year ("2020", "1978") or a disc */
    size_t len = strlen(name);
    if (len == 4 && isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) &&
        isdigit((unsigned char)name[2]) && isdigit((unsigned char)name[3]))
        return "";
    static const char *const generic[] = { "Music", "My Music", "Musik", "Musique", "Musica",
                                           "Audio", "Songs", "Albums", "Artists", "MP3", "FLAC",
                                           "Lossless", "Hi-Res", "HiRes" };
    for (unsigned i = 0; i < sizeof(generic) / sizeof(generic[0]); i++)
        if (!strcasecmp(name, generic[i]))
            return "";
    return name;
}

/* Every folder on the card that has songs is an album, however the folders
 * are laid out (Music/Artist/Album, Album alone, Artist/Year/Album...): the
 * player doesn't ask for a layout. The folder's name is the album's (tags
 * win), the folder above is the artist's; a disc folder ("CD1") takes both
 * from the folders above it. */
#define SCAN_DEPTH 6
static void walk(const char *path, int depth, const char *name, const char *up[SCAN_DEPTH])
{
    if (depth > 0 && library_disc_folder(name))
        return;                                 /* part of the album above */
    if (depth > 0) {
        /* the artist: the nearest folder above that isn't a year or "Music" */
        const char *artist = "";
        for (int k = depth - 2; k >= 0 && !artist[0]; k--)
            artist = artist_hint(up[k]);
        scan_album(artist, path, name);
    }
    if (depth >= SCAN_DEPTH)
        return;
    lv_fs_dir_t d;
    char entry[160];
    if (lv_fs_dir_open(&d, path) != LV_FS_RES_OK)
        return;
    /* each level's folder stays open on the way down (the file system
     * allows enough: boards/hifiwalker_h2.conf) */
    while (lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        if (entry[0] != '/' || skip_folder(entry + 1))
            continue;                               /* files; not music folders */
        char sub[420];
        snprintf(sub, sizeof(sub), "%s%s", path, entry);
        if (depth > 0)
            up[depth - 1] = name;
        walk(sub, depth + 1, entry + 1, up);
    }
    lv_fs_dir_close(&d);
}

/* An album folder's songs and cover into 'a'; its disc folders' too
 * (disc 0 is the album folder itself; 'prefix' is the disc folder's name
 * and a slash) */
static void add_folder(los_album *a, const char *path, const char *prefix, int disc,
                       album_tags *tags, bool *have_file)
{
    lv_fs_dir_t d;
    char entry[160];
    if (lv_fs_dir_open(&d, path) != LV_FS_RES_OK)
        return;
    while (lv_fs_dir_read(&d, entry, sizeof(entry)) == LV_FS_RES_OK && entry[0]) {
        char full[420];
        if (entry[0] == '/') {
            int n = disc ? 0 : library_disc_folder(entry + 1);
            if (n) {
                char sub[64];
                snprintf(full, sizeof(full), "%s%s", path, entry);
                snprintf(sub, sizeof(sub), "%.62s/", entry + 1);   /* "CD1", "Disc 2": short */
                add_folder(a, full, sub, n, tags, have_file);
            }
            continue;
        }
        if (entry[0] == '.')
            continue;
        snprintf(full, sizeof(full), "%s/%s", path, entry);
        bool pcm = ends_with(entry, ".wav") || ends_with(entry, ".aif") ||
                   ends_with(entry, ".aiff") || ends_with(entry, ".aifc");
        if (pcm || ends_with(entry, ".flac") || ends_with(entry, ".mp3") ||
            ends_with(entry, ".m4a") || ends_with(entry, ".opus") ||
            ends_with(entry, ".ogg") || ends_with(entry, ".oga")) {
            if (a->ntracks < LIB_MAX_TRACKS) {
                los_track *t = &a->tracks[a->ntracks++];
                memset(t, 0, sizeof(*t));
                parse_track_file(entry, t);
                snprintf(t->file, sizeof(t->file), "%s%s", prefix, entry);
                t->disc = disc;
                if (ends_with(entry, ".flac"))
                    read_flac(full, t, tags);
                else if (pcm)
                    read_pcm_info(full + 2, t);   /* no drive letter: los_file */
                else if (ends_with(entry, ".m4a") && !read_m4a(full + 2, t, tags, NULL))
                    a->ntracks--;                 /* AAC: not played */
                else if (ends_with(entry, ".mp3") && !read_mp3(full + 2, t, tags, NULL))
                    a->ntracks--;
                else if ((ends_with(entry, ".opus") || ends_with(entry, ".ogg") ||
                          ends_with(entry, ".oga")) && !read_opus(full + 2, t, tags))
                    a->ntracks--;
            }
        } else if (!*have_file && (!strcasecmp(entry, "cover.jpg") || !strcasecmp(entry, "folder.jpg"))) {
            /* without the drive letter: widgets add it */
            size_t n = strlen(full + 2);       /* a path too long for it: no cover */
            if (n < sizeof(a->cover)) {
                memcpy(a->cover, full + 2, n + 1);
                a->cover_offset = 0;
            }
            *have_file = true;
        }
    }
    lv_fs_dir_close(&d);
}

static void scan_album_files(const char *artist, const char *path, const char *name)
{
    los_album *a = &albums[nalbums];
    memset(a, 0, sizeof(*a));
    strncpy(a->artist, artist, sizeof(a->artist) - 1);
    parse_album_dir(name, a);
    snprintf(a->dir, sizeof(a->dir), "%s", path);

    bool have_file = false;
    album_tags tags;
    memset(&tags, 0, sizeof(tags));
    add_folder(a, path, "", 0, &tags, &have_file);
    /* The album's tags over its folder names */
    if (tags.album[0])
        snprintf(a->title, sizeof(a->title), "%s", tags.album);
    if (tags.album_artist[0] || tags.artist[0])
        snprintf(a->artist, sizeof(a->artist), "%s",
                 tags.album_artist[0] ? tags.album_artist : tags.artist);
    if (tags.year > 0)
        a->year = tags.year;
    plain_text(a->title);     /* folder names, when there were no tags */
    plain_text(a->artist);
    /* No cover file: the picture inside the first track, if any */
    if (!have_file && a->ntracks) {
        char full[420];
        const los_track *first = &a->tracks[0];
        for (int i = 1; i < a->ntracks; i++)
            if (a->tracks[i].number && a->tracks[i].number < first->number)
                first = &a->tracks[i];
        snprintf(full, sizeof(full), "%s/%s", path, first->file);
        uint32_t at = 0;
        if (ends_with(first->file, ".flac"))
            at = find_flac_picture(full);
        else if (ends_with(first->file, ".m4a"))
            read_m4a(full + 2, NULL, NULL, &at);
        else if (ends_with(first->file, ".mp3"))
            read_mp3(full + 2, NULL, NULL, &at);
        if (at && strlen(full + 2) < sizeof(a->cover)) {
            memcpy(a->cover, full + 2, strlen(full + 2) + 1);
            a->cover_offset = at;
        }
    }
    if (a->ntracks) {
        qsort(a->tracks, a->ntracks, sizeof(los_track), cmp_track);
        nalbums++;
    }
}

int library_scan(const char *root)
{
    if (!albums)
        albums = lv_malloc_zeroed(sizeof(los_album) * LIB_MAX_ALBUMS);
    nalbums = 0;
    rescanned = false;
    cache_load();
    char rootdir[256];
    snprintf(rootdir, sizeof(rootdir), "A:%s", root);

    /* songs loose at the top: an album of their own */
    scan_album("", rootdir, "Songs");
    const char *up[SCAN_DEPTH] = { 0 };
    walk(rootdir, 0, "", up);
    qsort(albums, nalbums, sizeof(los_album), cmp_album);
    /* save what changed (an album gone also changes the count) */
    int cached = 0;
    for (size_t at = 12; cache && at + ALBUM_HEAD <= cache_len; cached++) {
        int n;
        memcpy(&n, cache + at + offsetof(los_album, ntracks), sizeof(n));
        if (n < 0 || n > LIB_MAX_TRACKS)
            break;
        at += ALBUM_HEAD + (size_t)n * sizeof(los_track);
    }
    if (rescanned || cached != nalbums)
        cache_save();
    if (cache)
        lv_free(cache);
    cache = NULL;
    ignore_cache = false;
    return nalbums;
}

int library_rescan(const char *root)
{
    ignore_cache = true;
    return library_scan(root);
}

int library_albums(void) { return nalbums; }
const los_album *library_album(int i) { return &albums[i]; }

int library_artists(void)
{
    int n = 0;
    for (int i = 0; i < nalbums; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++)
            if (!strcmp(albums[i].artist, albums[j].artist))
                seen = true;
        n += !seen;
    }
    return n;
}

int library_tracks(void)
{
    int n = 0;
    for (int i = 0; i < nalbums; i++)
        n += albums[i].ntracks;
    return n;
}

void library_format(const los_album *a, char *buf, int len)
{
    buf[0] = '\0';
    if (a->ntracks)
        library_track_format(&a->tracks[0], buf, len);
}

void library_track_format(const los_track *t, char *buf, int len)
{
    const char *codec = strrchr(t->file, '.');
    char c[8] = "";
    for (int i = 0; codec && codec[i + 1] && i < 6; i++)
        c[i] = (char)toupper((unsigned char)codec[i + 1]);
    if (!strcmp(c, "M4A"))
        strcpy(c, "ALAC");                       /* only lossless M4As are listed */
    /* lossy formats have no bit depth of their own: just the rate */
    if (!strcmp(c, "OPUS") || !strcmp(c, "MP3") || !strcmp(c, "OGG")) {
        snprintf(buf, len, t->rate ? "%s %d" : "%s", c, t->rate / 1000);
        return;
    }
    /* "16/44.1": whole numbers only (players' printf has no floating point) */
    if (t->rate && t->rate % 1000 >= 100)
        snprintf(buf, len, "%s %d/%d.%d", c, t->bits, t->rate / 1000, t->rate % 1000 / 100);
    else if (t->rate)
        snprintf(buf, len, "%s %d/%d", c, t->bits, t->rate / 1000);
    else
        snprintf(buf, len, "%s", c);
}
