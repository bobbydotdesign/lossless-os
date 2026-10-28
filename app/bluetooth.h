/*
 * Bluetooth headphones: lbt (lib/lbt) on its own thread, tied to the
 * player. The UI calls these from its thread; everything Bluetooth happens
 * on the Bluetooth thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_BLUETOOTH_H
#define LOS_BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

/* lbt's source states, as the UI shows them */
enum bt_state {
    BT_OFF = 0, BT_STARTING, BT_READY, BT_SCANNING, BT_CONNECTING, BT_CONNECTED,
    BT_STREAMING, BT_FAILED,
};

#define BT_DEVICES  8
#define BT_KNOWN    8
#define BT_LOG_LINES 40

typedef struct {
    char name[32];
    uint8_t addr[6];          /* least significant first, as lbt has it */
} bt_device;

typedef struct {
    bool on;                  /* turned on (it may still be starting) */
    enum bt_state state;
    char message[64];
    char peer[32];            /* the headphones connected or being reached */
    bool have_last;
    char last_name[32];       /* the headphones to reconnect to (known[0]) */
    int ndevices;             /* found by the last search */
    bt_device devices[BT_DEVICES];
    int nknown;               /* headphones we have a pairing with, most recent first */
    bt_device known[BT_KNOWN];
    int peer_known;           /* which of them is connected or connecting, or -1 */
    int rate, bitpool;
    /* pairing with a car or a phone: a code to show */
    bool confirm;             /* a code waits for yes / no */
    bool passkey;             /* or: they type it (nothing to answer) */
    uint32_t code;            /* six digits */
    char confirm_name[32];
} bt_status;

void bluetooth_init(void);
void bluetooth_power(bool on);
void bluetooth_scan(void);
/* Found by other devices' searches, so a car can add the player from
 * its own screen: while the search screen is open */
void bluetooth_visible(bool on);
void bluetooth_connect(int device);     /* index in devices; -1: the last headphones */
void bluetooth_connect_known(int i);    /* index in known (switches from any other) */
void bluetooth_forget(int i);           /* index in known: its pairing goes */
void bluetooth_disconnect(void);
/* The pairing code on both screens matched (yes) or not */
void bluetooth_confirm(bool yes);
void bluetooth_status(bt_status *out);

/* The log, oldest first; returns the number of lines */
int bluetooth_log_lines(char lines[][100], int max);
unsigned bluetooth_log_count(void);     /* lines ever logged: changes with each new one */

/* ---- the player ---- */
void bluetooth_play_status(int state);  /* 0 stopped, 1 playing, 2 paused */
/* The song selected (title NULL: none), for a car's screen; serial
 * changes with each new song */
void bluetooth_now_playing(const char *title, const char *artist, const char *album,
                           int number, int total, uint32_t length_ms, uint32_t position_ms,
                           uint32_t serial, uint32_t cover_id);
/* A car is connected for covers: hand it the song's (bluetooth_cover) */
bool bluetooth_wants_cover(void);
/* The cover with this id (the album's), 200 x 200 RGB565, stride in pixels */
void bluetooth_cover(const uint16_t *px, int stride, uint32_t cover_id);
/* A button pressed on the headphones: 0 none, else one of these */
enum { BT_ACTION_PAUSE = 1, BT_ACTION_PLAY, BT_ACTION_NEXT, BT_ACTION_PREVIOUS,
       BT_ACTION_VOLUME_UP, BT_ACTION_VOLUME_DOWN };
int bluetooth_take_action(void);
/* One volume with the headphones (dB, over the player's range) */
void bluetooth_set_volume(int db, int min_db, int max_db);
bool bluetooth_take_volume(int *db);    /* they changed it */
bool bluetooth_has_volume(void);        /* their volume is the one in use */

#endif
