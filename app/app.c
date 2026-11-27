/*
 * Lossless OS: screens and navigation.
 *
 * Each screen is rebuilt when it's shown; a small stack remembers where
 * you were and what was selected. The player is a stand-in until audio
 * output exists: it keeps time and moves through tracks, but plays nothing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
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
#include "coverflow.h"
#include "misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop */
#include "playback.h"
#include "bluetooth.h"
#include "calendar.h"
#include "settings.h"
#include "skins.h"
#include "version.h"
#include "output.h"
#include "playlists.h"
#include "install.h"
#include "startlog.h"

#define MINI_H 22      /* the mini player's height */
#define ROW_H   (theme.classic ? 28 : 36)   /* a plain list row */
#define SONG_ROW_H (theme.classic ? 28 : 32)   /* a song on an album's page */
#define ALBUM_H (theme.classic ? 44 : 52)   /* an album row: cover and two lines */

static const char *const month_names[] = { "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December" };
static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday" };


/* Brightness: each Play on it goes up a step, from the top back to the
 * bottom. Never below 20%: darker is hard to read, and a screen too dark
 * to see is too dark to find the setting on */
#define BRIGHT_MIN  20
#define BRIGHT_STEP 10
#define BRIGHT_DEFAULT 60   /* bright enough indoors and out, and kinder to the battery than 100 */

/* Settings with choices (Settings, saved with the rest); the defaults also
 * come back with Reset settings. The wheel: volume, as on an iPod */
#define OPT_DEFAULTS { BRIGHT_DEFAULT, 60, false, true, 0, true, false, 15, false }
static struct {
    int brightness;      /* percent of the screen's normal light */
    int dim_s;           /* dim after this long without a key; 0 never */
    bool clock24;
    bool wheel_volume;   /* the wheel on Now Playing: volume, not scrubbing */
    int usb;             /* plugged into a computer: ask, transfer files, charge only */
    bool lock_wheel;     /* the wheel when locked: volume (or nothing) */
    bool battery_pct;    /* the battery's percent in the header always (or while plugged in) */
    int idle_off_min;    /* switch off after this long with nothing playing and no key; 0 never */
    bool mono;           /* both sides averaged into each (off: stereo, bit-perfect) */
} opt = OPT_DEFAULTS;
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
    SCR_SKIPPED,     /* what the library couldn't add */
    SCR_COVERS,      /* every album as covers to flip through */
    SCR_THEMES,      /* Settings > Themes: every theme, built in and on the card */
    SCR_ADD_TO,      /* Add to playlist: New playlist, then the ones that can be changed */
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
static lv_obj_t *skipped_hint;       /* the line under Couldn't add */
static const char *skipped_line(int i);
static const char *settings_default(int row);
static lv_obj_t *songs_letter;       /* Gallery's Songs: the selected row's letter, big and faint */
static void songs_letter_set(int i);

/* Long lists (Songs, an artist's songs, a playlist) can be thousands of
 * rows, and making every row took seconds. These keep the count and each
 * row's letter, make only the rows on screen, and make them again as the
 * selection moves past the edge. */
static struct {
    bool on;
    int row_h, top;              /* the first row on screen */
    void (*make)(int i);         /* adds row i to list.cont (row_begin ...) */
    lv_obj_t *bar;               /* Classic: a slim scroll bar of its own */
} vl;
static int list_height(void);
static int list_w;                   /* below: the list's width */

static void vl_fill(void)
{
    marquee_row = NULL;          /* its row is about to go */
    lv_obj_clean(list.cont);
    int rows = list_height() / vl.row_h + 1, n = list.count;
    for (int i = vl.top; i < vl.top + rows && i < n; i++) {
        vl.make(i);
        list.count = n;          /* row_begin counted it */
        lv_obj_set_pos(lv_obj_get_child(list.cont, -1), 0, (i - vl.top) * vl.row_h);
    }
    if (vl.bar) {                /* the bar: where the rows on screen are in the whole list */
        int h = list_height(), shown = h / vl.row_h;
        int bh = n > shown ? h * shown / n : h, by = n > shown ? (h - bh) * vl.top / (n - shown) : 0;
        lv_obj_set_size(vl.bar, 5, bh < 12 ? 12 : bh);
        lv_obj_set_pos(vl.bar, list_w - 7, LOS_HEADER_H + 1 + (by + bh > h ? h - bh : by));
    }
}

static void list_select(int i)
{
    if (!list.cont || list.count == 0)
        return;
    if (i < 0)
        i = 0;
    if (i >= list.count)
        i = list.count - 1;
    if (vl.on) {
        int full = list_height() / vl.row_h, top = vl.top;
        if (full < 1)
            full = 1;
        if (i < top)
            top = i;
        else if (i >= top + full)
            top = i - full + 1;
        if (top != vl.top || !lv_obj_get_child_count(list.cont)) {
            vl.top = top;
            vl_fill();
        } else {
            lv_obj_t *old = lv_obj_get_child(list.cont, list.selected - vl.top);
            if (old && list.selected >= vl.top)
                set_state_deep(old, LV_STATE_CHECKED, false);
        }
        list.selected = i;
        stack[depth - 1].selected = i;
        lv_obj_t *row = lv_obj_get_child(list.cont, i - vl.top);
        if (row)
            set_state_deep(row, LV_STATE_CHECKED, true);
        marquee_follow(row);
        songs_letter_set(i);
        return;
    }
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
    if (skipped_hint && stack[depth - 1].screen == SCR_SKIPPED)
        lv_label_set_text(skipped_hint, skipped_line(i));
    songs_letter_set(i);
}

/* Room left for the list: above the mini player while music plays */
static int list_height(void)
{
    return (player.state && !theme.classic ? LOS_H - MINI_H : LOS_H) - LOS_HEADER_H - 1;   /* Classic: no mini player */
}

static int list_w = LOS_W;   /* the list's width: narrower beside a split Home's cover */

/* ---- the letter jump ----
 * In a list sorted by name (Albums, Artists, Songs, Playlists), spinning the
 * wheel fast shows a big letter, and each step then moves to the next (or
 * previous) letter instead of the next row, as on an iPod. It ends when
 * the wheel rests. */

#define JUMP_GAP_MS   110   /* steps closer than this are a fast spin */
#define JUMP_AFTER    5     /* fast steps in a row before letters take over */
#define JUMP_REST_MS  700   /* the letter goes this long after the last step */

static char list_keys[QUEUE_MAX];   /* each row's letter: A to Z, or # */
static bool list_keyed;             /* this list has them */

static struct {
    uint32_t last;
    int dir, streak;
    bool on;
    lv_obj_t *box, *letter;
} jump;

/* The letter the row just added sorts under */
static void row_key(const char *text)
{
    if (list.count < 1 || list.count > QUEUE_MAX)
        return;
    char c = library_sort_name(text)[0];
    list_keys[list.count - 1] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (c >= 'A' && c <= 'Z') ? c : '#';
    list_keyed = true;
}

static void songs_letter_set(int i)
{
    if (!songs_letter || i < 0 || i >= list.count || i >= QUEUE_MAX)
        return;
    char t[2] = { i == 0 ? 0 : list_keys[i], 0 };   /* none on Shuffle, the first row */
    if (strcmp(lv_label_get_text(songs_letter), t))
        lv_label_set_text(songs_letter, t);
}

static void jump_hide(void)
{
    if (songs_letter && jump.on) {
        lv_obj_set_style_text_color(songs_letter, lv_color_mix(theme.fg, theme.bg, 34), 0);
        lv_obj_move_background(songs_letter);          /* behind the list again */
    }
    if (jump.box)
        lv_obj_delete(jump.box);
    jump.box = jump.letter = NULL;
    jump.on = false;
    jump.streak = 0;
}

static void jump_show(char c)
{
    char t[2] = { c, 0 };
    if (songs_letter) {               /* Gallery's Songs: its own big letter lights up instead, in front */
        lv_obj_set_style_text_color(songs_letter, theme.fg, 0);
        lv_obj_move_foreground(songs_letter);
        return;
    }
    if (theme.gallery) {              /* Gallery elsewhere (covers): the letter big, on a dark square */
        if (!jump.box) {
            jump.box = theme_box(lv_layer_top());
            lv_obj_set_size(jump.box, 84, 84);
            lv_obj_align(jump.box, LV_ALIGN_CENTER, 0, -10);
            lv_obj_set_style_radius(jump.box, 14, 0);
            lv_obj_set_style_bg_color(jump.box, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(jump.box, 190, 0);
            jump.letter = lv_label_create(jump.box);
            lv_obj_set_style_text_font(jump.letter, theme.big, 0);
            lv_obj_set_style_text_color(jump.letter, lv_color_white(), 0);
        }
        lv_label_set_text(jump.letter, t);
        lv_obj_center(jump.letter);
        return;
    }
    if (theme.classic) {              /* Classic: a dark, see-through square and a big letter */
        if (!jump.box) {
            jump.box = theme_box(lv_layer_top());
            lv_obj_set_size(jump.box, 72, 72);
            lv_obj_align(jump.box, LV_ALIGN_CENTER, 0, LOS_HEADER_H / 2);
            lv_obj_set_style_radius(jump.box, 10, 0);
            lv_obj_set_style_bg_color(jump.box, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(jump.box, 170, 0);
            jump.letter = lv_label_create(jump.box);
            lv_obj_set_style_text_font(jump.letter, theme.letter, 0);
            lv_obj_set_style_text_color(jump.letter, lv_color_white(), 0);
        }
        lv_label_set_text(jump.letter, t);
        lv_obj_center(jump.letter);
        return;
    }
    if (!jump.box) {
        jump.box = theme_box(lv_layer_top());
        lv_obj_set_size(jump.box, 84, 84);
        lv_obj_center(jump.box);
        lv_obj_set_style_radius(jump.box, LOS_RADIUS_SURFACE, 0);
        lv_obj_set_style_bg_color(jump.box, theme.raised, 0);
        lv_obj_set_style_bg_opa(jump.box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(jump.box, theme.line, 0);
        lv_obj_set_style_border_width(jump.box, 1, 0);
        jump.letter = dots_create(jump.box, 6, theme.fg);
    }
    dots_set_text(jump.letter, t);
    lv_obj_center(jump.letter);
}

/* The row the next letter starts at (dir +1), or the start of this letter,
 * then the previous one (dir -1) */
static int jump_target(int dir)
{
    int cur = list.selected, n = list.count;
    char k = list_keys[cur];
    if (dir > 0) {
        int i = cur + 1;
        while (i < n && list_keys[i] == k)
            i++;
        return i < n ? i : n - 1;
    }
    int i = cur;
    if (i > 0 && list_keys[i - 1] != k)
        i--;                              /* at a letter's start: into the one before */
    k = list_keys[i];
    while (i > 0 && list_keys[i - 1] == k)
        i--;
    return i;
}

/* A wheel step in a keyed list: true if the letter jump took it */
static bool letter_jump(uint32_t key)
{
    if (!list_keyed || !list.cont || list.count < 2)
        return false;
    uint32_t now = lv_tick_get();
    int dir = key == LV_KEY_DOWN ? 1 : -1;
    if (dir == jump.dir && lv_tick_diff(now, jump.last) < JUMP_GAP_MS)
        jump.streak++;
    else if (!jump.on)
        jump.streak = 0;
    jump.dir = dir;
    jump.last = now;
    if (!jump.on && jump.streak < JUMP_AFTER)
        return false;
    jump.on = true;
    list_select(jump_target(dir));
    jump_show(list_keys[list.selected]);
    return true;
}

/* The wheel at rest: the letter goes */
static void jump_tick(lv_timer_t *t)
{
    (void)t;
    if (jump.on && lv_tick_elaps(jump.last) >= JUMP_REST_MS)
        jump_hide();
}

static void list_begin(void)
{
    list.cont = theme_box(screen_obj);
    lv_obj_set_pos(list.cont, 0, LOS_HEADER_H + 1);
    lv_obj_set_size(list.cont, list_w, list_height());
    lv_obj_set_flex_flow(list.cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list.cont, LV_OBJ_FLAG_SCROLLABLE);
    if (theme.classic) {              /* a slim scroll bar at the right, when it scrolls */
        lv_obj_set_scroll_dir(list.cont, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_AUTO);   /* only when it's longer than the screen */
        lv_obj_set_style_width(list.cont, 5, LV_PART_SCROLLBAR);
        lv_obj_set_style_radius(list.cont, 2, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(list.cont, lv_color_hex(0x8C8C8C), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(list.cont, LV_OPA_COVER, LV_PART_SCROLLBAR);
        lv_obj_set_style_pad_right(list.cont, 2, LV_PART_SCROLLBAR);
        lv_obj_set_style_pad_ver(list.cont, 2, LV_PART_SCROLLBAR);
    } else {
        lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_OFF);
    }
    list.count = 0;
    list.selected = 0;
    list_keyed = false;
    vl.on = false;
    vl.bar = NULL;
}

/* A long list of n rows, each row_h tall, made by make(i) as they come
 * into view; each row's letter is set with list_keys[] by the caller */
static void vl_begin(int n, int row_h, void (*make)(int i))
{
    list_begin();
    lv_obj_set_layout(list.cont, LV_LAYOUT_NONE);
    lv_obj_remove_flag(list.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_OFF);
    list.count = n;
    vl.on = true;
    vl.top = 0;
    vl.row_h = row_h;
    vl.make = make;
    if (theme.classic && n * row_h > list_height()) {
        vl.bar = theme_box(screen_obj);
        lv_obj_set_style_radius(vl.bar, 2, 0);
        lv_obj_set_style_bg_color(vl.bar, lv_color_hex(0x8C8C8C), 0);
        lv_obj_set_style_bg_opa(vl.bar, LV_OPA_COVER, 0);
    }
}

/* The letter row i sorts under */
static char key_of(const char *text)
{
    char c = library_sort_name(text)[0];
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (c >= 'A' && c <= 'Z') ? c : '#';
}

static lv_obj_t *row_begin(int height)
{
    lv_obj_t *row = theme_box(list.cont);
    lv_obj_set_size(row, list_w, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(row, theme.select, LV_STATE_CHECKED);
    if (theme.select_grad) {          /* a skin's selection bar that fades down */
        lv_obj_set_style_bg_grad_color(row, theme.select_bottom, LV_STATE_CHECKED);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, LV_STATE_CHECKED);
    }
    /* a hairline under each row keeps neighbours apart (Classic has none) */
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, theme.classic ? 0 : 1, 0);
    lv_obj_set_style_border_color(row, theme.line, 0);
    lv_obj_set_style_border_color(row, theme.select_grad ? theme.select_bottom : theme.select,
                                  LV_STATE_CHECKED);
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
    lv_obj_set_style_text_color(l, theme.select_fg, LV_STATE_CHECKED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

/* A plain row: one line of list text */
static void text_row(const char *text, const char *right)
{
    lv_obj_t *row = row_begin(ROW_H);
    int b = centred_baseline(theme.row, ROW_H);
    bool chevron = theme.chevrons && !right;   /* a skin's > on rows without a value */
    row_label(row, text, theme.row, theme.fg, LOS_EDGE, top_for_baseline(theme.row, b),
              right ? list_w - 110 : list_w - 30 - (chevron ? 16 : 0));
    if (right) {
        lv_obj_t *r = row_label(row, right, theme.mono, theme.faint, list_w - 90,
                                top_for_baseline(theme.mono, b), 80);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    } else if (chevron) {
        lv_obj_t *r = row_label(row, "\xe2\x80\xba", theme.row, theme.faint, list_w - 28,
                                top_for_baseline(theme.row, b), 16);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* A song row: its album's cover, small, then the title */
static void song_row(const los_album *a, const char *title)
{
    if (theme.classic) {              /* Classic: the title alone */
        lv_obj_t *row = row_begin(ROW_H);
        row_label(row, title, theme.row, theme.fg, LOS_EDGE,
                  top_for_baseline(theme.row, centred_baseline(theme.row, ROW_H)), list_w - 24);
        return;
    }
    if (theme.gallery) {              /* Gallery: a bigger cover, the title, then the artist */
        lv_obj_t *row = row_begin(46);
        lv_obj_t *c = cover_create(row, a->cover, a->cover_offset, 36, 5);
        lv_obj_set_pos(c, 14, 5);
        row_label(row, title, theme.body, theme.fg, 60, 5, list_w - 110);
        row_label(row, a->artist, theme.mono, theme.muted, 60, 26, list_w - 110);
        return;
    }
    lv_obj_t *row = row_begin(36);
    lv_obj_t *c = cover_create(row, a->cover, a->cover_offset, 28, 3);
    lv_obj_set_pos(c, 12, 4);
    row_label(row, title, theme.row, theme.fg, 50,
              top_for_baseline(theme.row, centred_baseline(theme.row, 36)), 260);
}

/* An album row: cover, album, second line */
static void album_row(const los_album *a, const char *second)
{
    int h = ALBUM_H, side = theme.classic ? 34 : 40, gap = theme.classic ? 16 : 21;
    lv_obj_t *row = row_begin(h);
    lv_obj_t *c = cover_create(row, a->cover, a->cover_offset, side, theme.classic ? 2 : 4);
    lv_obj_set_pos(c, theme.classic ? 3 : 10, (h - side) / 2);
    int x = theme.classic ? 44 : 62;
    /* two lines, their baselines 'gap' apart, centred as a block */
    int b1 = (h + cap_height(theme.row) - gap) / 2, b2 = b1 + gap;
    row_label(row, a->title, theme.row, theme.fg, x, top_for_baseline(theme.row, b1), list_w - x - 12);
    row_label(row, second, theme.row_sub, theme.second, x,
              top_for_baseline(theme.row_sub, b2), list_w - x - 12);
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
    snprintf(line, sizeof(line), "%s   \xc2\xb7   %s", t->title, library_album(player.album)->artist);
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
    lv_obj_set_pos(mini_icon, LOS_EDGE, 2 + (MINI_H - 2 - 9) / 2);
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

/* A split Home (a skin's): the list on the left, a cover on the right:
 * what's playing, or else a slow slideshow of the library */
#define SPLIT_W     (theme.classic ? 180 : 176)
#define SPLIT_COVER 124
static lv_obj_t *split_art;
static int split_album = -1, split_size;   /* the album shown, and its cover's size */
static uint32_t split_at;
static bool split_left;                    /* Classic's pan: this way next */

static void split_pan(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void split_art_show(int album)
{
    int n = library_albums();
    if (!n || !screen_obj)
        return;
    if (album < 0 || album >= n)
        album = 0;
    if (split_art)
        lv_obj_delete(split_art);
    /* Classic's big covers are shown once: let the last one go (not the playing one's) */
    if (split_size > SPLIT_COVER && split_album >= 0 && split_album < n && split_album != album &&
        split_album != player.album)
        cover_forget(library_album(split_album)->cover, library_album(split_album)->cover_offset,
                     split_size);
    const los_album *a = library_album(album);
    if (theme.classic) {
        /* the cover fills the right half, slowly panning across, under a soft
         * shadow from the list */
        int pane = LOS_W - list_w, h = list_height();
        split_art = theme_box(screen_obj);
        lv_obj_set_pos(split_art, list_w, LOS_HEADER_H + 1);
        lv_obj_set_size(split_art, pane, h);
        lv_obj_set_style_clip_corner(split_art, true, 0);
        lv_obj_remove_flag(split_art, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *c = cover_create(split_art, a->cover, a->cover_offset, h, 0);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, c);
        lv_anim_set_exec_cb(&an, split_pan);
        lv_anim_set_values(&an, split_left ? 0 : pane - h, split_left ? pane - h : 0);
        lv_anim_set_duration(&an, 5000);
        lv_anim_start(&an);
        split_left = !split_left;
        lv_obj_t *shade = theme_box(split_art);
        lv_obj_set_size(shade, 10, h);
        lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(shade, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(shade, 90, 0);
        lv_obj_set_style_bg_grad_color(shade, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_opa(shade, 0, 0);
        lv_obj_set_style_bg_grad_dir(shade, LV_GRAD_DIR_HOR, 0);
        split_size = h;
    } else {
        split_art = cover_create(screen_obj, a->cover, a->cover_offset, SPLIT_COVER, 4);
        lv_obj_set_pos(split_art, list_w + (LOS_W - list_w - SPLIT_COVER) / 2,
                       LOS_HEADER_H + 1 + (list_height() - SPLIT_COVER) / 2);
        split_size = SPLIT_COVER;
    }
    split_album = album;
    split_at = lv_tick_get();
}

static void split_begin(void)
{
    if (!theme.split_home)
        return;
    list_w = SPLIT_W;
    if (!theme.classic) {
        lv_obj_t *edge = theme_box(screen_obj);   /* a hairline between list and cover */
        lv_obj_set_pos(edge, SPLIT_W, LOS_HEADER_H + 1);
        lv_obj_set_size(edge, 1, list_height());
        lv_obj_set_style_bg_color(edge, theme.line, 0);
        lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
    }
    split_art_show(player.album >= 0 ? player.album : split_album);
}

/* Every half second: the cover follows the music, or moves on every 5 s */
static void split_tick(lv_timer_t *t)
{
    (void)t;
    if (!split_art)
        return;
    if (player.album >= 0) {
        if (player.album != split_album)
            split_art_show(player.album);
        return;
    }
    if (lv_tick_elaps(split_at) < 5000)
        return;
    int n = library_albums(), next = split_album;
    for (int k = 0; k < n; k++) {             /* the next album that has a cover */
        next = (next + 1) % n;
        if (library_album(next)->cover[0])
            break;
    }
    split_art_show(next);
}

/* What Home's and Music's rows are: their order differs on Classic */
enum menu_row { MR_MUSIC, MR_PLAYLISTS, MR_NOW, MR_SETTINGS, MR_SHUFFLE, MR_COVERS, MR_ARTISTS,
                MR_ALBUMS, MR_SONGS, MR_SKIPPED };
static int menu_rows[8], nmenu;

static void menu_row(int what, const char *text, const char *right)
{
    text_row(text, right);
    if (nmenu < (int)LV_ARRAYLEN(menu_rows))
        menu_rows[nmenu++] = what;
}

static void build_home(void)
{
    header = header_create(screen_obj, NULL);
    split_begin();
    list_begin();
    nmenu = 0;
    if (theme.classic) {
        /* as on the classic players: Now Playing at the bottom, while there's something */
        menu_row(MR_MUSIC, "Music", NULL);
        menu_row(MR_SETTINGS, "Settings", NULL);
        menu_row(MR_SHUFFLE, "Shuffle Songs", NULL);
        if (player.album >= 0)
            menu_row(MR_NOW, "Now Playing", NULL);
        return;
    }
    menu_row(MR_MUSIC, "Music", NULL);
    menu_row(MR_PLAYLISTS, "Playlists", NULL);
    /* beside a split Home's cover there's no room for the state: the cover says it */
    menu_row(MR_NOW, "Now Playing", theme.split_home ? NULL : player.state == 1 ? "PLAYING" :
                                    player.state == 2 ? "PAUSED" : NULL);
    menu_row(MR_SETTINGS, "Settings", NULL);
}

static void build_music(void)
{
    header = header_create(screen_obj, "Music");
    split_begin();
    list_begin();
    nmenu = 0;
    if (theme.classic) {
        menu_row(MR_COVERS, "Covers", NULL);
        menu_row(MR_PLAYLISTS, "Playlists", NULL);
        menu_row(MR_ARTISTS, "Artists", NULL);
        menu_row(MR_ALBUMS, "Albums", NULL);
        menu_row(MR_SONGS, "Songs", NULL);
    } else {
        menu_row(MR_ALBUMS, "Albums", NULL);
        menu_row(MR_ARTISTS, "Artists", NULL);
        menu_row(MR_SONGS, "Songs", NULL);
    }
    if (library_skipped()) {
        char n[12];
        snprintf(n, sizeof(n), "%d", library_skipped());
        menu_row(MR_SKIPPED, "Couldn't add", n);
    }
}

/* What the library couldn't add, and why (the line under says it in full) */
static lv_obj_t *list_footer(const char *text);

static const char *skipped_reason(const char *why)
{
    if (!strcmp(why, "HE-AAC"))
        return "HE-AAC isn't played yet";
    if (!strcmp(why, "AAC (OTHER)"))
        return "this kind of AAC isn't played";
    if (!strcmp(why, "40+ SONGS"))
        return "an album holds up to 40 songs";
    if (!strcmp(why, "FULL"))
        return "the library holds 256 albums";
    return "the files can't be read";
}

static const char *skipped_line(int i)
{
    static char line[80];
    if (i < 0 || i >= library_skipped())
        return "";
    const los_skipped *k = library_skipped_album(i);
    if (k->whole)
        snprintf(line, sizeof(line), "Whole album: %s", skipped_reason(k->why));
    else
        snprintf(line, sizeof(line), "%d song%s left out: %s", k->songs, k->songs == 1 ? "" : "s",
                 skipped_reason(k->why));
    return line;
}

static void build_skipped(void)
{
    header = header_create(screen_obj, "Couldn't add");
    list_begin();
    for (int i = 0; i < library_skipped(); i++) {
        const los_skipped *k = library_skipped_album(i);
        char v[36];
        if (k->whole)
            snprintf(v, sizeof(v), "%s", k->why);
        else
            snprintf(v, sizeof(v), "%d SONG%s", k->songs, k->songs == 1 ? "" : "S");
        text_row(k->name, v);
    }
    skipped_hint = list_footer(skipped_line(0));
}

/* The skipped list as one number, to see whether it changed */
static uint32_t skipped_signature(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < library_skipped(); i++)
        for (const char *c = library_skipped_album(i)->name; *c; c++)
            h = (h ^ (uint8_t)*c) * 16777619u;
    return library_skipped() ? h : 0;
}
static uint32_t skipped_seen;     /* the list as last shown (or at start) */

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
        for (int j = i; j > 0 && library_name_cmp(artist_names[j - 1], artist_names[j]) > 0; j--) {
            const char *t = artist_names[j];
            artist_names[j] = artist_names[j - 1];
            artist_names[j - 1] = t;
        }
}

/* While the library is read: the mark, what it's doing and the albums so
 * far, over everything. At start only once it takes a moment (a read from
 * the cache is quick); after USB or Rescan at once. */
static struct {
    const char *text;
    lv_obj_t *box, *count;
    uint32_t t0, drawn;
} scan_ui;

static void scan_ui_show(void)
{
    scan_ui.box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scan_ui.box);
    lv_obj_set_size(scan_ui.box, LOS_W, LOS_H);
    /* the mark's colours are the title bar's: on its colour where it has one */
    bool fill = theme.header_fill;
    lv_obj_set_style_bg_color(scan_ui.box, fill ? theme.header_bg : theme.bg, 0);
    lv_obj_set_style_bg_opa(scan_ui.box, LV_OPA_COVER, 0);
    lv_obj_t *m = mark_create(scan_ui.box, 5);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, -34);
    lv_obj_t *t = theme_tag(scan_ui.box, scan_ui.text, fill ? theme.header_fg : theme.fg);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 6);
    scan_ui.count = lv_label_create(scan_ui.box);
    lv_label_set_text(scan_ui.count, "");
    lv_obj_set_style_text_font(scan_ui.count, theme.small, 0);
    lv_obj_set_style_text_color(scan_ui.count, fill ? theme.header_muted : theme.muted, 0);
    lv_obj_set_style_text_align(scan_ui.count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(scan_ui.count, LOS_W);
    lv_obj_align(scan_ui.count, LV_ALIGN_CENTER, 0, 30);
}

static void scan_progress(int albums)
{
    if (!scan_ui.box) {
        if (lv_tick_elaps(scan_ui.t0) < 400)
            return;
        scan_ui_show();
    } else if (lv_tick_elaps(scan_ui.drawn) < 200) {
        return;
    }
    if (albums)
        lv_label_set_text_fmt(scan_ui.count, "%d album%s", albums, albums == 1 ? "" : "s");
    lv_refr_now(NULL);
    scan_ui.drawn = lv_tick_get();
}

static void scan_begin(const char *text, bool at_once)
{
    scan_ui.text = text;
    scan_ui.t0 = lv_tick_get();
    library_set_progress(scan_progress);
    if (at_once) {
        scan_ui_show();
        lv_refr_now(NULL);
        scan_ui.drawn = lv_tick_get();
    }
}

static void scan_end(void)
{
    library_set_progress(NULL);
    if (scan_ui.box)
        lv_obj_delete(scan_ui.box);
    scan_ui.box = NULL;
}

/* An empty library's lists say why they're empty, and what to do */
static void no_music_note(void)
{
    bool skipped = library_skipped() > 0;
    lv_obj_t *t = theme_tag(screen_obj, skipped ? "Nothing it can play" : "No music yet",
                            theme.muted);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, LOS_HEADER_H / 2 - 12);
    lv_obj_t *h = lv_label_create(screen_obj);
    lv_label_set_text(h, skipped ? "The Couldn't add list says why."
                                 : "Copy albums to the card\nfrom your computer.");
    lv_obj_set_style_text_font(h, theme.small, 0);
    lv_obj_set_style_text_color(h, theme.faint, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(h, t, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

/* album indexes shown by the current Albums screen */
static int shown_albums[LIB_MAX_ALBUMS];

static void build_albums(int artist)
{
    header = header_create(screen_obj, artist >= 0 ? artist_names[artist] : "Albums");
    if (!library_albums()) {
        no_music_note();
        return;
    }
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
        if (artist < 0)
            row_key(a->title);               /* all albums: by title */
        shown_albums[n++] = i;
    }
}

static void build_artists(void)
{
    header = header_create(screen_obj, "Artists");
    if (!library_albums()) {
        no_music_note();
        return;
    }
    list_begin();
    for (int i = 0; i < nartists; i++) {
        text_row(artist_names[i], NULL);
        row_key(artist_names[i]);
    }
}

static song_ref song_index[QUEUE_MAX];
static int nsongs;     /* in song_index: the songs the current list shows */

/* A to Z by title; the same title by album, then by its place there */
static int song_order(const void *pa, const void *pb)
{
    const song_ref *x = pa, *y = pb;
    int c = library_name_cmp(library_album(x->album)->tracks[x->track].title,
                             library_album(y->album)->tracks[y->track].title);
    if (c)
        return c;
    return x->album != y->album ? x->album - y->album : x->track - y->track;
}

/* Every song, A to Z by title (as Songs lists them; resume rebuilds the
 * same list) */
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
    qsort(out, (size_t)n, sizeof(song_ref), song_order);
    return n;
}

/* Row i of song_index, as the song lists show it */
static void song_make(int i)
{
    const los_album *a = library_album(song_index[i].album);
    song_row(a, a->tracks[song_index[i].track].title);
}

static int song_row_h(void)
{
    return theme.classic ? ROW_H : theme.gallery ? 46 : 36;
}

/* Songs, an artist's All songs and a playlist start with Shuffle: every
 * song in them, in a random order (as the iPod's Shuffle Songs) */
/* Gallery's Shuffle row: in the cover's place, an orange square with two
 * crossing arrows */
static void shuffle_icon_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int x = a.x1 + 9, y = a.y1 + 11, w = 18, h = 14;
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = theme.accent_fg;
    l.width = 2;
    l.round_start = l.round_end = 1;
    /* two lines that cross, each ending in an arrowhead at the right */
    const int seg[][4] = {
        { x, y + h, x + w, y }, { x, y, x + w, y + h },
        { x + w - 5, y, x + w, y }, { x + w, y, x + w, y + 5 },
        { x + w - 5, y + h, x + w, y + h }, { x + w, y + h, x + w, y + h - 5 },
    };
    for (size_t i = 0; i < sizeof(seg) / sizeof(seg[0]); i++) {
        l.p1.x = seg[i][0]; l.p1.y = seg[i][1];
        l.p2.x = seg[i][2]; l.p2.y = seg[i][3];
        lv_draw_line(layer, &l);
    }
}

static void shuffle_row(void)
{
    int h = song_row_h();
    lv_obj_t *row = row_begin(h);
    if (theme.gallery) {
        lv_obj_t *sq = theme_box(row);
        lv_obj_set_size(sq, 36, 36);
        lv_obj_set_pos(sq, 14, 5);
        lv_obj_set_style_radius(sq, 5, 0);
        lv_obj_set_style_bg_color(sq, theme.accent, 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(sq, shuffle_icon_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    }
    const lv_font_t *f = theme.gallery ? theme.body : theme.row;
    row_label(row, "Shuffle", f, theme.classic ? theme.fg : theme.accent,
              theme.classic ? LOS_EDGE : theme.gallery ? 60 : 50, top_for_baseline(f, centred_baseline(f, h)), 200);
}

static void song_make_shuffled(int i)
{
    if (i == 0)
        shuffle_row();
    else
        song_make(i - 1);
}

/* A song list of song_index with Shuffle first; 'keyed' (A to Z lists):
 * each row's letter, for the letter jump */
static void songs_list(bool keyed)
{
    vl_begin(nsongs ? nsongs + 1 : 0, song_row_h(), song_make_shuffled);
    if (!keyed)
        return;                       /* a playlist's own order: no letters */
    for (int i = 0; i < nsongs && i + 1 < QUEUE_MAX; i++)
        list_keys[i + 1] = key_of(library_album(song_index[i].album)->tracks[song_index[i].track].title);
    list_keys[0] = nsongs ? list_keys[1] : '#';
    list_keyed = nsongs > 0;
}

static void build_songs(void)
{
    header = header_create(screen_obj, "Songs");
    if (theme.gallery) {              /* behind the list, at the right */
        songs_letter = lv_label_create(screen_obj);
        lv_label_set_text(songs_letter, "");
        lv_obj_set_style_text_font(songs_letter, theme.big, 0);
        lv_obj_set_style_text_color(songs_letter, lv_color_mix(theme.fg, theme.bg, 34), 0);
        lv_obj_set_style_text_align(songs_letter, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(songs_letter, 80);
        lv_obj_set_pos(songs_letter, LOS_W - 92, LOS_HEADER_H + 6);
    }
    nsongs = collect_all_songs(song_index);
    if (!nsongs) {
        no_music_note();
        return;
    }
    songs_list(true);
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
    nsongs = collect_artist_songs(artist, song_index);
    songs_list(false);
}

static lv_color_t cover_tint(const lv_draw_buf_t *b);

/* Gallery's album page: on the cover's colour, the cover centred with the
 * album and artist under it, then the songs; as the list moves down, the
 * cover and title slide up and fade */
static lv_obj_t *ga_hero[3];
static int ga_hero_y[3];
static lv_obj_t *np_cover;           /* Gallery's Now Playing cover */

/* ---- Gallery: the cover moves with you ----
 * From Covers to an album's page, from there to Now Playing, and back:
 * the cover glides from where it was to where it goes, a copy over the
 * new screen while the real one waits hidden. */
static struct {
    bool on;
    int album, x, y, size;
    lv_obj_t *img, *dest;
    int tx, ty, tsize;
    lv_opa_t dest_opa;                /* as it was: faded on a scrolled album page */
} morph;

static void morph_from(int album, int x, int y, int size)
{
    if (!theme.gallery)
        return;
    morph.on = true;
    morph.album = album;
    morph.x = x;
    morph.y = y;
    morph.size = size;
}

/* The cover of a screen's object, where it is: scrolled up out of sight
 * on an album page, it comes from above the screen */
static bool morph_from_obj(int album, lv_obj_t *o)
{
    if (!o)
        return false;
    lv_obj_update_layout(o);
    morph_from(album, lv_obj_get_x(o), lv_obj_get_y(o), lv_obj_get_width(o));
    return true;
}

static void morph_step(void *v, int32_t t)
{
    (void)v;
    if (!morph.img)
        return;
    int x = morph.x + (morph.tx - morph.x) * t / 256, y = morph.y + (morph.ty - morph.y) * t / 256;
    int size = morph.size + (morph.tsize - morph.size) * t / 256;
    lv_image_set_scale(morph.img, (uint32_t)(size * 256 / COVERFLOW_COVER));
    lv_obj_set_pos(morph.img, x, y);
    lv_obj_set_style_radius(morph.img, size * 8 / 100, 0);
}

static void cf_redraw(void);

/* A glide stopped where it is: its copy gone, and the cover it was going
 * to forgotten (its screen is going away) */
static void morph_abort(void)
{
    if (morph.img) {
        lv_anim_delete(&morph, NULL);
        lv_obj_delete(morph.img);
        morph.img = NULL;
    }
    morph.dest = NULL;
    coverflow_skip(-1);
}

static void morph_done(lv_anim_t *a)
{
    (void)a;
    if (morph.img)
        lv_obj_delete(morph.img);
    morph.img = NULL;
    coverflow_skip(-1);
    cf_redraw();
    if (morph.dest)
        lv_obj_set_style_opa(morph.dest, morph.dest_opa, 0);
    morph.dest = NULL;
}

static void morph_run(void);

static void ga_scroll(lv_event_t *e)
{
    lv_obj_t *cont = lv_event_get_target_obj(e);
    int off = lv_obj_get_scroll_y(cont);
    int opa = 255 - off * 3;
    for (int k = 0; k < 3; k++)
        if (ga_hero[k]) {
            lv_obj_set_y(ga_hero[k], ga_hero_y[k] - off);
            lv_obj_set_style_opa(ga_hero[k], (lv_opa_t)(opa < 0 ? 0 : opa > 255 ? 255 : opa), 0);
        }
}

static void build_album_gallery(int idx)
{
    const los_album *a = library_album(idx);
    const lv_draw_buf_t *src = a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
    lv_obj_set_style_bg_color(screen_obj, src ? cover_tint(src) : theme.bg, 0);
    header = header_create(screen_obj, "");
    ga_hero[0] = cover_create(screen_obj, a->cover, a->cover_offset, 100, 8);
    ga_hero_y[0] = 24;
    lv_obj_set_pos(ga_hero[0], (LOS_W - 100) / 2, 24);
    ga_hero[1] = lv_label_create(screen_obj);
    lv_label_set_text(ga_hero[1], a->title);
    lv_obj_set_style_text_font(ga_hero[1], theme.title_small, 0);
    lv_obj_set_style_text_color(ga_hero[1], theme.fg, 0);
    lv_obj_set_style_text_align(ga_hero[1], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ga_hero[1], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(ga_hero[1], LOS_W - 32, lv_font_get_line_height(theme.title_small));
    ga_hero_y[1] = 130;
    lv_obj_set_pos(ga_hero[1], 16, 130);
    char sub[96];
    if (a->year)
        snprintf(sub, sizeof(sub), "%s \xc2\xb7 %d", a->artist, a->year);
    else
        snprintf(sub, sizeof(sub), "%s", a->artist);
    ga_hero[2] = lv_label_create(screen_obj);
    lv_label_set_text(ga_hero[2], sub);
    lv_obj_set_style_text_font(ga_hero[2], theme.small, 0);
    lv_obj_set_style_text_color(ga_hero[2], theme.muted, 0);
    lv_obj_set_style_text_align(ga_hero[2], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ga_hero[2], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(ga_hero[2], LOS_W - 32, lv_font_get_line_height(theme.small));
    ga_hero_y[2] = 151;
    lv_obj_set_pos(ga_hero[2], 16, 151);

    /* the songs, under the cover at first: the list's top is empty room */
    list_begin();
    lv_obj_set_height(list.cont, LOS_H - LOS_HEADER_H - 1);
    lv_obj_set_style_pad_top(list.cont, 174 - (LOS_HEADER_H + 1), 0);
    lv_obj_set_style_pad_bottom(list.cont, 6, 0);
    lv_obj_set_style_pad_hor(list.cont, 8, 0);
    lv_obj_add_event_cb(list.cont, ga_scroll, LV_EVENT_SCROLL, NULL);
    list_w = LOS_W - 16;
    bool playing_here = player.album == idx;
    for (int k = 0; k < a->ntracks; k++) {
        const los_track *tr = &a->tracks[k];
        lv_obj_t *row = row_begin(30);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, 34, LV_STATE_CHECKED);   /* a soft, see-through highlight */
        int b = centred_baseline(theme.body, 30);
        char num[8], len[12] = "";
        snprintf(num, sizeof(num), "%d", tr->number ? tr->number : k + 1);
        lv_obj_t *n = row_label(row, num, theme.mono, playing_here && k == player.track ? theme.accent : theme.faint,
                                8, top_for_baseline(theme.mono, b), 24);
        (void)n;
        row_label(row, tr->title, theme.body, theme.fg, 34, top_for_baseline(theme.body, b), list_w - 34 - 56);
        if (tr->seconds)
            fmt_time(len, sizeof(len), tr->seconds);
        lv_obj_t *d = row_label(row, len, theme.mono, theme.muted, list_w - 58, top_for_baseline(theme.mono, b), 50);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_album(int idx)
{
    const los_album *a = library_album(idx);
    if (theme.gallery) {
        build_album_gallery(idx);
        return;
    }
    if (theme.classic) {              /* Classic: the album's name, then its songs */
        header = header_create(screen_obj, a->title);
        list_begin();
        for (int k = 0; k < a->ntracks; k++) {
            char len[12];
            fmt_time(len, sizeof(len), a->tracks[k].seconds);
            text_row(a->tracks[k].title, a->tracks[k].seconds ? len : NULL);
        }
        return;
    }
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
        snprintf(year, sizeof(year), "  \xc2\xb7  %d", a->year);
    snprintf(meta, sizeof(meta), "%s%s%d TRACKS%s", fmt, fmt[0] ? "  \xc2\xb7  " : "",
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
    lv_obj_set_size(list.cont, LOS_W, (player.state && !theme.classic ? LOS_H - MINI_H : LOS_H) - top);
    lv_obj_set_flex_flow(list.cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list.cont, LV_SCROLLBAR_MODE_OFF);
    list.count = 0;
    for (int k = 0; k < a->ntracks; k++) {
        const los_track *tr = &a->tracks[k];
        lv_obj_t *row = row_begin(SONG_ROW_H);
        char num[8], len[12];
        snprintf(num, sizeof(num), "%02d", tr->number ? tr->number : k + 1);
        fmt_time(len, sizeof(len), tr->seconds);
        int b = centred_baseline(theme.track, SONG_ROW_H);
        row_label(row, num, theme.mono, theme.faint, 14, top_for_baseline(theme.mono, b), 24);
        row_label(row, tr->title, theme.track, theme.fg, 40, top_for_baseline(theme.track, b), 220);
        lv_obj_t *r = row_label(row, tr->seconds ? len : "", theme.mono, theme.faint, 250,
                                top_for_baseline(theme.mono, b), 58);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* Classic's Now Playing: the cover with a soft reflection on the left;
 * "1 of 12", the song, artist and album on the right; a smooth progress
 * bar with the times in plain text under it */
#define NPC_COVER 130
#define NPC_REFL  26
static uint16_t npc_refl_px[NPC_COVER * NPC_REFL] __attribute__((aligned(4)));
static lv_draw_buf_t npc_refl;

static lv_obj_t *np_label(int x, int y, int w, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(screen_obj);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    if (w > 0)
        lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

/* A small speaker for Classic's volume bar: a body and a cone, with sound
 * waves for the loud end ('waves' in the user data) */
static lv_obj_t *np_spk_lo, *np_spk_hi;

static void speaker_draw(lv_event_t *e)
{
    /* drawn pixel by pixel: a body, a cone, and for the loud end two
     * curved waves */
    static const char *const art[] = {
        ".....#.......",
        "....##....#..",
        "...###.....#.",
        "######..#...#",
        "######...#..#",
        "######...#..#",
        "######..#...#",
        "...###.....#.",
        "....##....#..",
        ".....#.......",
    };
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cols = lv_obj_get_user_data(o) ? 13 : 6;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = theme.fg;
    r.bg_opa = LV_OPA_COVER;
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < cols; x++)
            if (art[y][x] == '#') {
                lv_area_t px = { a.x1 + x, a.y1 + y, a.x1 + x, a.y1 + y };
                lv_draw_rect(layer, &r, &px);
            }
}

static lv_obj_t *speaker_create(int x, int y, bool waves)
{
    lv_obj_t *o = theme_box(screen_obj);
    lv_obj_set_size(o, 13, 10);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_user_data(o, (void *)(intptr_t)waves);
    lv_obj_add_event_cb(o, speaker_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void build_now_playing_classic(const los_album *a)
{
    int top = LOS_HEADER_H + 12;
    lv_obj_t *c = cover_create(screen_obj, a->cover, a->cover_offset, NPC_COVER, 0);
    lv_obj_set_pos(c, 12, top);
    /* the reflection: the cover's bottom rows upside down, fading into the background */
    const lv_draw_buf_t *src = cover_thumb(a->cover, a->cover_offset, NPC_COVER);
    if (src) {
        uint16_t bg = lv_color_to_u16(theme.bg);
        for (int y = 0; y < NPC_REFL; y++) {
            const uint16_t *row = (const uint16_t *)(src->data + (uint32_t)(NPC_COVER - 1 - y) * src->header.stride);
            unsigned k = 90u * (unsigned)(NPC_REFL - y) / NPC_REFL;   /* of 256 */
            for (int x = 0; x < NPC_COVER; x++) {
                uint16_t p = row[x];
                unsigned r = ((p >> 11) * k + (bg >> 11) * (256 - k)) >> 8;
                unsigned g = (((p >> 5) & 63) * k + ((bg >> 5) & 63) * (256 - k)) >> 8;
                unsigned b = ((p & 31) * k + (bg & 31) * (256 - k)) >> 8;
                npc_refl_px[y * NPC_COVER + x] = (uint16_t)(r << 11 | g << 5 | b);
            }
        }
        lv_draw_buf_init(&npc_refl, NPC_COVER, NPC_REFL, LV_COLOR_FORMAT_RGB565, NPC_COVER * 2,
                         npc_refl_px, sizeof(npc_refl_px));
        lv_image_cache_drop(&npc_refl);
        lv_obj_t *rf = lv_image_create(screen_obj);
        lv_image_set_src(rf, &npc_refl);
        lv_obj_set_pos(rf, 12, top + NPC_COVER + 1);
    }

    int x = 12 + NPC_COVER + 12, w = LOS_W - x - 10;
    np_count = np_label(x, top + 6, w, theme.mono, theme.muted);
    np_title = np_label(x, top + 24, w, theme.title_small, theme.fg);
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_t *ar = np_label(x, top + 46, w, theme.row, theme.fg);
    lv_label_set_text(ar, a->artist);
    lv_obj_t *al = np_label(x, top + 66, w, theme.row, theme.muted);
    lv_label_set_text(al, a->title);
    np_modes = np_label(x, top + 92, w, theme.mono, theme.muted);
    np_icon = state_icon_create(screen_obj, 7, 9);
    lv_obj_add_flag(np_icon, LV_OBJ_FLAG_HIDDEN);   /* the title bar has the state */

    np_bar = segbar_create(screen_obj, LOS_W - 24, 6, (LOS_W - 24) / 2, 0, theme.accent,
                           theme.dark ? theme.line : lv_color_hex(0xCFCFCF));
    lv_obj_set_pos(np_bar, 12, LOS_H - 36);
    np_time = np_label(12, LOS_H - 26, 0, theme.label, theme.fg);
    np_left = np_label(0, LOS_H - 26, 0, theme.label, theme.fg);
    /* while the volume changes, the bar shows it, between a quiet and a loud speaker */
    np_spk_lo = speaker_create(12, LOS_H - 24, false);
    np_spk_hi = speaker_create(LOS_W - 12 - 13, LOS_H - 24, true);
}

static lv_color_t cover_tint(const lv_draw_buf_t *b);

/* Gallery's Now Playing: on the cover's own colour, the cover big at the
 * left, the song at the right, a thin progress line with the times under */
static void build_now_playing_gallery(const los_album *a)
{
    const lv_draw_buf_t *src = a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
    lv_color_t tint = src ? cover_tint(src) : theme.bg;
    lv_obj_set_style_bg_color(screen_obj, tint, 0);
    int top = LOS_HEADER_H + 4;
    lv_obj_t *c = cover_create(screen_obj, a->cover, a->cover_offset, 132, 8);
    lv_obj_set_pos(c, 16, top);
    np_cover = c;
    int x = 16 + 132 + 14, w = LOS_W - x - 12;
    np_count = np_label(x, top + 4, w, theme.mono, theme.muted);
    np_title = np_label(x, top + 22, w, theme.title, theme.fg);
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_t *ar = np_label(x, top + 49, w, theme.body, theme.second);
    lv_label_set_text(ar, a->artist);
    lv_obj_t *al = np_label(x, top + 70, w, theme.small, theme.muted);
    lv_label_set_text(al, a->title);
    np_modes = np_label(x, top + 100, w, theme.mono, theme.muted);
    np_icon = state_icon_create(screen_obj, 7, 9);
    lv_obj_add_flag(np_icon, LV_OBJ_FLAG_HIDDEN);
    np_bar = segbar_create(screen_obj, LOS_W - 32, 3, (LOS_W - 32) / 2, 0, theme.fg,
                           lv_color_mix(theme.fg, tint, 60));
    lv_obj_set_pos(np_bar, 16, 196);
    np_time = np_label(16, 205, 0, theme.mono, theme.muted);
    np_left = np_label(0, 205, 0, theme.mono, theme.muted);
    /* while the volume changes, the line shows it, between a quiet and a loud speaker */
    np_spk_lo = speaker_create(16, 206, false);
    np_spk_hi = speaker_create(LOS_W - 16 - 13, 206, true);
}

static void build_now_playing(void)
{
    header = header_create(screen_obj, "Now Playing");
    np_time = np_left = np_bar = np_count = np_title = np_next = NULL;
    np_spk_lo = np_spk_hi = NULL;
    if (player.album < 0) {
        lv_obj_t *t = theme_tag(screen_obj, "Nothing playing", theme.muted);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 10);
        return;
    }
    const los_album *a = library_album(player.album);
    const los_track *t = current_track();
    if (theme.classic) {
        build_now_playing_classic(a);
        return;
    }
    if (theme.gallery) {
        build_now_playing_gallery(a);
        return;
    }

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
    lv_obj_set_pos(ar, 146, 87);
    lv_obj_set_size(ar, 162, lv_font_get_line_height(theme.body));

    lv_obj_t *al = lv_label_create(screen_obj);
    lv_label_set_text(al, a->title);
    lv_obj_set_style_text_font(al, theme.small, 0);
    lv_obj_set_style_text_color(al, theme.faint, 0);
    lv_label_set_long_mode(al, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(al, 146, 106);
    lv_obj_set_size(al, 162, lv_font_get_line_height(theme.small));

    /* format chip */
    char fmt[32];
    library_track_format(t, fmt, sizeof(fmt));
    if (fmt[0] && eq_enabled)
        strcat(fmt, "  \xc2\xb7  EQ");     /* not bit-perfect: say so */
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

/* The skin in use (skins.h): its colours go to the theme, and screens
 * built from now on have them */
static char skin_sel[SKIN_NAME] = "Graphite";

static void skin_use(const char *name)
{
    int i = skin_find(name);
    los_palette p;
    bool dark;
    los_skin_extra extra;
    skin_palette(i < 0 ? 0 : i, &p, &dark, &extra);
    theme_apply(&p, dark, &extra);
    snprintf(skin_sel, sizeof(skin_sel), "%s", skin_name(i < 0 ? 0 : i));
}

/* ---- Settings: a short top level, and groups under it ----
 * Every row has a name (enum set_row) and each group lists its rows, so
 * adding a setting never moves another. SCR_SETTINGS's param is the group;
 * settings_choose (by the keys) does what a row does. */

enum set_row {
    /* the top level */
    SR_BLUETOOTH, SR_SOUND, SR_SCREEN, SR_THEMES, SR_USB, SR_BATTERY, SR_WHEEL, SR_DATE_TIME,
    SR_SLEEP, SR_ABOUT,
    /* in the groups */
    SR_EQ, SR_MONO, SR_BRIGHTNESS, SR_DIM, SR_BATTERY_PCT, SR_IDLE_OFF, SR_WHEEL_NP,
    SR_WHEEL_LOCK, SR_SET_CLOCK, SR_CLOCK24, SR_USB_MODE, SR_USB_CONNECTION, SR_PLAYER,
    SR_RESCAN, SR_RESET, SR_UNINSTALL,
};

enum set_group { SG_TOP, SG_SOUND, SG_SCREEN, SG_BATTERY, SG_WHEEL, SG_DATE_TIME, SG_USB, SG_ABOUT };

static const struct {
    const char *title;
    signed char rows[11];   /* ends with -1 */
} set_groups[] = {
    [SG_TOP] = { "Settings", { SR_BLUETOOTH, SR_SOUND, SR_SCREEN, SR_THEMES, SR_USB, SR_BATTERY,
                               SR_WHEEL, SR_DATE_TIME, SR_SLEEP, SR_ABOUT, -1 } },
    [SG_SOUND] = { "Sound", { SR_EQ, SR_MONO, -1 } },
    [SG_SCREEN] = { "Screen", { SR_BRIGHTNESS, SR_DIM, -1 } },
    [SG_BATTERY] = { "Battery", { SR_BATTERY_PCT, SR_IDLE_OFF, -1 } },
    [SG_WHEEL] = { "Wheel", { SR_WHEEL_NP, SR_WHEEL_LOCK, -1 } },
    [SG_DATE_TIME] = { "Date & time", { SR_SET_CLOCK, SR_CLOCK24, -1 } },
    [SG_USB] = { "USB", { SR_USB_MODE, SR_USB_CONNECTION, -1 } },
    [SG_ABOUT] = { "About", { SR_PLAYER, SR_RESCAN, SR_RESET, SR_UNINSTALL, -1 } },
};

/* The row at 'i' in group 'g'; -1 if there's none */
static int set_row_at(int g, int i)
{
    if (g < 0 || g >= (int)(sizeof(set_groups) / sizeof(set_groups[0])) || i < 0)
        return -1;
    for (int k = 0; k <= i; k++)
        if (set_groups[g].rows[k] < 0)
            return -1;
    return set_groups[g].rows[i];
}

static const char *const set_names[] = {
    [SR_BLUETOOTH] = "Bluetooth", [SR_SOUND] = "Sound", [SR_SCREEN] = "Screen",
    [SR_BATTERY] = "Battery", [SR_WHEEL] = "Wheel", [SR_DATE_TIME] = "Date & time",
    [SR_SLEEP] = "Sleep timer", [SR_USB] = "USB", [SR_ABOUT] = "About",
    [SR_EQ] = "EQ", [SR_MONO] = "Mono", [SR_BRIGHTNESS] = "Brightness",
    [SR_DIM] = "Dim screen after", [SR_THEMES] = "Themes", [SR_BATTERY_PCT] = "Percentage",
    [SR_IDLE_OFF] = "Switch off when idle", [SR_WHEEL_NP] = "On Now Playing",
    [SR_WHEEL_LOCK] = "When locked", [SR_SET_CLOCK] = "Set the clock", [SR_CLOCK24] = "Clock",
    [SR_USB_MODE] = "When plugged in", [SR_USB_CONNECTION] = "Connection",
    [SR_PLAYER] = "This player", [SR_RESCAN] = "Rescan library",
    [SR_RESET] = "Reset settings", [SR_UNINSTALL] = "Uninstall Lossless OS",
};

/* What a row shows on its right ("" for nothing) */
static const char *set_value(int r, char *v, size_t n)
{
    v[0] = '\0';
    switch (r) {
    case SR_BLUETOOTH: {
        bt_status b;
        bluetooth_status(&b);
        /* connected only while it is (BT_FAILED comes after the connected states) */
        bool linked = b.on && (b.state == BT_CONNECTED || b.state == BT_STREAMING);
        return linked ? "CONNECTED" : b.on ? "ON" : "OFF";
    }
    case SR_SOUND:
        return eq_enabled && opt.mono ? "EQ, MONO" : eq_enabled ? "EQ" : opt.mono ? "MONO" : "BIT-PERFECT";
    case SR_EQ:
        return eq_enabled ? "ON" : "OFF";
    case SR_MONO:
        return opt.mono ? "ON" : "OFF";
    case SR_SCREEN:
    case SR_BRIGHTNESS:
        snprintf(v, n, "%d%%", opt.brightness);
        return v;
    case SR_THEMES: {
        size_t k = 0;
        for (; k + 1 < n && skin_sel[k]; k++)
            v[k] = (char)toupper((unsigned char)skin_sel[k]);
        v[k] = '\0';
        return v;
    }
    case SR_DIM:
        if (!opt.dim_s)
            return "NEVER";
        snprintf(v, n, opt.dim_s < 60 ? "%d S" : "%d MIN", opt.dim_s < 60 ? opt.dim_s : opt.dim_s / 60);
        return v;
    case SR_BATTERY_PCT:
        return opt.battery_pct ? "ALWAYS" : "CHARGING";
    case SR_IDLE_OFF:
        if (!opt.idle_off_min)
            return "NEVER";
        snprintf(v, n, "%d MIN", opt.idle_off_min);
        return v;
    case SR_WHEEL_NP:
        return opt.wheel_volume ? "VOLUME" : "SCRUB";
    case SR_WHEEL_LOCK:
        return opt.lock_wheel ? "VOLUME" : "OFF";
    case SR_DATE_TIME:
    case SR_SET_CLOCK: {
        int64_t now;
        if (!los_clock_get(&now))
            return "NOT SET";
        los_date d = date_from_seconds(now);
        const char *ampm;
        clock_text(v, n, &d, &ampm);
        if (ampm[0])
            snprintf(v + strlen(v), n - strlen(v), " %s", ampm);
        return v;
    }
    case SR_CLOCK24:
        return opt.clock24 ? "24-HOUR" : "12-HOUR";
    case SR_SLEEP:
        if (!sleep_at)
            return "OFF";
        {
            int32_t left = (int32_t)(sleep_at - lv_tick_get());
            snprintf(v, n, "%d MIN", left > 0 ? (left + 30000) / 60000 : 0);   /* nearest minute */
        }
        return v;
    case SR_USB:
    case SR_USB_MODE:
        return usb_words[opt.usb];
    case SR_USB_CONNECTION:
        return usb_row_ready ? "READY" : "NONE";
    }
    return v;
}

static void build_settings(int g)
{
    if (g < 0 || g >= (int)(sizeof(set_groups) / sizeof(set_groups[0])))
        g = SG_TOP;
    header = header_create(screen_obj, set_groups[g].title);
    list_begin();
    usb_row_ready = los_usb_state() == LOS_USB_COMPUTER;
    char v[24];
    for (int i = 0; set_groups[g].rows[i] >= 0; i++) {
        int r = set_groups[g].rows[i];
        const char *value = set_value(r, v, sizeof(v));
        text_row(set_names[r], value[0] ? value : NULL);
    }
    settings_hint = list_footer("");
}

/* Settings > Themes: every theme, the one in use marked; Play uses one */
static void build_themes(void)
{
    header = header_create(screen_obj, "Themes");
    list_begin();
    int in_use = skin_find(skin_sel);
    for (int i = 0; i < skins_count(); i++)
        text_row(skin_name(i), i == in_use ? "SET" : "");   /* "": no chevron, it opens nothing */
    list_footer("Add your own in /Themes");
}

/* The line under Settings for the selected row: its default, or what it
 * holds */
static const char *settings_default(int i)
{
    static const char *const hint[] = {
        [SR_BLUETOOTH] = "Default: off", [SR_SOUND] = "EQ and mono",
        [SR_SCREEN] = "Brightness, dimming", [SR_BATTERY] = "Percentage, switching off",
        [SR_WHEEL] = "What the wheel does", [SR_DATE_TIME] = "The clock, 12 or 24-hour",
        [SR_SLEEP] = "Default: off", [SR_USB] = "Plugged into a computer",
        [SR_ABOUT] = "Version, rescan, uninstall",
        [SR_EQ] = "Default: off (bit-perfect)", [SR_MONO] = "Default: off (stereo, bit-perfect)",
        [SR_BRIGHTNESS] = "Default: 60%", [SR_DIM] = "Default: 1 min",
        [SR_THEMES] = "Default: Graphite. Add your own in /Themes",
        [SR_BATTERY_PCT] = "Default: while charging",
        [SR_IDLE_OFF] = "Default: 15 min, with nothing playing",
        [SR_WHEEL_NP] = "Default: volume", [SR_WHEEL_LOCK] = "Default: volume (on the lock screen)",
        [SR_SET_CLOCK] = "Play sets the clock", [SR_CLOCK24] = "Default: 12-hour",
        [SR_USB_MODE] = "Default: ask (or transfer, DAC, charge)",
        [SR_PLAYER] = "Version, card, battery", [SR_RESCAN] = "Reads every song again",
        [SR_RESET] = "Keeps your music and playlists",
        [SR_UNINSTALL] = "Back to the original firmware",
    };
    int r = set_row_at(stack[depth - 1].param, i);
    if (r == SR_USB_CONNECTION)       /* what it does now */
        return usb_row_ready ? "Transfer files, DAC or charge" : "Plug into a computer first";
    return r >= 0 && r < (int)(sizeof(hint) / sizeof(hint[0])) && hint[r] ? hint[r] : "";
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

static void fav_reload(void);
static void toast(const char *text);
static song_ref add_song;            /* the song Add to playlist is for */

/* Add to playlist: New playlist first, then the playlists in /Playlists
 * (Favourites among them) */
static int add_map[PLAYLISTS_MAX], add_n;

static void build_add_to(void)
{
    header = header_create(screen_obj, "Add to playlist");
    list_begin();
    text_row("New playlist", "");
    add_n = 0;
    char count[16];
    for (int i = 0; i < playlists_count(); i++) {
        if (!playlist_editable(i))
            continue;
        const los_playlist *p = playlist_get(i);
        snprintf(count, sizeof(count), "%d", p->songs);
        text_row(p->name, count);
        add_map[add_n++] = i;
    }
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
        row_key(p->name);
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
    nsongs = p ? collect_playlist(pl, song_index) : 0;
    songs_list(false);
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
    /* a soft fill under the line, whatever the theme's highlight colour */
    uint16_t fill = lv_color_to_u16(eq_enabled ? lv_color_mix(theme.fg, theme.bg, 52) : theme.raised);
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
             eq_sel >= EQ_CUSTOM && eq_changed(eq_sel) ? "  \xc2\xb7  edited" : "",
             eq_enabled ? "" : "  \xc2\xb7  off");
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
    lv_label_set_text(hint, "\xe2\x97\x80 \xe2\x96\xb6 band  \xc2\xb7  wheel level  \xc2\xb7  Menu presets");
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
/* ---- Covers ---- */

/* The album facing you is 'target' (and the screen's selection); 'pos'
 * follows it smoothly, so a fast spin of the wheel flows past */
static struct {
    lv_obj_t *img, *title, *artist;
    float pos;
    int target;
    lv_timer_t *timer;
    /* Classic: the cover turned over, its songs on the back */
    lv_obj_t *back, *back_rows[LIB_MAX_TRACKS];
    int back_n, back_sel;
    int kind;                         /* albums, artists, playlists or an artist's albums (SHELF_*) */
    int param, who;                   /* the screen's param; the artist, for an artist's albums */
    float tint[3];                    /* Gallery: the background now, going to tint_to */
    lv_color_t tint_to;
} cf;

#define BACK_W   220
#define BACK_H   150
#define BACK_ROW 20

static void cf_back_select(int sel)
{
    if (sel < 0 || sel >= cf.back_n)
        return;
    lv_obj_t *old = cf.back_rows[cf.back_sel], *row = cf.back_rows[sel];
    lv_obj_set_style_bg_opa(old, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(old, 0), theme.fg, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(old, 1), theme.muted, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(row, 0), theme.select_fg, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(row, 1), theme.select_fg, 0);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    cf.back_sel = sel;
}

static void cf_back_width(void *obj, int32_t w)
{
    lv_obj_set_width(obj, w);
    lv_obj_set_x(obj, (LOS_W - w) / 2);
}

/* Turns the facing cover over: the album's name, then its songs */
static void cf_flip(void)
{
    const los_album *a = library_album(cf.target);
    if (!a || !a->ntracks)
        return;
    cf.back = theme_box(screen_obj);
    lv_obj_set_size(cf.back, BACK_W, BACK_H);
    lv_obj_set_pos(cf.back, (LOS_W - BACK_W) / 2, LOS_HEADER_H + 4);
    lv_obj_set_style_bg_color(cf.back, theme.bg, 0);
    lv_obj_set_style_bg_opa(cf.back, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(cf.back, true, 0);
    lv_obj_remove_flag(cf.back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *head = theme_box(cf.back);           /* the album, on a dark band */
    lv_obj_set_size(head, BACK_W, 34);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, a->title);
    lv_obj_set_style_text_font(t, theme.bar_title, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, BACK_W - 16, lv_font_get_line_height(theme.bar_title));
    lv_obj_set_pos(t, 8, 2);
    lv_obj_t *ar = lv_label_create(head);
    lv_label_set_text(ar, a->artist);
    lv_obj_set_style_text_font(ar, theme.label, 0);
    lv_obj_set_style_text_color(ar, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_long_mode(ar, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(ar, BACK_W - 16, lv_font_get_line_height(theme.label));
    lv_obj_set_pos(ar, 8, 19);
    lv_obj_t *songs = theme_box(cf.back);
    lv_obj_set_pos(songs, 0, 34);
    lv_obj_set_size(songs, BACK_W, BACK_H - 34);
    lv_obj_set_flex_flow(songs, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(songs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(songs, LV_SCROLLBAR_MODE_OFF);
    cf.back_n = a->ntracks < LIB_MAX_TRACKS ? a->ntracks : LIB_MAX_TRACKS;
    for (int k = 0; k < cf.back_n; k++) {
        lv_obj_t *row = theme_box(songs);
        lv_obj_set_size(row, BACK_W, BACK_ROW);
        lv_obj_set_style_bg_color(row, theme.select, 0);
        if (theme.select_grad) {
            lv_obj_set_style_bg_grad_color(row, theme.select_bottom, 0);
            lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
        }
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, a->tracks[k].title);
        lv_obj_set_style_text_font(name, theme.row_sub, 0);
        lv_obj_set_style_text_color(name, theme.fg, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(name, BACK_W - 58, lv_font_get_line_height(theme.row_sub));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);
        char len[12] = "";
        if (a->tracks[k].seconds)
            fmt_time(len, sizeof(len), a->tracks[k].seconds);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, len);
        lv_obj_set_style_text_font(l, theme.label, 0);
        lv_obj_set_style_text_color(l, theme.muted, 0);
        lv_obj_align(l, LV_ALIGN_RIGHT_MID, -8, 0);
        cf.back_rows[k] = row;
    }
    cf.back_sel = 0;
    cf_back_select(0);
    /* it turns: narrow to full width */
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, cf.back);
    lv_anim_set_exec_cb(&an, cf_back_width);
    lv_anim_set_values(&an, 6, BACK_W);
    lv_anim_set_duration(&an, 160);
    lv_anim_start(&an);
}

static void cf_unflip(void)
{
    if (cf.back)
        lv_obj_delete(cf.back);
    cf.back = NULL;
}

/* What Covers shows (SCR_COVERS's param): albums (also -1), artists,
 * playlists, or one artist's albums (SHELF_ARTIST_BASE + the artist), All
 * songs first */
enum { SHELF_ALBUMS, SHELF_ARTISTS, SHELF_PLAYLISTS, SHELF_ARTIST_ALBUMS };
#define SHELF_ARTIST_BASE 1000

static int shelf_kind(int param)
{
    if (param >= SHELF_ARTIST_BASE)
        return SHELF_ARTIST_ALBUMS;
    return param == SHELF_ARTISTS || param == SHELF_PLAYLISTS ? param : SHELF_ALBUMS;
}

static int cf_albums[LIB_MAX_ALBUMS], cf_nalb;   /* an artist's albums, oldest first */

static int shelf_count(int kind)
{
    return kind == SHELF_ARTISTS ? nartists : kind == SHELF_PLAYLISTS ? playlists_count() :
           kind == SHELF_ARTIST_ALBUMS ? cf_nalb + 1 : library_albums();
}

/* An artist's picture: the first of their albums that has a cover */
static int artist_cover_album(int artist)
{
    int first = -1;
    for (int i = 0; i < library_albums(); i++) {
        const los_album *a = library_album(i);
        if (strcmp(a->artist, artist_names[artist]))
            continue;
        if (a->cover[0])
            return i;
        if (first < 0)
            first = i;
    }
    return first;
}

/* A playlist's picture: four of its albums' covers in a square (one when
 * it has only one), made once and kept while Playlists is open */
#define PL_CACHE 8
static struct {
    int pl;                       /* -1: free */
    lv_draw_buf_t buf;
    uint16_t px[COVERFLOW_COVER * COVERFLOW_COVER];
} pl_pic[PL_CACHE];
static int pl_pic_next;

static void pl_pics_forget(void)
{
    for (int k = 0; k < PL_CACHE; k++)
        pl_pic[k].pl = -1;
}

/* Four covers in a square, from up to four albums (fewer repeat) */
static void cover_square(const int *pick, int np, uint16_t *px, lv_draw_buf_t *buf)
{
    const int C = COVERFLOW_COVER, H = COVERFLOW_COVER / 2;
    for (int q = 0; q < 4; q++) {
        const los_album *a = library_album(pick[(q == 3 && np == 2) ? 0 : q % np]);
        const lv_draw_buf_t *src = a && a->cover[0] ? cover_thumb(a->cover, a->cover_offset, C) : NULL;
        int ox = (q & 1) * H, oy = (q >> 1) * H;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < H; x++)
                px[(oy + y) * C + ox + x] = src ?
                    ((const uint16_t *)(src->data + (uint32_t)(y * 2) * src->header.stride))[x * 2] :
                    lv_color_to_u16(theme.raised);
    }
    lv_draw_buf_init(buf, C, C, LV_COLOR_FORMAT_RGB565, C * 2, px, (uint32_t)(C * C * 2));
    lv_image_cache_drop(buf);
}

/* An artist's All songs: their albums' covers in a square */
static uint16_t all_songs_px[COVERFLOW_COVER * COVERFLOW_COVER];
static lv_draw_buf_t all_songs_buf;
static int all_songs_for = -1;

static const lv_draw_buf_t *all_songs_cover(int artist)
{
    if (all_songs_for != artist) {
        int pick[4], np = 0;
        for (int k = 0; k < cf_nalb && np < 4; k++)
            if (library_album(cf_albums[k])->cover[0])
                pick[np++] = cf_albums[k];
        if (!np)
            return NULL;
        cover_square(pick, np, all_songs_px, &all_songs_buf);
        all_songs_for = artist;
    }
    return &all_songs_buf;
}

static const lv_draw_buf_t *pl_cover(int i)
{
    for (int k = 0; k < PL_CACHE; k++)
        if (pl_pic[k].pl == i)
            return &pl_pic[k].buf;
    static int albums[QUEUE_MAX], tracks[QUEUE_MAX];
    int n = playlist_load(i, albums, tracks, QUEUE_MAX), pick[4], np = 0;
    for (int k = 0; k < n && np < 4; k++) {
        const los_album *a = library_album(albums[k]);
        bool seen = false;
        for (int j = 0; j < np; j++)
            seen |= pick[j] == albums[k];
        if (!seen && a && a->cover[0])
            pick[np++] = albums[k];
    }
    if (np == 0)
        return NULL;
    if (np < 4) {                     /* one cover, whole */
        const los_album *a = library_album(pick[0]);
        return cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER);
    }
    int slot = pl_pic_next;
    pl_pic_next = (pl_pic_next + 1) % PL_CACHE;
    cover_square(pick, np, pl_pic[slot].px, &pl_pic[slot].buf);
    pl_pic[slot].pl = i;
    return &pl_pic[slot].buf;
}

static const lv_draw_buf_t *cf_cover(int i)
{
    if (cf.kind == SHELF_PLAYLISTS)
        return pl_cover(i);
    if (cf.kind == SHELF_ARTIST_ALBUMS) {
        if (i == 0)
            return all_songs_cover(cf.who);
        const los_album *a = i - 1 < cf_nalb ? library_album(cf_albums[i - 1]) : NULL;
        return a && a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
    }
    const los_album *a = library_album(cf.kind == SHELF_ARTISTS ? artist_cover_album(i) : i);
    return a && a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
}

/* Gallery's background: the cover's own colour, dark (about a third of
 * its brightness, a little richer than the average) */
static lv_color_t cover_tint(const lv_draw_buf_t *b)
{
    if (!b || !b->header.w || !b->header.h)
        return theme.bg;
    unsigned r = 0, g = 0, bl = 0, n = 0;
    for (uint32_t y = 3; y < b->header.h; y += 8) {
        const uint16_t *row = (const uint16_t *)(b->data + y * b->header.stride);
        for (uint32_t x = 3; x < b->header.w; x += 8) {
            uint16_t p = row[x];
            r += (p >> 11) << 3;
            g += ((p >> 5) & 63) << 2;
            bl += (p & 31) << 3;
            n++;
        }
    }
    int c[3] = { (int)(r / n), (int)(g / n), (int)(bl / n) }, grey = (c[0] + c[1] + c[2]) / 3;
    for (int k = 0; k < 3; k++) {
        int v = grey + (c[k] - grey) * 3 / 2;          /* richer */
        v = v * 34 / 100 + 8;                           /* dark */
        c[k] = v < 8 ? 8 : v > 110 ? 110 : v;
    }
    /* as the covers' own format has it (RGB565), so the screen around them
     * is exactly the same colour */
    lv_color_t t = lv_color_make((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
    uint16_t q = lv_color_to_u16(t);
    unsigned r5 = q >> 11, g6 = (q >> 5) & 63, b5 = q & 31;
    return lv_color_make((uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g6 << 2 | g6 >> 4), (uint8_t)(b5 << 3 | b5 >> 2));
}

static void cf_text(void)
{
    char sub[80] = "";
    const char *title = "";
    if (cf.kind == SHELF_ARTISTS && cf.target < nartists) {
        title = artist_names[cf.target];
        int albums = 0;
        for (int i = 0; i < library_albums(); i++)
            albums += !strcmp(library_album(i)->artist, title);
        snprintf(sub, sizeof(sub), albums == 1 ? "1 album" : "%d albums", albums);
    } else if (cf.kind == SHELF_PLAYLISTS && cf.target < playlists_count()) {
        const los_playlist *pl = playlist_get(cf.target);
        title = pl->name;
        snprintf(sub, sizeof(sub), pl->songs == 1 ? "1 song" : "%d songs", pl->songs);
    } else if (cf.kind == SHELF_ARTIST_ALBUMS) {
        if (cf.target == 0) {
            int songs = 0;
            for (int k = 0; k < cf_nalb; k++)
                songs += library_album(cf_albums[k])->ntracks;
            title = "All songs";
            snprintf(sub, sizeof(sub), songs == 1 ? "1 song" : "%d songs", songs);
        } else if (cf.target - 1 < cf_nalb) {
            const los_album *a = library_album(cf_albums[cf.target - 1]);
            title = a->title;
            if (a->year)
                snprintf(sub, sizeof(sub), "%d", a->year);
        }
    } else if (cf.kind == SHELF_ALBUMS) {
        const los_album *a = library_album(cf.target);
        title = a ? a->title : "";
        snprintf(sub, sizeof(sub), "%s", a ? a->artist : "");
    }
    lv_label_set_text(cf.title, title);
    lv_label_set_text(cf.artist, sub);
    if (theme.gallery)
        cf.tint_to = cover_tint(cf_cover(cf.target));
}

/* The background now, as RGB565 has it (the covers' format), so the
 * screen and the space around the covers always match */
static lv_color_t cf_tint_now(void)
{
    lv_color_t c = lv_color_make((uint8_t)cf.tint[0], (uint8_t)cf.tint[1], (uint8_t)cf.tint[2]);
    uint16_t q = lv_color_to_u16(c);
    unsigned r5 = q >> 11, g6 = (q >> 5) & 63, b5 = q & 31;
    return lv_color_make((uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g6 << 2 | g6 >> 4), (uint8_t)(b5 << 3 | b5 >> 2));
}

static void cf_tint_apply(void)
{
    lv_obj_set_style_bg_color(screen_obj, cf_tint_now(), 0);
}

static void cf_draw(void)
{
    int n = shelf_count(cf.kind);
    if (theme.gallery) {
        coverflow_draw_flat(cf.img, cf.pos, n, cf_cover, cf_tint_now(), theme.raised, theme.accent,
                            cf.kind == SHELF_ARTISTS);
        return;
    }
    /* Classic's covers are on black, as on the classic players */
    coverflow_draw(cf.img, cf.pos, n, cf_cover, theme.classic ? lv_color_black() : theme.bg,
                   theme.classic ? lv_color_hex(0x3A3A3A) : theme.line, theme.accent);
}

static void cf_tick(lv_timer_t *t)
{
    (void)t;
    if (!cf.img || stack[depth - 1].screen != SCR_COVERS)
        return;
    float gap = cf.target - cf.pos;
    bool tinting = false;
    if (theme.gallery) {
        float to[3] = { cf.tint_to.red, cf.tint_to.green, cf.tint_to.blue };
        for (int k = 0; k < 3; k++) {
            float d = to[k] - cf.tint[k];
            if (d != 0) {
                cf.tint[k] = fabsf(d) < 1.5f ? to[k] : cf.tint[k] + d * 0.18f;
                tinting = true;
            }
        }
    }
    if (gap == 0 && !tinting)
        return;
    if (gap != 0)
        cf.pos = fabsf(gap) < 0.02f ? cf.target : cf.pos + gap * 0.3f;
    if (tinting)
        cf_tint_apply();
    cf_draw();
}

static void build_covers(void)
{
    int param = stack[depth - 1].param, kind = shelf_kind(param);
    if (kind == SHELF_ARTIST_ALBUMS) {
        cf.who = param - SHELF_ARTIST_BASE;
        if (cf.who < 0 || cf.who >= nartists)
            cf.who = 0;
        cf_nalb = artist_albums(cf.who, cf_albums);
        all_songs_for = -1;
    }
    static const char *const names[] = { "Albums", "Artists", "Playlists", "" };
    header = header_create(screen_obj, kind == SHELF_ARTIST_ALBUMS ? artist_names[cf.who] : names[kind]);
    if (param != cf.param)
        cf.pos = -1;                          /* another shelf: no slide from the last one */
    cf.kind = kind;
    cf.param = param;
    if (kind == SHELF_PLAYLISTS)
        pl_pics_forget();                     /* playlists may have changed */
    int n = shelf_count(kind);
    if (!n && kind != SHELF_PLAYLISTS && !library_albums()) {
        no_music_note();
        return;
    }
    if (!n) {
        static const char *const none[] = { "No albums", "No artists", "No playlists", "No albums" };
        lv_obj_t *t = theme_tag(screen_obj, none[kind], theme.muted);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    cf.back = NULL;                           /* the screen it was on is gone */
    int sel = stack[depth - 1].selected;
    cf.target = sel < 0 ? 0 : sel >= n ? n - 1 : sel;
    if (cf.pos < 0 || cf.pos > n - 1 || fabsf(cf.pos - cf.target) > 3)
        cf.pos = (float)cf.target;            /* coming back: no long slide */
    if (theme.classic) {
        lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
    }
    cf.img = coverflow_create(screen_obj);
    lv_obj_set_pos(cf.img, 0, LOS_HEADER_H + (theme.gallery ? 12 : 0));   /* Gallery: even space above and below */
    cf.title = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(cf.title, theme.title_small, 0);
    lv_obj_set_style_text_color(cf.title, theme.classic ? lv_color_white() : theme.fg, 0);
    lv_obj_set_style_text_align(cf.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(cf.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(cf.title, LOS_W - 2 * LOS_EDGE, lv_font_get_line_height(theme.title_small));   /* one line */
    lv_obj_set_pos(cf.title, LOS_EDGE, LOS_HEADER_H + COVERFLOW_H - 2);
    cf.artist = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(cf.artist, theme.small, 0);
    lv_obj_set_style_text_color(cf.artist, theme.classic ? lv_color_hex(0xB0B0B0) : theme.muted, 0);
    lv_obj_set_style_text_align(cf.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(cf.artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(cf.artist, LOS_W - 2 * LOS_EDGE, lv_font_get_line_height(theme.small));      /* one line */
    lv_obj_set_pos(cf.artist, LOS_EDGE, LOS_HEADER_H + COVERFLOW_H + 18);
    cf_text();
    if (theme.gallery) {                      /* straight to the cover's colour on arrival */
        cf.tint[0] = cf.tint_to.red;
        cf.tint[1] = cf.tint_to.green;
        cf.tint[2] = cf.tint_to.blue;
        cf_tint_apply();
    }
    cf_draw();
    if (!cf.timer)
        cf.timer = lv_timer_create(cf_tick, 16, NULL);
}

/* What cover i sorts under: the album's title, the artist's or playlist's name */
static char cf_key(int i)
{
    if (cf.kind == SHELF_ARTISTS)
        return key_of(i < nartists ? artist_names[i] : "");
    if (cf.kind == SHELF_PLAYLISTS)
        return key_of(i < playlists_count() ? playlist_get(i)->name : "");
    const los_album *a = library_album(i);
    return key_of(a ? a->title : "");
}

/* A fast spin on Covers: as in the lists, each step then goes to the next
 * letter (not an artist's own albums: too few to need it) */
static bool cf_letter_jump(uint32_t key, int n)
{
    if (cf.kind == SHELF_ARTIST_ALBUMS || n < 2)
        return false;
    uint32_t now = lv_tick_get();
    int dir = key == LV_KEY_DOWN ? 1 : -1;
    if (dir == jump.dir && lv_tick_diff(now, jump.last) < JUMP_GAP_MS)
        jump.streak++;
    else if (!jump.on)
        jump.streak = 0;
    jump.dir = dir;
    jump.last = now;
    if (!jump.on && jump.streak < JUMP_AFTER)
        return false;
    jump.on = true;
    int i = cf.target;
    char k = cf_key(i);
    if (dir > 0) {
        while (i < n - 1 && cf_key(i) == k)
            i++;
    } else {
        if (i > 0 && cf_key(i - 1) != k)
            i--;                          /* at a letter's start: into the one before */
        k = cf_key(i);
        while (i > 0 && cf_key(i - 1) == k)
            i--;
    }
    cf.target = i;
    stack[depth - 1].selected = i;
    cf_text();
    jump_show(cf_key(i));
    return true;
}

/* The wheel and Prev / Next flip through, Play opens what's in front */
static bool covers_key(uint32_t key)
{
    if (stack[depth - 1].screen != SCR_COVERS || !cf.img)
        return false;
    if (cf.back) {                    /* turned over: the songs */
        switch (key) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            cf_back_select(cf.back_sel - 1);
            return true;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            cf_back_select(cf.back_sel + 1);
            return true;
        case LV_KEY_ENTER: {
            int album = cf.target, track = cf.back_sel;
            cf_unflip();
            queue_from = FROM_ALBUM;
            snprintf(queue_key, sizeof(queue_key), "%s", library_album(album)->dir);
            player_play(album, track);
            push(SCR_NOW_PLAYING, 0);
            return true;
        }
        case LV_KEY_ESC:
            cf_unflip();
            return true;
        default:
            return false;
        }
    }
    int n = shelf_count(cf.kind);
    if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && cf_letter_jump(key, n))
        return true;
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_LEFT:
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT: {
        int next = cf.target + (key == LV_KEY_DOWN || key == LV_KEY_RIGHT ? 1 : -1);   /* clockwise: on */
        if (next < 0 || next >= n)
            return true;
        cf.target = next;
        stack[depth - 1].selected = next;
        cf_text();
        return true;
    }
    case LV_KEY_ENTER:
        if (cf.kind == SHELF_ARTIST_ALBUMS) {
            if (cf.target == 0)
                push(SCR_ARTIST_SONGS, cf.who);
            else
                push(SCR_ALBUM, cf_albums[cf.target - 1]);
        } else if (cf.kind == SHELF_ARTISTS)
            push(theme.gallery ? SCR_COVERS : SCR_ALBUMS,   /* Gallery: their albums as covers */
                 theme.gallery ? SHELF_ARTIST_BASE + cf.target : cf.target);
        else if (cf.kind == SHELF_PLAYLISTS)
            push(SCR_PLAYLIST, cf.target);
        else if (theme.classic)
            cf_flip();                /* Classic: turn the cover over */
        else
        {
            morph_from(cf.target, (LOS_W - COVERFLOW_COVER) / 2, LOS_HEADER_H + 12 + 6, COVERFLOW_COVER);
            push(SCR_ALBUM, cf.target);
        }
        return true;
    default:
        return false;
    }
}

/* ---- Gallery's menu ----
 * Back on one of Gallery's views (Albums, Artists, Songs, Playlists: the
 * bottom of the stack) slides a panel in from the left and moves the view
 * over, dimmed (Menu goes to Now Playing, as everywhere; with nothing
 * playing it opens this too). The wheel picks, Play goes there, Back or
 * Menu closes. */
#define GM_W      128
#define GM_SHIFT  96
#define GM_N      6
static struct {
    lv_obj_t *panel, *item[GM_N];
    int sel;
    bool closing;
} gm;
static int root_sel[4];               /* where each view was, to go back to it */

static const char *const gm_names[GM_N] = { "Albums", "Artists", "Songs", "Playlists",
                                            "Now Playing", "Settings" };

/* The view at the bottom of the stack as a menu row (0-3) */
static int gm_view(void)
{
    if (stack[0].screen == SCR_SONGS)
        return 2;
    int k = shelf_kind(stack[0].param);
    return k == SHELF_ARTISTS ? 1 : k == SHELF_PLAYLISTS ? 3 : 0;
}

static int gm_y(int i)
{
    return i < 4 ? 40 + i * 30 : 172 + (i - 4) * 26;
}

/* The chosen one big and bright; the others smaller, and fainter the
 * farther they are from it */
static void gm_mark(void)
{
    static const lv_opa_t fade[] = { LV_OPA_COVER, 150, 105, 80, 64, 56 };
    for (int i = 0; i < GM_N; i++) {
        bool on = i == gm.sel, off = i == 4 && player.album < 0;   /* nothing to go to */
        const lv_font_t *f = i < 4 ? (on ? theme.title : theme.body) : (on ? theme.body : theme.small);
        int d = i > gm.sel ? i - gm.sel : gm.sel - i, slot = i < 4 ? 30 : 26;
        lv_obj_set_style_text_font(gm.item[i], f, 0);
        lv_obj_set_style_text_color(gm.item[i], off ? theme.faint : theme.fg, 0);
        lv_obj_set_style_text_opa(gm.item[i], fade[d], 0);
        lv_obj_set_y(gm.item[i], gm_y(i) + (slot - lv_font_get_line_height(f)) / 2);
    }
}

/* 0 closed to 256 open: the panel in, the view over and dimmed */
static void gm_slide(void *v, int32_t t)
{
    (void)v;
    if (!gm.panel)
        return;
    lv_obj_set_x(gm.panel, -GM_W + GM_W * t / 256);
    uint32_t n = lv_obj_get_child_count(screen_obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(screen_obj, (int32_t)i);
        if (c == gm.panel || c == header)
            continue;
        lv_obj_set_style_translate_x(c, GM_SHIFT * t / 256, 0);
        lv_obj_set_style_opa(c, (lv_opa_t)(255 - 160 * t / 256), 0);
    }
}

static void gm_animate(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, &gm);
    lv_anim_set_exec_cb(&an, gm_slide);
    lv_anim_set_values(&an, from, to);
    lv_anim_set_duration(&an, 200);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    if (done)
        lv_anim_set_completed_cb(&an, done);
    lv_anim_start(&an);
}

static void gm_open(void)
{
    gm.panel = theme_box(screen_obj);
    lv_obj_set_size(gm.panel, GM_W, LOS_H);
    lv_obj_set_pos(gm.panel, -GM_W, 0);
    lv_obj_set_style_bg_color(gm.panel, lv_color_hex(0x080808), 0);
    lv_obj_set_style_bg_opa(gm.panel, LV_OPA_COVER, 0);
    lv_obj_t *cap = lv_label_create(gm.panel);
    lv_label_set_text(cap, "Library");
    lv_obj_set_style_text_font(cap, theme.mono, 0);
    lv_obj_set_style_text_color(cap, theme.faint, 0);
    lv_obj_set_pos(cap, 18, 16);
    for (int i = 0; i < GM_N; i++) {
        gm.item[i] = lv_label_create(gm.panel);
        lv_label_set_text(gm.item[i], gm_names[i]);
        lv_obj_set_style_text_font(gm.item[i], i < 4 ? theme.row : theme.small, 0);
        lv_obj_set_pos(gm.item[i], 18, gm_y(i));
    }
    gm.sel = gm_view();
    gm.closing = false;
    gm_mark();
    gm_animate(0, 256, NULL);
}

static void gm_gone(lv_anim_t *a)
{
    (void)a;
    if (gm.panel)
        lv_obj_delete(gm.panel);
    gm.panel = NULL;
}

static void gm_close(void)
{
    if (!gm.panel || gm.closing)
        return;
    gm.closing = true;
    gm_animate(256, 0, gm_gone);
}

static void gm_choose(void)
{
    int sel = gm.sel;
    lv_anim_delete(&gm, gm_slide);
    if (sel == 4 && player.album < 0)
        return;
    gm.panel = NULL;                  /* the screen goes, and the panel with it */
    if (sel >= 4) {
        push(sel == 4 ? SCR_NOW_PLAYING : SCR_SETTINGS, sel == 4 ? 0 : SG_TOP);
        return;
    }
    root_sel[gm_view()] = stack[0].selected;
    depth = 1;
    stack[0].screen = sel == 2 ? SCR_SONGS : SCR_COVERS;
    stack[0].param = sel == 1 ? SHELF_ARTISTS : sel == 3 ? SHELF_PLAYLISTS : SHELF_ALBUMS;
    stack[0].selected = root_sel[sel];
    show();
}

static void gm_key(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        gm.sel += key == LV_KEY_DOWN ? 1 : -1;
        gm.sel = gm.sel < 0 ? 0 : gm.sel >= GM_N ? GM_N - 1 : gm.sel;
        gm_mark();
        break;
    case LV_KEY_ENTER:
        gm_choose();
        break;
    case LV_KEY_ESC:
    case LV_KEY_HOME:
    case LV_KEY_LEFT:
        gm_close();
        break;
    default:
        break;
    }
}

/* The bottom of the stack for the theme: Gallery starts on Albums as
 * covers, the others on Home */
static void root_fix(void)
{
    bool gallery_root = stack[0].screen == SCR_COVERS || stack[0].screen == SCR_SONGS;
    if (theme.gallery && !gallery_root) {
        stack[0].screen = SCR_COVERS;
        stack[0].param = SHELF_ALBUMS;
        stack[0].selected = 0;
    } else if (!theme.gallery && stack[0].screen != SCR_HOME) {
        stack[0].screen = SCR_HOME;
        stack[0].param = 0;
        stack[0].selected = 0;
    }
}

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
    char lines[8][96];
    int n = 0, songs = 0;
    for (int i = 0; i < library_albums(); i++)
        songs += library_album(i)->ntracks;
    snprintf(lines[n++], 96, "Lossless OS %s (%s)", LOS_VERSION, LOS_BUILD);
    snprintf(lines[n++], 96, "Player: %s", los_player_name());
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
        lv_obj_set_pos(l, LOS_EDGE, LOS_HEADER_H + 14 + i * lh + (i ? 6 : 0));
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
    lv_obj_t *row = row_begin(ROW_H);
    int b = centred_baseline(theme.row, ROW_H);
    row_label(row, name, theme.row, theme.fg, LOS_EDGE, top_for_baseline(theme.row, b), 140);
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
    list_footer(clk.editing ? "Wheel changes it  \xc2\xb7  Play sets  \xc2\xb7  Back cancels"
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
    if (theme.classic) {              /* Classic: no line under lists (kept, hidden, for its users) */
        lv_obj_t *l = lv_label_create(screen_obj);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(l, text);
        return l;
    }
    int h = list_height();
    lv_obj_set_height(list.cont, h - 22);
    lv_obj_t *l = lv_label_create(screen_obj);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, theme.mono, 0);
    lv_obj_set_style_text_color(l, theme.muted, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, LOS_EDGE, LOS_HEADER_H + 1 + h - 18);
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
    if (b.state == BT_FAILED)
        snprintf(line, sizeof(line), "%s", b.message);   /* why it went off */
    else if (!b.on)
        snprintf(line, sizeof(line), b.nknown ? "Off  \xc2\xb7  reconnects to %s when on" : "Off",
                 b.nknown ? b.known[0].name : "");
    else if (b.state == BT_STREAMING && b.rate)
        snprintf(line, sizeof(line), "%s  \xc2\xb7  SBC %d.%d kHz", b.peer, b.rate / 1000,
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
                b.ndevices ? "Play to connect  \xc2\xb7  visible as Lossless" :
                "None found. Play searches  \xc2\xb7  visible as Lossless");
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
    lv_label_set_text(h, b->passkey ? "Back closes" : "Play pairs  \xc2\xb7  Back cancels");
    lv_obj_set_style_text_font(h, theme.mono, 0);
    lv_obj_set_style_text_color(h, theme.faint, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* ---- installing (install.c): the update question at start, Uninstall in Settings ---- */

enum ibox_kind { IBOX_ASK_UPDATE, IBOX_ASK_RESET, IBOX_DONE, IBOX_OFF };
static void reset_settings(void);
static lv_obj_t *ibox;
static enum ibox_kind ibox_kind;
static uint32_t ibox_off_at;      /* IBOX_OFF: switch off at this time */

static void ibox_close(void)
{
    if (ibox)
        lv_obj_delete(ibox);
    ibox = NULL;
}

static lv_obj_t *ibox_text(const char *s, const lv_font_t *f, lv_color_t c, int y)
{
    lv_obj_t *l = lv_label_create(ibox);
    lv_label_set_text(l, s);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LOS_W - 40);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, 20, y);
    return l;
}

static void ibox_open(enum ibox_kind kind, const char *title, const char *body, const char *keys)
{
    ibox_close();
    ibox_kind = kind;
    screen_light(100);
    last_key_at = lv_tick_get();
    ibox = theme_box(lv_layer_top());
    lv_obj_set_size(ibox, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(ibox, theme.bg, 0);
    lv_obj_set_style_bg_opa(ibox, LV_OPA_COVER, 0);
    header_create(ibox, NULL);
    lv_obj_t *t = ibox_text(title, theme.title_small, theme.fg, LOS_HEADER_H + 12);
    lv_obj_update_layout(t);
    ibox_text(body, theme.small, theme.second, LOS_HEADER_H + 20 + lv_obj_get_height(t));
    lv_obj_t *h = lv_label_create(ibox);
    lv_label_set_text(h, keys);
    lv_obj_set_style_text_font(h, theme.mono, 0);
    lv_obj_set_style_text_color(h, theme.faint, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -8);
    if (kind == IBOX_OFF)
        ibox_off_at = (lv_tick_get() + 8000) | 1;
}

/* At start: the release on the card is newer than what starts at power-on */
static void install_ask(void)
{
    char probe[240];
    snprintf(probe, sizeof(probe), "%s" BOOTFILE_PACKAGE, los_card_path());
    void *f = los_file_open(probe);           /* the full look reads it all: only if it's there */
    if (!f)
        return;
    los_file_close(f);
    static install_state s;
    install_look(&s);
    if (install_offer(&s) != INSTALL_UPDATE)
        return;
    char title[64], body[200], have[16], next[16];
    bootfile_version_text(s.package.version, next, sizeof(next));
    bootfile_version_text(s.current.version, have, sizeof(have));
    snprintf(title, sizeof(title), "Update to Lossless OS %s?", next);
    snprintf(body, sizeof(body), "This is %s. Your music, settings and pairings stay.", have);
    ibox_open(IBOX_ASK_UPDATE, title, body, "Play updates  \xc2\xb7  Back: not now");
}

static void install_answer(bool yes)
{
    if (!yes) {
        ibox_close();
        return;
    }
    ibox_open(IBOX_DONE, "Updating...", "Writing the new version and checking it. A few seconds.", "");
    lv_refr_now(NULL);
    playback_pause(true);
    const char *err = install_run();
    if (err) {
        ibox_open(IBOX_DONE, "Not updated", err, "Any key closes");
        return;
    }
    ibox_open(IBOX_DONE, "Updated",
              "The new version starts next time. You can delete the Lossless folder "
              "from the card.",
              "Any key closes");
}

static void ibox_key(uint32_t key)
{
    if (ibox_kind == IBOX_OFF) {
        ibox_off_at = lv_tick_get() | 1;        /* now */
    } else if (ibox_kind == IBOX_DONE) {
        ibox_close();
        show();
    } else if (ibox_kind == IBOX_ASK_RESET) {
        if (key == LV_KEY_ENTER) {
            ibox_close();
            reset_settings();
            show();
            toast("Settings reset");
        } else if (key == LV_KEY_ESC) {
            ibox_close();
        }
    } else if (key == LV_KEY_ENTER) {
        install_answer(true);
    } else if (key == LV_KEY_ESC) {
        install_answer(false);
    }
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

static bool np_volume(int *at, int *span);
static bool fav_has(int album, int track);

/* Now Playing's heart, by the count, while the song is in Favourites */
static lv_obj_t *np_heart;

static void heart_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int x = a.x1, y = a.y1;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = theme.accent;
    r.bg_opa = LV_OPA_COVER;
    r.radius = LV_RADIUS_CIRCLE;
    lv_area_t l = { x, y, x + 5, y + 5 }, rt = { x + 5, y, x + 10, y + 5 };
    lv_draw_rect(layer, &r, &l);
    lv_draw_rect(layer, &r, &rt);
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = theme.accent;
    t.opa = LV_OPA_COVER;
    t.p[0].x = x; t.p[0].y = y + 3;
    t.p[1].x = x + 11; t.p[1].y = y + 3;
    t.p[2].x = x + 5.5f; t.p[2].y = y + 10;
    lv_draw_triangle(layer, &t);
}

static void np_heart_place(void)
{
    if (!np_heart || !np_count || player.album < 0)
        return;
    if (!fav_has(player.album, player.track)) {
        lv_obj_add_flag(np_heart, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(np_count), lv_obj_get_style_text_font(np_count, 0), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_update_layout(np_count);
    lv_obj_set_pos(np_heart, lv_obj_get_x(np_count) + sz.x + 8, lv_obj_get_y(np_count) + (sz.y - 10) / 2);
    lv_obj_remove_flag(np_heart, LV_OBJ_FLAG_HIDDEN);
}

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
        if (theme.classic || theme.gallery)
            lv_label_set_text(np_time, buf);   /* Classic: plain text times */
        else
            dots_set_text(np_time, buf);
        buf[0] = '-';
        fmt_time(buf + 1, sizeof(buf) - 1, len > at ? len - at : 0);
        if (theme.classic || theme.gallery)
            lv_label_set_text(np_left, buf);
        else
            dots_set_text(np_left, buf);
        lv_obj_update_layout(np_left);    /* its new width, before placing it */
        lv_obj_set_x(np_left, LOS_W - (theme.gallery ? 16 : 12) - lv_obj_get_width(np_left));
        int vat, vspan;
        bool vol_bar = np_spk_lo && np_volume(&vat, &vspan);   /* Classic: the volume, for a moment */
        segbar_set(np_bar, vol_bar ? vat : at, vol_bar ? vspan : len);
        if (np_spk_lo) {
            lv_obj_t *times[] = { np_time, np_left }, *spk[] = { np_spk_lo, np_spk_hi };
            for (int k = 0; k < 2; k++) {
                if (vol_bar) {
                    lv_obj_add_flag(times[k], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(spk[k], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_remove_flag(times[k], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(spk[k], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        snprintf(buf, sizeof(buf), (theme.classic || theme.gallery) ? "%d of %d" : "%d OF %d", queue_pos + 1, queue_len);
        state_icon_set(np_icon, player.state);
        lv_label_set_text(np_count, buf);
        np_heart_place();
        lv_label_set_text(np_title, t->title);
        const char *rep = repeat_mode == REPEAT_ALL ? ((theme.classic || theme.gallery) ? "Repeat" : "REPEAT") :
                          repeat_mode == REPEAT_ONE ? ((theme.classic || theme.gallery) ? "Repeat one" : "REPEAT 1") : "";
        snprintf(buf, sizeof(buf), "%s%s%s", shuffle ? ((theme.classic || theme.gallery) ? "Shuffle" : "SHUFFLE") : "",
                 shuffle && rep[0] ? "   \xc2\xb7   " : "", rep);
        lv_label_set_text(np_modes, buf);
    }
}

/* Four times a second: the time from what has been heard, and on to the
 * next song when one ends (or can't be played) */
/* ---- volume (the player's buttons change it; the header shows it) ---------------------- */

#define VOLUME_FLASH_MS 2000   /* the dB shows this long after the last change */

static lv_obj_t *lock_scr;        /* the lock screen, below */
static lv_obj_t *lk_cover;        /* its cover */
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
    /* darker, never black: a simulator window that looks dead confuses (a key wakes it) */
    lv_obj_set_style_bg_opa(shade, percent >= 100 ? LV_OPA_TRANSP :
                            percent > 0 ? LV_OPA_40 : LV_OPA_70, 0);
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

/* The volume as a bar's level, while it's just been changed (Classic's
 * Now Playing shows it in place of the progress) */
static bool np_volume(int *at, int *span)
{
    (void)at;
    (void)span;
    return false;                     /* the volume pill shows it now (fx_volume) */
}

/* Classic's volume bar has had its moment: the progress again */
static void volume_bar_done(lv_timer_t *t)
{
    *(lv_timer_t **)lv_timer_get_user_data(t) = NULL;   /* it goes after this run */
    if (stack[depth - 1].screen == SCR_NOW_PLAYING)
        refresh();
}

static void volume_changed(void)
{
    settings_changed();
    vol.changed_at = lv_tick_get();
    if (!vol.changed_at)
        vol.changed_at = 1;
    if ((theme.classic || theme.gallery) && stack[depth - 1].screen == SCR_NOW_PLAYING) {
        refresh();                    /* the volume bar, now, and the progress back after */
        static lv_timer_t *back;
        if (back)
            lv_timer_reset(back);
        else {
            back = lv_timer_create(volume_bar_done, VOLUME_FLASH_MS + 50, &back);
            lv_timer_set_repeat_count(back, 1);
        }
    }
}

static void fx_volume(void);

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
    fx_volume();
}

static void player_toggle(void);
static void player_skip(int dir);
static void show(void);
static void refresh(void);
static void bt_screen_watch(void);


static los_settings saved;      /* as loaded at start */
void los_app_before_off(void);

/* ---- the volume, felt on screen ----
 * As a phone answers its side buttons: a slim pill that slides in at the
 * right edge (where the volume buttons are), the dB beside it. */
#define VP_W   10
#define VP_H   110
#define VP_GAP 8
static struct {
    lv_obj_t *pill, *fill, *db;
    lv_timer_t *hide;
} vfx;

static void vfx_x(void *o, int32_t x)
{
    lv_obj_set_x(o, x);
    if (vfx.db)
        lv_obj_set_x(vfx.db, x - lv_obj_get_width(vfx.db) - 8);
}

static void vfx_gone(lv_anim_t *a)
{
    (void)a;
    if (vfx.pill)
        lv_obj_delete(vfx.pill);
    if (vfx.db)
        lv_obj_delete(vfx.db);
    vfx.pill = vfx.fill = vfx.db = NULL;
}

static void vfx_hide(lv_timer_t *t)
{
    (void)t;
    vfx.hide = NULL;
    if (!vfx.pill)
        return;
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, vfx.pill);
    lv_anim_set_exec_cb(&an, vfx_x);
    lv_anim_set_values(&an, lv_obj_get_x(vfx.pill), LOS_W + 4);
    lv_anim_set_duration(&an, 220);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&an, vfx_gone);
    lv_anim_start(&an);
}

static void fx_volume(void)
{
    if (!vol.known || vol.hi <= vol.lo)
        return;
    bool fresh = !vfx.pill;
    if (fresh) {
        vfx.pill = theme_box(lv_layer_top());
        lv_obj_set_size(vfx.pill, VP_W, VP_H);
        lv_obj_set_pos(vfx.pill, LOS_W + 4, (LOS_H - VP_H) / 2 + 8);
        lv_obj_set_style_radius(vfx.pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(vfx.pill, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(vfx.pill, 220, 0);
        lv_obj_set_style_border_color(vfx.pill, lv_color_white(), 0);
        lv_obj_set_style_border_opa(vfx.pill, 70, 0);
        lv_obj_set_style_border_width(vfx.pill, 1, 0);
        lv_obj_set_style_clip_corner(vfx.pill, true, 0);
        vfx.fill = theme_box(vfx.pill);
        lv_obj_set_width(vfx.fill, LV_PCT(100));
        lv_obj_set_style_bg_color(vfx.fill, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(vfx.fill, LV_OPA_COVER, 0);
        lv_obj_set_align(vfx.fill, LV_ALIGN_BOTTOM_MID);
        vfx.db = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(vfx.db, theme.small, 0);
        lv_obj_set_style_text_color(vfx.db, lv_color_white(), 0);
        lv_obj_set_style_bg_color(vfx.db, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(vfx.db, 200, 0);
        lv_obj_set_style_pad_hor(vfx.db, 6, 0);
        lv_obj_set_style_pad_ver(vfx.db, 2, 0);
        lv_obj_set_style_radius(vfx.db, 6, 0);
        lv_obj_set_y(vfx.db, (LOS_H - VP_H) / 2 + 8 + VP_H / 2 - 10);
    }
    char t[16];
    snprintf(t, sizeof(t), "%d dB", vol.db);
    lv_label_set_text(vfx.db, t);
    lv_obj_update_layout(vfx.db);
    int h = (VP_H - 2) * (vol.db - vol.lo) / (vol.hi - vol.lo);
    lv_obj_set_height(vfx.fill, h < 1 ? 1 : h);
    if (fresh || lv_obj_get_x(vfx.pill) > LOS_W - VP_W - VP_GAP) {    /* in, from the edge */
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, vfx.pill);
        lv_anim_delete(vfx.pill, vfx_x);
        lv_anim_set_exec_cb(&an, vfx_x);
        lv_anim_set_values(&an, lv_obj_get_x(vfx.pill), LOS_W - VP_W - VP_GAP);
        lv_anim_set_duration(&an, 180);
        lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
        lv_anim_start(&an);
    }
    if (vfx.hide)
        lv_timer_reset(vfx.hide);
    else {
        vfx.hide = lv_timer_create(vfx_hide, 1300, NULL);
        lv_timer_set_repeat_count(vfx.hide, 1);
    }
}

/* The battery in the header every second; the cable going in lights the
 * screen (so you can see it's charging). Nearly empty and not on USB
 * power: save, say so, switch off */
#define BATTERY_EMPTY 2
static void battery_watch(void)
{
    static uint32_t last;
    static int was_plugged = -1;      /* -1: not read yet (no wake at start) */
    if (last && lv_tick_elaps(last) < 1000)
        return;
    last = lv_tick_get() | 1;
    int percent;
    bool charging, usb;
    if (!los_battery(&percent, &charging, &usb)) {
        header_set_battery(-1, false, false);
        return;
    }
    header_set_battery(percent, charging, usb || charging);
    if (was_plugged == 0 && (usb || charging)) {
        last_key_at = lv_tick_get();
        if (light < 100) {
            screen_light(100);
            if (lock_scr)
                lock_update();
        }
    }
    was_plugged = usb || charging;
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
    st.idle_off_min = opt.idle_off_min;
    st.clock24 = opt.clock24;
    st.wheel_volume = opt.wheel_volume;
    st.lock_wheel = opt.lock_wheel;
    st.battery_pct = opt.battery_pct;
    st.eq = eq_enabled;
    st.mono = opt.mono;
    snprintf(st.skin, sizeof(st.skin), "%s", skin_sel);
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

/* Reset settings (About): every setting as it came, every Bluetooth pairing
 * forgotten and Bluetooth off. The music, playlists and favourites stay,
 * and so do the volume, the clock and what was playing. */
static void reset_settings(void)
{
    __typeof__(opt) d = OPT_DEFAULTS;
    opt = d;
    header_set_percent_always(opt.battery_pct);
    output_set_mono(opt.mono);
    skin_use("Graphite");
    root_fix();
    eq_enabled = false;
    for (int c = 0; c < EQ_CHOICES; c++)
        eq_reset(c);
    eq_choose(EQ_CUSTOM);                /* Flat */
    eq_apply();
    bluetooth_forget_all();
    bluetooth_power(false);
    light = -1;
    screen_light(100);                   /* the default brightness */
    save_settings();
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
    install_started_ok();        /* switched off on purpose: not a failed start */
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
        "  \"audio_formats\": [\"flac\", \"alac\", \"aac\", \"wav\", \"aiff\", \"mp3\", \"vorbis\", \"opus\"],\n"
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
        lv_obj_set_style_text_color(l, on ? theme.select_fg : theme.second, 0);   /* the theme's text on its highlight */
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
        if (theme.select_grad) {
            lv_obj_set_style_bg_grad_color(r, theme.select_bottom, 0);
            lv_obj_set_style_bg_grad_dir(r, LV_GRAD_DIR_VER, 0);
        }
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
        snprintf(f, sizeof(f), "%u.%u kHz  \xc2\xb7  %u-bit", rate / 1000, rate % 1000 / 100, bits);
    else if (on)
        snprintf(f, sizeof(f), "%u kHz  \xc2\xb7  %u-bit", rate / 1000, bits);
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
    lv_label_set_text(hint, "Wheel: volume  \xc2\xb7  Back or unplug: stop");
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

/* ---- test runs (boot/stage2, test_run): a request on the card makes the
 * player restart into one of the original firmware's kernels once; back
 * here, what that kernel left in memory is saved for reading on a computer */

static bool test_requested(char *path, size_t n)
{
    snprintf(path, n, "%s/.lossless/test/linux.txt", los_card_path());
    void *f = los_file_open(path);
    if (f)
        los_file_close(f);
    return f != NULL;
}

/* At start: a test has run (the bootloader runs each request once) */
static void test_result_save(void)
{
    char req[300], path[300], text[256] = { 0 };
    if (!los_writes_ok() || !test_requested(req, sizeof(req)))
        return;
    void *f = los_file_open(req);
    size_t n = f ? los_file_read(f, text, sizeof(text) - 1) : 0;
    if (f)
        los_file_close(f);
    snprintf(path, sizeof(path), "%s/.lossless/logs", los_card_path());
    los_mkdir(path);
    snprintf(path, sizeof(path), "%s/.lossless/logs/linux-ram.bin", los_card_path());
    los_save_low_memory(path);
    snprintf(path, sizeof(path), "%s/.lossless/logs/linux-request.txt", los_card_path());
    los_file_save(path, text, n);
    los_file_remove(req);
}

/* The card back from a computer with a request on it: run it */
static void test_start_if_requested(void)
{
    char req[300];
    if (!test_requested(req, sizeof(req)))
        return;
    lv_obj_t *t = theme_tag(lv_layer_top(), "Running a test. Back in a minute.", theme.fg);
    lv_obj_center(t);
    lv_refr_now(NULL);
    los_sleep_ms(1500);
    los_restart();
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
    if (depth > 0 && stack[depth - 1].screen == SCR_SETTINGS && !opts && !usb_away &&
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
        test_start_if_requested();
        scan_begin("Reading the card...", true);
        player.album = -1;
        player.state = 0;
        queue_len = 0;
        library_scan(los_music_path());
        collect_artists();
        playlists_scan();
        fav_reload();
        load_resume();
        write_player_json();
        scan_end();
        if (usb_scr)
            lv_obj_delete(usb_scr);
        usb_scr = NULL;
        depth = 1;                    /* the lists may have changed: from Home */
        stack[0].selected = 0;
        show();
        if (skipped_signature() != skipped_seen) {
            /* music just copied that it couldn't add: say so now */
            skipped_seen = skipped_signature();
            if (library_skipped())
                push(SCR_SKIPPED, 0);
        }
    }
}

static void hold_watch(void);

/* Switch off when idle: nothing playing (here, over Bluetooth or as a USB
 * sound card) and no key for opt.idle_off_min minutes. Not while the card
 * is the computer's (volume_watch stops before this then). */
static void idle_off_watch(void)
{
    static uint32_t active_at;
    if (player.state == 1 || dac_mode)
        active_at = lv_tick_get();
    if (!opt.idle_off_min)
        return;
#ifdef LOS_HOST
    return;                           /* the simulator's window stays open: closing it confused */
#endif
    uint32_t idle = lv_tick_elaps(last_key_at);
    uint32_t quiet = lv_tick_elaps(active_at);
    if ((idle < quiet ? idle : quiet) < (uint32_t)opt.idle_off_min * 60000)
        return;
    los_app_before_off();
    lv_obj_t *t = theme_tag(lv_layer_top(), "Switching off", theme.fg);
    lv_obj_center(t);
    lv_refr_now(NULL);
    los_sleep_ms(1000);
    los_power_off();
}

static void enter_watch(void);

/* Every 40 ms: the volume, and the headphones' buttons */
static void volume_watch(lv_timer_t *tm)
{
    (void)tm;
    usb_watch();
    if (usb_away)
        return;                   /* the card is the computer's */
    hold_watch();
    enter_watch();
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
    idle_off_watch();
    static uint32_t settings_shown;
    if (sleep_at && depth > 0 && stack[depth - 1].screen == SCR_SETTINGS && !opts &&
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
    /* the volume shows in its own pill now (fx_volume): no dB in the title bar */
    bool flash = false;
    /* Classic's Now Playing shows the volume as its own bar, where the
     * time bar is (as the iPod does): no second one under the title bar */
    if ((theme.classic || theme.gallery) && depth > 0 && stack[depth - 1].screen == SCR_NOW_PLAYING && !lock_scr)
        flash = false;
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
    if (lv_tick_get() > 20000)
        install_started_ok();    /* running a while: it got going */
    if (ibox && ibox_kind == IBOX_OFF && (int32_t)(lv_tick_get() - ibox_off_at) >= 0)
        los_power_off();
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
    marquee_row = NULL;              /* the old screen's row goes with it: never touch it again */
    if (!lock_scr || morph.dest != lk_cover)
        morph_abort();               /* a glide onto the old screen: stopped */
    if (marquee_timer)
        lv_timer_pause(marquee_timer);
    header = NULL;
    np_time = NULL;
    np_icon = NULL;
    settings_hint = NULL;
    skipped_hint = NULL;
    cf.img = NULL;
    gm.panel = NULL;
    songs_letter = NULL;
    ga_hero[0] = ga_hero[1] = ga_hero[2] = NULL;
    np_cover = NULL;
    split_art = NULL;
    jump_hide();
    list_w = LOS_W;
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
    case SCR_SETTINGS:    build_settings(n->param); break;
    case SCR_SKIPPED:     build_skipped(); break;
    case SCR_COVERS:      build_covers(); break;
    case SCR_THEMES:      build_themes(); break;
    case SCR_ADD_TO:      build_add_to(); break;
    case SCR_BLUETOOTH:   build_bluetooth(); break;
    case SCR_BT_SEARCH:   build_bt_search(); break;
    case SCR_CLOCK:       build_clock(); break;
    case SCR_ABOUT:       build_about(); break;
    case SCR_EQ:          build_eq(); break;
    case SCR_EQ_PRESETS:  build_eq_presets(); break;
    case SCR_PLAYLISTS:   build_playlists(); break;
    case SCR_PLAYLIST:    build_playlist(n->param); break;
    }
    if (n->screen != SCR_NOW_PLAYING && n->screen != SCR_EQ && n->screen != SCR_COVERS && !theme.classic &&
        !(theme.gallery && n->screen == SCR_ALBUM))   /* Gallery's album page marks what plays itself */
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
    np_heart = NULL;
    if (n->screen == SCR_NOW_PLAYING && player.album >= 0 && np_count) {
        np_heart = theme_box(screen_obj);
        lv_obj_set_size(np_heart, 12, 11);
        lv_obj_add_event_cb(np_heart, heart_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_add_flag(np_heart, LV_OBJ_FLAG_HIDDEN);
    }
    list_select(n->selected);
    refresh();
    morph_run();
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

/* Covers drawn again, if they're on screen (the glide's copy has landed) */
static void cf_redraw(void)
{
    if (cf.img && stack[depth - 1].screen == SCR_COVERS)
        cf_draw();
}

/* After a screen is built: glide to its cover, when it shows the same album */
static void morph_run(void)
{
    if (!morph.on)
        return;
    morph.on = false;
    morph_abort();
    enum screen scr = stack[depth - 1].screen;
    int tx, ty, ts;
    lv_obj_t *dest = NULL;
    if (lock_scr) {
        if (!lk_cover || player.album != morph.album)
            return;
        dest = lk_cover;                             /* into the lock screen */
    } else if (scr == SCR_ALBUM && stack[depth - 1].param == morph.album && ga_hero[0]) {
        dest = ga_hero[0];
    } else if (scr == SCR_NOW_PLAYING && player.album == morph.album && np_cover) {
        dest = np_cover;
    } else if (scr == SCR_COVERS && cf.img && cf.kind == SHELF_ALBUMS && cf.target == morph.album) {
        tx = (LOS_W - COVERFLOW_COVER) / 2;          /* the facing cover, drawn in the strip */
        ty = LOS_HEADER_H + 12 + 6;
        ts = COVERFLOW_COVER;
        coverflow_skip(morph.album);                 /* left out until the copy lands */
        cf_draw();
    } else {
        return;
    }
    if (dest) {
        lv_obj_update_layout(dest);
        tx = lv_obj_get_x(dest);
        ty = lv_obj_get_y(dest);
        ts = lv_obj_get_width(dest);
    }
    const los_album *a = library_album(morph.album);
    const lv_draw_buf_t *src = a && a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
    if (!src)
        return;
    morph.img = lv_image_create(lv_layer_top());
    lv_image_set_src(morph.img, src);
    lv_image_set_pivot(morph.img, 0, 0);
    lv_obj_set_style_clip_corner(morph.img, true, 0);
    morph.tx = tx;
    morph.ty = ty;
    morph.tsize = ts;
    morph.dest = dest;
    if (dest) {
        morph.dest_opa = lv_obj_get_style_opa(dest, 0);
        lv_obj_set_style_opa(dest, LV_OPA_TRANSP, 0);
    }
    morph_step(NULL, 0);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, &morph);
    lv_anim_set_exec_cb(&an, morph_step);
    lv_anim_set_values(&an, 0, 256);
    lv_anim_set_duration(&an, 260);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&an, morph_done);
    lv_anim_start(&an);
}

static void back(void)
{
    if (depth > 1) {
        enum screen scr = stack[depth - 1].screen;
        if (scr == SCR_ALBUM)
            morph_from_obj(stack[depth - 1].param, ga_hero[0]);
        else if (scr == SCR_NOW_PLAYING && player.album >= 0)
            morph_from_obj(player.album, np_cover);
        /* Gallery: back to an album's page at its top, the cover there for
         * the glide to land on */
        if (theme.gallery && scr == SCR_NOW_PLAYING && depth >= 2 && stack[depth - 2].screen == SCR_ALBUM)
            stack[depth - 2].selected = 0;
        depth--;
        show();
    }
}

/* Every song in a shuffled order, from a random one (Home > Shuffle Songs) */
static void shuffle_songs(void)
{
    int n = collect_all_songs(song_index);
    if (n <= 0)
        return;
    nsongs = n;
    shuffle = true;
    settings_changed();
    queue_from = FROM_SONGS;
    queue_key[0] = '\0';
    player_play_list(song_index, n, (int)(lv_tick_get() % (uint32_t)n));
    push(SCR_NOW_PLAYING, 0);
}

/* What a Home or Music row opens */
static void menu_choose(int what)
{
    switch (what) {
    case MR_MUSIC:     push(SCR_MUSIC, 0); break;
    case MR_PLAYLISTS: push(SCR_PLAYLISTS, 0); break;
    case MR_NOW:       push(SCR_NOW_PLAYING, 0); break;
    case MR_SETTINGS:  push(SCR_SETTINGS, 0); break;
    case MR_SHUFFLE:   shuffle_songs(); break;
    case MR_COVERS:    push(SCR_COVERS, -1); break;
    case MR_ARTISTS:   push(SCR_ARTISTS, 0); break;
    case MR_ALBUMS:    push(theme.albums_covers ? SCR_COVERS : SCR_ALBUMS, -1); break;   /* a skin may show covers */
    case MR_SONGS:     push(SCR_SONGS, 0); break;
    case MR_SKIPPED:   push(SCR_SKIPPED, 0); break;
    default: break;
    }
}

/* What a Settings row does when chosen */
static void settings_choose(int r)
{
    static const int dims[] = { 15, 30, 60, 120, 300, 0 };
    static const int sleeps[] = { 0, 15, 30, 45, 60, 90 };
    static const int idle_offs[] = { 5, 15, 30, 60, 0 };
    switch (r) {
    case SR_BLUETOOTH:
        push(SCR_BLUETOOTH, 0);
        return;
    case SR_SOUND:
        push(SCR_SETTINGS, SG_SOUND);
        return;
    case SR_SCREEN:
        push(SCR_SETTINGS, SG_SCREEN);
        return;
    case SR_BATTERY:
        push(SCR_SETTINGS, SG_BATTERY);
        return;
    case SR_WHEEL:
        push(SCR_SETTINGS, SG_WHEEL);
        return;
    case SR_DATE_TIME:
        push(SCR_SETTINGS, SG_DATE_TIME);
        return;
    case SR_USB:
        push(SCR_SETTINGS, SG_USB);
        return;
    case SR_ABOUT:
        push(SCR_SETTINGS, SG_ABOUT);
        return;
    case SR_EQ:
        eq_open();
        return;
    case SR_MONO:
        opt.mono = !opt.mono;
        output_set_mono(opt.mono);
        break;
    case SR_BRIGHTNESS: {
        /* the next step up (a saved value between steps rounds up) */
        int b = (opt.brightness / BRIGHT_STEP + 1) * BRIGHT_STEP;
        opt.brightness = b > 100 ? BRIGHT_MIN : b;
        light = -1;
        screen_light(100);           /* the new brightness, now */
        break;
    }
    case SR_THEMES: {
        skins_scan();                /* a theme just copied to the card shows up */
        int k = skin_find(skin_sel);
        if (depth < NAV_DEPTH) {     /* the one in use selected */
            stack[depth].screen = SCR_THEMES;
            stack[depth].param = 0;
            stack[depth].selected = k < 0 ? 0 : k;
            depth++;
            show();
        }
        return;
    }
    case SR_DIM: {
        int k = 0;
        while (k < 5 && dims[k] != opt.dim_s)
            k++;
        opt.dim_s = dims[(k + 1) % 6];
        break;
    }
    case SR_BATTERY_PCT:
        opt.battery_pct = !opt.battery_pct;
        header_set_percent_always(opt.battery_pct);
        break;
    case SR_IDLE_OFF: {
        int k = 0;
        while (k < 4 && idle_offs[k] != opt.idle_off_min)
            k++;
        opt.idle_off_min = idle_offs[(k + 1) % 5];
        break;
    }
    case SR_WHEEL_NP:
        opt.wheel_volume = !opt.wheel_volume;
        break;
    case SR_WHEEL_LOCK:
        opt.lock_wheel = !opt.lock_wheel;
        break;
    case SR_SET_CLOCK:
        clk.editing = false;
        clock_load();
        push(SCR_CLOCK, 0);
        return;
    case SR_CLOCK24:
        opt.clock24 = !opt.clock24;
        break;
    case SR_SLEEP: {                 /* not saved: a timer for now */
        int left = sleep_at ? (int)((int32_t)(sleep_at - lv_tick_get()) / 60000) + 1 : 0, k = 0;
        while (k < 5 && sleeps[k] < left)    /* the next step up from what's left */
            k++;
        int next = sleep_at ? sleeps[(k + 1) % 6] : sleeps[1];
        sleep_at = next ? (lv_tick_get() + (uint32_t)next * 60000) | 1 : 0;
        show();
        return;
    }
    case SR_USB_MODE: {
        static const int next[] = { USB_SHARE, USB_DAC, USB_ASK, USB_CHARGE };   /* ask, transfer, DAC, charge */
        opt.usb = next[opt.usb];
        break;
    }
    case SR_USB_CONNECTION:
        /* the choice from plugging in, again (after Back, or after
         * charging only, with the cable still in) */
        if (los_usb_state() == LOS_USB_COMPUTER && !usb_ask_box) {
            usb_done = true;         /* the box answers; nothing else starts */
            usb_ask_open();
        }
        return;
    case SR_PLAYER:
        push(SCR_ABOUT, 0);
        return;
    case SR_RESCAN: {
        /* every file read again; what was playing stops (the list changes) */
        scan_begin("Scanning the library...", true);
        playback_stop();
        player.state = 0;
        player.album = -1;
        queue_len = 0;
        library_rescan(los_music_path());
        collect_artists();
        playlists_scan();
        fav_reload();
        scan_end();
        show();
        if (library_skipped()) {          /* what it couldn't add, at once */
            skipped_seen = skipped_signature();
            push(SCR_SKIPPED, 0);
        }
        return;
    }
    case SR_RESET:
        ibox_open(IBOX_ASK_RESET, "Reset settings?",
                  "Settings go back to how they came and Bluetooth forgets every pairing. "
                  "Your music, playlists and favourites stay.",
                  "Play resets  \xc2\xb7  Back: cancel");
        return;
    case SR_UNINSTALL:
        ibox_open(IBOX_DONE, "Uninstall Lossless OS", los_player_uninstall(), "Any key closes");
        return;
    }
    settings_changed();
    show();
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
    case SCR_SETTINGS: {
        int r = set_row_at(n->param, i);
        if (r >= 0)
            settings_choose(r);
        break;
    }
    case SCR_ADD_TO: {
        int pl = i == 0 ? playlist_new() : i - 1 < add_n ? add_map[i - 1] : -1;
        bool ok = pl >= 0 && playlist_add(pl, add_song.album, add_song.track);
        char msg[96];
        snprintf(msg, sizeof(msg), ok ? "Added to %s" : "Couldn't add it", ok ? playlist_get(pl)->name : "");
        fav_reload();
        back();
        toast(msg);
        break;
    }
    case SCR_THEMES:
        skin_use(skin_name(i));
        root_fix();
        settings_changed();
        show();
        break;
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
    case SCR_HOME:
    case SCR_MUSIC:
        menu_choose(i < nmenu ? menu_rows[i] : -1);
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
        if (i == 0) {                         /* Shuffle first, then the songs */
            set_shuffle(true);
            settings_changed();
            i = (int)(lv_tick_get() % (uint32_t)nsongs);
        } else {
            i--;
        }
        player_play_list(song_index, nsongs, i);
        push(SCR_NOW_PLAYING, 0);
        break;
    case SCR_ALBUM:
        queue_from = FROM_ALBUM;
        snprintf(queue_key, sizeof(queue_key), "%s", library_album(n->param)->dir);
        player_play(n->param, i);
        morph_from_obj(n->param, ga_hero[0]);
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
                *lk_cover, *lk_bar, *lk_left, *lk_right, *lk_none;
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
    if (theme.gallery) {
        lv_label_set_text(lk_time, buf);      /* hidden: the title bar shows the time */
        lv_label_set_text(lk_ampm, ampm);
    } else {
        dots_set_text(lk_time, buf);
        dots_set_text(lk_ampm, ampm);
        lv_obj_update_layout(lk_time);
        int tw = lv_obj_get_width(lk_time), aw = ampm[0] ? 26 : 0;
        lv_obj_set_pos(lk_time, (LOS_W - tw - aw) / 2, LOS_HEADER_H + 16);
        lv_obj_set_pos(lk_ampm, (LOS_W - tw - aw) / 2 + tw + 8, LOS_HEADER_H + 16 + 32 - 16);
    }
    header_set_playing(lk_header, player.state);
    bool vol_flash = false;    /* the header shows the volume */
    if (clock_ok && theme.gallery)        /* short, in the title bar */
        snprintf(buf, sizeof(buf), "%.3s %d %.3s", day_names[d.weekday], d.day, month_names[d.month - 1]);
    else if (clock_ok)
        snprintf(buf, sizeof(buf), "%s  %d %s", day_names[d.weekday], d.day, month_names[d.month - 1]);
    else
        snprintf(buf, sizeof(buf), "Set the time in Settings");
    char caps[64];
    size_t i;
    for (i = 0; buf[i] && i < sizeof(caps) - 1; i++)
        caps[i] = (char)(buf[i] >= 'a' && buf[i] <= 'z' && !vol_flash && !theme.gallery ? buf[i] - 32 : buf[i]);
    caps[i] = '\0';
    lv_label_set_text(lk_date, caps);
    lv_obj_set_style_text_color(lk_date, vol_flash ? theme.fg : theme.muted, 0);


    /* what's playing */
    const los_track *t = current_track();
    bool any = t != NULL;
    lv_obj_t *song[] = { lk_cover, lk_title, lk_artist, theme.gallery ? NULL : lk_album, lk_bar, lk_left,
                         lk_right, lk_icon };
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
        if (theme.gallery) {          /* bigger, and the screen takes its colour */
            lk_cover = cover_create(lock_scr, a->cover, a->cover_offset, 128, 8);
            lv_obj_set_pos(lk_cover, (LOS_W - 128) / 2, LOS_HEADER_H);
            const lv_draw_buf_t *src = a->cover[0] ? cover_thumb(a->cover, a->cover_offset, COVERFLOW_COVER) : NULL;
            lv_obj_set_style_bg_color(lock_scr, src ? cover_tint(src) : theme.bg, 0);
        } else {
            lk_cover = cover_create(lock_scr, a->cover, a->cover_offset, 56, 5);
            lv_obj_set_pos(lk_cover, 20, 124);
        }
        lk_album_shown = player.album;
    }
    if (strcmp(lv_label_get_text(lk_title), t->title))
        lv_label_set_text(lk_title, t->title);
    lv_label_set_text(lk_artist, a->artist);
    lv_label_set_text(lk_album, a->title);
    /* where the song is, or where it's being moved to (as on Now Playing) */
    int len = track_length(t);
    int at = scrub.active ? scrub.sec : player.elapsed;
    segbar_set(lk_bar, at < len ? at : len, len);
    state_icon_set(lk_icon, player.state);
    fmt_time(buf, sizeof(buf), at);
    if (theme.gallery)
        lv_label_set_text(lk_left, buf);
    else
        dots_set_text(lk_left, buf);
    buf[0] = '-';
    fmt_time(buf + 1, sizeof(buf) - 1, len > at ? len - at : 0);
    if (theme.gallery)
        lv_label_set_text(lk_right, buf);
    else
        dots_set_text(lk_right, buf);
    lv_obj_update_layout(lk_right);       /* its new width, before placing it */
    lv_obj_set_x(lk_right, LOS_W - 20 - lv_obj_get_width(lk_right));
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

/* Classic's hold: no lock screen. A big padlock in a dark square shows for
 * a moment when it locks, and when a button is pressed while locked; the
 * title bar keeps a small one. */
static lv_obj_t *hold_box;
static lv_timer_t *hold_timer;

static void hold_padlock_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_draw_rect_dsc_t s;
    lv_draw_rect_dsc_init(&s);
    s.bg_opa = LV_OPA_TRANSP;
    s.border_color = lv_color_white();
    s.border_width = 4;
    s.radius = 11;
    lv_area_t shackle = { cx - 11, cy - 22, cx + 11, cy + 4 };
    lv_draw_rect(layer, &s, &shackle);
    s.bg_opa = LV_OPA_COVER;
    s.bg_color = lv_color_white();
    s.border_width = 0;
    s.radius = 3;
    lv_area_t body = { cx - 16, cy - 6, cx + 16, cy + 20 };
    lv_draw_rect(layer, &s, &body);
}

static void hold_hide(lv_timer_t *t)
{
    (void)t;
    if (hold_box)
        lv_obj_delete(hold_box);
    hold_box = NULL;
    hold_timer = NULL;
}

static void hold_flash(void)
{
    if (!hold_box) {
        hold_box = theme_box(lv_layer_top());
        lv_obj_set_size(hold_box, 80, 80);
        lv_obj_align(hold_box, LV_ALIGN_CENTER, 0, LOS_HEADER_H / 2);
        lv_obj_set_style_radius(hold_box, 12, 0);
        lv_obj_set_style_bg_color(hold_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(hold_box, 170, 0);
        lv_obj_add_event_cb(hold_box, hold_padlock_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    }
    if (hold_timer)
        lv_timer_reset(hold_timer);
    else {
        hold_timer = lv_timer_create(hold_hide, 1200, NULL);
        lv_timer_set_repeat_count(hold_timer, 1);
    }
}

/* Gallery's lock screen: on the playing cover's colour, a big clock and
 * the date, then the cover, the song and a thin progress line */
static void lock_open_gallery(void)
{
    /* the time is the title bar's, at the left; these stay hidden */
    lk_time = lock_label(theme.small, theme.fg, 0, 0, 10);
    lk_ampm = lock_label(theme.small, theme.fg, 0, 0, 10);
    lv_obj_add_flag(lk_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lk_ampm, LV_OBJ_FLAG_HIDDEN);
    /* the date in the middle of the title bar */
    lk_date = lock_label(theme.mono, theme.muted, 90, (LOS_HEADER_H - LOS_VOLUME_H - lv_font_get_line_height(theme.mono)) / 2,
                         LOS_W - 180);
    lv_obj_set_style_text_align(lk_date, LV_TEXT_ALIGN_CENTER, 0);
    lk_cover = NULL;
    lk_album_shown = -2;
    lk_title = lock_label(theme.title_small, theme.fg, 20, 170, LOS_W - 40);
    lv_obj_set_style_text_align(lk_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lk_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lk_artist = lock_label(theme.small, theme.second, 20, 191, LOS_W - 40);
    lv_obj_set_style_text_align(lk_artist, LV_TEXT_ALIGN_CENTER, 0);
    lk_album = lock_label(theme.mono, theme.muted, 0, 0, 10);
    lv_obj_add_flag(lk_album, LV_OBJ_FLAG_HIDDEN);
    /* a thin line under the cover, the times either side of it */
    lk_bar = segbar_create(lock_scr, 140, 3, 70, 0, theme.fg, lv_color_mix(theme.fg, theme.bg, 60));
    lv_obj_set_pos(lk_bar, (LOS_W - 140) / 2, 222);
    lk_icon = state_icon_create(lock_scr, 7, 9);
    lv_obj_set_pos(lk_icon, 20, 219);
    lk_left = lock_label(theme.mono, theme.muted, 0, 216, 52);
    lv_obj_set_style_text_align(lk_left, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_x(lk_left, (LOS_W - 140) / 2 - 60);
    lk_right = lock_label(theme.mono, theme.muted, (LOS_W + 140) / 2 + 8, 216, 52);
    lk_none = theme_tag(lock_scr, "Nothing playing", theme.faint);
    lv_obj_align(lk_none, LV_ALIGN_TOP_MID, 0, 110);
    last_key_at = lv_tick_get();
    lock_update();
}

/* Where the playing song's cover is on screen now, for the glide */
static void morph_from_screen(void)
{
    if (!theme.gallery || player.album < 0)
        return;
    enum screen scr = stack[depth - 1].screen;
    if (scr == SCR_NOW_PLAYING)
        morph_from_obj(player.album, np_cover);
    else if (scr == SCR_ALBUM && stack[depth - 1].param == player.album)
        morph_from_obj(player.album, ga_hero[0]);
    else if (scr == SCR_COVERS && cf.img && cf.kind == SHELF_ALBUMS && cf.target == player.album)
        morph_from(player.album, (LOS_W - COVERFLOW_COVER) / 2, LOS_HEADER_H + 12 + 6, COVERFLOW_COVER);
}

static void lock_open(void)
{
    if (theme.classic) {
        hold_flash();
        return;
    }
    if (lock_scr)
        return;
    morph_from_screen();
    lock_scr = theme_box(lv_layer_top());
    lv_obj_set_size(lock_scr, LOS_W, LOS_H);
    lv_obj_set_style_bg_color(lock_scr, theme.bg, 0);
    lv_obj_set_style_bg_opa(lock_scr, LV_OPA_COVER, 0);
    lk_header = header_create(lock_scr, "");
    if (theme.gallery) {              /* its title bar keeps the clock at the left */
        lock_open_gallery();
        morph_run();                  /* the cover glides in from the screen under it */
        return;
    }
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
    /* the progress as on Now Playing: segments, then the times in dots */
    lk_bar = segbar_create(lock_scr, LOS_W - 40, 5, 38, 3, theme.accent, theme.seg_off);
    lv_obj_set_pos(lk_bar, 20, 190);
    lk_icon = state_icon_create(lock_scr, 7, 9);
    lv_obj_set_pos(lk_icon, 20, 203 + (20 - 9) / 2);
    lk_left = dots_create(lock_scr, 2, theme.ink);
    lv_obj_set_pos(lk_left, 33, 203);
    lk_right = dots_create(lock_scr, 2, theme.dim);
    lv_obj_set_y(lk_right, 203);
    lk_none = theme_tag(lock_scr, "Nothing playing", theme.faint);
    lv_obj_align(lk_none, LV_ALIGN_TOP_MID, 0, 148);
    last_key_at = lv_tick_get();
    lock_update();
}

static void lock_close(void)
{
    if (theme.classic && !lock_scr) {
        if (hold_timer)
            lv_timer_delete(hold_timer);
        hold_hide(NULL);
        return;
    }
    if (lock_scr && player.album >= 0)
        morph_from_obj(player.album, lk_cover);      /* show() glides it back */
    if (morph.dest == lk_cover)
        morph_abort();                               /* a glide into it, not finished */
    if (lock_scr)
        lv_obj_delete(lock_scr);
    lock_scr = NULL;
    lk_cover = NULL;
    lk_header = NULL;
    show();                  /* the screen under it, with its own header live again */
}

/* ---- Favourites, and a word on screen ---------------------------------------------------- */

/* Favourites' songs, kept in memory (the heart on Now Playing asks often) */
static struct {
    int album[QUEUE_MAX], track[QUEUE_MAX], n;
    bool loaded;
} fav;

static void fav_reload(void)
{
    int i = playlist_favourites(false);
    fav.n = i >= 0 ? playlist_load(i, fav.album, fav.track, QUEUE_MAX) : 0;
    fav.loaded = true;
}

static bool fav_has(int album, int track)
{
    if (!fav.loaded)
        fav_reload();
    for (int k = 0; k < fav.n; k++)
        if (fav.album[k] == album && fav.track[k] == track)
            return true;
    return false;
}

/* A short line at the bottom for a moment ("Added to Favourites") */
static lv_obj_t *toast_box;
static lv_timer_t *toast_timer;

static void toast_gone(lv_timer_t *t)
{
    (void)t;
    if (toast_box)
        lv_obj_delete(toast_box);
    toast_box = NULL;
    toast_timer = NULL;
}

static void toast(const char *text)
{
    if (toast_box)
        lv_obj_delete(toast_box);
    toast_box = theme_box(lv_layer_top());
    lv_obj_set_size(toast_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(toast_box, 14, 0);
    lv_obj_set_style_pad_ver(toast_box, 7, 0);
    lv_obj_set_style_radius(toast_box, 8, 0);
    lv_obj_set_style_bg_color(toast_box, theme.raised, 0);
    lv_obj_set_style_bg_opa(toast_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast_box, theme.line, 0);
    lv_obj_set_style_border_width(toast_box, 1, 0);
    lv_obj_t *l = lv_label_create(toast_box);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, theme.small, 0);
    lv_obj_set_style_text_color(l, theme.fg, 0);
    lv_obj_align(toast_box, LV_ALIGN_BOTTOM_MID, 0, -18);
    if (toast_timer)
        lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(toast_gone, 1500, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

/* In Favourites or out; says which */
static void fav_toggle(int album, int track)
{
    bool in = fav_has(album, track);
    int i = playlist_favourites(!in);
    bool ok = i >= 0 && (in ? playlist_remove(i, album, track) : playlist_add(i, album, track));
    fav_reload();
    toast(!ok ? "Couldn't change Favourites" : in ? "Removed from Favourites" : "Added to Favourites");
}

/* Plays straight after the current song (or now, if nothing is playing) */
static void play_next(song_ref s)
{
    if (player.album < 0 || !queue_len) {
        player_play_list(&s, 1, 0);
        return;
    }
    if (queue_len >= QUEUE_MAX)
        return;
    song_ref cur = queue[queue_pos];
    memmove(&queue[queue_pos + 2], &queue[queue_pos + 1], (size_t)(queue_len - queue_pos - 1) * sizeof(song_ref));
    queue[queue_pos + 1] = s;
    int j = 0;                        /* the list's own order: after the current song there too */
    while (j < queue_len && (queue_orig[j].album != cur.album || queue_orig[j].track != cur.track))
        j++;
    j = j < queue_len ? j + 1 : queue_len;
    memmove(&queue_orig[j + 1], &queue_orig[j], (size_t)(queue_len - j) * sizeof(song_ref));
    queue_orig[j] = s;
    queue_len++;
    tell_next();
}

/* ---- option panels (Menu) ------------------------------------------------------------- */

/* A panel over the bottom of the screen. For a song (Now Playing, an
 * album page, or held Play on a song in a list): Favourite, Add to
 * playlist, Play next, Go to Album, Repeat and Shuffle, as fits the
 * screen; in a playlist, Remove from playlist; on a playlist, Delete
 * playlist (asks again). On a pair of headphones in the Bluetooth list:
 * Connect (or Disconnect) and Forget, which asks again first. The wheel
 * picks a row, Play acts (Prev / Next change a value too), Back or Menu
 * closes. */
#define OPTS_MAX   6
enum { OR_FAV, OR_ADD, OR_NEXT, OR_ALBUM, OR_REPEAT, OR_SHUFFLE, OR_REMOVE, OR_DELETE };

static int opts_kind, opts_device, opts_sel, opts_n, opts_row_h;
static int opts_ids[OPTS_MAX];
static song_ref opts_song;           /* the song the panel is for */
static int opts_pl = -1;             /* the playlist: to take the song out of, or to delete */
static bool opts_confirm;            /* Forget (or Delete) pressed once */
static bool opts_connected;          /* the device is connected (or connecting) */
static lv_obj_t *opts_row[OPTS_MAX], *opts_label[OPTS_MAX], *opts_value[OPTS_MAX];

static const char *opts_name(int id)
{
    static const char *const names[] = { "Favourite", "Add to playlist", "Play next", "Go to Album",
                                         "Repeat", "Shuffle", "Remove from playlist", "Delete playlist" };
    return names[id];
}

static void opts_update(void)
{
    if (opts_kind == OPTS_PLAY) {
        static const char *const repeat_names[] = { "Off", "All", "One" };
        for (int i = 0; i < opts_n; i++) {
            int id = opts_ids[i];
            const char *v = id == OR_FAV ? (fav_has(opts_song.album, opts_song.track) ? "On" : "Off") :
                            id == OR_REPEAT ? repeat_names[repeat_mode] :
                            id == OR_SHUFFLE ? (shuffle ? "On" : "Off") : "";
            lv_label_set_text(opts_label[i], id == OR_DELETE && opts_confirm ? "Delete? Play again" :
                                             id == OR_REMOVE && opts_confirm ? "Remove? Play again" :
                                             opts_name(id));
            lv_label_set_text(opts_value[i], v);
        }
    } else {
        lv_label_set_text(opts_label[0], opts_connected ? "Disconnect" : "Connect");
        lv_label_set_text(opts_label[1], opts_confirm ? "Forget? Play again" : "Forget");
    }
    for (int i = 0; i < opts_n; i++)
        set_state_deep(opts_row[i], LV_STATE_CHECKED, i == opts_sel);
}

static void opts_build(const char *title)
{
    opts_row_h = opts_n > 3 ? 32 : 40;       /* five rows still leave the screen's top showing */
    int h = 30 + opts_n * opts_row_h;
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
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_update_layout(t);
    lv_obj_set_size(t, LOS_W - 40, lv_obj_get_height(t));   /* one line, whatever the title */
    for (int i = 0; i < opts_n; i++) {
        lv_obj_t *row = theme_box(opts);
        lv_obj_set_pos(row, 0, 30 + i * opts_row_h);
        lv_obj_set_size(row, LOS_W, opts_row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(row, theme.select, LV_STATE_CHECKED);
        if (theme.select_grad) {
            lv_obj_set_style_bg_grad_color(row, theme.select_bottom, LV_STATE_CHECKED);
            lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, LV_STATE_CHECKED);
        }
        int b = centred_baseline(theme.row, opts_row_h);
        opts_label[i] = row_label(row, "", theme.row, theme.fg, LOS_EDGE, top_for_baseline(theme.row, b), 230);
        opts_value[i] = row_label(row, "", theme.row, theme.second, 200, top_for_baseline(theme.row, b), 100);
        lv_obj_set_style_text_align(opts_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        opts_row[i] = row;
    }
    opts_sel = 0;
    opts_update();
}

/* The Bluetooth list's panel for a pair of headphones */
static void opts_open(int kind, int device)
{
    bt_status b;
    opts_kind = kind;
    opts_device = device;
    opts_confirm = false;
    bluetooth_status(&b);
    if (device < 0 || device >= b.nknown)
        return;
    opts_connected = b.peer_known == device && b.state >= BT_CONNECTING;
    opts_n = 2;
    opts_build(b.known[device].name);
}

static void sheet_open(const char *title, const int *ids, int n)
{
    opts_kind = OPTS_PLAY;
    opts_confirm = false;
    opts_n = n < OPTS_MAX ? n : OPTS_MAX;
    memcpy(opts_ids, ids, (size_t)opts_n * sizeof(int));
    opts_build(title);
}

/* The panel for what's here: the playing song on Now Playing, the
 * selected song on an album page or in a list, a playlist; false if
 * there's nothing to offer */
static bool sheet_here(void)
{
    enum screen scr = stack[depth - 1].screen;
    int sel = stack[depth - 1].selected, ids[OPTS_MAX], n = 0;
    opts_pl = -1;
    if (scr == SCR_NOW_PLAYING) {
        if (player.album < 0)
            return false;
        opts_song = (song_ref){ player.album, player.track };
        ids[n++] = OR_FAV; ids[n++] = OR_ADD; ids[n++] = OR_ALBUM; ids[n++] = OR_REPEAT; ids[n++] = OR_SHUFFLE;
    } else if (scr == SCR_ALBUM) {
        const los_album *a = library_album(stack[depth - 1].param);
        if (!a || sel < 0 || sel >= a->ntracks)
            return false;
        opts_song = (song_ref){ stack[depth - 1].param, sel };
        ids[n++] = OR_NEXT; ids[n++] = OR_ADD; ids[n++] = OR_FAV; ids[n++] = OR_REPEAT; ids[n++] = OR_SHUFFLE;
    } else if (scr == SCR_SONGS || scr == SCR_ARTIST_SONGS || scr == SCR_PLAYLIST) {
        sel--;                        /* Shuffle is the first row */
        if (sel < 0 || sel >= nsongs)
            return false;
        opts_song = song_index[sel];
        ids[n++] = OR_NEXT; ids[n++] = OR_ADD; ids[n++] = OR_FAV;
        if (scr == SCR_PLAYLIST && playlist_editable(stack[depth - 1].param)) {
            opts_pl = stack[depth - 1].param;
            ids[n++] = OR_REMOVE;
        }
    } else if (scr == SCR_PLAYLISTS || (scr == SCR_COVERS && cf.kind == SHELF_PLAYLISTS)) {
        int pl = scr == SCR_COVERS ? cf.target : sel;
        if (!playlist_editable(pl))
            return false;
        opts_pl = pl;
        ids[n++] = OR_DELETE;
        sheet_open(playlist_get(pl)->name, ids, n);
        return true;
    } else {
        return false;
    }
    sheet_open(library_album(opts_song.album)->tracks[opts_song.track].title, ids, n);
    return true;
}

static void opts_close(void)
{
    if (opts)
        lv_obj_delete(opts);
    opts = NULL;
}

/* Now Playing > Menu > Go to Album: the playing song's album, the song
 * selected; back to it if that's where Now Playing came from */
static void go_to_album(void)
{
    opts_close();
    if (player.album < 0)
        return;
    if (depth >= 2 && stack[depth - 2].screen == SCR_ALBUM && stack[depth - 2].param == player.album) {
        depth--;
    } else if (depth < NAV_DEPTH) {
        stack[depth].screen = SCR_ALBUM;
        stack[depth].param = player.album;
        depth++;
    }
    stack[depth - 1].selected = player.track;
    show();
}

static void opts_act(int id, uint32_t key)
{
    switch (id) {
    case OR_REPEAT:
        repeat_mode = (repeat_mode + (key == LV_KEY_LEFT ? 2 : 1)) % 3;
        tell_next();
        settings_changed();
        break;
    case OR_SHUFFLE:
        set_shuffle(!shuffle);
        settings_changed();
        break;
    case OR_FAV:
        if (key != LV_KEY_ENTER)
            return;
        fav_toggle(opts_song.album, opts_song.track);
        if (stack[depth - 1].screen == SCR_PLAYLIST) {
            opts_close();             /* the playlist may be Favourites: built again, without */
            show();                   /* the panel (it goes with the old screen: don't touch it) */
            return;
        }
        break;
    case OR_ADD:
        if (key != LV_KEY_ENTER)
            return;
        add_song = opts_song;
        opts_close();
        push(SCR_ADD_TO, 0);
        return;
    case OR_NEXT:
        if (key != LV_KEY_ENTER)
            return;
        play_next(opts_song);
        opts_close();
        toast("Plays next");
        refresh();
        return;
    case OR_ALBUM:
        if (key == LV_KEY_ENTER)
            go_to_album();
        return;
    case OR_REMOVE:
    case OR_DELETE:
        if (key != LV_KEY_ENTER)
            return;
        if (!opts_confirm) {
            opts_confirm = true;
            break;
        }
        {
            bool ok = id == OR_REMOVE ? playlist_remove(opts_pl, opts_song.album, opts_song.track)
                                      : playlist_delete(opts_pl);
            fav_reload();
            opts_close();
            toast(!ok ? "Couldn't change the playlist" : id == OR_REMOVE ? "Removed" : "Playlist deleted");
            show();
        }
        return;
    }
    opts_update();
}

static void opts_key(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        opts_sel = key == LV_KEY_UP ? (opts_sel > 0 ? opts_sel - 1 : 0)
                                    : (opts_sel < opts_n - 1 ? opts_sel + 1 : opts_n - 1);
        opts_confirm = false;
        break;
    case LV_KEY_ENTER:
    case LV_KEY_RIGHT:
    case LV_KEY_LEFT:
        if (opts_kind == OPTS_PLAY) {
            opts_act(opts_ids[opts_sel], key);
            return;
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

/* Play held on a song (or a playlist) opens its panel; on Now Playing it
 * puts the song in Favourites, or takes it out. A tap does what Play
 * always does, when it's let go. */
#define ENTER_HOLD_MS 450
static struct {
    bool on, wait_release;
    uint32_t since;
} ehold;
static bool enter_replay;            /* a tap, being passed on */

static bool enter_holds_here(void)
{
    enum screen scr = stack[depth - 1].screen;
    if (scr == SCR_NOW_PLAYING)
        return player.album >= 0;
    if (scr == SCR_COVERS)
        return cf.img && cf.kind == SHELF_PLAYLISTS && !cf.back;
    return (scr == SCR_ALBUM || scr == SCR_SONGS || scr == SCR_ARTIST_SONGS || scr == SCR_PLAYLIST ||
            scr == SCR_PLAYLISTS) && list.count > 0;
}

static void handle_key(uint32_t key);

static void enter_watch(void)
{
    bool down = app_keypad && lv_indev_get_state(app_keypad) == LV_INDEV_STATE_PRESSED &&
                lv_indev_get_key(app_keypad) == LV_KEY_ENTER;
    if (ehold.wait_release) {
        if (!down)
            ehold.wait_release = false;
        return;
    }
    if (!ehold.on)
        return;
    if (down && lv_tick_elaps(ehold.since) >= ENTER_HOLD_MS) {
        ehold.on = false;
        ehold.wait_release = true;
        if (stack[depth - 1].screen == SCR_NOW_PLAYING) {
            fav_toggle(player.album, player.track);
            refresh();
        } else {
            sheet_here();
        }
    } else if (!down) {
        ehold.on = false;
        enter_replay = true;
        handle_key(LV_KEY_ENTER);
        enter_replay = false;
    }
}

static void handle_key(uint32_t key)
{
    bool np = stack[depth - 1].screen == SCR_NOW_PLAYING;
    last_key_at = lv_tick_get();
    install_started_ok();        /* it got going: the failed-start count goes back to 0 */
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
    if (ibox) {
        ibox_key(key);
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
        if (theme.classic && !((key == LV_KEY_UP || key == LV_KEY_DOWN) && opt.lock_wheel))
            hold_flash();                 /* Classic: "it's on hold" */
        if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && opt.lock_wheel) {
            los_app_volume(key == LV_KEY_DOWN ? +2 : -2);     /* clockwise: louder, as on Now Playing */
        } else if (key == LV_KEY_ENTER && player.album >= 0) {
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
    if (gm.panel) {
        gm_key(key);
        return;
    }
    if (key == LV_KEY_ENTER && !enter_replay && enter_holds_here()) {
        if (!ehold.on && !ehold.wait_release) {      /* decided when it's let go, or held (enter_watch) */
            ehold.on = true;
            ehold.since = lv_tick_get();
        }
        return;
    }
    if (clock_key(key))
        return;
    if (eq_key(key))
        return;
    if (covers_key(key))
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
    if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && letter_jump(key))
        return;
    switch (key) {
    case LV_KEY_UP:    list_select(list.selected - 1); break;
    case LV_KEY_DOWN:  list_select(list.selected + 1); break;
    case LV_KEY_ESC:
        if (theme.gallery && depth == 1)
            gm_open();                    /* Gallery's top views: Back opens the menu too */
        else
            back();
        break;
    case LV_KEY_HOME:
        if (theme.gallery && depth == 1 && player.album < 0)
            gm_open();                    /* Gallery, nothing playing: the menu (Back opens it too) */
        else if (scr == SCR_NOW_PLAYING || scr == SCR_ALBUM)
            sheet_here();
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

static void startlog_timer(lv_timer_t *t)
{
    (void)t;
    startlog_write();
}

void los_app_start(lv_display_t *display, lv_indev_t *keypad)
{
    (void)display;
    theme_init(true);
    if (install_at_start()) {
        /* three starts in a row didn't get going: the version before is back */
        lv_obj_t *t = theme_tag(lv_layer_top(), "The version before is back. Switching off.", theme.fg);
        lv_obj_center(t);
        lv_refr_now(NULL);
        los_sleep_ms(6000);
        los_power_off();
    }
    test_result_save();
    startlog_mark("app: fonts loaded, install check");
    scan_begin("Reading your music...", false);
    int albums = library_scan(los_music_path());
    scan_end();
    startlog_mark("app: library read (%d albums)", albums);
    skipped_seen = skipped_signature();   /* known already: the Music list has it */
    collect_artists();
    playlists_scan();
    fav_reload();
    skins_scan();
    startlog_mark("app: artists and playlists");

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
        if (saved.idle_off_min >= 0)
            opt.idle_off_min = saved.idle_off_min;
        opt.clock24 = saved.clock24;
        opt.wheel_volume = saved.wheel_volume;
        opt.lock_wheel = saved.lock_wheel;
        opt.battery_pct = saved.battery_pct;
        header_set_percent_always(opt.battery_pct);
        opt.usb = saved.usb;
        opt.mono = saved.mono;
        output_set_mono(opt.mono);
        if (saved.skin[0])
            skin_use(saved.skin);
        eq_enabled = saved.eq;
        for (int b = 0; b < EQ_BANDS; b++)
            eq_gains[b] = saved.eq_gains[b] < -EQ_MAX_DB ? -EQ_MAX_DB :
                          saved.eq_gains[b] > EQ_MAX_DB ? EQ_MAX_DB : saved.eq_gains[b];
        eq_load_curves(&saved);
        eq_apply();
        if (saved.bluetooth)
            bluetooth_power(true);
    }
    light = -1;
    screen_light(100);                   /* the chosen brightness, or the default */
    startlog_mark("app: settings%s", saved.bluetooth ? ", Bluetooth on" : "");
    load_resume();
    write_player_json();
    lv_timer_create(tick, 250, NULL);
    lv_timer_create(split_tick, 500, NULL);
    lv_timer_create(jump_tick, 100, NULL);
    volume_watch(NULL);
    lv_timer_create(volume_watch, 40, NULL);
    lv_timer_create(warm_covers, 40, NULL);

    depth = 0;
    push(theme.gallery ? SCR_COVERS : SCR_HOME, 0);   /* Gallery: Albums as covers */
    install_ask();
    startlog_mark("app: home screen built");
    lv_timer_set_repeat_count(lv_timer_create(startlog_timer, 15000, NULL), 1);
}
