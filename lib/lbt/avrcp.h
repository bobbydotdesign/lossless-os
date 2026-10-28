/*
 * lbt AVRCP: the headphones' buttons and one shared volume (AVRCP 1.6 over
 * AVCTP 1.4).
 *
 * As target, we take their pass-through buttons (play, pause, skip,
 * volume), tell them our play state, the song (title, artist, album,
 * number, length: a car shows them) and where it is, when they register
 * for it or ask. As controller, we register for
 * their volume changes and set their volume (absolute volume, AVRCP 1.4).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_AVRCP_H
#define LBT_AVRCP_H

#include "lbt_util.h"

/* Pass-through operations (AV/C panel subunit) */
#define AVRCP_OP_VOLUME_UP    0x41
#define AVRCP_OP_VOLUME_DOWN  0x42
#define AVRCP_OP_MUTE         0x43
#define AVRCP_OP_PLAY         0x44
#define AVRCP_OP_STOP         0x45
#define AVRCP_OP_PAUSE        0x46
#define AVRCP_OP_REWIND       0x48
#define AVRCP_OP_FAST_FORWARD 0x49
#define AVRCP_OP_FORWARD      0x4b
#define AVRCP_OP_BACKWARD     0x4c

/* Play status */
#define AVRCP_STOPPED 0x00
#define AVRCP_PLAYING 0x01
#define AVRCP_PAUSED  0x02

#define AVRCP_TIMEOUT_MS 3000

/* The song playing, as the player has it */
typedef struct {
    bool loaded;              /* a song is selected (playing or not) */
    uint32_t serial;          /* changes with each new song */
    char title[96], artist[96], album[96];
    uint32_t number, total;   /* track number and count; 0 if unknown */
    uint32_t length_ms;       /* 0 if unknown */
    uint32_t position_ms;
    char cover[8];            /* its cover's image handle ("0000001"), "" if none
                                 or no car connected for covers (AVRCP 1.6 5.14) */
} avrcp_track;

typedef struct {
    void (*connected)(bool ok);
    /* a button on the headphones: pressed, then released */
    void (*button)(uint8_t op, bool pressed);
    /* their volume, 0..127: when we first hear it and after every change */
    void (*volume)(uint8_t volume);
    void (*disconnected)(void);
    /* our play status, for when they ask (AVRCP_STOPPED, _PLAYING, _PAUSED) */
    uint8_t (*play_status)(void);
    /* the song, for when they ask (NULL: none) */
    void (*track)(avrcp_track *out);
} avrcp_ops;

void avrcp_init(const avrcp_ops *ops);

/* Open the control channel to the headphones on this connection */
bool avrcp_connect(uint16_t handle);
void avrcp_disconnect(void);

/* Tell them our play state changed, or a new track started */
void avrcp_play_status_changed(uint8_t status);
void avrcp_track_changed(void);

/* Set their volume (0..127). False if they don't take absolute volume. */
bool avrcp_set_volume(uint8_t volume);
bool avrcp_has_absolute_volume(void);

/* Call every few hundred ms: answer timeouts; tell them of a new song
 * and of where it is, as they asked */
void avrcp_tick(void);

#endif
