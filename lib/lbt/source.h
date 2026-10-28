/*
 * lbt source: a music player's Bluetooth, on top of the protocol layers.
 *
 * - connects to headphones, or lets them connect, and secures the link
 * - sets up SBC audio and sends the player's audio out in real time
 * - turns their buttons into player actions; one shared volume
 * - suspends the stream while nothing plays, starts it again on play
 * - reconnects when the link drops (out of range) and keeps looking for
 *   the last headphones for a while after start-up (still in the case)
 *
 * The platform runs everything on one thread: source_poll() often (every
 * 10 ms or so) and hci_receive() with the bytes from the controller.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_SOURCE_H
#define LBT_SOURCE_H

#include "lbt_util.h"
#include "avrcp.h"

#define SOURCE_NAME_MAX 32

/* How long to keep looking for the headphones after start-up or after the
 * link dropped, and how long to wait between tries */
#define SOURCE_SEEK_MS        (5 * 60 * 1000)
#define SOURCE_RETRY_MS       3000
/* Not playing for this long while streaming: suspend the stream (long
 * enough that a short pause changes nothing for the headphones) */
#define SOURCE_IDLE_MS        30000
/* After headphones connect to us: how long to let them set up audio
 * before we do it ourselves */
#define SOURCE_INCOMING_MS    1500
/* Their "play" within this long of the controls connecting may be an
 * automatic resume; later, a "play" while playing means pause */
#define SOURCE_AUTO_PLAY_MS   3000
/* They hung up while we set up audio (busy with a phone, say): try again
 * after this, a few times */
#define SOURCE_BUSY_RETRY_MS  10000
#define SOURCE_SETUP_TRIES    3

/* Same order as the player's own Bluetooth states */
enum source_state {
    SOURCE_OFF = 0,
    SOURCE_STARTING,
    SOURCE_READY,
    SOURCE_SCANNING,
    SOURCE_CONNECTING,
    SOURCE_CONNECTED,
    SOURCE_STREAMING,
    SOURCE_FAILED,         /* Bluetooth itself didn't start */
};

enum source_action {
    SOURCE_PAUSE,
    SOURCE_RESUME,
    SOURCE_NEXT,
    SOURCE_PREVIOUS,
    SOURCE_VOLUME_UP,      /* only from headphones without absolute volume */
    SOURCE_VOLUME_DOWN,
};

/* Player play status, as AVRCP has it */
#define SOURCE_STOPPED 0
#define SOURCE_PLAYING 1
#define SOURCE_PAUSED  2

typedef struct {
    void (*state)(enum source_state state, const char *message);
    /* search results; called again when a name arrives */
    void (*found)(const lbt_addr *addr, const char *name, uint32_t cod, int8_t rssi);
    /* link keys */
    bool (*get_key)(const lbt_addr *addr, uint8_t key[16]);
    void (*put_key)(const lbt_addr *addr, const uint8_t key[16], uint8_t type);
    void (*forget_key)(const lbt_addr *addr);
    /* audio works with these headphones: reconnect to them next time */
    void (*remember)(const lbt_addr *addr, const char *name);
    /* send the player's audio to Bluetooth (rate in Hz), or stop */
    void (*audio_begin)(int rate);
    void (*audio_end)(void);
    /* 'frames' stereo frames of 16-bit audio, interleaved; false if
     * nothing is playing (fill with silence then) */
    bool (*audio_read)(int16_t *pcm, int frames);
    uint8_t (*play_status)(void);            /* SOURCE_STOPPED, _PLAYING, _PAUSED */
    void (*action)(enum source_action action);
    /* the headphones' volume, 0..127; 'first' on the first report of a
     * connection (the player decides whose volume wins) */
    void (*volume)(uint8_t volume, bool first);
    /* headphone controls (AVRCP) came or went */
    void (*controls)(bool connected);
    /* pairing with a car or a phone: show code; answer with
     * source_confirm. passkey: they type it, nothing to answer. NULL: yes */
    void (*confirm)(const char *name, uint32_t code, bool passkey);
    /* the song playing, for a car's screen (NULL: none) */
    void (*track)(avrcp_track *out);
    /* its cover, a 200 x 200 JPEG, for the handle in out->cover (coverart.h) */
    bool (*cover)(const char *handle, const uint8_t **data, uint32_t *len);
    /* a car connected for covers, or went: covers are wanted (or not) */
    void (*cover_client)(bool up);
} source_ops;

typedef struct {
    enum source_state state;
    char message[64];
    lbt_addr peer;
    char peer_name[SOURCE_NAME_MAX];
    int rate, bitpool, frame_bytes, frames_per_packet;
    uint32_t packets, frames_audio, frames_silent;
    bool absolute_volume;
} source_info;

/* Once, before anything else (sets up all the protocol layers) */
void source_init(const source_ops *ops, const char *local_name);

/* The controller is up (patched, at speed): start the stack. With 'last',
 * look for those headphones and connect when they're around. */
void source_start(const lbt_addr *last, const char *last_name);

/* Say goodbye to the headphones before powering off (give it ~200 ms) */
void source_stop(void);

void source_poll(void);

bool source_scan(int seconds);
/* Found by other devices' searches (for pairing from a car's screen) */
void source_set_visible(bool on);
/* The pairing code shown matched, or not */
void source_confirm(bool yes);
/* Connect now (from a menu): one try */
void source_connect(const lbt_addr *addr, const char *name);
void source_disconnect(void);

/* The player's volume changed: tell the headphones (0..127) */
void source_set_volume(uint8_t volume);

const source_info *source_get_info(void);

#endif
