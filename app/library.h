/*
 * Lossless OS: the music library. For now a scan of Artist/Album folders
 * (the layout the Lossless desktop app syncs), reading FLAC headers for
 * format and length. A real index on the card comes later.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_LIBRARY_H
#define LOS_LIBRARY_H

#include <stdint.h>

#define LIB_MAX_ALBUMS 256
#define LIB_MAX_TRACKS 40

typedef struct {
    char title[72];
    char file[160];      /* name inside the album folder ("CD2/01 x.flac" on a disc) */
    int disc;            /* from a disc folder (CD1, Disc 2), 0 if none */
    int number;
    int seconds;
    int bits, rate;      /* 0 if unknown */
} los_track;

typedef struct {
    char title[72];
    char artist[72];
    char dir[256];       /* full path, LVGL drive letter included */
    char cover[256];     /* full path without drive letter, "" if none */
    uint32_t cover_offset; /* where the JPEG starts in that file (embedded in a FLAC) */
    int year;
    uint32_t sig;        /* fingerprint of the folder's file names (the cache) */
    int dropped;         /* songs in the folder it couldn't add */
    char dropped_why[32];
    int ntracks;
    los_track tracks[LIB_MAX_TRACKS];
} los_album;

/* "CD1", "Disc 2": a disc folder's number (its songs belong to the album
 * folder above), else 0 */
int library_disc_folder(const char *name);

/* Scan the library: every folder under 'root' (the whole card) with songs,
 * whatever the layout. Returns the number of albums. Albums whose folders
 * haven't changed come from the cache on the card. */
int library_scan(const char *root);
/* The same, reading every file again (for changed tags) */
int library_rescan(const char *root);
/* Told the albums so far as a scan goes (NULL: no one), to show it's working */
void library_set_progress(void (*fn)(int albums));

int library_albums(void);
const los_album *library_album(int i);

/* What the last scan couldn't add: whole albums (no song it can play) and
 * albums with some songs left out, each with how many and why */
#define LIB_MAX_SKIPPED 32
typedef struct {
    char name[150];      /* "Artist - Album" */
    char why[32];        /* "HE-AAC", "UNREADABLE", "40+ SONGS", "FULL", ... */
    int songs;           /* left out */
    bool whole;          /* the whole album */
} los_skipped;
int library_skipped(void);
const los_skipped *library_skipped_album(int i);
int library_artists(void);
int library_tracks(void);

/* "FLAC 16/44.1" for an album's first track, or "" */
void library_format(const los_album *a, char *buf, int len);
/* The same for one track ("WAV 24/96") */
void library_track_format(const los_track *t, char *buf, int len);

/* A name as it sorts: without a leading "The", "A" or "An" (as the
 * iPod sorts), so The Killers go under K */
const char *library_sort_name(const char *s);
/* Two names in that order (then as they are, to keep it stable) */
int library_name_cmp(const char *a, const char *b);

#endif
