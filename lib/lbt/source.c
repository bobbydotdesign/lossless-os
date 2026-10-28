/*
 * lbt source: a music player's Bluetooth (see source.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "source.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "sdp.h"
#include "avdtp.h"
#include "a2dp.h"
#include "avrcp.h"
#include "coverart.h"
#include "lbt_platform.h"

/* HCI disconnection reasons */
#define REASON_TIMEOUT        0x08   /* supervision timeout: out of range */
#define REASON_LMP_TIMEOUT    0x22
#define REASON_USER           0x13
#define REASON_POWER_OFF      0x15
#define REASON_NO_RESOURCES   0x0d
#define REASON_LOCAL_HOST     0x16   /* we hung up */
#define STATUS_PAGE_TIMEOUT   0x04

static const source_ops *ops;
static source_info info;

static struct {
    bool ready;                 /* controller set up */
    uint16_t handle;            /* link to the headphones, 0 if none */
    bool paging;                /* our connection attempt is under way */
    bool drop_when_connected;   /* disconnected while paging */
    bool incoming;              /* they connected to us */
    bool visible;               /* found by searches (source_set_visible) */
    bool secured;
    bool setup_failed;          /* audio set-up failed: decided in poll */
    bool hanging_up;            /* we're dropping the link; say why after */
    char hangup_msg[48];
    int setup_drops;            /* they hung up mid set-up, in a row */
    uint32_t avdtp_at;          /* incoming: open audio ourselves at this time */
    bool audio_on;              /* audio_begin() was called */
    bool open;                  /* media channel up */
    bool had_audio;             /* it was up at some point on this link */
    bool streaming;
    bool start_pending;
    uint32_t start_sent;
    bool we_suspended;
    uint32_t idle_since;

    /* the headphones to (re)connect to */
    bool have_target;
    lbt_addr target;
    char target_name[SOURCE_NAME_MAX];
    bool seeking;               /* keep trying until seek_until */
    int scan_pending;           /* a search asked for while paging: its seconds */
    uint32_t seek_until, retry_at;

    uint32_t controls_since;
    bool controls_up;
    bool volume_seen;

    /* audio pacing */
    int rate;
    uint32_t last_ms, samples_ready, remainder;

    uint32_t next_tick, next_sync;
} s;

static int16_t pcm[15 * 128 * 2];   /* one packet's worth at most */

static uint8_t play_status(void);

/* ---- state ---------------------------------------------------------------------------- */

static uint32_t now(void)
{
    return lbt_platform_ms();
}

static bool after(uint32_t a, uint32_t b)   /* a is at or after b */
{
    return (int32_t)(a - b) >= 0;
}

static void set_state(enum source_state st, const char *msg)
{
    info.state = st;
    if (msg) {
        strncpy(info.message, msg, sizeof(info.message) - 1);
        info.message[sizeof(info.message) - 1] = '\0';
    }
    lbt_platform_log("source: %d %s", st, info.message);
    if (ops->state)
        ops->state(st, info.message);
}

static const char *name_or(const char *name, const char *fallback)
{
    return name && name[0] ? name : fallback;
}

static void copy_name(char *dst, const char *src)
{
    strncpy(dst, src ? src : "", SOURCE_NAME_MAX - 1);
    dst[SOURCE_NAME_MAX - 1] = '\0';
}

static void audio_off(void)
{
    s.streaming = false;
    s.start_pending = false;
    s.idle_since = 0;
    if (s.audio_on) {
        /* the headphones' audio is gone: pause, don't play on to nobody */
        if (play_status() == SOURCE_PLAYING && ops->action)
            ops->action(SOURCE_PAUSE);
        s.audio_on = false;
        if (ops->audio_end)
            ops->audio_end();
    }
}

static void link_gone(void)
{
    s.handle = 0;
    s.secured = false;
    s.incoming = false;
    s.avdtp_at = 0;
    s.open = false;
    s.had_audio = false;
    audio_off();
    if (s.controls_up && ops->controls)
        ops->controls(false);
    s.controls_up = false;
    s.volume_seen = false;
}

/* ---- connecting ------------------------------------------------------------------------- */

static void page(void)
{
    char msg[64];
    if (info.state == SOURCE_SCANNING)
        gap_inquiry_cancel();
    snprintf(msg, sizeof(msg), s.seeking ? "Looking for %s..." : "Connecting to %s...",
             name_or(s.target_name, "headphones"));
    set_state(SOURCE_CONNECTING, msg);
    s.paging = true;
    copy_name(info.peer_name, s.target_name);
    if (!gap_connect(&s.target)) {
        s.paging = false;
        s.retry_at = now() + SOURCE_RETRY_MS;
    }
}

/* Drop the link for a reason the user should see. Bluetooth stays on. */
static void hang_up(const char *why)
{
    snprintf(s.hangup_msg, sizeof(s.hangup_msg), "%s", why);
    s.hanging_up = true;
    set_state(SOURCE_READY, why);
    gap_disconnect(s.handle, REASON_USER);
}

static void seek(uint32_t for_ms)
{
    s.seeking = true;
    s.seek_until = now() + for_ms;
    s.retry_at = now();
}

static void stop_seeking(void)
{
    s.seeking = false;
}

/* ---- GAP -------------------------------------------------------------------------------- */

static void on_ready(bool ok)
{
    if (!ok) {
        set_state(SOURCE_FAILED, "Bluetooth didn't start (see log)");
        return;
    }
    s.ready = true;
    set_state(SOURCE_READY, "Ready");
    if (s.visible)
        gap_set_discoverable(true);
    if (s.seeking)
        page();
}

static void on_found(const lbt_addr *a, const char *name, uint32_t cod, int8_t rssi)
{
    if (ops->found)
        ops->found(a, name, cod, rssi);
}

static void on_named(const lbt_addr *a, const char *name)
{
    if (ops->found)
        ops->found(a, name, 0, 0);
}

static void on_inquiry_done(void)
{
    if (info.state == SOURCE_SCANNING)
        set_state(SOURCE_READY, "Search finished");
}

static void on_connected(uint16_t handle, const lbt_addr *a, uint8_t status)
{
    bool ours = s.paging && lbt_addr_equal(a, &s.target);
    if (ours)
        s.paging = false;

    if (status) {
        if (!ours || s.handle)
            return;   /* not ours, or they reached us first */
        if (s.scan_pending)
            return;   /* we stopped it to search: that starts in poll */
        if (s.drop_when_connected) {
            s.drop_when_connected = false;   /* cancelled anyway */
            return;
        }
        if (s.seeking && !after(now(), s.seek_until)) {
            /* not there yet (in the case, out of range): try again soon */
            s.retry_at = now() + SOURCE_RETRY_MS;
            return;
        }
        if (s.seeking) {   /* looked long enough */
            stop_seeking();
            set_state(SOURCE_READY, "Headphones not found");
            return;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), status == STATUS_PAGE_TIMEOUT ?
                 "Headphones not found" : "Couldn't connect (0x%02x)", status);
        set_state(SOURCE_READY, msg);
        return;
    }

    if (s.handle || (ours && s.drop_when_connected)) {
        /* one pair of headphones at a time; or the user changed their mind */
        s.drop_when_connected = false;
        gap_disconnect(handle, s.handle ? REASON_NO_RESOURCES : REASON_USER);
        return;
    }
    stop_seeking();
    s.handle = handle;
    s.incoming = !ours;
    info.peer = *a;
    if (s.incoming) {
        /* they came to us: if it's who we wanted, keep the name */
        bool known = s.have_target && lbt_addr_equal(a, &s.target);
        copy_name(info.peer_name, known ? s.target_name : "");
        lbt_platform_log("source: headphones connected to us");
    }
    set_state(SOURCE_CONNECTING, "Securing the link...");
    gap_secure(handle);
}

static void on_secured(uint16_t handle, uint8_t status)
{
    if (handle != s.handle)
        return;
    if (status) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Pairing failed (0x%02x)", status);
        hang_up(msg);
        return;
    }
    s.secured = true;
    set_state(SOURCE_CONNECTED, "Connected, opening audio...");
    if (s.incoming)
        s.avdtp_at = now() + SOURCE_INCOMING_MS;   /* their turn first */
    else
        avdtp_connect(handle);
}

static void on_disconnected(uint16_t handle, uint8_t reason)
{
    if (handle != s.handle)
        return;
    bool had_audio = s.had_audio, we_hung_up = s.hanging_up;
    lbt_platform_log("source: disconnected (reason 0x%02x)", reason);
    s.hanging_up = false;
    s.setup_failed = false;
    link_gone();
    char msg[64];
    if (we_hung_up) {
        set_state(SOURCE_READY, s.hangup_msg);
        return;
    }
    if (s.have_target && (reason == REASON_TIMEOUT || reason == REASON_LMP_TIMEOUT)) {
        /* walked out of range: keep trying for a while */
        seek(SOURCE_SEEK_MS);
        s.retry_at = now() + SOURCE_RETRY_MS;
        snprintf(msg, sizeof(msg), "Lost %s, looking...", name_or(info.peer_name, "headphones"));
        set_state(SOURCE_CONNECTING, msg);
        return;
    }
    if (s.have_target && !had_audio && reason != REASON_LOCAL_HOST) {
        /* They hung up before audio was set up. The AirPods do this with
         * "low resources" when a phone or Mac has them: try again soon. */
        if (++s.setup_drops <= SOURCE_SETUP_TRIES) {
            seek(SOURCE_SEEK_MS);
            s.retry_at = now() + SOURCE_BUSY_RETRY_MS;
            snprintf(msg, sizeof(msg), "Waiting for %s...", name_or(s.target_name, "headphones"));
            set_state(SOURCE_CONNECTING, msg);
            return;
        }
        s.setup_drops = 0;
        stop_seeking();
        snprintf(msg, sizeof(msg), "%s is in use by another device",
                 name_or(s.target_name, "Headphones"));
        set_state(SOURCE_READY, msg);
        return;
    }
    if (info.state != SOURCE_OFF && info.state != SOURCE_FAILED)
        set_state(SOURCE_READY, "Disconnected");
}

static bool get_key(const lbt_addr *a, uint8_t key[16])
{
    return ops->get_key && ops->get_key(a, key);
}

static void put_key(const lbt_addr *a, const uint8_t key[16], uint8_t type)
{
    if (ops->put_key)
        ops->put_key(a, key, type);
}

static void forget_key(const lbt_addr *a)
{
    if (ops->forget_key)
        ops->forget_key(a);
}

static lbt_addr confirm_addr;           /* whose pairing code is shown */

static void on_confirm(const lbt_addr *a, uint32_t code, bool passkey)
{
    confirm_addr = *a;
    if (ops->confirm)
        ops->confirm(info.peer_name, code, passkey);
    else if (!passkey)
        gap_confirm(a, true);
}

static const gap_ops gap_callbacks = {
    on_ready, on_found, on_named, on_inquiry_done, on_connected, on_secured,
    on_disconnected, get_key, put_key, forget_key, on_confirm,
};

/* ---- audio ---------------------------------------------------------------------------- */

static uint8_t play_status(void)
{
    return ops->play_status ? ops->play_status() : SOURCE_STOPPED;
}

static void on_configured(const avdtp_sbc_config *c)
{
    s.rate = c->rate;
    a2dp_setup(c, 0);
    info.rate = c->rate;
    info.bitpool = c->max_bitpool;
    info.frame_bytes = a2dp_frame_bytes();
    lbt_platform_log("source: SBC %d Hz, mode %d, blocks %d, subbands %d, bitpool %d "
                     "(%d-byte frames)", c->rate, c->mode, c->blocks, c->subbands,
                     c->max_bitpool, info.frame_bytes);
}

static void on_open(bool ok)
{
    if (!s.handle)
        return;
    if (!ok) {
        /* Refused, or the link is going: the disconnect, if it's that,
         * arrives after this. Decide in poll. */
        s.setup_failed = true;
        return;
    }
    s.open = true;
    s.had_audio = true;
    s.setup_drops = 0;
    info.frames_per_packet = a2dp_frames_per_packet();   /* known once media is up */
    s.target = info.peer;
    s.have_target = true;
    if (info.peer_name[0])
        copy_name(s.target_name, info.peer_name);
    if (ops->remember)
        ops->remember(&info.peer, s.target_name);
    /* the player's audio goes to the headphones from now on; the stream
     * itself starts when something plays */
    s.audio_on = true;
    if (ops->audio_begin)
        ops->audio_begin(s.rate);
    avrcp_connect(s.handle);   /* some headphones wait for us to open it */
    set_state(SOURCE_CONNECTED, "Connected");
    s.next_sync = now();       /* start right away if playing */
}

static void on_started(void)
{
    s.start_pending = false;
    s.we_suspended = false;
    s.streaming = true;
    s.last_ms = 0;
    s.samples_ready = 0;
    s.remainder = 0;
    s.idle_since = 0;
    set_state(SOURCE_STREAMING, "Playing over Bluetooth");
    avrcp_play_status_changed(play_status());
}

static void on_suspended(void)
{
    bool ours = s.we_suspended;
    s.streaming = false;
    s.start_pending = false;
    s.we_suspended = false;
    s.idle_since = 0;
    /* they suspended (an AirPod out): pause, so it doesn't play on unheard */
    if (!ours && play_status() == SOURCE_PLAYING && ops->action)
        ops->action(SOURCE_PAUSE);
    set_state(SOURCE_CONNECTED, "Paused");
}

static void on_stream_closed(void)
{
    s.open = false;
    audio_off();
    if (s.handle)
        set_state(SOURCE_CONNECTED, "Audio channel closed");
}

static const avdtp_ops avdtp_callbacks = {
    on_configured, on_open, on_started, on_suspended, on_stream_closed,
};

static void pump_audio(void)
{
    uint32_t t = now();
    uint32_t period = s.last_ms ? t - s.last_ms : 0;
    s.last_ms = t;
    if (period > 200)
        period = 200;   /* we were held up: don't try to catch up all of it */
    uint32_t add = period * (uint32_t)s.rate;
    s.samples_ready += add / 1000;
    s.remainder += add % 1000;
    if (s.remainder >= 1000) {
        s.samples_ready += s.remainder / 1000;
        s.remainder %= 1000;
    }
    if (s.samples_ready > (uint32_t)s.rate / 4)
        s.samples_ready = (uint32_t)s.rate / 4;

    int fpp = a2dp_frames_per_packet();
    int per_packet = fpp * a2dp_frame_samples();
    while (fpp > 0 && s.samples_ready >= (uint32_t)per_packet &&
           avdtp_can_send((uint16_t)(fpp * a2dp_frame_bytes()))) {
        bool audio = ops->audio_read && ops->audio_read(pcm, per_packet);
        if (!audio)
            memset(pcm, 0, sizeof(int16_t) * 2 * (size_t)per_packet);
        int used = a2dp_push(pcm, per_packet);
        if (used <= 0)
            break;
        s.samples_ready -= (uint32_t)used;
        info.packets++;
        if (audio)
            info.frames_audio += (uint32_t)fpp;
        else
            info.frames_silent += (uint32_t)fpp;
    }
}

/* ---- headphone controls ------------------------------------------------------------------ */

static void on_avrcp_connected(bool ok)
{
    lbt_platform_log("source: headphone controls %s", ok ? "connected" : "failed");
    if (!ok)
        return;
    s.controls_up = true;
    s.controls_since = now();
    s.volume_seen = false;
    if (ops->controls)
        ops->controls(true);
}

static void on_button(uint8_t op, bool pressed)
{
    if (!pressed || !ops->action)
        return;
    bool playing = play_status() == SOURCE_PLAYING;
    switch (op) {
    case AVRCP_OP_PLAY:
    case AVRCP_OP_PAUSE:
        /* By what's happening: some headphones send pause for every press,
         * and AirPods send play for their first presses even after we told
         * them we're playing. Only a play just after the controls connected
         * (an automatic resume) leaves playing music alone. */
        if (!playing)
            ops->action(SOURCE_RESUME);
        else if (op != AVRCP_OP_PLAY ||
                 after(now(), s.controls_since + SOURCE_AUTO_PLAY_MS))
            ops->action(SOURCE_PAUSE);
        break;
    case AVRCP_OP_STOP:     if (playing) ops->action(SOURCE_PAUSE); break;
    case AVRCP_OP_FORWARD:  ops->action(SOURCE_NEXT); break;
    case AVRCP_OP_BACKWARD: ops->action(SOURCE_PREVIOUS); break;
    case AVRCP_OP_VOLUME_UP:
        if (!avrcp_has_absolute_volume())
            ops->action(SOURCE_VOLUME_UP);
        break;
    case AVRCP_OP_VOLUME_DOWN:
        if (!avrcp_has_absolute_volume())
            ops->action(SOURCE_VOLUME_DOWN);
        break;
    default:
        break;
    }
}

static void on_volume(uint8_t v)
{
    bool first = !s.volume_seen;
    s.volume_seen = true;
    info.absolute_volume = true;
    if (ops->volume)
        ops->volume(v, first);
}

static void on_avrcp_disconnected(void)
{
    lbt_platform_log("source: headphone controls disconnected");
    if (s.controls_up && ops->controls)
        ops->controls(false);
    s.controls_up = false;
    s.volume_seen = false;
    info.absolute_volume = false;
}

static void on_track(avrcp_track *t)
{
    if (ops->track)
        ops->track(t);
    if (!coverart_connected())
        t->cover[0] = '\0';                     /* a handle means nothing to them yet */
}

static bool on_cover(const char *handle, const uint8_t **data, uint32_t *len)
{
    return ops->cover && ops->cover(handle, data, len);
}

static void on_cover_client(bool up)
{
    if (ops->cover_client)
        ops->cover_client(up);
    if (up)
        avrcp_track_changed();                 /* ask again: the song now has a cover */
}

static const coverart_ops coverart_callbacks = { on_cover, on_cover_client };

static const avrcp_ops avrcp_callbacks = {
    on_avrcp_connected, on_button, on_volume, on_avrcp_disconnected, play_status, on_track,
};

/* ---- API ---------------------------------------------------------------------------------- */

static char local_name[SOURCE_NAME_MAX];

void source_init(const source_ops *o, const char *name)
{
    ops = o;
    copy_name(local_name, name);
}

void source_start(const lbt_addr *last, const char *last_name)
{
    memset(&s, 0, sizeof(s));
    memset(&info, 0, sizeof(info));
    hci_init();
    l2cap_init();
    sdp_init();
    avdtp_init(&avdtp_callbacks);
    avrcp_init(&avrcp_callbacks);
    coverart_init(&coverart_callbacks);
    gap_init(&gap_callbacks, local_name, GAP_COD_PORTABLE_AUDIO);
    if (last) {
        s.have_target = true;
        s.target = *last;
        copy_name(s.target_name, last_name);
        seek(SOURCE_SEEK_MS);
    }
    set_state(SOURCE_STARTING, "Starting Bluetooth...");
    gap_start();
}

void source_stop(void)
{
    stop_seeking();
    if (s.handle)
        gap_disconnect(s.handle, REASON_POWER_OFF);
    link_gone();
    s.ready = false;
    set_state(SOURCE_OFF, "Off");
}

void source_poll(void)
{
    uint32_t t = now();
    if (s.streaming)
        pump_audio();
    if (!s.ready)
        return;

    if (s.scan_pending && !s.paging) {
        int seconds = s.scan_pending;
        s.scan_pending = 0;
        if (!s.handle && gap_inquiry(seconds))
            set_state(SOURCE_SCANNING, "Searching (put headphones in pairing mode)");
    }

    if (s.setup_failed) {
        /* the link is still up, so they refused our audio. A car that
         * connected to us may refuse ours and set up audio itself, when
         * it's ready: keep the link for it. Otherwise hang up. */
        s.setup_failed = false;
        if (s.handle && s.incoming && !s.open) {
            lbt_platform_log("source: they connected to us: waiting for them to set up audio");
            set_state(SOURCE_CONNECTED, "Connected, waiting for audio...");
        } else if (s.handle) {
            hang_up("Couldn't set up audio (see log)");
        }
    }

    if (after(t, s.next_tick)) {
        s.next_tick = t + 200;
        hci_tick();
        l2cap_tick();
        coverart_tick();
        avdtp_tick();
        avrcp_tick();
    }

    /* looking for the headphones */
    if (s.seeking && !s.handle && !s.paging && after(t, s.retry_at)) {
        if (after(t, s.seek_until)) {
            stop_seeking();
            set_state(SOURCE_READY, "Headphones not found");
        } else if (info.state != SOURCE_SCANNING) {
            page();
        }
    }

    /* they connected to us but haven't set up audio: we do it */
    if (s.avdtp_at && after(t, s.avdtp_at)) {
        s.avdtp_at = 0;
        if (s.handle && !s.open)
            avdtp_connect(s.handle);   /* refused if they already started */
    }

    if (s.handle && after(t, s.next_sync)) {
        s.next_sync = t + 300;
        uint8_t ps = play_status();
        avrcp_play_status_changed(ps);
        if (ps == SOURCE_PLAYING) {
            s.idle_since = 0;
            if (s.start_pending && after(t, s.start_sent + AVDTP_TIMEOUT_MS))
                s.start_pending = false;   /* no answer: try again */
            if (s.open && !s.streaming && !s.start_pending) {
                lbt_platform_log("source: starting the stream");
                s.start_pending = avdtp_start();
                s.start_sent = t;
            }
        } else if (s.streaming) {
            if (!s.idle_since)
                s.idle_since = t;
            else if (after(t, s.idle_since + SOURCE_IDLE_MS)) {
                lbt_platform_log("source: nothing playing, suspending the stream");
                s.we_suspended = avdtp_suspend();
                s.idle_since = t;
            }
        }
    }
}

bool source_scan(int seconds)
{
    if (!s.ready || s.handle)
        return false;
    stop_seeking();
    if (s.paging) {
        /* still trying to reach the last headphones (after start-up, say):
         * search as soon as that attempt ends, and stop looking for them */
        s.scan_pending = seconds;
        gap_connect_cancel(&s.target);
        lbt_platform_log("source: stopping the connection attempt to search");
        set_state(SOURCE_SCANNING, "Searching (put headphones in pairing mode)");
        return true;
    }
    if (!gap_inquiry(seconds))
        return false;
    set_state(SOURCE_SCANNING, "Searching (put headphones in pairing mode)");
    return true;
}

void source_confirm(bool yes)
{
    gap_confirm(&confirm_addr, yes);
}

void source_set_visible(bool on)
{
    if (on == s.visible)
        return;
    s.visible = on;
    if (s.ready)
        gap_set_discoverable(on);
}

void source_connect(const lbt_addr *addr, const char *name)
{
    if (!s.ready || s.handle || s.paging)
        return;
    s.have_target = true;
    s.target = *addr;
    copy_name(s.target_name, name);
    s.drop_when_connected = false;
    s.setup_drops = 0;
    stop_seeking();
    page();
}

void source_disconnect(void)
{
    stop_seeking();
    if (s.handle) {
        if (s.open)
            avdtp_close();
        gap_disconnect(s.handle, REASON_USER);
    } else if (s.paging) {
        s.drop_when_connected = true;
        set_state(SOURCE_READY, "Cancelled");
    }
}

void source_set_volume(uint8_t volume)
{
    avrcp_set_volume(volume);
}

const source_info *source_get_info(void)
{
    return &info;
}
