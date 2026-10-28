/*
 * Lossless OS: screens and navigation.
 *
 * Each screen is rebuilt when it's shown; a small stack remembers where
 * you were and what was selected. The player is a stand-in until audio
 * output exists: it keeps time and moves through tracks, but plays nothing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "los.h"
#include "theme.h"
#include "widgets.h"
#include "library.h"
#include "covers.h"
#include "playback.h"
#include "bluetooth.h"
#include "calendar.h"
#include "settings.h"
#include "version.h"
#include "output.h"
#include "playlists.h"

#define MINI_H 22      /* the mini player's height */

static const char *const month_names[] = { "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December" };
static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday" };


/* Brightness: each Play on it goes up a step, from the top back to the
 * bottom. Never below 20%: darker is hard to read, and a screen too dark
 * to see is too dark to find the setting on */
#define BRIGHT_MIN  20
#define BRIGHT_STEP 10

/* Settings with choices (Settings, saved with the rest) */
static struct {
    int brightness;      /* percent of the screen's normal light */
    int dim_s;           /* dim after this long without a key; 0 never */
    bool clock24;
    bool wheel_volume;   /* the wheel on Now Playing: volume, not scrubbing */
    int usb;             /* plugged into a computer: ask, transfer files, charge only */
} opt = { 100, 60, false, true, 0 };     /* the wheel on Now Playing: volume, as on an iPod */
enum { USB_ASK, USB_SHARE, USB_CHARGE, USB_DAC };          /* saved as numbers: order kept */
static const char *const usb_words[] = { "ASK", "TRANSFER", "CHARGE", "DAC" };

/* The time as the clock setting has it: "3:46" (12-hour; *ampm gets AM or
 * PM) or "15:46" (24-hour; *ampm gets "") */
static void clock_text(char *buf, size_t n, const los_date *d, const char **ampm)
{
    if (opt.clock24) {
        snprintf(buf, n, "%d:%02d", d->hour, d->minute);
        if (ampm)
            *ampm = "";
    } else {
        snprintf(buf, n, "%d:%02d", d->hour % 12 ? d->hour % 12 : 12, d->minute);
        if (ampm)
            *ampm = d->hour < 12 ? "AM" : "PM";
    }
}

/* EQ: off by default (bit-perfect) */
static bool eq_enabled;
static int eq_gains[EQ_BANDS];

/* Sleep timer: at the end, pause, save and switch off; 0 off */
static uint32_t sleep_at;

/* ---- screens ------------------------------------------------------------------------- */

enum screen {
    SCR_HOME,
    SCR_MUSIC,
    SCR_ALBUMS,      /* param: artist index, or -1 for all */
    SCR_ARTISTS,
    SCR_SONGS,
    SCR_ALBUM,       /* param: album index */
    SCR_NOW_PLAYING,
    SCR_SOON,        /* param: index into soon_titles */
    SCR_ARTIST_SONGS,/* param: artist index: all their songs */
    SCR_SETTINGS,
    SCR_BLUETOOTH,
    SCR_BT_SEARCH,
    SCR_CLOCK,       /* setting the date and time */
    SCR_ABOUT,
    SCR_EQ,
    SCR_EQ_PRESETS,
    SCR_PLAYLISTS,
    SCR_PLAYLIST,    /* param: playlist index */
};

struct nav {
    enum screen screen;
    int param;
    int selected;
};

#define NAV_DEPTH 8
static struct nav stack[NAV_DEPTH];
static int depth;

static lv_group_t *group;
static lv_obj_t *screen_obj;     /* the current LVGL screen */
static lv_obj_t *header;

/* the current list, if the screen has one */
static struct {
    lv_obj_t *cont;
    int count;
    int selected;
} list;

static const char *const soon_titles[] = { "Playlists", "Settings" };

/* ---- the stand-in player --------------------------------------------------------- */

static struct {
    int album, track;
    int state;          /* 0 stopped, 1 playing, 2 paused */
    int elapsed;        /* seconds */
} player = { -1, 0, 0, 0 };

static void show(void);

static const los_track *current_track(void)
{
    if (player.album < 0)
        return NULL;
    return &library_album(player.album)->tracks[player.track];
}

static int track_length(const los_track *t)
{
    return t && t->seconds > 0 ? t->seconds : 180;
}

/* What plays: a queue of songs, made from the list a song was chosen in
 * (an album, all songs, an artist's songs). Next and Prev, and the end of a
 * song, move through it. */
typedef struct { int album, track; } song_ref;

#define QUEUE_MAX (LIB_MAX_ALBUMS * 4)
static song_ref queue[QUEUE_MAX];
static song_ref queue_orig[QUEUE_MAX];   /* the list's own order (shuffle off) */
static int queue_len, queue_pos;

/* Scrubbing: the wheel on Now Playing moves through the song; it jumps
 * there once the wheel rests */
#define SCRUB_STEP_S      5
#define SCRUB_FAST_STEP_S 15    /* turned quickly */
#define SCRUB_FAST_MS     150
#define SCRUB_COMMIT_MS   600
static struct {
    bool active;
    int sec;
    uint32_t last;
} scrub;

/* Prev / Next, as on an iPod: a tap skips (when it's let go),
 * held it seeks through the song, faster the longer it's held, and the
 * song jumps there when it's let go. The keypad says when it's let go. */
#define HOLD_MS       400        /* longer than this is a hold */
#define SEEK_EVERY_MS 100
static lv_indev_t *app_keypad;
static struct {
    uint32_t key;                /* LV_KEY_LEFT / RIGHT, 0 if none held */
    uint32_t since, last_step;
    bool seeking;
} hold;

/* Where the queue came from, to make it again after a restart */
enum { FROM_ALBUM, FROM_SONGS, FROM_ARTIST, FROM_PLAYLIST };
static int queue_from;
static char queue_key[256];       /* the album's folder, the artist's name, the playlist's path */

/* Play modes (the options on Menu): repeat the queue or the song, shuffle */
enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };
static int repeat_mode;
static bool shuffle;

static uint32_t rng_state;
static uint32_t rng(void)
{
    if (!rng_state)
        rng_state = lv_tick_get() * 2654435761u + 1;
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* The queue in the list's order, or shuffled with the song at 'keep'
 * (in the list's order) first */
static void queue_arrange(int keep)
{
    memcpy(queue, queue_orig, (size_t)queue_len * sizeof(song_ref));
    if (!shuffle) {
        queue_pos = keep;
        return;
    }
    song_ref first = queue[keep];
    queue[keep] = queue[0];
    queue[0] = first;
    for (int i = queue_len - 1; i > 1; i--) {
        int j = 1 + (int)(rng() % (uint32_t)i);   /* 1..i */
        song_ref t = queue[i];
        queue[i] = queue[j];
        queue[j] = t;
    }
    queue_pos = 0;
}

/* Where the queue goes from here: dir +1 next, -1 previous; -1 at the end */
static int queue_step(int dir)
{
    int pos = queue_pos + dir;
    if (pos >= queue_len)
        return repeat_mode == REPEAT_ALL ? 0 : -1;
    if (pos < 0)
        return repeat_mode == REPEAT_ALL ? queue_len - 1 : 0;
    return pos;
}

/* Start the current song in the playback engine (paused if the player is) */
static int failures;   /* songs in a row that couldn't play */

/* A song's path for the playback engine: the album folder without its
 * LVGL drive letter, then the file */
static void song_path(char *path, size_t size, int album, int track)
{
    const los_album *a = library_album(album);
    snprintf(path, size, "%s/%s", a->dir + 2, a->tracks[track].file);
}

/* Tell the engine what follows the current song, so it plays on without a
 * gap. Again after anything that changes it (song, repeat, shuffle). */
static void tell_next(void)
{
    char path[512] = "";
    if (player.album >= 0 && player.state) {
        if (repeat_mode == REPEAT_ONE)
            song_path(path, sizeof(path), player.album, player.track);
        else {
            int pos = queue_pos + 1 < queue_len ? queue_pos + 1 : repeat_mode == REPEAT_ALL ? 0 : -1;
            if (pos >= 0 && queue_len)
                song_path(path, sizeof(path), queue[pos].album, queue[pos].track);
        }
    }
    playback_set_next(path);
}

static void start_current_from(int sec)
{
    char path[512];
    song_path(path, sizeof(path), player.album, player.track);
    playback_play_from(path, (uint32_t)sec * 1000);
    if (player.state == 2)
        playback_pause(true);
    tell_next();
}

static void start_current(void)
{
    start_current_from(0);
}

static void queue_go(int pos)
{
    queue_pos = pos;
    player.album = queue[pos].album;
    player.track = queue[pos].track;
    player.elapsed = 0;
}

static void player_play_list(const song_ref *songs, int n, int start)
{
    if (n <= 0)
        return;
    queue_len = n < QUEUE_MAX ? n : QUEUE_MAX;
    memcpy(queue_orig, songs, (size_t)queue_len * sizeof(song_ref));
    queue_arrange(start < queue_len ? start : 0);
    queue_go(queue_pos);
    player.state = 1;
    failures = 0;
    start_current();
}

/* An album from one of its tracks */
static void player_play(int album, int track)
{
    static song_ref songs[LIB_MAX_TRACKS];
    const los_album *a = library_album(album);
    for (int k = 0; k < a->ntracks; k++) {
        songs[k].album = album;
        songs[k].track = k;
    }
    player_play_list(songs, a->ntracks, track);
}

static const los_track *queued_track(int pos)
{
    if (pos < 0 || pos >= queue_len)
        return NULL;
    return &library_album(queue[pos].album)->tracks[queue[pos].track];
}

static void player_skip(int dir)
{
    if (player.album < 0)
        return;
    if (dir < 0 && player.elapsed > 3) {
        player.elapsed = 0;
        start_current();              /* back to the start of this song */
        return;
    }
    int pos = queue_step(dir);
    if (pos < 0) {
        /* the end of the queue: stop, ready to play it again from the top */
        player.state = 0;
        pos = 0;
    }
    queue_go(pos);
    if (player.state)
        start_current();
    else
        playback_stop();
}

/* Play / pause; from stopped, play the current song from its start */
static void player_toggle(void)
{
    if (player.album < 0)
        return;
    if (player.state == 0) {
        player.state = 1;
        player.elapsed = 0;
        failures = 0;
        start_current();
        return;
    }
    player.state = player.state == 1 ? 2 : 1;
    playback_pause(player.state == 2);
}

static void set_shuffle(bool on)
{
    if (on == shuffle || queue_len == 0) {
        shuffle = on;
        return;
    }
    /* keep playing the same song, now in the other order */
    song_ref now = queue[queue_pos];
    int keep = 0;
    for (int i = 0; i < queue_len; i++)
        if (queue_orig[i].album == now.album && queue_orig[i].track == now.track)
            keep = i;
    shuffle = on;
    queue_arrange(keep);
    tell_next();
}

/* ---- list building --------------------------------------------------------------- */

static void set_state_deep(lv_obj_t *o, lv_state_t st, bool on)
{
    if (on)
        lv_obj_add_state(o, st);
    else
        lv_obj_remove_state(o, st);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++)
        set_state_deep(lv_obj_get_child(o, (int32_t)i), st, on);
}

/* Long text in the selected row scrolls after a second, like the title on
 * Now Playing; other rows keep "..." */
#define MARQUEE_DELAY_MS 1000

static lv_obj_t *marquee_row;
static lv_timer_t *marquee_timer;

static void row_scroll(lv_obj_t *row, bool on)
{
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if (!lv_obj_check_type(c, &lv_label_class))
            continue;
        lv_label_long_mode_t m = lv_label_get_long_mode(c);
        if (on && m == LV_LABEL_LONG_MODE_DOTS)
            lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        else if (!on && m == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR)
            lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void marquee_start(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (marquee_row)
        row_scroll(marquee_row, true);
}

static void marquee_follow(lv_obj_t *row)
{
    if (marquee_row && marquee_row != row)
        row_scroll(marquee_row, false);
    marquee_row = row;
    if (!marquee_timer)
        marquee_timer = lv_timer_create(marquee_start, MARQUEE_DELAY_MS, NULL);
    lv_timer_reset(marquee_timer);
    if (row)
        lv_timer_resume(marquee_timer);
    else
        lv_timer_pause(marquee_timer);
}

static lv_obj_t *settings_hint;      /* the line under Settings */
static const char *settings_default(int row);

static void list_select(int i)
{
    if (!list.cont || list.count == 0)
        return;
    if (i < 0)
        i = 0;
    if (i >= list.count)
        i = list.count - 1;
    lv_obj_t *old = lv_obj_get_child(list.cont, list.selected);
    if (old)
        set_state_deep(old, LV_STATE_CHECKED, false);
    list.selected = i;
    stack[depth - 1].selected = i;
    lv_obj_t *row = lv_obj_get_child(list.cont, i);
    if (row) {
        set_state_deep(row, LV_STATE_CHECKED, true);
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
    marquee_follow(row);
    if (settings_hint && stack[depth - 1].screen == SCR_SETTINGS)
        lv_label_set_text(settings_hint, settings_default(i));
}

/* Room left for the list: above the mini player while music plays */
static int list_height(void)
{
    return (player.state ? LOS_H - MINI_H : LOS_H) - LOS_HEADER_H - 1;
}

static void list_begin(void)
{
    list.cont = theme_box(screen_obj);
    lv_obj_set_pos(list.cont, 0, LOS_HEADER_H + 1);
    lv_obj_set_size(list.cont, LOS_W, list_height());
    lv_obj_set_flex_flow(list.cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_OFF);
    list.count = 0;
    list.selected = 0;
}

static lv_obj_t *row_begin(int height)
{
    lv_obj_t *row = theme_box(list.cont);
    lv_obj_set_size(row, LOS_W, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(row, theme.select, LV_STATE_CHECKED);
    /* a hairline under each row keeps neighbours apart */
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, theme.line, 0);
    lv_obj_set_style_border_color(row, theme.select, LV_STATE_CHECKED);
    list.count++;
    return row;
}

/* Text in rows is centred on its capital letters, not on the line box
 * (which has room below for descenders, so text sat high); labels on one
 * row share a baseline. */
static int cap_height(const lv_font_t *f)
{
    lv_font_glyph_dsc_t g;
    if (lv_font_get_glyph_dsc(f, &g, 'H', 0) && g.box_h > 0)
        return g.box_h;
    return lv_font_get_line_height(f) * 7 / 10;
}

/* The baseline that centres capitals in a row h tall */
static int centred_baseline(const lv_font_t *f, int h)
{
    return (h + cap_height(f)) / 2;
}

/* A label's top, for its text to sit on baseline */
static int top_for_baseline(const lv_font_t *f, int baseline)
{
    return baseline - (f->line_height - f->base_line);
}

static lv_obj_t *row_label(lv_obj_t *row, const char *text, const lv_font_t *font,
                           lv_color_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_color(l, theme.fg, LV_STATE_CHECKED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

/* A plain row: one line of list text */
static void text_row(const char *text, const char *right)
{
    lv_obj_t *row = row_begin(36);
    int b = centred_baseline(theme.row, 36);
    row_label(row, text, theme.row, theme.fg, 20, top_for_baseline(theme.row, b),
              right ? 210 : 290);
    if (right) {
        lv_obj_t *r = row_label(row, right, theme.mono, theme.faint, 230,
                                top_for_baseline(theme.mono, b), 80);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* A song row: its album's cover, small, then the title */
static void song_row(const los_album *a, const char *title)
{
    lv_obj_t *row = row_begin(36);
    lv_obj_t *c = cover_create(row, a->cover, a->cover_offset, 28, 3);
    lv_obj_set_pos(c, 12, 4);
    row_label(row, title, theme.row, theme.fg, 50,
              top_for_baseline(theme.row, centred_baseline(theme.row, 36)), 260);
}

/* An album row: cover, album, second line */
static void album_row(const los_album *a, const char *second)
{
    lv_obj_t *row = row_begin(52);
    lv_obj_t *c = cover_create(row, a->cover, a->cover_offset, 40, 4);
    lv_obj_set_pos(c, 10, 6);
    /* two lines, their baselines 21 apart, centred as a block */
    int b1 = (52 + cap_height(theme.row) - 21) / 2, b2 = b1 + 21;
    row_label(row, a->title, theme.row, theme.fg, 62, top_for_baseline(theme.row, b1), 248);
    row_label(row, second, theme.row_sub, theme.second, 62,
              top_for_baseline(theme.row_sub, b2), 248);
}

/* ---- mini player ------------------------------------------------------------------ */

/* While something plays, one thin line at the bottom of every list screen:
 * "title · artist" and a hairline of progress above it (MINI_H tall). */

static lv_obj_t *mini_title, *mini_bar, *mini_icon;

static void mini_set(void)
{
    const los_track *t = current_track();
    if (!mini_bar || !t)
        return;
    state_icon_set(mini_icon, player.state);
    int len = track_length(t);
    lv_obj_set_width(mini_bar, len > 0 ? LOS_W * player.elapsed / len : 0);
    char line[160];
    snprintf(line, sizeof(line), "%s  \xc2\xb7  %s", t->title, library_album(player.album)->artist);
    lv_label_set_text(mini_title, line);
}

static void mini_player(void)
{
    mini_title = mini_bar = mini_icon = NULL;
    if (!player.state)
        return;
    lv_obj_t *p = theme_box(screen_obj);
    lv_obj_set_pos(p, 0, LOS_H - MINI_H);
    lv_obj_set_size(p, LOS_W, MINI_H);
    lv_obj_set_style_bg_color(p, theme.bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_t *track = theme_box(p);
    lv_obj_set_size(track, LOS_W, 2);
    lv_obj_set_style_bg_color(track, theme.line, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    mini_bar = theme_box(p);
    lv_obj_set_size(mini_bar, 0, 2);
    lv_obj_set_style_bg_color(mini_bar, theme.accent, 0);
    lv_obj_set_style_bg_opa(mini_bar, LV_OPA_COVER, 0);
    mini_icon = state_icon_create(p, 7, 9);
    lv_obj_set_pos(mini_icon, 12, 2 + (MINI_H - 2 - 9) / 2);
    mini_title = lv_label_create(p);
    lv_obj_set_style_text_font(mini_title, theme.mono, 0);
    lv_obj_set_style_text_color(mini_title, theme.muted, 0);
    lv_label_set_long_mode(mini_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(mini_title, 26, 5);
    lv_obj_set_size(mini_title, LOS_W - 38, lv_font_get_line_height(theme.mono));
    mini_set();
}

/* ---- screens ------------------------------------------------------------------------ */

static lv_obj_t *np_time, *np_left, *np_bar, *np_count, *np_title, *np_next, *np_modes, *np_icon;
static lv_obj_t *opts;          /* the play options panel, or NULL; goes with its screen */

static void fmt_time(char *buf, size_t len, int s)
{
    snprintf(buf, len, "%d:%02d", s / 60, s % 60);
}

static void build_home(void)
{
    header = header_create(screen_obj, NULL);
    list_begin();
    text_row("Music", NULL);
    text_row("Playlists", NULL);
    text_row("Now Playing", player.state == 1 ? "PLAYING" : player.state == 2 ? "PAUSED" : NULL);
    text_row("Settings", NULL);
}

static void build_music(void)
{
    header = header_create(screen_obj, "Music");
    list_begin();
    text_row("Albums", NULL);
    text_row("Artists", NULL);
    text_row("Songs", NULL);
}

/* artist names, in the order the Artists list shows them */
static const char *artist_names[LIB_MAX_ALBUMS];
static int nartists;

static void collect_artists(void)
{
    nartists = 0;
    for (int i = 0; i < library_albums(); i++) {
        const char *a = library_album(i)->artist;
        bool seen = false;
        for (int j = 0; j < nartists; j++)
            if (!strcmp(artist_names[j], a))
                seen = true;
        if (!seen)
            artist_names[nartists++] = a;
    }
    for (int i = 1; i < nartists; i++)
        for (int j = i; j > 0 && strcasecmp(artist_names[j - 1], artist_names[j]) > 0; j--) {
            const char *t = artist_names[j];
            artist_names[j] = artist_names[j - 1];
            artist_names[j - 1] = t;
        }
}

/* album indexes shown by the current Albums screen */
static int shown_albums[LIB_MAX_ALBUMS];

static void build_albums(int artist)
{
    header = header_create(screen_obj, artist >= 0 ? artist_names[artist] : "Albums");
    list_begin();
    if (artist >= 0) {
        /* an artist's page starts with all their songs, like an iPod */
        int songs = 0;
        char count[16];
        for (int i = 0; i < library_albums(); i++)
            if (!strcmp(library_album(i)->artist, artist_names[artist]))
                songs += library_album(i)->ntracks;
        snprintf(count, sizeof(count), "%d", songs);
        text_row("All songs", count);
    }
    int n = 0;
    for (int i = 0; i < library_albums(); i++) {
        const los_album *a = library_album(i);
        if (artist >= 0 && strcmp(a->artist, artist_names[artist]))
            continue;
        char year[8] = "";
        if (a->year)
            snprintf(year, sizeof(year), "%d", a->year);
        album_row(a, artist >= 0 ? year : a->artist);
        shown_albums[n++] = i;
    }
}

static void build_artists(void)
{
    header = header_create(screen_obj, "Artists");
    list_begin();
    for (int i = 0; i < nartists; i++)
        text_row(artist_names[i], NULL);
}

static song_ref song_index[QUEUE_MAX];
static int nsongs;     /* in song_index: the songs the current list shows */

/* Every song, album by album */
static int collect_all_songs(song_ref *out)
{
    int n = 0;
    for (int i = 0; i < library_albums() && n < QUEUE_MAX; i++) {
        const los_album *a = library_album(i);
        for (int t = 0; t < a->ntracks && n < QUEUE_MAX; t++) {
            out[n].album = i;
            out[n].track = t;
            n++;
        }
    }
    return n;
}

static void build_songs(void)
{
    header = header_create(screen_obj, "Songs");
    list_begin();
    nsongs = collect_all_songs(song_index);
    for (int i = 0; i < nsongs; i++) {
        const los_album *a = library_album(song_index[i].album);
        song_row(a, a->tracks[song_index[i].track].title);
    }
}

/* An artist's albums, oldest first (undated last), then by title */
static int artist_albums(int artist, int *out)
{
    int n = 0;
    for (int i = 0; i < library_albums(); i++)
        if (!strcmp(library_album(i)->artist, artist_names[artist]))
            out[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const los_album *x = library_album(out[j - 1]), *y = library_album(out[j]);
            int xy = x->year ? x->year : 9999, yy = y->year ? y->year : 9999;
            if (xy < yy || (xy == yy && strcasecmp(x->title, y->title) <= 0))
                break;
            int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

/* All of an artist's songs, album by album: choosing one plays them all */
/* An artist's songs, album by album, oldest album first */
static int collect_artist_songs(int artist, song_ref *out)
{
    static int idx[LIB_MAX_ALBUMS];
    int na = artist_albums(artist, idx), n = 0;
    for (int i = 0; i < na; i++) {
        const los_album *a = library_album(idx[i]);
        for (int t = 0; t < a->ntracks && n < QUEUE_MAX; t++) {
            out[n].album = idx[i];
            out[n].track = t;
            n++;
        }
    }
    return n;
}

static void build_artist_songs(int artist)
{
    header = header_create(screen_obj, artist_names[artist]);
    list_begin();
    nsongs = collect_artist_songs(artist, song_index);
    for (int i = 0; i < nsongs; i++) {
        const los_album *a = library_album(song_index[i].album);
        song_row(a, a->tracks[song_index[i].track].title);
    }
}

static void build_album(int idx)
{
    const los_album *a = library_album(idx);
    header = header_create(screen_obj, a->artist);
    /* A compact head: cover, title, one line of facts; the list gets the rest */
    int x = 72, w = LOS_W - x - 12;
    lv_obj_t *c = cover_create(screen_obj, a->cover, a->cover_offset, 48, 4);
    lv_obj_set_pos(c, 12, LOS_HEADER_H + 6);

    /* the title in dot-matrix capitals, scrolling when long */
    char caps[96];
    size_t i;
    for (i = 0; a->title[i] && i < sizeof(caps) - 1; i++) {
        char ch = a->title[i];
        caps[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    caps[i] = '\0';
    lv_obj_t *d = dots_create(screen_obj, 2, theme.fg);
    dots_set_max_width(d, w);
    dots_set_text(d, caps);
    lv_obj_set_pos(d, x, LOS_HEADER_H + 9);

    char fmt[24], meta[80], year[12] = "";
    library_format(a, fmt, sizeof(fmt));
    if (a->year)
        snprintf(year, sizeof(year), " \xc2\xb7 %d", a->year);
    snprintf(meta, sizeof(meta), "%s%s%d TRACKS%s", fmt, fmt[0] ? " \xc2\xb7 " : "",
             a->ntracks, year);
    lv_obj_t *dot = theme_box(screen_obj);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, theme.accent, 0);
    lv_obj_set_pos(dot, x, LOS_HEADER_H + 42);
    lv_obj_t *m = lv_label_create(screen_obj);
    lv_label_set_text(m, meta);
    lv_obj_set_style_text_font(m, theme.mono, 0);
    lv_obj_set_style_text_color(m, theme.muted, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(m, x + 10, LOS_HEADER_H + 37);
    lv_obj_set_size(m, w - 10, lv_font_get_line_height(theme.mono));

    int top = LOS_HEADER_H + 60;
    list.cont = theme_box(screen_obj);
    lv_obj_set_pos(list.cont, 0, top);
    lv_obj_set_size(list.cont, LOS_W, (player.state ? LOS_H - MINI_H : LOS_H) - top);
    lv_obj_set_flex_flow(list.cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_OFF);
    list.count = 0;
    for (int k = 0; k < a->ntracks; k++) {
        const los_track *tr = &a->tracks[k];
        lv_obj_t *row = row_begin(32);
        char num[8], len[12];
        snprintf(num, sizeof(num), "%02d", tr->number ? tr->number : k + 1);
        fmt_time(len, sizeof(len), tr->seconds);
        int b = centred_baseline(theme.track, 32);
        row_label(row, num, theme.mono, theme.faint, 14, top_for_baseline(theme.mono, b), 24);
        row_label(row, tr->title, theme.track, theme.fg, 40, top_for_baseline(theme.track, b), 220);
        lv_obj_t *r = row_label(row, tr->seconds ? len : "", theme.mono, theme.faint, 250,
                                top_for_baseline(theme.mono, b), 58);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_now_playing(void)
{
    header = header_create(screen_obj, "Now Playing");
    np_time = np_left = np_bar = np_count = np_title = np_next = NULL;
    if (player.album < 0) {
        lv_obj_t *t = theme_tag(screen_obj, "Nothing playing", theme.muted);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
        return;
    }
    const los_album *a = library_album(player.album);
    const los_track *t = current_track();

    lv_obj_t *c = cover_create(screen_obj, a->cover, a->cover_offset, 120, 7);
    lv_obj_set_pos(c, 12, 44);

    np_icon = state_icon_create(screen_obj, 7, 9);
    lv_obj_set_pos(np_icon, 146, 46 + (lv_font_get_line_height(theme.mono) - 9) / 2);
    np_count = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(np_count, theme.mono, 0);
    lv_obj_set_style_text_color(np_count, theme.faint, 0);
    lv_obj_set_pos(np_count, 159, 46);

    np_title = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(np_title, theme.title, 0);
    lv_obj_set_style_text_color(np_title, theme.fg, 0);
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_pos(np_title, 146, 64);
    lv_obj_set_size(np_title, 162, lv_font_get_line_height(theme.title));

    lv_obj_t *ar = lv_label_create(screen_obj);
    lv_label_set_text(ar, a->artist);
    lv_obj_set_style_text_font(ar, theme.body, 0);
    lv_obj_set_style_text_color(ar, theme.second, 0);
    lv_label_set_long_mode(ar, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(ar, 146, 92);
    lv_obj_set_size(ar, 162, lv_font_get_line_height(theme.body));

    lv_obj_t *al = lv_label_create(screen_obj);
    lv_label_set_text(al, a->title);
    lv_obj_set_style_text_font(al, theme.small, 0);
    lv_obj_set_style_text_color(al, theme.faint, 0);
    lv_label_set_long_mode(al, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(al, 146, 116);
    lv_obj_set_size(al, 162, lv_font_get_line_height(theme.small));

    /* format chip */
    char fmt[32];
    library_track_format(t, fmt, sizeof(fmt));
    if (fmt[0] && eq_enabled)
        strcat(fmt, " \xc2\xb7 EQ");     /* not bit-perfect: say so */
    if (fmt[0]) {
        lv_obj_t *chip = theme_box(screen_obj);
        lv_obj_set_pos(chip, 146, 142);
        lv_obj_set_height(chip, 19);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_left(chip, 17, 0);
        lv_obj_set_style_pad_right(chip, 9, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(chip, theme.raised, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, theme.line, 0);
        lv_obj_t *dot = theme_box(chip);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, theme.accent, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, -9, 0);
        lv_obj_t *ft = lv_label_create(chip);
        lv_label_set_text(ft, fmt);
        lv_obj_set_style_text_font(ft, theme.mono, 0);
        lv_obj_set_style_text_color(ft, theme.fg, 0);
        lv_obj_align(ft, LV_ALIGN_LEFT_MID, 0, 0);
    }

    np_bar = segbar_create(screen_obj, 296, 5, 40, 3, theme.accent, theme.seg_off);
    lv_obj_set_pos(np_bar, 12, 172);
    np_time = dots_create(screen_obj, 2, theme.ink);
    lv_obj_set_pos(np_time, 12, 184);
    np_left = dots_create(screen_obj, 2, theme.dim);
    lv_obj_set_y(np_left, 184);
    /* shuffle and repeat, between the two times */
    np_modes = lv_label_create(screen_obj);
    lv_label_set_text(np_modes, "");
    lv_obj_set_style_text_font(np_modes, theme.mono, 0);
    lv_obj_set_style_text_color(np_modes, theme.muted, 0);
    lv_obj_set_style_text_align(np_modes, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(np_modes, 150, lv_font_get_line_height(theme.mono));
    lv_obj_set_pos(np_modes, (LOS_W - 150) / 2, 184 + 12 - lv_font_get_line_height(theme.mono) / 2);

    const los_track *next = repeat_mode == REPEAT_ONE ? t : queued_track(queue_step(+1));
    if (next) {
        lv_obj_t *nl = theme_tag(screen_obj, "Next", theme.faint);
        lv_obj_set_pos(nl, 12, 220);
        np_next = lv_label_create(screen_obj);
        lv_label_set_text(np_next, next->title);
        lv_obj_set_style_text_font(np_next, theme.small, 0);
        lv_obj_set_style_text_color(np_next, theme.second, 0);
        lv_label_set_long_mode(np_next, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_update_layout(nl);
        int nx = 12 + lv_obj_get_width(nl) + 8;
        /* the song on the same baseline as "NEXT" (different sizes) */
        const lv_font_t *fl = theme.label, *fs = theme.small;
        int ny = 220 + (fl->line_height - fl->base_line) - (fs->line_height - fs->base_line);
        lv_obj_set_pos(np_next, nx, ny);
        lv_obj_set_size(np_next, LOS_W - 12 - nx, lv_font_get_line_height(theme.small));
    }
    (void)t;
}

/* ---- settings and Bluetooth ----------------------------------------------------------- */

static void clock_row(const char *name, const char *value, bool editing);

static lv_obj_t *list_footer(const char *text);

static bool usb_row_ready;   /* Settings' USB connection row: a computer was there when built */

static void build_settings(void)
{
    header = header_create(screen_obj, "Settings");
    bt_status b;
    bluetooth_status(&b);
    list_begin();
    text_row("Bluetooth", b.state >= BT_CONNECTED ? "CONNECTED" : b.on ? "ON" : "OFF");
    text_row("EQ", eq_enabled ? "ON" : "OFF");
    int64_t now;
    char when[16] = "NOT SET";
    if (los_clock_get(&now)) {
        los_date d = date_from_seconds(now);
        const char *ampm;
        clock_text(when, sizeof(when), &d, &ampm);
        if (ampm[0])
            snprintf(when + strlen(when), sizeof(when) - strlen(when), " %s", ampm);
    }
    text_row("Date & time", when);
    char v[24];
    snprintf(v, sizeof(v), "%d%%", opt.brightness);
    text_row("Brightness", v);
    if (opt.dim_s)
        snprintf(v, sizeof(v), opt.dim_s < 60 ? "%d S" : "%d MIN", opt.dim_s < 60 ? opt.dim_s : opt.dim_s / 60);
    text_row("Dim screen after", opt.dim_s ? v : "NEVER");
    text_row("Clock", opt.clock24 ? "24-HOUR" : "12-HOUR");
    text_row("Wheel on Now Playing", opt.wheel_volume ? "VOLUME" : "SCRUB");
    if (sleep_at) {
        int32_t left = (int32_t)(sleep_at - lv_tick_get());
        snprintf(v, sizeof(v), "%d MIN", left > 0 ? (left + 30000) / 60000 : 0);   /* nearest minute */
    }
    text_row("Sleep timer", sleep_at ? v : "OFF");
    text_row("USB", usb_words[opt.usb]);
    usb_row_ready = los_usb_state() == LOS_USB_COMPUTER;
    text_row("USB connection", usb_row_ready ? "READY" : "NONE");
    text_row("About", NULL);
    text_row("Rescan library", NULL);
    settings_hint = list_footer("");
}

/* What the selected setting is when nobody has changed it (the line under
 * Settings) */
static const char *settings_default(int row)
{
    static const char *const hint[] = {
        "Default: off", "Default: off (bit-perfect)", "Play sets the clock",
        "Default: 100%", "Default: 1 min", "Default: 12-hour", "Default: volume",
        "Default: off", "Default: ask (or transfer, DAC, charge)", "", "Version, card, battery",
        "Reads every song again",
    };
    if (row == 9)                    /* USB connection: what it does now */
        return usb_row_ready ? "Transfer files, DAC or charge" : "Plug into a computer first";
    return row >= 0 && row < (int)(sizeof(hint) / sizeof(hint[0])) ? hint[row] : "";
}

/* ---- playlists ---- */

static int collect_playlist(int pl, song_ref *out)
{
    static int al[QUEUE_MAX], tr[QUEUE_MAX];
    int n = playlist_load(pl, al, tr, QUEUE_MAX);
    for (int i = 0; i < n; i++) {
        out[i].album = al[i];
        out[i].track = tr[i];
    }
    return n;
}

static void build_playlists(void)
{
    header = header_create(screen_obj, "Playlists");
    list_begin();
    char count[16];
    for (int i = 0; i < playlists_count(); i++) {
        const los_playlist *p = playlist_get(i);
        snprintf(count, sizeof(count), "%d", p->songs);
        text_row(p->name, count);
    }
    if (!playlists_count()) {
        lv_obj_t *t = theme_tag(screen_obj, "No playlists", theme.muted);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, -6);
        lv_obj_t *h = lv_label_create(screen_obj);
        lv_label_set_text(h, "M3U files in /Playlists on the card");
        lv_obj_set_style_text_font(h, theme.mono, 0);
        lv_obj_set_style_text_color(h, theme.faint, 0);
        lv_obj_align(h, LV_ALIGN_CENTER, 0, 16);
    }
}

static void build_playlist(int pl)
{
    const los_playlist *p = playlist_get(pl);
    header = header_create(screen_obj, p ? p->name : "Playlist");
    list_begin();
    nsongs = p ? collect_playlist(pl, song_index) : 0;
    for (int i = 0; i < nsongs; i++) {
        const los_album *a = library_album(song_index[i].album);
        song_row(a, a->tracks[song_index[i].track].title);
    }
}

/* ---- EQ ---- */

static void clock_row(const char *name, const char *value, bool editing);
static lv_obj_t *list_footer(const char *text);
static void settings_changed(void);

/* The EQ screen: the curve the bands make (worked out from the filters
 * themselves, eq_response), and a slider for each band under it. Prev and
 * Next pick a band, the wheel moves it (heard at once; it turns the EQ
 * on), Play plays or pauses the music so there's something to hear (the
 * screen starts it if nothing plays), Menu has the presets and EQ on /
 * off. Gray and white: the playing bar stays the only orange. */

static void push(enum screen s, int param);
static void refresh(void);

static void eq_apply(void)
{
    output_set_eq(eq_enabled, eq_gains);
}

/* Presets as good players have them, gentle on the ten bands (31 Hz to
 * 16 kHz). The EQ turns its level down by the curve's highest point, so
 * none of them clips. */
static const struct { const char *name; int8_t gain[EQ_BANDS]; } eq_presets[] = {
    { "Flat",       {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 } },
    { "Bass Boost", {  6,  6,  5,  3,  1,  0,  0,  0,  0,  0 } },
    { "Warm",       {  3,  3,  2,  2,  1,  0,  0, -1, -2, -2 } },
    { "Bright",     {  0,  0,  0,  0,  0,  0,  1,  2,  3,  4 } },
    { "Vocal",      { -2, -2, -1,  0,  2,  3,  3,  2,  0, -1 } },
    { "Rock",       {  4,  4,  3,  1, -1, -1,  0,  2,  3,  3 } },
    { "Pop",        { -1, -1,  0,  1,  3,  3,  2,  1, -1, -1 } },
    { "Jazz",       {  2,  2,  1,  1, -1, -1,  0,  1,  2,  2 } },
    { "Classical",  {  3,  3,  2,  1,  0,  0,  0,  1,  2,  3 } },
    { "Electronic", {  5,  5,  4,  1,  0, -1,  0,  1,  3,  4 } },
};
#define EQ_PRESETS ((int)(sizeof(eq_presets) / sizeof(eq_presets[0])))
static const char *const eq_band_names[EQ_BANDS] = { "31", "62", "125", "250", "500",
                                                     "1k", "2k", "4k", "8k", "16k" };

/* Every EQ there is to choose: Custom EQ 1 to 3, then the presets. Each
 * keeps its own bands: a preset changed stays changed until Reset gives
 * its own back; a Custom EQ starts flat. eq_gains is the chosen one's. */
#define EQ_CUSTOM  3
#define EQ_CHOICES (EQ_CUSTOM + EQ_PRESETS)
static int eq_curves[EQ_CHOICES][EQ_BANDS];
static int eq_sel = EQ_CUSTOM;           /* Flat */

static int eq_default_gain(int c, int b)
{
    return c < EQ_CUSTOM ? 0 : eq_presets[c - EQ_CUSTOM].gain[b];
}

/* Changed from its own: a preset edited, or a Custom EQ that isn't flat */
static bool eq_changed(int c)
{
    for (int b = 0; b < EQ_BANDS; b++)
        if (eq_curves[c][b] != eq_default_gain(c, b))
            return true;
    return false;
}

static void eq_reset(int c)
{
    for (int b = 0; b < EQ_BANDS; b++)
        eq_curves[c][b] = eq_default_gain(c, b);
}

static void eq_name(int c, char *out, size_t n)
{
    if (c < EQ_CUSTOM)
        snprintf(out, n, "Custom EQ %d", c + 1);
    else
        snprintf(out, n, "%s", eq_presets[c - EQ_CUSTOM].name);
}

/* The chosen one's bands are the ones played */
static void eq_choose(int c)
{
    eq_sel = c;
    memcpy(eq_gains, eq_curves[c], sizeof(eq_gains));
}

/* The preset the bands are set to, or -1 (none: settings saved before
 * there were Custom EQs) */
static int eq_preset_now(void)
{
    for (int p = 0; p < EQ_PRESETS; p++) {
        int b = 0;
        while (b < EQ_BANDS && eq_presets[p].gain[b] == eq_gains[b])
            b++;
        if (b == EQ_BANDS)
            return p;
    }
    return -1;
}

#define EQ_X       10                    /* the curve and the sliders: 10 .. 310 */
#define EQ_W       300
#define EQ_COL     (EQ_W / EQ_BANDS)
#define CURVE_Y    (LOS_HEADER_H + 22)
#define CURVE_H    50
#define CURVE_DB   8                     /* the curve's top and bottom: +-8 dB (more is cut off) */
#define GAIN_Y     (CURVE_Y + CURVE_H + 4)
#define TRACK_Y    (GAIN_Y + 16)
#define TRACK_H    56                    /* -12 .. +12 dB */
#define FREQ_Y     (TRACK_Y + TRACK_H + 6)
#define KNOB       9

/* The curve's pixels. Aligned by hand: LVGL's own static buffer macro
 * doesn't align it on every build, and the players' CPU can't write a
 * 16-bit pixel at an odd address (it stops: an address error). */
static uint16_t eq_curve_px[EQ_W * CURVE_H] __attribute__((aligned(64)));
static lv_draw_buf_t eq_curve_buf;

static struct {
    int band;                            /* the selected one */
    lv_obj_t *canvas, *info, *master;
    lv_obj_t *fill[EQ_BANDS], *knob[EQ_BANDS], *gain[EQ_BANDS], *freq[EQ_BANDS];
} eqs;

/* The curve: the bands' response from 20 Hz to 20 kHz (log), filled from
 * 0 dB, at every fourth pixel and joined */
static void eq_draw_curve(void)
{
    enum { STEP = 4, N = EQ_W / STEP + 1 };
    static float hz[N], db[N];
    if (!hz[0])
        for (int i = 0; i < N; i++)
            hz[i] = (float)(20.0 * pow(1000.0, (double)i / (N - 1)));
    eq_response(eq_gains, 44100, hz, N, db);
    uint16_t bg = lv_color_to_u16(theme.bg), grid = lv_color_to_u16(theme.line);
    uint16_t fill = lv_color_to_u16(eq_enabled ? theme.select : theme.raised);
    uint16_t line = lv_color_to_u16(eq_enabled ? theme.fg : theme.muted);
    uint32_t stride = eq_curve_buf.header.stride / 2;
    uint16_t *px = (uint16_t *)eq_curve_buf.data;
    int mid = CURVE_H / 2, prev_y = -1;
    for (int x = 0; x < EQ_W; x++) {
        int i = x / STEP;
        float t = (float)(x % STEP) / STEP;
        float d = db[i] + (i + 1 < N ? (db[i + 1] - db[i]) * t : 0);
        int y = mid - (int)lroundf(d * (CURVE_H / 2 - 2) / CURVE_DB);
        y = y < 1 ? 1 : y > CURVE_H - 2 ? CURVE_H - 2 : y;
        bool guide = (x - EQ_COL / 2) % EQ_COL == 0;     /* under each slider */
        for (int r = 0; r < CURVE_H; r++) {
            uint16_t c = bg;
            if ((r >= y && r <= mid) || (r <= y && r >= mid))
                c = fill;
            else if (r == mid || (guide && r % 3 == 0))
                c = grid;
            px[r * stride + x] = c;
        }
        /* the line itself, joined to the last column */
        int a = prev_y < 0 ? y : prev_y, lo = a < y ? a : y, hi = a < y ? y : a;
        for (int r = lo; r <= hi; r++)
            px[r * stride + x] = line;
        if (y + 1 < CURVE_H && lo == hi)
            px[(y + 1) * stride + x] = line;            /* two pixels thick */
        prev_y = y;
    }
    lv_obj_invalidate(eqs.canvas);
}

static int eq_knob_y(int g)
{
    return TRACK_Y + TRACK_H / 2 - g * (TRACK_H / 2) / EQ_MAX_DB;
}

/* Everything that follows the gains and the selection */
static void eq_update(void)
{
    eq_draw_curve();
    for (int b = 0; b < EQ_BANDS; b++) {
        bool sel = b == eqs.band;
        int x = EQ_X + b * EQ_COL + EQ_COL / 2, y0 = eq_knob_y(0), y = eq_knob_y(eq_gains[b]);
        lv_color_t c = sel ? theme.fg : eq_gains[b] ? theme.second : theme.muted;
        lv_obj_set_pos(eqs.fill[b], x - 1, y < y0 ? y : y0);
        lv_obj_set_size(eqs.fill[b], 3, (y < y0 ? y0 - y : y - y0) + 1);
        lv_obj_set_style_bg_color(eqs.fill[b], c, 0);
        lv_obj_set_pos(eqs.knob[b], x - KNOB / 2, y - KNOB / 2);
        lv_obj_set_style_bg_color(eqs.knob[b], c, 0);
        lv_obj_set_style_border_width(eqs.knob[b], sel ? 0 : 1, 0);
        char v[8];
        snprintf(v, sizeof(v), "%+d", eq_gains[b]);
        lv_label_set_text(eqs.gain[b], eq_gains[b] ? v : "0");
        lv_obj_set_style_text_color(eqs.gain[b], sel ? theme.fg : theme.muted, 0);
        lv_obj_set_style_text_color(eqs.freq[b], sel ? theme.fg : theme.muted, 0);
    }
    char line[64], name[24];
    eq_name(eq_sel, name, sizeof(name));
    snprintf(line, sizeof(line), "%s%s%s", name,
             eq_sel >= EQ_CUSTOM && eq_changed(eq_sel) ? " \xc2\xb7 edited" : "",
             eq_enabled ? "" : " \xc2\xb7 off");
    lv_label_set_text(eqs.info, line);
    lv_obj_set_style_text_color(eqs.info, eq_enabled ? theme.fg : theme.muted, 0);
    /* the EQ turns the level down so nothing clips; through the jack the
     * DAC's volume gives it back, as far as the volume has room to go up
     * (Bluetooth headphones' volume isn't ours to raise) */
    int tenths = eq_enabled ? (int)lroundf(eq_headroom_db(eq_gains, 44100) * 10) : 0;
    int db, lo, hi;
    if (tenths && !output_is_bluetooth() && los_volume(&db, &lo, &hi))
        tenths = tenths > (hi - db) * 10 ? tenths - (hi - db) * 10 : 0;
    if (!eq_enabled)
        line[0] = '\0';                 /* off: no level to tell (and room for the name) */
    else if (tenths == 0)
        snprintf(line, sizeof(line), "LEVEL KEPT");
    else
        snprintf(line, sizeof(line), "LEVEL -%d.%d DB", tenths / 10, tenths % 10);
    lv_label_set_text(eqs.master, line);
    lv_obj_align(eqs.master, LV_ALIGN_TOP_RIGHT, -20, LOS_HEADER_H + 5);
}

static int eq_clamp(int g)
{
    return g < -EQ_MAX_DB ? -EQ_MAX_DB : g > EQ_MAX_DB ? EQ_MAX_DB : g;
}

/* The saved EQs, and which is chosen. Saved before there were Custom
 * EQs: the bands as a preset if they are one, or else as Custom EQ 1. */
static void eq_load_curves(const los_settings *st)
{
    for (int c = 0; c < EQ_CUSTOM; c++)
        for (int b = 0; b < EQ_BANDS; b++)
            eq_curves[c][b] = eq_clamp(st->eq_custom[c][b]);
    for (int e = 0; e < st->eq_n_edited; e++)
        for (int p = 0; p < EQ_PRESETS; p++)
            if (!strcmp(st->eq_edited[e].name, eq_presets[p].name))
                for (int b = 0; b < EQ_BANDS; b++)
                    eq_curves[EQ_CUSTOM + p][b] = eq_clamp(st->eq_edited[e].gains[b]);
    int sel = -1;
    if (!strncmp(st->eq_sel, "custom", 6) && st->eq_sel[6] >= '1' && st->eq_sel[6] <= '3')
        sel = st->eq_sel[6] - '1';
    for (int p = 0; p < EQ_PRESETS && sel < 0 && st->eq_sel[0]; p++)
        if (!strcmp(st->eq_sel, eq_presets[p].name))
            sel = EQ_CUSTOM + p;
    if (sel < 0) {
        int p = eq_preset_now();
        sel = p >= 0 ? EQ_CUSTOM + p : 0;
        if (p < 0)
            memcpy(eq_curves[0], eq_gains, sizeof(eq_gains));
    }
    eq_choose(sel);
}

static void build_eq(void)
{
    header = header_create(screen_obj, "EQ");
    eqs.info = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(eqs.info, theme.small, 0);
    lv_obj_set_pos(eqs.info, 20, LOS_HEADER_H + 2);
    eqs.master = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(eqs.master, theme.label, 0);
    lv_obj_set_style_text_color(eqs.master, theme.muted, 0);
    lv_obj_set_style_text_letter_space(eqs.master, 1, 0);
    lv_obj_align(eqs.master, LV_ALIGN_TOP_RIGHT, -20, LOS_HEADER_H + 5);

    lv_draw_buf_init(&eq_curve_buf, EQ_W, CURVE_H, LV_COLOR_FORMAT_RGB565, EQ_W * 2,
                     eq_curve_px, sizeof(eq_curve_px));
    eqs.canvas = lv_canvas_create(screen_obj);
    lv_canvas_set_draw_buf(eqs.canvas, &eq_curve_buf);
    lv_obj_set_pos(eqs.canvas, EQ_X, CURVE_Y);

    for (int b = 0; b < EQ_BANDS; b++) {
        int x = EQ_X + b * EQ_COL + EQ_COL / 2;
        lv_obj_t *track = theme_box(screen_obj);
        lv_obj_set_pos(track, x, TRACK_Y);
        lv_obj_set_size(track, 1, TRACK_H + 1);
        lv_obj_set_style_bg_color(track, theme.line, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        eqs.fill[b] = theme_box(screen_obj);
        lv_obj_set_style_bg_opa(eqs.fill[b], LV_OPA_COVER, 0);
        eqs.knob[b] = theme_box(screen_obj);
        lv_obj_set_size(eqs.knob[b], KNOB, KNOB);
        lv_obj_set_style_radius(eqs.knob[b], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(eqs.knob[b], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(eqs.knob[b], theme.bg, 0);
        for (int k = 0; k < 2; k++) {
            lv_obj_t *l = lv_label_create(screen_obj);
            lv_obj_set_style_text_font(l, theme.mono, 0);
            lv_obj_set_width(l, EQ_COL);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, EQ_X + b * EQ_COL, k ? FREQ_Y : GAIN_Y);
            if (k) {
                lv_label_set_text(l, eq_band_names[b]);
                eqs.freq[b] = l;
            } else {
                eqs.gain[b] = l;
            }
        }
    }
    lv_obj_t *hint = lv_label_create(screen_obj);
    lv_label_set_text(hint, "\xe2\x97\x80 \xe2\x96\xb6 band \xc2\xb7 wheel level \xc2\xb7 Menu presets");
    lv_obj_set_style_text_font(hint, theme.mono, 0);
    lv_obj_set_style_text_color(hint, theme.faint, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    eq_update();
}

/* Opening the EQ: music to hear it on (the current song, or the first) */
static void player_play(int album, int track);
static void player_toggle(void);
static void eq_open(void)
{
    if (player.state != 1) {
        if (player.album >= 0)
            player_toggle();
        else if (library_albums() > 0)
            player_play(0, 0);
    }
    push(SCR_EQ, 0);
}

/* Keys on the EQ screen; false for the ones it leaves (Back) */
static bool eq_key(uint32_t key)
{
    if (stack[depth - 1].screen != SCR_EQ)
        return false;
    switch (key) {
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        eqs.band = (eqs.band + (key == LV_KEY_RIGHT ? 1 : EQ_BANDS - 1)) % EQ_BANDS;
        eq_update();
        return true;
    case LV_KEY_UP:
    case LV_KEY_DOWN: {
        int g = eq_gains[eqs.band] + (key == LV_KEY_DOWN ? 1 : -1);   /* clockwise: more */
        eq_gains[eqs.band] = g < -EQ_MAX_DB ? -EQ_MAX_DB : g > EQ_MAX_DB ? EQ_MAX_DB : g;
        eq_curves[eq_sel][eqs.band] = eq_gains[eqs.band];             /* kept with that EQ */
        eq_enabled = true;                                           /* moving one turns it on */
        eq_apply();
        settings_changed();
        eq_update();
        return true;
    }
    case LV_KEY_ENTER:
        if (player.album < 0 && library_albums() > 0)
            player_play(0, 0);
        else
            player_toggle();
        refresh();
        return true;
    case LV_KEY_HOME:
        push(SCR_EQ_PRESETS, 0);
        return true;
    default:
        return false;
    }
}

/* Menu on the EQ screen: EQ on / off; Reset, when the chosen EQ is
 * changed from its own; then Custom EQ 1 to 3 and the presets (the one set
 * now says so, an edited preset says that). Play on one takes it and goes
 * back. */
static void build_eq_presets(void)
{
    header = header_create(screen_obj, "Presets");
    list_begin();
    text_row("EQ", eq_enabled ? "ON" : "OFF");
    char name[24], reset[40];
    if (eq_changed(eq_sel)) {
        eq_name(eq_sel, name, sizeof(name));
        snprintf(reset, sizeof(reset), "Reset %s", name);
        text_row(reset, NULL);
    }
    for (int c = 0; c < EQ_CHOICES; c++) {
        eq_name(c, name, sizeof(name));
        text_row(name, c == eq_sel ? "SET" : c >= EQ_CUSTOM && eq_changed(c) ? "EDITED" : NULL);
    }
}

/* ---- about ---- */

static void build_about(void)
{
    header = header_create(screen_obj, "About");
    char lines[7][96];
    int n = 0, songs = 0;
    for (int i = 0; i < library_albums(); i++)
        songs += library_album(i)->ntracks;
    snprintf(lines[n++], 96, "Lossless OS %s (%s)", LOS_VERSION, LOS_BUILD);
    snprintf(lines[n++], 96, "%d albums, %d songs", library_albums(), songs);
    uint64_t used, total;
    if (los_storage(&used, &total))
        snprintf(lines[n++], 96, "Card: %u.%u GB free of %u.%u GB",
                 (unsigned)((total - used) / 1000000000u), (unsigned)((total - used) / 100000000u % 10),
                 (unsigned)(total / 1000000000u), (unsigned)(total / 100000000u % 10));
    int pct;
    bool charging, usb;
    if (los_battery(&pct, &charging, &usb))
        snprintf(lines[n++], 96, "Battery: %d%%%s", pct, charging ? ", charging" : usb ? ", on USB" : "");
    snprintf(lines[n++], 96, "Saving to the card: %s", los_writes_ok() ? "on" : "off");
    snprintf(lines[n++], 96, "Open source: Apache-2.0");
    int lh = lv_font_get_line_height(theme.small) + 8;
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = lv_label_create(screen_obj);
        lv_label_set_text(l, lines[i]);
        lv_obj_set_style_text_font(l, i == 0 ? theme.title_small : theme.small, 0);
        lv_obj_set_style_text_color(l, i == 0 ? theme.fg : theme.second, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(l, 20, LOS_HEADER_H + 14 + i * lh + (i ? 6 : 0));
        lv_obj_set_size(l, LOS_W - 40, lv_font_get_line_height(i == 0 ? theme.title_small : theme.small));
    }
}

/* ---- setting the clock ---- */

static lv_obj_t *list_footer(const char *text);

/* Rows: year, month, day, hour, minute. Play on a row changes it: the
 * wheel moves its value, Play sets the clock, Back leaves it as it was. */
static struct {
    bool editing;
    los_date d;
} clk;

static void clock_load(void)
{
    int64_t now;
    if (los_clock_get(&now)) {
        clk.d = date_from_seconds(now);
    } else {
        los_date d = { 2026, 1, 1, 12, 0, 0, 0 };
        clk.d = d;
    }
    clk.d.second = 0;
}

static void clock_row(const char *name, const char *value, bool editing)
{
    lv_obj_t *row = row_begin(36);
    int b = centred_baseline(theme.row, 36);
    row_label(row, name, theme.row, theme.fg, 20, top_for_baseline(theme.row, b), 140);
    lv_obj_t *v = row_label(row, value, theme.row, editing ? theme.accent : theme.second, 160,
                            top_for_baseline(theme.row, b), 140);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    if (editing)
        lv_obj_set_style_text_color(v, theme.accent, LV_STATE_CHECKED);
}

static void build_clock(void)
{
    header = header_create(screen_obj, "Date & time");
    int field = stack[depth - 1].selected;
    char v[24];
    list_begin();
    snprintf(v, sizeof(v), "%d", clk.d.year);
    clock_row("Year", v, clk.editing && field == 0);
    clock_row("Month", month_names[clk.d.month - 1], clk.editing && field == 1);
    snprintf(v, sizeof(v), "%d", clk.d.day);
    clock_row("Day", v, clk.editing && field == 2);
    if (opt.clock24)
        snprintf(v, sizeof(v), "%d", clk.d.hour);
    else
        snprintf(v, sizeof(v), "%d %s", clk.d.hour % 12 ? clk.d.hour % 12 : 12, clk.d.hour < 12 ? "AM" : "PM");
    clock_row("Hour", v, clk.editing && field == 3);
    snprintf(v, sizeof(v), "%02d", clk.d.minute);
    clock_row("Minute", v, clk.editing && field == 4);
    list_footer(clk.editing ? "Wheel changes it \xc2\xb7 Play sets \xc2\xb7 Back cancels"
                            : "Play changes the highlighted one");
}

static int wrap(int v, int lo, int hi)
{
    return v < lo ? hi : v > hi ? lo : v;
}

/* A key while a value is being changed; false if it isn't */
static bool clock_key(uint32_t key)
{
    if (stack[depth - 1].screen != SCR_CLOCK || !clk.editing)
        return false;
    int field = stack[depth - 1].selected, step = 0;
    switch (key) {
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT: step = +1; break;
    case LV_KEY_UP:
    case LV_KEY_LEFT:  step = -1; break;
    case LV_KEY_ENTER:
        los_clock_set(seconds_from_date(&clk.d));
        clk.editing = false;
        show();
        return true;
    case LV_KEY_ESC:
    case LV_KEY_HOME:
        clk.editing = false;
        clock_load();
        show();
        return true;
    default:
        return true;
    }
    los_date *d = &clk.d;
    switch (field) {
    case 0: d->year = wrap(d->year + step, 2020, 2099); break;
    case 1: d->month = wrap(d->month + step, 1, 12); break;
    case 2: d->day = wrap(d->day + step, 1, days_in_month(d->year, d->month)); break;
    case 3: d->hour = wrap(d->hour + step, 0, 23); break;
    case 4: d->minute = wrap(d->minute + step, 0, 59); break;
    }
    if (d->day > days_in_month(d->year, d->month))
        d->day = days_in_month(d->year, d->month);
    show();
    return true;
}

/* What each row of the Bluetooth screen does, as built; BTR_KNOWN + i is
 * the i-th pair of headphones we know */
enum { BTR_POWER, BTR_SEARCH, BTR_KNOWN = 100 };

/* The option panels (below): play mode, or a pair of headphones */
enum { OPTS_PLAY, OPTS_DEVICE };
static void opts_open(int kind, int device);
static int bt_rows[2 + BT_KNOWN + 2], bt_nrows;
static unsigned bt_seen;          /* the status the screen was built from */

static unsigned bt_signature(const bt_status *b)
{
    unsigned h = (unsigned)b->state * 31u + (unsigned)b->on * 7u + (unsigned)b->ndevices * 131u +
                 (unsigned)b->nknown * 17u + (unsigned)(b->peer_known + 1) * 1009u;
    for (const char *c = b->message; *c; c++)
        h = h * 33u + (unsigned char)*c;
    for (int i = 0; i < b->ndevices; i++)
        h = h * 33u + (unsigned char)b->devices[i].name[0];
    for (int i = 0; i < b->nknown; i++)
        h = h * 33u + (unsigned char)b->known[i].name[0] + b->known[i].addr[0];
    return h;
}

/* A line of status under a list */
static lv_obj_t *list_footer(const char *text)
{
    int h = list_height();
    lv_obj_set_height(list.cont, h - 22);
    lv_obj_t *l = lv_label_create(screen_obj);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, theme.mono, 0);
    lv_obj_set_style_text_color(l, theme.muted, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, 20, LOS_HEADER_H + 1 + h - 18);
    lv_obj_set_size(l, LOS_W - 40, lv_font_get_line_height(theme.mono));
    return l;
}

static const char *bt_state_word(enum bt_state st)
{
    switch (st) {
    case BT_STARTING:   return "STARTING";
    case BT_SCANNING:   return "SEARCHING";
    case BT_CONNECTING: return "CONNECTING";
    case BT_CONNECTED:  return "CONNECTED";
    case BT_STREAMING:  return "PLAYING";
    case BT_FAILED:     return "FAILED";
    default:            return NULL;
    }
}

static void build_bluetooth(void)
{
    header = header_create(screen_obj, "Bluetooth");
    bt_status b;
    bluetooth_status(&b);
    bt_seen = bt_signature(&b);
    bt_nrows = 0;
    list_begin();
    text_row("Bluetooth", b.on ? "ON" : "OFF");
    bt_rows[bt_nrows++] = BTR_POWER;
    bool up = b.on && b.state >= BT_READY && b.state != BT_FAILED;
    if (b.on) {
        /* the headphones we know, most recent first */
        for (int k = 0; k < b.nknown; k++) {
            bool them = b.peer_known == k && b.state >= BT_CONNECTING;
            text_row(b.known[k].name, them ? bt_state_word(b.state) : NULL);
            bt_rows[bt_nrows++] = BTR_KNOWN + k;
        }
    }
    if (up) {
        text_row("Pair new device", NULL);
        bt_rows[bt_nrows++] = BTR_SEARCH;
    }
    char line[96];
    if (!b.on)
        snprintf(line, sizeof(line), b.nknown ? "Off \xc2\xb7 reconnects to %s when on" : "Off",
                 b.nknown ? b.known[0].name : "");
    else if (b.state == BT_STREAMING && b.rate)
        snprintf(line, sizeof(line), "%s \xc2\xb7 SBC %d.%d kHz", b.peer, b.rate / 1000,
                 b.rate % 1000 / 100);
    else if (b.nknown && b.state >= BT_READY && b.state <= BT_SCANNING)
        snprintf(line, sizeof(line), "Play: connect, disconnect or forget");
    else
        snprintf(line, sizeof(line), "%s", b.message);
    list_footer(line);
}

static void build_bt_search(void)
{
    header = header_create(screen_obj, "Headphones");
    bt_status b;
    bluetooth_status(&b);
    bt_seen = bt_signature(&b);
    list_begin();
    for (int i = 0; i < b.ndevices; i++) {
        char addr[24];
        const uint8_t *a = b.devices[i].addr;
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
        text_row(b.devices[i].name[0] ? b.devices[i].name : addr, NULL);
    }
    /* while this screen is open the player is visible too: a car adds it
     * from its own screen, as it would a phone */
    list_footer(b.state == BT_SCANNING ? "Searching, and visible as Lossless" :
                b.ndevices ? "Play to connect \xc2\xb7 visible as Lossless" :
                "None found. Play searches \xc2\xb7 visible as Lossless");
}

/* Pairing with a car or a phone: the code both screens show, Play to
 * pair, Back not to; or a code to type on their keyboard */
static lv_obj_t *bt_pair_box;
static void screen_light(int percent);
static uint32_t last_key_at;
static bool bt_pair_passkey;
static uint32_t bt_pair_code;

static void bt_pair_close(void)
{
    if (bt_pair_box)
        lv_obj_delete(bt_pair_box);
    bt_pair_box = NULL;
}

static void bt_pair_open(const bt_status *b)
{
    bt_pair_close();
    bt_pair_passkey = b->passkey;
    bt_pair_code = b->code;
    screen_light(100);
    last_key_at = lv_tick_get();
    bt_pair_box = theme_box(lv_layer_top());
    lv_obj_set_size(bt_pair_box, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(bt_pair_box, theme.bg, 0);
    lv_obj_set_style_bg_opa(bt_pair_box, LV_OPA_COVER, 0);
    header_create(bt_pair_box, NULL);
    char line[64];
    lv_obj_t *t = lv_label_create(bt_pair_box);
    snprintf(line, sizeof(line), "Pair with %s?", b->confirm_name);
    lv_label_set_text(t, line);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, LOS_W - 40);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t, theme.title_small, 0);
    lv_obj_set_style_text_color(t, theme.fg, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 14);
    char digits[8];
    snprintf(digits, sizeof(digits), "%06u", (unsigned)(b->code % 1000000));
    lv_obj_t *d = dots_create(bt_pair_box, 3, theme.fg);
    dots_set_text(d, digits);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 56);
    lv_obj_t *q = lv_label_create(bt_pair_box);
    lv_label_set_text(q, b->passkey ? "Type this code on it" : "Is this code on its screen too?");
    lv_obj_set_style_text_font(q, theme.small, 0);
    lv_obj_set_style_text_color(q, theme.muted, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 120);
    lv_obj_t *h = lv_label_create(bt_pair_box);
    lv_label_set_text(h, b->passkey ? "Back closes" : "Play pairs \xc2\xb7 Back cancels");
    lv_obj_set_style_text_font(h, theme.mono, 0);
    lv_obj_set_style_text_color(h, theme.faint, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* The Bluetooth screens follow the status as it changes */
static void bt_screen_watch(void)
{
    static uint32_t last;
    if (lv_tick_elaps(last) < 250)
        return;
    last = lv_tick_get();
    bluetooth_visible(stack[depth - 1].screen == SCR_BT_SEARCH);
    {
        bt_status pb;
        bluetooth_status(&pb);
        bool want = pb.confirm || pb.passkey;
        if (want && (!bt_pair_box || pb.code != bt_pair_code || pb.passkey != bt_pair_passkey))
            bt_pair_open(&pb);
        else if (!want && bt_pair_box)
            bt_pair_close();
    }
    bt_status b;
    bluetooth_status(&b);
    header_set_bluetooth(b.state >= BT_CONNECTED ? 2 : b.on ? 1 : 0);
    enum screen scr = stack[depth - 1].screen;
    unsigned sig = bt_signature(&b);
    if ((scr == SCR_BLUETOOTH || scr == SCR_BT_SEARCH || scr == SCR_SETTINGS) && sig != bt_seen && !opts)
        show();
}

static void build_soon(int which)
{
    header = header_create(screen_obj, soon_titles[which]);
    lv_obj_t *t = theme_tag(screen_obj, "Coming soon", theme.muted);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
}

/* ---- refresh (once a second, and after keys) ------------------------------------------ */

static void refresh(void)
{
    if (header)
        header_set_playing(header, player.state);
    const los_track *t = current_track();
    mini_set();
    if (np_time && t) {
        char buf[64];
        int len = track_length(t);
        int at = scrub.active ? scrub.sec : player.elapsed;
        fmt_time(buf, sizeof(buf), at);
        dots_set_text(np_time, buf);
        buf[0] = '-';
        fmt_time(buf + 1, sizeof(buf) - 1, len > at ? len - at : 0);
        dots_set_text(np_left, buf);
        lv_obj_update_layout(np_left);    /* its new width, before placing it */
        lv_obj_set_x(np_left, LOS_W - 12 - lv_obj_get_width(np_left));
        segbar_set(np_bar, at, len);
        snprintf(buf, sizeof(buf), "%d OF %d", queue_pos + 1, queue_len);
        state_icon_set(np_icon, player.state);
        lv_label_set_text(np_count, buf);
        lv_label_set_text(np_title, t->title);
        const char *rep = repeat_mode == REPEAT_ALL ? "REPEAT" :
                          repeat_mode == REPEAT_ONE ? "REPEAT 1" : "";
        snprintf(buf, sizeof(buf), "%s%s%s", shuffle ? "SHUFFLE" : "",
                 shuffle && rep[0] ? "  \xc2\xb7  " : "", rep);
        lv_label_set_text(np_modes, buf);
    }
}

/* Four times a second: the time from what has been heard, and on to the
 * next song when one ends (or can't be played) */
/* ---- volume (the player's buttons change it; the header shows it) ---------------------- */

#define VOLUME_FLASH_MS 2000   /* the dB shows this long after the last change */

static lv_obj_t *lock_scr;        /* the lock screen, below */
static bool keys_locked;          /* key lock, below */
static void lock_update(void);



/* The screen dims after a while without a key, then goes dark; the first
 * key after that only wakes it. The lock screen dims sooner. */
#define LOCK_DIM_MS    10000
#define LOCK_OFF_MS    (5 * 60 * 1000)     /* after dimming */
#define SCREEN_OFF_MS  30000               /* after dimming, elsewhere */
#define DIM_LEVEL      10                  /* percent of the chosen brightness */
static int light = 100;                    /* 100 lit, DIM_LEVEL dim, 0 dark */
#ifdef LOS_HOST
static lv_obj_t *shade;                    /* the simulator has no backlight: shade it */
#endif

static void screen_light(int percent)
{
    if (percent == light)
        return;
    light = percent;
    int b = opt.brightness * percent / 100;
    los_backlight(percent && b < 2 ? 2 : b);
#ifdef LOS_HOST
    if (!shade) {
        shade = theme_box(lv_layer_top());
        lv_obj_set_size(shade, LOS_W, LOS_H);
        lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    }
    lv_obj_move_foreground(shade);
    lv_obj_set_style_bg_opa(shade, percent >= 100 ? LV_OPA_TRANSP :
                            percent > 0 ? LV_OPA_80 : LV_OPA_COVER, 0);
#endif
}

/* One volume: the DAC's, and the headphones' while they're connected
 * (each change goes to both; theirs comes back when they change it) */
static struct {
    bool known;
    int db, lo, hi;
    uint32_t changed_at;
} vol;

/* Settings to remember: saved a few seconds after the last change */
#define SETTINGS_SAVE_MS 3000
static uint32_t settings_changed_at;
static void settings_changed(void)
{
    settings_changed_at = lv_tick_get() | 1;
}

static void volume_changed(void)
{
    settings_changed();
    vol.changed_at = lv_tick_get();
    if (!vol.changed_at)
        vol.changed_at = 1;
}

void los_app_volume(int db)
{
    last_key_at = lv_tick_get();
    if (light < 100) {
        screen_light(100);
        lock_update();
    }
    if (!vol.known)
        return;
    int v = vol.db + db;
    vol.db = v < vol.lo ? vol.lo : v > vol.hi ? vol.hi : v;
    los_volume_set(vol.db);
    bluetooth_set_volume(vol.db, vol.lo, vol.hi);
    volume_changed();
}

static void player_toggle(void);
static void player_skip(int dir);
static void show(void);
static void refresh(void);
static void bt_screen_watch(void);


static los_settings saved;      /* as loaded at start */
void los_app_before_off(void);

/* The battery in the header every few seconds; nearly empty and not on
 * USB power: save, say so, switch off */
#define BATTERY_EMPTY 2
static void battery_watch(void)
{
    static uint32_t last;
    if (last && lv_tick_elaps(last) < 5000)
        return;
    last = lv_tick_get() | 1;
    int percent;
    bool charging, usb;
    if (!los_battery(&percent, &charging, &usb)) {
        header_set_battery(-1, false);
        return;
    }
    header_set_battery(percent, charging);
    if (percent <= BATTERY_EMPTY && !usb && !charging) {
        playback_pause(true);
        los_app_before_off();
        lv_obj_t *t = theme_tag(lv_layer_top(), "Battery empty", theme.fg);
        lv_obj_center(t);
        lv_refr_now(NULL);
        los_sleep_ms(1500);
        los_power_off();
    }
}

static void save_settings(void)
{
    bt_status b;
    bluetooth_status(&b);
    static los_settings st;
    memset(&st, 0, sizeof(st));
    st.have_volume = true;
    st.volume_db = vol.db;
    st.repeat = repeat_mode;
    st.shuffle = shuffle;
    st.bluetooth = b.on;
    st.brightness = opt.brightness;
    st.dim_s = opt.dim_s;
    st.clock24 = opt.clock24;
    st.wheel_volume = opt.wheel_volume;
    st.eq = eq_enabled;
    st.usb = opt.usb;
    memcpy(st.eq_gains, eq_gains, sizeof(st.eq_gains));
    memcpy(st.eq_custom, eq_curves, sizeof(st.eq_custom));
    for (int c = EQ_CUSTOM; c < EQ_CHOICES && st.eq_n_edited < 12; c++)
        if (eq_changed(c)) {
            snprintf(st.eq_edited[st.eq_n_edited].name, sizeof(st.eq_edited[0].name), "%s",
                     eq_presets[c - EQ_CUSTOM].name);
            memcpy(st.eq_edited[st.eq_n_edited].gains, eq_curves[c], sizeof(eq_curves[c]));
            st.eq_n_edited++;
        }
    if (eq_sel < EQ_CUSTOM)
        snprintf(st.eq_sel, sizeof(st.eq_sel), "custom%d", eq_sel + 1);
    else
        snprintf(st.eq_sel, sizeof(st.eq_sel), "%s", eq_presets[eq_sel - EQ_CUSTOM].name);
    settings_save(&st);
    settings_changed_at = 0;
}

/* ---- resume: what was playing, saved now and then, played paused at start ---- */

#define RESUME_EVERY_MS 60000
static uint32_t resume_saved_at;
static int resume_album = -2, resume_track = -1, resume_state = -1;

static void resume_path(char *p, size_t n)
{
    snprintf(p, n, "%s/.lossless/state/resume.txt", los_card_path());
}

static void save_resume(void)
{
    char path[200], dir[200], text[900];
    resume_saved_at = lv_tick_get() | 1;
    resume_album = player.album;
    resume_track = player.track;
    resume_state = player.state;
    if (!los_writes_ok() || player.album < 0)
        return;
    const los_album *a = library_album(player.album);
    int n = snprintf(text, sizeof(text), "from=%d\nkey=%s\ndir=%s\nfile=%s\nsec=%d\n", queue_from,
                     queue_key, a->dir, a->tracks[player.track].file, player.elapsed);
    snprintf(dir, sizeof(dir), "%s/.lossless", los_card_path());
    los_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/.lossless/state", los_card_path());
    los_mkdir(dir);
    resume_path(path, sizeof(path));
    los_file_save(path, text, (size_t)n);
}

/* When the song or play state changed, and a minute into playing */
static void resume_watch(void)
{
    if (player.album < 0)
        return;
    bool changed = player.album != resume_album || player.track != resume_track ||
                   player.state != resume_state;
    if (changed || (player.state == 1 && lv_tick_elaps(resume_saved_at) >= RESUME_EVERY_MS))
        save_resume();
}

static void load_resume(void)
{
    static song_ref list[QUEUE_MAX];
    char path[200], text[1000], key[256] = "", dir[256] = "", file[160] = "";
    int from = -1, sec = 0;
    resume_path(path, sizeof(path));
    void *f = los_file_open(path);
    if (!f) {
        strcat(path, ".tmp");
        f = los_file_open(path);
    }
    if (!f)
        return;
    size_t got = los_file_read(f, text, sizeof(text) - 1);
    los_file_close(f);
    text[got] = '\0';
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "from")) from = atoi(eq);
        else if (!strcmp(line, "key")) snprintf(key, sizeof(key), "%s", eq);
        else if (!strcmp(line, "dir")) snprintf(dir, sizeof(dir), "%s", eq);
        else if (!strcmp(line, "file")) snprintf(file, sizeof(file), "%s", eq);
        else if (!strcmp(line, "sec")) sec = atoi(eq);
    }
    /* the list it was playing from, as it is now */
    int n = 0;
    if (from == FROM_ALBUM) {
        for (int i = 0; i < library_albums(); i++)
            if (!strcmp(library_album(i)->dir, key)) {
                for (int t = 0; t < library_album(i)->ntracks && n < QUEUE_MAX; t++) {
                    list[n].album = i;
                    list[n].track = t;
                    n++;
                }
                break;
            }
    } else if (from == FROM_SONGS) {
        n = collect_all_songs(list);
    } else if (from == FROM_ARTIST) {
        for (int i = 0; i < nartists; i++)
            if (!strcmp(artist_names[i], key))
                n = collect_artist_songs(i, list);
    } else if (from == FROM_PLAYLIST) {
        int pl = playlist_find(key);
        if (pl >= 0)
            n = collect_playlist(pl, list);
    }
    int start = -1;
    for (int i = 0; i < n && start < 0; i++) {
        const los_album *a = library_album(list[i].album);
        if (!strcmp(a->dir, dir) && !strcmp(a->tracks[list[i].track].file, file))
            start = i;
    }
    if (start < 0)
        return;      /* the music changed: start fresh */
    queue_from = from;
    snprintf(queue_key, sizeof(queue_key), "%s", key);
    queue_len = n;
    memcpy(queue_orig, list, (size_t)n * sizeof(song_ref));
    queue_arrange(start);
    queue_go(queue_pos);
    player.state = 2;                  /* paused, ready: Play carries on */
    failures = 0;
    int len = track_length(current_track());
    if (sec < 0 || sec >= len)
        sec = 0;
    player.elapsed = sec;
    start_current_from(sec);
    resume_album = player.album;
    resume_track = player.track;
    resume_state = player.state;
}

/* Before the player switches off: what changed is saved */
void los_app_before_off(void)
{
    if (settings_changed_at)
        save_settings();
    save_resume();
}

/* ---- the card describes the player, for computers ---- */

/* /.lossless/player.json: what's on this card and what plays, for the
 * desktop app (or anything) to set itself up without asking. The player
 * finds music anywhere on the card; "music_folder" and "music_layout"
 * only suggest where a computer might put new music. Written at
 * start and when the card comes back from a computer, only if it changed.
 * docs/card-layout.md describes it. */
static void write_player_json(void)
{
    static char text[1400], old[1400];
    char path[200];
    if (!los_writes_ok())
        return;
    const char *card = los_card_path();
    int n = snprintf(text, sizeof(text),
        "{\n"
        "  \"format\": \"lossless-player\",\n"
        "  \"format_version\": 1,\n"
        "  \"os\": \"Lossless OS\",\n"
        "  \"os_version\": \"%s\",\n"
        "  \"os_build\": \"%s\",\n"
        "  \"player\": \"%s\",\n"
        "  \"music_anywhere\": true,\n"
        "  \"music_folder\": \"Music\",\n"
        "  \"music_layout\": \"Artist/Album\",\n"
        "  \"playlists_folder\": \"Playlists\",\n"
        "  \"playlist_extensions\": [\".m3u\", \".m3u8\"],\n"
        "  \"audio_formats\": [\"flac\", \"alac\", \"wav\", \"aiff\", \"mp3\", \"vorbis\", \"opus\"],\n"
        "  \"audio_extensions\": [\".flac\", \".m4a\", \".wav\", \".aif\", \".aiff\", \".aifc\",\n"
        "                       \".mp3\", \".ogg\", \".oga\", \".opus\"],\n"
        "  \"max_sample_rate\": 384000,\n"
        "  \"max_bits\": 32,\n"
        "  \"max_channels\": 2,\n"
        "  \"gapless\": true,\n"
        "  \"tags\": [\"vorbis_comment\", \"id3v2\", \"id3v1\", \"mp4\"],\n"
        "  \"cover_files\": [\"cover.jpg\", \"folder.jpg\"],\n"
        "  \"cover_embedded\": true,\n"
        "  \"cover_image\": \"jpeg_baseline\",\n"
        "  \"private_folder\": \".lossless\"\n"
        "}\n",
        LOS_VERSION, LOS_BUILD, los_player_name());
    snprintf(path, sizeof(path), "%s/.lossless/player.json", card);
    void *f = los_file_open(path);
    if (f) {
        size_t got = los_file_read(f, old, sizeof(old) - 1);
        los_file_close(f);
        old[got] = '\0';
        if (got == (size_t)n && !memcmp(old, text, (size_t)n))
            return;                         /* as it is */
    }
    char dir[200];
    snprintf(dir, sizeof(dir), "%s/.lossless", card);
    los_mkdir(dir);
    los_file_save(path, text, (size_t)n);
}

/* ---- USB: the card on a computer ---- */

/* A computer wants the card: stop using it, hand it over, and show that
 * until the computer ejects it or the cable comes out; then take it back,
 * read the library again and carry on where we were (paused). */
static lv_obj_t *usb_scr;
static bool usb_away;        /* the card is the computer's */
static bool usb_done;        /* taken back with the cable still in: not again until it's out */

static void usb_open(void)
{
    usb_scr = theme_box(lv_layer_top());
    lv_obj_set_size(usb_scr, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(usb_scr, theme.bg, 0);
    lv_obj_set_style_bg_opa(usb_scr, LV_OPA_COVER, 0);
    header_create(usb_scr, NULL);       /* the mark, as on Home */
    lv_obj_t *d = dots_create(usb_scr, 3, theme.fg);
    dots_set_text(d, "USB");
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 40);
    lv_obj_t *t = lv_label_create(usb_scr);
    lv_label_set_text(t, "Connected to a computer");
    lv_obj_set_style_text_font(t, theme.title_small, 0);
    lv_obj_set_style_text_color(t, theme.fg, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 112);
    lv_obj_t *m = lv_label_create(usb_scr);
    lv_label_set_text(m, "Eject it on the computer, then unplug.\nThe music comes back after.");
    lv_obj_set_style_text_font(m, theme.small, 0);
    lv_obj_set_style_text_color(m, theme.second, 0);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 142);
}

/* Setting "Ask": a computer wants the card. A screen of its own, so it's
 * plain what's happening: Transfer files (the card goes to the computer)
 * or Charge only (the music plays on). The wheel picks, Play chooses,
 * Back is Charge only. It goes if the cable comes out first. */
#define USB_CHOICES 3
static lv_obj_t *usb_ask_box, *usb_ask_rows[USB_CHOICES];
static int usb_ask_sel;

static void usb_ask_show(void)
{
    for (int i = 0; i < USB_CHOICES; i++) {
        bool on = i == usb_ask_sel;
        lv_obj_set_style_bg_opa(usb_ask_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *l = lv_obj_get_child(usb_ask_rows[i], 0);
        lv_obj_set_style_text_color(l, on ? theme.fg : theme.second, 0);
    }
}

static void usb_ask_open(void)
{
    static const char *const choices[USB_CHOICES] = { "Transfer files", "Use as DAC", "Charge only" };
    usb_ask_box = theme_box(lv_layer_top());
    lv_obj_set_size(usb_ask_box, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(usb_ask_box, theme.bg, 0);
    lv_obj_set_style_bg_opa(usb_ask_box, LV_OPA_COVER, 0);
    header_create(usb_ask_box, NULL);
    lv_obj_t *d = dots_create(usb_ask_box, 3, theme.fg);
    dots_set_text(d, "USB");
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 6);
    lv_obj_t *t = lv_label_create(usb_ask_box);
    lv_label_set_text(t, "Connected to a computer");
    lv_obj_set_style_text_font(t, theme.title_small, 0);
    lv_obj_set_style_text_color(t, theme.fg, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 64);
    for (int i = 0; i < USB_CHOICES; i++) {
        lv_obj_t *r = theme_box(usb_ask_box);
        lv_obj_set_size(r, LOS_W, 34);
        lv_obj_set_pos(r, 0, LOS_HEADER_H + 96 + i * 34);
        lv_obj_set_style_bg_color(r, theme.select, 0);
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, choices[i]);
        lv_obj_set_style_text_font(l, theme.row, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
        usb_ask_rows[i] = r;
    }
    usb_ask_sel = 0;
    usb_ask_show();
}

static void usb_ask_close(void)
{
    if (usb_ask_box)
        lv_obj_delete(usb_ask_box);
    usb_ask_box = NULL;
}

/* Before the card or the DAC goes to the computer: saved, nothing
 * playing, keys unlocked, the screen lit */
static void usb_quiet(void)
{
    save_resume();
    if (settings_changed_at)
        save_settings();
    playback_stop();
    for (int i = 0; i < 100 && !playback_idle(); i++)
        los_sleep_ms(10);
    player.state = 0;
    if (keys_locked) {
        keys_locked = false;
        header_set_locked(false);
        if (lock_scr)
            lv_obj_delete(lock_scr);
        lock_scr = NULL;
    }
    screen_light(100);
}

/* ---- the player as a USB DAC ---- */

/* The computer's sound goes to the DAC as it comes; a screen says so,
 * with the rate and depth it comes at. The wheel and the volume buttons
 * set the level (the DAC's own). Back stops it (charging only); so does
 * unplugging. */
static lv_obj_t *dac_scr, *dac_title, *dac_format;
static bool dac_mode;

static void dac_screen_update(void)
{
    unsigned rate, bits;
    bool on = los_usb_dac_info(&rate, &bits);
    char f[48];
    lv_label_set_text(dac_title, on ? "Playing from the computer" : "Waiting for the computer");
    if (on && rate % 1000)
        snprintf(f, sizeof(f), "%u.%u kHz \xc2\xb7 %u-bit", rate / 1000, rate % 1000 / 100, bits);
    else if (on)
        snprintf(f, sizeof(f), "%u kHz \xc2\xb7 %u-bit", rate / 1000, bits);
    else
        snprintf(f, sizeof(f), "Choose Lossless OS as the sound output");
    lv_label_set_text(dac_format, f);
}

static void dac_open(void)
{
    dac_scr = theme_box(lv_layer_top());
    lv_obj_set_size(dac_scr, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(dac_scr, theme.bg, 0);
    lv_obj_set_style_bg_opa(dac_scr, LV_OPA_COVER, 0);
    header_create(dac_scr, NULL);
    lv_obj_t *d = dots_create(dac_scr, 3, theme.fg);
    dots_set_text(d, "DAC");
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 30);
    dac_title = lv_label_create(dac_scr);
    lv_obj_set_style_text_font(dac_title, theme.title_small, 0);
    lv_obj_set_style_text_color(dac_title, theme.fg, 0);
    lv_obj_align(dac_title, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 100);
    dac_format = lv_label_create(dac_scr);
    lv_obj_set_style_text_font(dac_format, theme.small, 0);
    lv_obj_set_style_text_color(dac_format, theme.second, 0);
    lv_obj_align(dac_format, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 128);
    lv_obj_t *hint = lv_label_create(dac_scr);
    lv_label_set_text(hint, "Wheel: volume \xc2\xb7 Back or unplug: stop");
    lv_obj_set_style_text_font(hint, theme.mono, 0);
    lv_obj_set_style_text_color(hint, theme.faint, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    dac_screen_update();
    lv_obj_align(dac_title, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 100);
    lv_obj_align(dac_format, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 128);
}

static void usb_dac_now(void)
{
    usb_quiet();
    if (los_usb_dac(true)) {
        dac_mode = true;
        dac_open();
    } else {
        usb_done = true;
        show();
    }
}

/* Out of DAC mode (unplugged, or Back): the music where it was, paused */
static void dac_close(void)
{
    if (dac_scr)
        lv_obj_delete(dac_scr);
    dac_scr = NULL;
    dac_mode = false;
    load_resume();
    show();
}

/* Stop using the card and hand it over */
static void usb_share_now(void)
{
    {
        /* stop using the card: saved, nothing playing, nothing open */
        save_resume();
        if (settings_changed_at)
            save_settings();
        playback_stop();
        for (int i = 0; i < 100 && !playback_idle(); i++)
            los_sleep_ms(10);
        player.state = 0;
        if (keys_locked) {
            keys_locked = false;
            header_set_locked(false);
            if (lock_scr)
                lv_obj_delete(lock_scr);
            lock_scr = NULL;
        }
        screen_light(100);
        if (los_usb_share(true)) {
            usb_away = true;
            usb_open();
        } else {
            usb_done = true;          /* couldn't: carry on, try at the next plug */
            show();
        }
    }
}

static void usb_watch(void)
{
    enum los_usb u = los_usb_state();
    if (dac_mode) {
        if (u != LOS_USB_DAC) {
            usb_done = u != LOS_USB_NONE;    /* stopped with the cable in: charge */
            dac_close();
        } else {
            static uint32_t shown;
            if (lv_tick_elaps(shown) > 300) {
                shown = lv_tick_get();
                dac_screen_update();
                lv_obj_align(dac_title, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 100);
                lv_obj_align(dac_format, LV_ALIGN_TOP_MID, 0, LOS_HEADER_H + 128);
            }
        }
        return;
    }
    if (u == LOS_USB_NONE || u == LOS_USB_CABLE)
        usb_done = false;
    if (stack[depth - 1].screen == SCR_SETTINGS && !opts && !usb_away &&
        (u == LOS_USB_COMPUTER) != usb_row_ready)
        show();                       /* the USB connection row: plugged in or out */
    if (usb_ask_box && u != LOS_USB_COMPUTER)
        usb_ask_close();              /* unplugged before answering */
    if (u == LOS_USB_COMPUTER && !usb_away && !usb_done && !usb_ask_box) {
        if (opt.usb == USB_SHARE) {
            usb_share_now();
        } else if (opt.usb == USB_DAC) {
            usb_dac_now();
        } else if (opt.usb == USB_CHARGE) {
            usb_done = true;          /* charging only, until it's plugged in again */
        } else {
            screen_light(100);
            last_key_at = lv_tick_get();
            usb_ask_open();
        }
    } else if (usb_away && u != LOS_USB_SHARED) {
        /* ejected, unplugged, or the computer let go: the card is ours again */
        los_usb_share(false);
        usb_away = false;
        usb_done = u != LOS_USB_NONE;
        lv_obj_t *t = theme_tag(lv_layer_top(), "Reading the card...", theme.fg);
        lv_obj_center(t);
        lv_refr_now(NULL);
        player.album = -1;
        player.state = 0;
        queue_len = 0;
        library_scan(los_music_path());
        collect_artists();
        playlists_scan();
        load_resume();
        write_player_json();
        lv_obj_delete(t);
        if (usb_scr)
            lv_obj_delete(usb_scr);
        usb_scr = NULL;
        depth = 1;                    /* the lists may have changed: from Home */
        stack[0].selected = 0;
        show();
    }
}

static void hold_watch(void);

/* Every 40 ms: the volume, and the headphones' buttons */
static void volume_watch(lv_timer_t *tm)
{
    (void)tm;
    usb_watch();
    if (usb_away)
        return;                   /* the card is the computer's */
    hold_watch();
    if (settings_changed_at && lv_tick_elaps(settings_changed_at) >= SETTINGS_SAVE_MS)
        save_settings();
    battery_watch();
    resume_watch();
    if (sleep_at && (int32_t)(lv_tick_get() - sleep_at) >= 0) {
        sleep_at = 0;
        if (player.state == 1) {
            player_toggle();
            refresh();
        }
        los_app_before_off();
        los_power_off();
    }
    static uint32_t settings_shown;
    if (sleep_at && stack[depth - 1].screen == SCR_SETTINGS && !opts &&
        lv_tick_elaps(settings_shown) >= 30000) {
        settings_shown = lv_tick_get();
        show();
    }
    if (!vol.known) {
        vol.known = los_volume(&vol.db, &vol.lo, &vol.hi);
        if (vol.known && saved.have_volume) {
            vol.db = saved.volume_db < vol.lo ? vol.lo : saved.volume_db > vol.hi ? vol.hi : saved.volume_db;
            los_volume_set(vol.db);
        }
        if (vol.known)
            bluetooth_set_volume(vol.db, vol.lo, vol.hi);
    }
    int db;
    if (bluetooth_take_volume(&db) && db != vol.db) {
        vol.db = db;
        los_volume_set(db);
        volume_changed();
    }
    bool flash = vol.changed_at && lv_tick_elaps(vol.changed_at) < VOLUME_FLASH_MS;
    header_set_volume(vol.known, vol.db, vol.lo, vol.hi, flash);

    bluetooth_play_status(player.state);
    {
        /* the song, for a car's screen: a few times a second is plenty */
        static uint32_t told;
        if (lv_tick_elaps(told) >= 250) {
            told = lv_tick_get();
            const los_track *t = current_track();
            if (t) {
                const los_album *a = library_album(player.album);
                /* the album's cover, for a car that shows covers: once an album */
                static int cover_sent = -1;
                uint32_t cover_id = a->cover[0] ? (uint32_t)player.album + 1 : 0;
                if (cover_id && bluetooth_wants_cover() && cover_sent != player.album) {
                    const lv_draw_buf_t *img = cover_thumb(a->cover, a->cover_offset, 200);
                    if (img && img->header.w == 200 && img->header.h == 200) {
                        bluetooth_cover((const uint16_t *)img->data, img->header.stride / 2, cover_id);
                        cover_sent = player.album;
                    }
                }
                if (!bluetooth_wants_cover())
                    cover_sent = -1;
                bluetooth_now_playing(t->title, a->artist, a->title,
                                      t->number ? t->number : player.track + 1, a->ntracks,
                                      t->seconds > 0 ? (uint32_t)t->seconds * 1000 : 0,
                                      (uint32_t)playback_position_ms(),
                                      ((uint32_t)player.album << 8 | (uint32_t)player.track) + 1,
                                      cover_id);
            } else {
                bluetooth_now_playing(NULL, NULL, NULL, 0, 0, 0, 0, 0, 0);
            }
        }
    }
    int action = bluetooth_take_action();
    switch (action) {
    case BT_ACTION_PAUSE:
        if (player.state == 1) {
            player_toggle();
            refresh();
        }
        break;
    case BT_ACTION_PLAY:
        if (player.state != 1 && player.album >= 0) {
            bool was_stopped = player.state == 0;
            player_toggle();
            if (was_stopped)
                show();
            else
                refresh();
        }
        break;
    case BT_ACTION_NEXT:
    case BT_ACTION_PREVIOUS:
        if (player.album >= 0) {
            player_skip(action == BT_ACTION_PREVIOUS ? -1 : +1);
            show();
        }
        break;
    case BT_ACTION_VOLUME_UP:   los_app_volume(+2); break;
    case BT_ACTION_VOLUME_DOWN: los_app_volume(-2); break;
    default: break;
    }
    bt_screen_watch();
    if (scrub.active && lv_tick_elaps(scrub.last) >= SCRUB_COMMIT_MS) {
        scrub.active = false;
        playback_seek_ms((uint32_t)scrub.sec * 1000);
        player.elapsed = scrub.sec;
        resume_state = -1;          /* save the new place */
        refresh();
    }
    int64_t now;
    char hm[12] = "";
    if (los_clock_get(&now)) {
        los_date d = date_from_seconds(now);
        clock_text(hm, sizeof(hm), &d, NULL);
    }
    header_set_time(hm);
    {
        uint32_t idle = lv_tick_elaps(last_key_at);
        uint32_t dim = lock_scr ? LOCK_DIM_MS : (uint32_t)opt.dim_s * 1000;
        uint32_t off = lock_scr ? LOCK_OFF_MS : SCREEN_OFF_MS;
        screen_light(!dim ? 100 : idle >= dim + off ? 0 : idle >= dim ? DIM_LEVEL : 100);
    }
    static uint32_t lock_last;
    if (lock_scr && light > 0 &&
        (lv_tick_elaps(lock_last) >= 500 || (vol.changed_at && lv_tick_elaps(vol.changed_at) < 100))) {
        lock_last = lv_tick_get();
        lock_update();
    }
}

static void tick(lv_timer_t *tm)
{
    (void)tm;
    if (player.state != 1)
        return;
    if (playback_took_gapless()) {
        /* the next song is playing already: move the player on to it */
        failures = 0;
        if (repeat_mode != REPEAT_ONE) {
            int pos = queue_step(+1);
            queue_go(pos < 0 ? 0 : pos);
        }
        player.elapsed = 0;
        tell_next();
        show();
        return;
    }
    bool next = playback_took_end();
    if (!next && playback_failed()) {
        next = ++failures <= queue_len;   /* give up after a whole queue of them */
        if (!next) {
            player.state = 0;
            playback_stop();
            show();
            return;
        }
    }
    if (next) {
        if (!playback_failed())
            failures = 0;
        int pos = queue_pos;
        if (repeat_mode == REPEAT_ONE && !playback_failed()) {
            player.elapsed = 0;
            start_current();
            refresh();
            return;
        }
        player_skip(+1);
        if (queue_pos != pos || !player.state)
            show();   /* the track changed: rebuild what shows it */
        return;
    }
    int s = (int)(playback_position_ms() / 1000);
    if (s != player.elapsed) {
        player.elapsed = s;
        refresh();
    }
}

/* ---- navigation ---------------------------------------------------------------------- */

static void key_event(lv_event_t *e);

static void show(void)
{
    struct nav *n = &stack[depth - 1];
    lv_obj_t *old = screen_obj;
    marquee_follow(NULL);         /* its row goes with the old screen */
    screen_obj = lv_obj_create(NULL);
    lv_obj_remove_flag(screen_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen_obj, theme.bg, 0);
    lv_obj_set_style_bg_opa(screen_obj, LV_OPA_COVER, 0);
    list.cont = NULL;
    list.count = 0;
    header = NULL;
    np_time = NULL;
    np_icon = NULL;
    settings_hint = NULL;
    scrub.active = false;
    opts = NULL;

    switch (n->screen) {
    case SCR_HOME:        build_home(); break;
    case SCR_MUSIC:       build_music(); break;
    case SCR_ALBUMS:      build_albums(n->param); break;
    case SCR_ARTISTS:     build_artists(); break;
    case SCR_SONGS:       build_songs(); break;
    case SCR_ALBUM:       build_album(n->param); break;
    case SCR_NOW_PLAYING: build_now_playing(); break;
    case SCR_SOON:        build_soon(n->param); break;
    case SCR_ARTIST_SONGS: build_artist_songs(n->param); break;
    case SCR_SETTINGS:    build_settings(); break;
    case SCR_BLUETOOTH:   build_bluetooth(); break;
    case SCR_BT_SEARCH:   build_bt_search(); break;
    case SCR_CLOCK:       build_clock(); break;
    case SCR_ABOUT:       build_about(); break;
    case SCR_EQ:          build_eq(); break;
    case SCR_EQ_PRESETS:  build_eq_presets(); break;
    case SCR_PLAYLISTS:   build_playlists(); break;
    case SCR_PLAYLIST:    build_playlist(n->param); break;
    }
    if (n->screen != SCR_NOW_PLAYING && n->screen != SCR_EQ)
        mini_player();
    else
        mini_title = mini_bar = NULL;

    lv_obj_add_event_cb(screen_obj, key_event, LV_EVENT_KEY, NULL);
    lv_group_remove_all_objs(group);
    lv_group_add_obj(group, screen_obj);
    lv_group_focus_obj(screen_obj);
    lv_screen_load(screen_obj);
    if (old)
        lv_obj_delete(old);
    list_select(n->selected);
    refresh();
}

static void push(enum screen s, int param)
{
    if (depth >= NAV_DEPTH)
        return;
    stack[depth].screen = s;
    stack[depth].param = param;
    stack[depth].selected = 0;
    depth++;
    show();
}

static void back(void)
{
    if (depth > 1) {
        depth--;
        show();
    }
}

static void select_item(void)
{
    struct nav *n = &stack[depth - 1];
    int i = list.selected;
    if (list.count == 0) {    /* an empty list: nothing to open */
        if (n->screen == SCR_BT_SEARCH)
            bluetooth_scan();
        return;
    }
    switch (n->screen) {
    case SCR_HOME:
        if (i == 0) push(SCR_MUSIC, 0);
        else if (i == 1) push(SCR_PLAYLISTS, 0);
        else if (i == 2) push(SCR_NOW_PLAYING, 0);
        else push(SCR_SETTINGS, 0);
        break;
    case SCR_SETTINGS: {
        static const int dims[] = { 15, 30, 60, 120, 300, 0 };
        static const int sleeps[] = { 0, 15, 30, 45, 60, 90 };
        if (i == 0) {
            push(SCR_BLUETOOTH, 0);
            break;
        }
        if (i == 1) {
            eq_open();
            break;
        }
        i--;                             /* the rows below, as before EQ came */
        if (i == 1) {
            clk.editing = false;
            clock_load();
            push(SCR_CLOCK, 0);
        } else if (i == 2) {
            /* the next step up (a saved value between steps rounds up) */
            int b = (opt.brightness / BRIGHT_STEP + 1) * BRIGHT_STEP;
            opt.brightness = b > 100 ? BRIGHT_MIN : b;
            light = -1;
            screen_light(100);           /* the new brightness, now */
        } else if (i == 3) {
            int k = 0;
            while (k < 5 && dims[k] != opt.dim_s)
                k++;
            opt.dim_s = dims[(k + 1) % 6];
        } else if (i == 4) {
            opt.clock24 = !opt.clock24;
        } else if (i == 5) {
            opt.wheel_volume = !opt.wheel_volume;
        } else if (i == 6) {
            int left = sleep_at ? (int)((int32_t)(sleep_at - lv_tick_get()) / 60000) + 1 : 0, k = 0;
            while (k < 5 && sleeps[k] < left)    /* the next step up from what's left */
                k++;
            int next = sleep_at ? sleeps[(k + 1) % 6] : sleeps[1];
            sleep_at = next ? (lv_tick_get() + (uint32_t)next * 60000) | 1 : 0;
        } else if (i == 7) {
            static const int next[] = { USB_SHARE, USB_DAC, USB_ASK, USB_CHARGE };   /* ask, transfer, DAC, charge */
            opt.usb = next[opt.usb];
            settings_changed();
            show();
            break;
        } else if (i == 8) {
            /* the choice from plugging in, again (after Back, or after
             * charging only, with the cable still in) */
            if (los_usb_state() == LOS_USB_COMPUTER && !usb_ask_box) {
                usb_done = true;         /* the box answers; nothing else starts */
                usb_ask_open();
            }
            break;
        } else if (i == 9) {
            push(SCR_ABOUT, 0);
            break;
        } else if (i == 10) {
            /* every file read again; what was playing stops (the list changes) */
            lv_obj_t *t = theme_tag(lv_layer_top(), "Scanning the library...", theme.fg);
            lv_obj_center(t);
            lv_refr_now(NULL);
            playback_stop();
            player.state = 0;
            player.album = -1;
            queue_len = 0;
            library_rescan(los_music_path());
            collect_artists();
            playlists_scan();
            lv_obj_delete(t);
            show();
            break;
        }
        if (i >= 2 && i <= 5)
            settings_changed();
        if (i >= 2 && i <= 6)
            show();
        break;
    }
    case SCR_CLOCK:
        clk.editing = true;
        show();
        break;
    case SCR_EQ_PRESETS:
        if (i == 0) {
            eq_enabled = !eq_enabled;            /* stay: on / off, then a preset perhaps */
        } else {
            int c = i - 1;
            if (eq_changed(eq_sel) && c-- == 0) {
                eq_reset(eq_sel);                /* Reset: its own bands again */
                c = eq_sel;
            }
            eq_choose(c);
            bool flat = true;
            for (int b = 0; b < EQ_BANDS; b++)
                flat = flat && eq_gains[b] == 0;
            if (!flat)
                eq_enabled = true;               /* one that changes something turns it on */
        }
        eq_apply();
        settings_changed();
        if (i == 0)
            show();
        else
            back();
        break;
    case SCR_BLUETOOTH: {
        bt_status b;
        bluetooth_status(&b);
        int what = i < bt_nrows ? bt_rows[i] : -1;
        if (what == BTR_POWER) {
            bluetooth_power(!b.on);
            settings_changed();
        }
        else if (what == BTR_SEARCH) {
            bluetooth_scan();
            push(SCR_BT_SEARCH, 0);
        }
        else if (what >= BTR_KNOWN)
            opts_open(OPTS_DEVICE, what - BTR_KNOWN);   /* connect or disconnect, forget */
        break;
    }
    case SCR_BT_SEARCH:
        bluetooth_connect(i);
        back();
        break;
    case SCR_MUSIC:
        if (i == 0) push(SCR_ALBUMS, -1);
        else if (i == 1) push(SCR_ARTISTS, 0);
        else push(SCR_SONGS, 0);
        break;
    case SCR_ARTISTS:
        push(SCR_ALBUMS, i);
        break;
    case SCR_ALBUMS:
        if (n->param >= 0 && i == 0)
            push(SCR_ARTIST_SONGS, n->param);
        else
            push(SCR_ALBUM, shown_albums[n->param >= 0 ? i - 1 : i]);
        break;
    case SCR_PLAYLISTS:
        push(SCR_PLAYLIST, i);
        break;
    case SCR_SONGS:
    case SCR_ARTIST_SONGS:
    case SCR_PLAYLIST:
        queue_from = n->screen == SCR_SONGS ? FROM_SONGS :
                     n->screen == SCR_PLAYLIST ? FROM_PLAYLIST : FROM_ARTIST;
        snprintf(queue_key, sizeof(queue_key), "%s",
                 n->screen == SCR_ARTIST_SONGS ? artist_names[n->param] :
                 n->screen == SCR_PLAYLIST ? playlist_get(n->param)->path : "");
        player_play_list(song_index, nsongs, i);
        push(SCR_NOW_PLAYING, 0);
        break;
    case SCR_ALBUM:
        queue_from = FROM_ALBUM;
        snprintf(queue_key, sizeof(queue_key), "%s", library_album(n->param)->dir);
        player_play(n->param, i);
        push(SCR_NOW_PLAYING, 0);
        break;
    default:
        break;
    }
}

/* ---- the lock screen (a tap of Power) ------------------------------------------------- */

/* Over everything (LVGL's top layer, so it stays while screens change
 * under it): the time, the date, what's playing. The music keys and
 * volume still work; another tap of Power takes it away. */
static lv_obj_t *lk_header;


static lv_obj_t *lk_icon;
static lv_obj_t *lock_scr, *lk_time, *lk_ampm, *lk_date, *lk_title, *lk_artist, *lk_album,
                *lk_cover, *lk_track, *lk_fill, *lk_left, *lk_right, *lk_none;
static int lk_album_shown = -2;


static void lock_update(void)
{
    if (!lock_scr)
        return;
    char buf[64];

    /* the time and date, or the volume while it changes */
    int64_t now;
    bool clock_ok = los_clock_get(&now);
    los_date d = date_from_seconds(clock_ok ? now : 0);
    const char *ampm = "";
    if (clock_ok)
        clock_text(buf, sizeof(buf), &d, &ampm);
    else
        snprintf(buf, sizeof(buf), "--:--");
    dots_set_text(lk_time, buf);
    dots_set_text(lk_ampm, ampm);
    lv_obj_update_layout(lk_time);
    int tw = lv_obj_get_width(lk_time), aw = ampm[0] ? 26 : 0;
    lv_obj_set_pos(lk_time, (LOS_W - tw - aw) / 2, LOS_HEADER_H + 16);
    lv_obj_set_pos(lk_ampm, (LOS_W - tw - aw) / 2 + tw + 8, LOS_HEADER_H + 16 + 32 - 16);
    header_set_playing(lk_header, player.state);
    bool vol_flash = false;    /* the header shows the volume */
    if (clock_ok)
        snprintf(buf, sizeof(buf), "%s  %d %s", day_names[d.weekday], d.day, month_names[d.month - 1]);
    else
        snprintf(buf, sizeof(buf), "Set the time in Settings");
    char caps[64];
    size_t i;
    for (i = 0; buf[i] && i < sizeof(caps) - 1; i++)
        caps[i] = (char)(buf[i] >= 'a' && buf[i] <= 'z' && !vol_flash ? buf[i] - 32 : buf[i]);
    caps[i] = '\0';
    lv_label_set_text(lk_date, caps);
    lv_obj_set_style_text_color(lk_date, vol_flash ? theme.fg : theme.muted, 0);

    /* what's playing */
    const los_track *t = current_track();
    bool any = t != NULL;
    lv_obj_t *song[] = { lk_cover, lk_title, lk_artist, lk_album, lk_track, lk_left, lk_right, lk_icon };
    for (size_t k = 0; k < sizeof(song) / sizeof(song[0]); k++)
        if (song[k]) {
            if (any)
                lv_obj_remove_flag(song[k], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(song[k], LV_OBJ_FLAG_HIDDEN);
        }
    if (any)
        lv_obj_add_flag(lk_none, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(lk_none, LV_OBJ_FLAG_HIDDEN);
    if (!any)
        return;
    const los_album *a = library_album(player.album);
    if (lk_album_shown != player.album) {
        if (lk_cover)
            lv_obj_delete(lk_cover);
        lk_cover = cover_create(lock_scr, a->cover, a->cover_offset, 56, 5);
        lv_obj_set_pos(lk_cover, 20, 124);
        lk_album_shown = player.album;
    }
    if (strcmp(lv_label_get_text(lk_title), t->title))
        lv_label_set_text(lk_title, t->title);
    lv_label_set_text(lk_artist, a->artist);
    lv_label_set_text(lk_album, a->title);
    int len = track_length(t);
    lv_obj_set_width(lk_fill, len > 0 ? (LOS_W - 40) * (player.elapsed < len ? player.elapsed : len) / len : 0);
    state_icon_set(lk_icon, player.state);
    fmt_time(buf, sizeof(buf), player.elapsed);
    lv_label_set_text(lk_left, buf);
    buf[0] = '-';
    fmt_time(buf + 1, sizeof(buf) - 1, len > player.elapsed ? len - player.elapsed : 0);
    lv_label_set_text(lk_right, buf);
}

static lv_obj_t *lock_label(const lv_font_t *f, lv_color_t c, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(lock_scr);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    return l;
}

static void lock_open(void)
{
    if (lock_scr)
        return;
    lock_scr = theme_box(lv_layer_top());
    lv_obj_set_size(lock_scr, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(lock_scr, theme.bg, 0);
    lv_obj_set_style_bg_opa(lock_scr, LV_OPA_COVER, 0);
    lk_header = header_create(lock_scr, "");
    header_show_mark_left(lk_header);
    lk_time = dots_create(lock_scr, 3, theme.ink);
    lk_ampm = dots_create(lock_scr, 1, theme.ink);
    lk_date = lock_label(theme.label, theme.muted, 0, LOS_HEADER_H + 58, LOS_W);
    lv_obj_set_style_text_align(lk_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(lk_date, 2, 0);
    lk_cover = NULL;
    lk_album_shown = -2;
    lk_title = lock_label(theme.title_small, theme.fg, 88, 122, 212);
    lv_label_set_long_mode(lk_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lk_artist = lock_label(theme.small, theme.second, 88, 145, 212);
    lk_album = lock_label(theme.mono, theme.faint, 88, 165, 212);
    lk_track = theme_box(lock_scr);
    lv_obj_set_pos(lk_track, 20, 200);
    lv_obj_set_size(lk_track, LOS_W - 40, 2);
    lv_obj_set_style_bg_color(lk_track, theme.line, 0);
    lv_obj_set_style_bg_opa(lk_track, LV_OPA_COVER, 0);
    lk_fill = theme_box(lk_track);
    lv_obj_set_size(lk_fill, 0, 2);
    lv_obj_set_style_bg_color(lk_fill, theme.accent, 0);
    lv_obj_set_style_bg_opa(lk_fill, LV_OPA_COVER, 0);
    lk_icon = state_icon_create(lock_scr, 7, 9);
    lv_obj_set_pos(lk_icon, 20, 208 + (lv_font_get_line_height(theme.mono) - 9) / 2);
    lk_left = lock_label(theme.mono, theme.muted, 33, 208, 127);
    lk_right = lock_label(theme.mono, theme.muted, 160, 208, 140);
    lv_obj_set_style_text_align(lk_right, LV_TEXT_ALIGN_RIGHT, 0);
    lk_none = theme_tag(lock_scr, "Nothing playing", theme.faint);
    lv_obj_align(lk_none, LV_ALIGN_TOP_MID, 0, 148);
    last_key_at = lv_tick_get();
    lock_update();
}

static void lock_close(void)
{
    if (lock_scr)
        lv_obj_delete(lock_scr);
    lock_scr = NULL;
    lk_cover = NULL;
    lk_header = NULL;
    show();                  /* the screen under it, with its own header live again */
}

/* ---- option panels (Menu) ------------------------------------------------------------- */

/* A panel over the bottom of the screen. On Now Playing and album pages:
 * Repeat (Off, All, One) and Shuffle (Off, On). On a pair of headphones
 * in the Bluetooth list: Connect (or Disconnect) and Forget, which asks
 * again first. The wheel picks a row, Play acts (Prev / Next change a
 * value too), Back or Menu closes. */
#define OPTS_ROW_H 40
#define OPTS_MAX   2

static int opts_kind, opts_device, opts_sel;
static bool opts_confirm;            /* Forget pressed once */
static bool opts_connected;          /* the device is connected (or connecting) */
static lv_obj_t *opts_row[OPTS_MAX], *opts_label[OPTS_MAX], *opts_value[OPTS_MAX];

static void opts_update(void)
{
    if (opts_kind == OPTS_PLAY) {
        static const char *const repeat_names[] = { "Off", "All", "One" };
        lv_label_set_text(opts_value[0], repeat_names[repeat_mode]);
        lv_label_set_text(opts_value[1], shuffle ? "On" : "Off");
    } else {
        lv_label_set_text(opts_label[0], opts_connected ? "Disconnect" : "Connect");
        lv_label_set_text(opts_label[1], opts_confirm ? "Forget? Play again" : "Forget");
    }
    for (int i = 0; i < OPTS_MAX; i++)
        set_state_deep(opts_row[i], LV_STATE_CHECKED, i == opts_sel);
}

static void opts_open(int kind, int device)
{
    const char *title = "Play mode";
    bt_status b;
    opts_kind = kind;
    opts_device = device;
    opts_confirm = false;
    if (kind == OPTS_DEVICE) {
        bluetooth_status(&b);
        if (device < 0 || device >= b.nknown)
            return;
        title = b.known[device].name;
        opts_connected = b.peer_known == device && b.state >= BT_CONNECTING;
    }
    int h = 30 + OPTS_MAX * OPTS_ROW_H;
    opts = theme_box(screen_obj);
    lv_obj_set_pos(opts, 0, LOS_H - h);
    lv_obj_set_size(opts, LOS_W, h);
    lv_obj_set_style_bg_color(opts, theme.raised, 0);
    lv_obj_set_style_bg_opa(opts, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(opts, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(opts, 1, 0);
    lv_obj_set_style_border_color(opts, theme.line, 0);
    lv_obj_t *t = theme_tag(opts, title, theme.muted);
    lv_obj_set_pos(t, 20, 9);
    lv_obj_set_width(t, LOS_W - 40);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    static const char *const play_names[OPTS_MAX] = { "Repeat", "Shuffle" };
    for (int i = 0; i < OPTS_MAX; i++) {
        lv_obj_t *row = theme_box(opts);
        lv_obj_set_pos(row, 0, 30 + i * OPTS_ROW_H);
        lv_obj_set_size(row, LOS_W, OPTS_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(row, theme.select, LV_STATE_CHECKED);
        int b = centred_baseline(theme.row, OPTS_ROW_H);
        opts_label[i] = row_label(row, kind == OPTS_PLAY ? play_names[i] : "", theme.row, theme.fg,
                                  20, top_for_baseline(theme.row, b), 260);
        opts_value[i] = row_label(row, "", theme.row, theme.second, 200,
                                  top_for_baseline(theme.row, b), 100);
        lv_obj_set_style_text_align(opts_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        opts_row[i] = row;
    }
    opts_sel = 0;
    opts_update();
}

static void opts_close(void)
{
    if (opts)
        lv_obj_delete(opts);
    opts = NULL;
}

static void opts_key(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        opts_sel = key == LV_KEY_UP ? 0 : OPTS_MAX - 1;
        opts_confirm = false;
        break;
    case LV_KEY_ENTER:
    case LV_KEY_RIGHT:
    case LV_KEY_LEFT:
        if (opts_kind == OPTS_PLAY) {
            if (opts_sel == 0)
            {
                repeat_mode = (repeat_mode + (key == LV_KEY_LEFT ? 2 : 1)) % 3;
                tell_next();
            } else
                set_shuffle(!shuffle);
            settings_changed();
            break;
        }
        if (key != LV_KEY_ENTER)
            return;
        if (opts_sel == 0) {
            if (opts_connected)
                bluetooth_disconnect();
            else
                bluetooth_connect_known(opts_device);
        } else if (!opts_confirm) {
            opts_confirm = true;
            break;
        } else {
            bluetooth_forget(opts_device);
        }
        opts_close();
        return;
    case LV_KEY_ESC:
    case LV_KEY_HOME:
        opts_close();
        /* Now Playing shows the modes and what's next: rebuild it */
        if (stack[depth - 1].screen == SCR_NOW_PLAYING)
            show();
        return;
    default:
        return;
    }
    opts_update();
}

/* Prev / Next pressed: what it does is decided when it's let go, or once
 * it has been held (hold_watch). Its repeats while held change nothing. */
static void hold_start(uint32_t key)
{
    if (player.album < 0 || hold.key == key)
        return;
    hold.key = key;
    hold.since = lv_tick_get();
    hold.seeking = false;
}

static void hold_watch(void)
{
    if (!hold.key)
        return;
    bool down = app_keypad && lv_indev_get_state(app_keypad) == LV_INDEV_STATE_PRESSED &&
                lv_indev_get_key(app_keypad) == hold.key;
    int dir = hold.key == LV_KEY_RIGHT ? +1 : -1;
    bool np = stack[depth - 1].screen == SCR_NOW_PLAYING;
    if (!down) {
        hold.key = 0;
        if (!hold.seeking) {
            /* a tap: the next (or previous) song */
            player_skip(dir);
            if (np)
                show();
            else
                refresh();
        } else if (scrub.active) {
            /* let go after seeking: there */
            scrub.active = false;
            playback_seek_ms((uint32_t)scrub.sec * 1000);
            player.elapsed = scrub.sec;
            resume_state = -1;
            refresh();
            lock_update();
        }
        return;
    }
    uint32_t held = lv_tick_elaps(hold.since);
    if (held < HOLD_MS)
        return;
    const los_track *t = current_track();
    if (!t || !player.state)
        return;                               /* nothing to seek in */
    if (!hold.seeking) {
        hold.seeking = true;
        hold.last_step = 0;
        scrub.sec = player.elapsed;
    }
    if (hold.last_step && lv_tick_elaps(hold.last_step) < SEEK_EVERY_MS)
        return;
    hold.last_step = lv_tick_get() | 1;
    /* faster the longer it's held: 2 s of song a step, then 5, then 10 */
    int step = held < 2400 ? 2 : held < 5400 ? 5 : 10, len = track_length(t);
    scrub.sec += dir * step;
    scrub.sec = scrub.sec < 0 ? 0 : scrub.sec > len - 1 ? len - 1 : scrub.sec;
    scrub.active = true;
    scrub.last = lv_tick_get();               /* the wheel's commit waits for us */
    refresh();
    lock_update();
}

/* Key lock: a tap of Power locks, another unlocks. Locked, only the
 * music keys work (play/pause, previous, next; volume is the player's own),
 * so nothing changes in a pocket. */
static bool keys_locked;

static void handle_key(uint32_t key)
{
    bool np = stack[depth - 1].screen == SCR_NOW_PLAYING;
    last_key_at = lv_tick_get();
    if (light < 100) {           /* the first key only wakes the screen */
        screen_light(100);
        lock_update();
        return;
    }
    if (usb_away)
        return;                  /* the card is on a computer: nothing to play */
    if (dac_mode) {
        if (key == LV_KEY_UP || key == LV_KEY_DOWN)
            los_app_volume(key == LV_KEY_DOWN ? +2 : -2);   /* clockwise: louder */
        else if (key == LV_KEY_ESC)
            los_usb_dac(false);                             /* a drive again: charging */
        return;
    }
    if (bt_pair_box) {
        if (key == LV_KEY_ENTER && !bt_pair_passkey) {
            bluetooth_confirm(true);
            bt_pair_close();
        } else if (key == LV_KEY_ESC) {
            if (!bt_pair_passkey)
                bluetooth_confirm(false);
            bt_pair_close();
        }
        return;
    }
    if (usb_ask_box) {
        if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            int sel = usb_ask_sel + (key == LV_KEY_DOWN ? 1 : -1);
            usb_ask_sel = sel < 0 ? 0 : sel >= USB_CHOICES ? USB_CHOICES - 1 : sel;
            usb_ask_show();
        } else if (key == LV_KEY_ENTER && usb_ask_sel == 0) {
            usb_ask_close();
            usb_share_now();
        } else if (key == LV_KEY_ENTER && usb_ask_sel == 1) {
            usb_ask_close();
            usb_dac_now();
        } else if (key == LV_KEY_ENTER || key == LV_KEY_ESC) {
            usb_ask_close();
            usb_done = true;     /* charging only; the music plays on */
        }
        return;
    }
    if (key == LOS_KEY_LOCK) {
        keys_locked = !keys_locked;
        header_set_locked(keys_locked);
        if (keys_locked)
            lock_open();
        else
            lock_close();
        return;
    }
    if (keys_locked) {
        if (key == LV_KEY_ENTER && player.album >= 0) {
            bool was_stopped = player.state == 0;
            player_toggle();
            if (was_stopped)
                show();
            else
                refresh();
        } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            hold_start(key);
        }
        return;
    }
    if (opts) {
        opts_key(key);
        return;
    }
    if (clock_key(key))
        return;
    if (eq_key(key))
        return;
    enum screen scr = stack[depth - 1].screen;
    if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && np && opt.wheel_volume) {
        los_app_volume(key == LV_KEY_DOWN ? +2 : -2);     /* clockwise: louder */
        return;
    }
    if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && np) {
        const los_track *t = current_track();
        if (t && player.state) {
            int len = track_length(t);
            int step = scrub.active && lv_tick_elaps(scrub.last) < SCRUB_FAST_MS ?
                       SCRUB_FAST_STEP_S : SCRUB_STEP_S;
            if (!scrub.active)
                scrub.sec = player.elapsed;
            scrub.sec += key == LV_KEY_DOWN ? step : -step;    /* clockwise: on */
            scrub.sec = scrub.sec < 0 ? 0 : scrub.sec > len - 1 ? len - 1 : scrub.sec;
            scrub.active = true;
            scrub.last = lv_tick_get();
            refresh();
        }
        return;
    }
    switch (key) {
    case LV_KEY_UP:    list_select(list.selected - 1); break;
    case LV_KEY_DOWN:  list_select(list.selected + 1); break;
    case LV_KEY_ESC:   back(); break;
    case LV_KEY_HOME:
        if (scr == SCR_NOW_PLAYING || scr == SCR_ALBUM)
            opts_open(OPTS_PLAY, 0);
        else if (player.album >= 0)
            push(SCR_NOW_PLAYING, 0);
        break;
    case LV_KEY_ENTER:
        if (np) {
            bool was_stopped = player.state == 0;
            player_toggle();
            if (was_stopped)
                show();
            else
                refresh();
        } else {
            select_item();
        }
        break;
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        hold_start(key);
        break;
    default:
        break;
    }
}

/* Keys are handled after LVGL has finished delivering them: a key that
 * opens another screen deletes the one receiving the event, which LVGL may
 * still be walking (and can crash, depending on the memory layout). */
static uint32_t keys_waiting[16];
static int nkeys_waiting;

static void handle_waiting_keys(void *unused)
{
    (void)unused;
    for (int i = 0; i < nkeys_waiting; i++)
        handle_key(keys_waiting[i]);
    nkeys_waiting = 0;
}

static void key_event(lv_event_t *e)
{
    if (nkeys_waiting == (int)LV_ARRAYLEN(keys_waiting))
        return;
    keys_waiting[nkeys_waiting++] = lv_event_get_key(e);
    if (nkeys_waiting == 1)
        lv_async_call(handle_waiting_keys, NULL);
}

/* ---- start ---------------------------------------------------------------------------- */

/* After start, decode the covers one album at a time, so lists show them
 * without waiting (reading a cover from a player's card takes a moment). */
static void warm_covers(lv_timer_t *t)
{
    static int next;
    if (next >= library_albums()) {
        lv_timer_delete(t);
        return;
    }
    const los_album *a = library_album(next++);
    cover_thumb(a->cover, a->cover_offset, 40);
}

void los_app_start(lv_display_t *display, lv_indev_t *keypad)
{
    (void)display;
    theme_init(true);
    library_scan(los_music_path());
    collect_artists();
    playlists_scan();

    group = lv_group_create();
    lv_indev_set_group(keypad, group);
    app_keypad = keypad;
    playback_init();
    bluetooth_init();
    for (int c = 0; c < EQ_CHOICES; c++)
        eq_reset(c);
    eq_choose(EQ_CUSTOM);                /* Flat, until settings say otherwise */
    if (settings_load(&saved)) {
        repeat_mode = saved.repeat;
        shuffle = saved.shuffle;
        if (saved.brightness)
            opt.brightness = saved.brightness < BRIGHT_MIN ? BRIGHT_MIN : saved.brightness;
        if (saved.dim_s >= 0)
            opt.dim_s = saved.dim_s;
        opt.clock24 = saved.clock24;
        opt.wheel_volume = saved.wheel_volume;
        opt.usb = saved.usb;
        eq_enabled = saved.eq;
        for (int b = 0; b < EQ_BANDS; b++)
            eq_gains[b] = saved.eq_gains[b] < -EQ_MAX_DB ? -EQ_MAX_DB :
                          saved.eq_gains[b] > EQ_MAX_DB ? EQ_MAX_DB : saved.eq_gains[b];
        eq_load_curves(&saved);
        eq_apply();
        if (opt.brightness != 100) {
            light = -1;
            screen_light(100);
        }
        if (saved.bluetooth)
            bluetooth_power(true);
    }
    load_resume();
    write_player_json();
    lv_timer_create(tick, 250, NULL);
    volume_watch(NULL);
    lv_timer_create(volume_watch, 40, NULL);
    lv_timer_create(warm_covers, 40, NULL);

    depth = 0;
    push(SCR_HOME, 0);
}
