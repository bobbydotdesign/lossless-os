/*
 * Playlists: M3U / M3U8 files in the card's /Playlists folder or at its
 * top.
 *
 * Entries are matched to the library by their last two path parts (the
 * album's folder and the file name; the artist folder too when that's
 * ambiguous), so a playlist works whether its paths are relative,
 * absolute on the card, or from another computer (Windows or Mac paths).
 * Entries not in the library are skipped.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_PLAYLISTS_H
#define LOS_PLAYLISTS_H

#define PLAYLISTS_MAX 64

typedef struct {
    char name[64];          /* the file name without .m3u / .m3u8 */
    char path[256];         /* without a drive letter */
    int songs;              /* found in the library */
} los_playlist;

/* Find the playlists (after the library scan). Returns how many. */
int playlists_scan(void);
int playlists_count(void);
const los_playlist *playlist_get(int i);
/* Index of the playlist at this path, or -1 */
int playlist_find(const char *path);

/* Its songs as (album, track) pairs into album[] / track[]; returns how many */
int playlist_load(int i, int *album, int *track, int max);

#endif
