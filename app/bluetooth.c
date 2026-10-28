/*
 * Bluetooth headphones (bluetooth.h): lbt's source layer on the Bluetooth
 * thread, with the player, the volume and the screens.
 *
 * Pairings: our own file, /.lossless/state/bluetooth.bin, saved whenever
 * they change (once the card can be written).
 * The log goes to /.lossless/logs/bluetooth.txt as well as the screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bluetooth.h"
#include "output.h"
#include "jpegenc.h"
#include "los_sys.h"
#include "../lib/lbt/source.h"
#include "../lib/lbt/hci.h"
#include "../lib/lbt/lbt_platform.h"

#define LOCAL_NAME "Lossless"
#define KEYS 8

/* ---- shared with the UI (los_lock) ---- */
static bt_status st;
static char log_ring[BT_LOG_LINES][100];
static int log_next, log_count;
static unsigned log_total;
enum { CMD_ON = 1, CMD_OFF = 2, CMD_SCAN = 4, CMD_CONNECT = 8, CMD_DISCONNECT = 16,
       CMD_CONNECT_KNOWN = 32, CMD_FORGET = 64, CMD_VISIBLE = 128, CMD_CONFIRM_YES = 256,
       CMD_CONFIRM_NO = 512 };
static unsigned cmds;
static bool visible_wanted;           /* the search screen is open */
static int connect_index, known_index, forget_index;
static int play_state;
static avrcp_track now_playing;       /* the song, for a car's screen */
static uint32_t now_cover;            /* its cover's id, 0 if none */
/* its cover for a car: pixels from the UI (under los_lock), and the JPEG
 * made from them here when asked */
#define COVER_PX 200
static uint16_t cover_px[COVER_PX * COVER_PX];
static uint32_t cover_px_id;
static bool cover_wanted;
static uint16_t cover_copy[COVER_PX * COVER_PX];
static uint8_t cover_jpeg[64 * 1024];
static uint32_t cover_jpeg_len, cover_jpeg_id;
static int action_pending;
static int vol_out = -1;              /* 0..127 for the headphones, -1 none */
static int vol_in = -1;               /* 0..127 from the headphones, -1 none */
static int vol_theirs = -1;           /* what the headphones are at, as far as we know */
static int vol_min = -74, vol_max = -2;
static bool vol_synced;               /* one volume with them (absolute volume) */

/* ---- the Bluetooth thread's own ---- */
static bool chip_on;
static struct { bool used; uint8_t addr[6]; uint8_t key[16]; } keys[KEYS];
/* Headphones we have a pairing with, most recent first; known[0] is the
 * one to reconnect to */
static bt_device known[BT_KNOWN];
static int nknown;

/* Copy the list to the status (los_lock held) */
static void publish_known(void)
{
    st.nknown = nknown;
    memcpy(st.known, known, sizeof(known));
    st.have_last = nknown > 0;
    snprintf(st.last_name, sizeof(st.last_name), "%s", nknown ? known[0].name : "");
}

static int find_known(const uint8_t addr[6])
{
    for (int i = 0; i < nknown; i++)
        if (!memcmp(known[i].addr, addr, 6))
            return i;
    return -1;
}

/* To the front of the list (added if new); a name replaces the old one */
static void known_to_front(const uint8_t addr[6], const char *name)
{
    int i = find_known(addr);
    bt_device d;
    if (i >= 0) {
        d = known[i];
        memmove(&known[1], &known[0], sizeof(bt_device) * (size_t)i);
    } else {
        memset(&d, 0, sizeof(d));
        memcpy(d.addr, addr, 6);
        snprintf(d.name, sizeof(d.name), "Headphones");
        memmove(&known[1], &known[0], sizeof(bt_device) * (size_t)(nknown < BT_KNOWN ? nknown : BT_KNOWN - 1));
        if (nknown < BT_KNOWN)
            nknown++;
    }
    if (name && name[0])
        snprintf(d.name, sizeof(d.name), "%s", name);
    known[0] = d;
}

static void known_remove(int i)
{
    memmove(&known[i], &known[i + 1], sizeof(bt_device) * (size_t)(nknown - i - 1));
    nknown--;
}

/* Lines waiting to go to the card (the Bluetooth thread writes them) */
static char log_pending[4096];
static size_t log_pending_len;

static void log_line(const char *line)
{
    los_lock();
    size_t n = strlen(line);
    if (log_pending_len + n + 1 < sizeof(log_pending)) {
        memcpy(log_pending + log_pending_len, line, n);
        log_pending_len += n;
        log_pending[log_pending_len++] = '\n';
    }
    snprintf(log_ring[log_next], sizeof(log_ring[0]), "%s", line);
    log_next = (log_next + 1) % BT_LOG_LINES;
    if (log_count < BT_LOG_LINES)
        log_count++;
    log_total++;
    los_unlock();
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char line[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_line(line);
}

unsigned bluetooth_log_count(void)
{
    los_lock();
    unsigned n = log_total;
    los_unlock();
    return n;
}

int bluetooth_log_lines(char lines[][100], int max)
{
    los_lock();
    int n = log_count < max ? log_count : max;
    int first = (log_next - n + BT_LOG_LINES) % BT_LOG_LINES;
    for (int i = 0; i < n; i++)
        memcpy(lines[i], log_ring[(first + i) % BT_LOG_LINES], sizeof(lines[0]));
    los_unlock();
    return n;
}

/* ---- our own file: the headphones and their pairings ---- */

#define STORE_MAGIC "LBT1"
#define STORE_ENTRY (6 + 32 + 1 + 16)    /* address, name, has key, key */

static void state_dir(char *p, size_t n)
{
    snprintf(p, n, "%s/.lossless/state", los_card_path());
}

static void store_path(char *p, size_t n)
{
    snprintf(p, n, "%s/.lossless/state/bluetooth.bin", los_card_path());
}

static int find_key_slot(const uint8_t addr[6])
{
    for (int i = 0; i < KEYS; i++)
        if (keys[i].used && !memcmp(keys[i].addr, addr, 6))
            return i;
    return -1;
}

/* All of it, most recent first */
static void save_store(void)
{
    uint8_t buf[5 + BT_KNOWN * STORE_ENTRY];
    char path[160], dir[160];
    if (!los_writes_ok())
        return;
    memcpy(buf, STORE_MAGIC, 4);
    los_lock();
    int n = nknown;
    buf[4] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *e = buf + 5 + i * STORE_ENTRY;
        memcpy(e, known[i].addr, 6);
        memcpy(e + 6, known[i].name, 32);
        int k = find_key_slot(known[i].addr);
        e[38] = k >= 0;
        if (k >= 0)
            memcpy(e + 39, keys[k].key, 16);
        else
            memset(e + 39, 0, 16);
    }
    los_unlock();
    char top[160];
    snprintf(top, sizeof(top), "%s/.lossless", los_card_path());
    state_dir(dir, sizeof(dir));
    store_path(path, sizeof(path));
    los_mkdir(top);
    los_mkdir(dir);
    if (!los_file_save(path, buf, 5 + (size_t)n * STORE_ENTRY))
        logf_("couldn't save the pairings");
}

static bool load_store(void)
{
    uint8_t buf[5 + BT_KNOWN * STORE_ENTRY];
    char path[160], tmp[170];
    store_path(path, sizeof(path));
    void *f = los_file_open(path);
    if (!f) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        f = los_file_open(tmp);
    }
    if (!f)
        return false;
    size_t got = los_file_read(f, buf, sizeof(buf));
    los_file_close(f);
    if (got < 5 || memcmp(buf, STORE_MAGIC, 4))
        return false;
    int n = buf[4] > BT_KNOWN ? BT_KNOWN : buf[4], nk = 0;
    if (got < 5 + (size_t)n * STORE_ENTRY)
        return false;
    los_lock();
    nknown = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = buf + 5 + i * STORE_ENTRY;
        memset(&known[nknown], 0, sizeof(known[0]));
        memcpy(known[nknown].addr, e, 6);
        memcpy(known[nknown].name, e + 6, 31);
        nknown++;
        if (e[38] && nk < KEYS) {
            keys[nk].used = true;
            memcpy(keys[nk].addr, e, 6);
            memcpy(keys[nk].key, e + 39, 16);
            nk++;
        }
    }
    publish_known();
    los_unlock();
    logf_("headphones from /.lossless: %d (%d pairings)", n, nk);
    return true;
}

/* Lines for the card: every few seconds, from the Bluetooth thread */
static void flush_log(void)
{
    static bool started;
    char buf[sizeof(log_pending)];
    char path[176];   /* the folder, then /bluetooth.txt */
    if (!los_writes_ok())
        return;
    los_lock();
    size_t n = log_pending_len;
    memcpy(buf, log_pending, n);
    log_pending_len = 0;
    los_unlock();
    if (!n)
        return;
    char top[160], dir[160];
    snprintf(top, sizeof(top), "%s/.lossless", los_card_path());
    snprintf(dir, sizeof(dir), "%s/.lossless/logs", los_card_path());
    snprintf(path, sizeof(path), "%s/bluetooth.txt", dir);
    if (!started) {
        started = true;
        los_mkdir(top);
        los_mkdir(dir);
        void *f = los_file_open(path);
        if (f) {      /* keep it small: start again past 512 KB */
            uint64_t size = los_file_size(f);
            los_file_close(f);
            if (size > 512 * 1024)
                los_file_remove(path);
        }
        los_file_append(path, "--- start ---\n", 14);
    }
    los_file_append(path, buf, n);
}

/* ---- lbt's source layer asks ---- */

static void on_state(enum source_state s, const char *msg)
{
    const source_info *i = source_get_info();
    los_lock();
    st.state = (enum bt_state)s;
    snprintf(st.message, sizeof(st.message), "%s", msg ? msg : "");
    st.rate = i->rate;
    st.bitpool = i->bitpool;
    if (s >= SOURCE_CONNECTING && i->peer_name[0])
        snprintf(st.peer, sizeof(st.peer), "%s", i->peer_name);
    st.peer_known = s >= SOURCE_CONNECTING ? find_known(i->peer.b) : -1;
    if (s != SOURCE_CONNECTING)
        st.confirm = st.passkey = false;   /* pairing over, one way or the other */
    los_unlock();
    logf_("state %d: %s", (int)s, msg ? msg : "");
}

static void on_found(const lbt_addr *a, const char *name, uint32_t cod, int8_t rssi)
{
    (void)cod;
    (void)rssi;
    los_lock();
    int i;
    for (i = 0; i < st.ndevices; i++)
        if (!memcmp(st.devices[i].addr, a->b, 6))
            break;
    if (i == st.ndevices && st.ndevices < BT_DEVICES) {
        memcpy(st.devices[i].addr, a->b, 6);
        st.devices[i].name[0] = '\0';
        st.ndevices++;
    }
    if (i < st.ndevices && name[0])
        snprintf(st.devices[i].name, sizeof(st.devices[i].name), "%s", name);
    los_unlock();
}

static bool get_key(const lbt_addr *a, uint8_t key[16])
{
    for (int i = 0; i < KEYS; i++)
        if (keys[i].used && !memcmp(keys[i].addr, a->b, 6)) {
            memcpy(key, keys[i].key, 16);
            return true;
        }
    return false;
}

static void put_key(const lbt_addr *a, const uint8_t key[16], uint8_t type)
{
    int slot = 0;
    for (int i = KEYS - 1; i >= 0; i--)
        if (!keys[i].used || !memcmp(keys[i].addr, a->b, 6))
            slot = i;
    keys[slot].used = true;
    memcpy(keys[slot].addr, a->b, 6);
    memcpy(keys[slot].key, key, 16);
    los_lock();
    if (find_known(a->b) < 0) {
        known_to_front(a->b, st.peer);
        publish_known();
    }
    los_unlock();
    logf_("paired (key type %u)", type);
    save_store();
}

static void forget_key(const lbt_addr *a)
{
    for (int i = 0; i < KEYS; i++)
        if (keys[i].used && !memcmp(keys[i].addr, a->b, 6))
            keys[i].used = false;
}

static void on_remember(const lbt_addr *a, const char *name)
{
    los_lock();
    known_to_front(a->b, name);
    publish_known();
    los_unlock();
    save_store();
}

static void on_audio_begin(int rate)
{
    logf_("audio to the headphones at %d Hz", rate);
    output_bluetooth(rate);
}

static void on_audio_end(void)
{
    logf_("audio to the headphones ended");
    output_bluetooth(0);
}

static bool on_audio_read(int16_t *pcm, int frames)
{
    return output_bluetooth_read(pcm, frames);
}

static uint8_t on_play_status(void)
{
    los_lock();
    int s = play_state;
    los_unlock();
    return s == 1 ? SOURCE_PLAYING : s == 2 ? SOURCE_PAUSED : SOURCE_STOPPED;
}

static void on_action(enum source_action a)
{
    static const int map[] = {
        [SOURCE_PAUSE] = BT_ACTION_PAUSE, [SOURCE_RESUME] = BT_ACTION_PLAY,
        [SOURCE_NEXT] = BT_ACTION_NEXT, [SOURCE_PREVIOUS] = BT_ACTION_PREVIOUS,
        [SOURCE_VOLUME_UP] = BT_ACTION_VOLUME_UP, [SOURCE_VOLUME_DOWN] = BT_ACTION_VOLUME_DOWN,
    };
    los_lock();
    action_pending = map[a];
    los_unlock();
}

static void on_volume(uint8_t v, bool first)
{
    los_lock();
    if (first) {
        /* the player's volume wins: send ours (the UI keeps vol_out set) */
        vol_synced = true;
        if (vol_out < 0)
            vol_in = v;
    } else {
        vol_in = v;
    }
    int send = first ? vol_out : -1;
    vol_theirs = send >= 0 ? send : v;
    los_unlock();
    if (send >= 0)
        source_set_volume((uint8_t)send);
    logf_("headphones' volume %u/127%s", v, first ? " (first)" : "");
}

static void on_controls(bool up)
{
    if (!up) {
        los_lock();
        vol_synced = false;
        los_unlock();
    }
}

static void on_track(avrcp_track *t)
{
    los_lock();
    *t = now_playing;
    t->cover[0] = '\0';
    if (now_cover && now_cover == cover_px_id)    /* only once its pixels are here */
        snprintf(t->cover, sizeof(t->cover), "%07u", (unsigned)(now_cover % 10000000));
    /* a cover that has just come is news too: the car asks again */
    t->serial = t->serial << 1 | (t->cover[0] ? 1u : 0u);
    los_unlock();
}

static bool on_cover(const char *handle, const uint8_t **data, uint32_t *len)
{
    uint32_t id = (uint32_t)strtoul(handle, NULL, 10);
    if (!id)
        return false;
    if (id != cover_jpeg_id) {
        los_lock();
        bool have = id == cover_px_id;
        if (have)
            memcpy(cover_copy, cover_px, sizeof(cover_copy));
        los_unlock();
        if (!have)
            return false;
        size_t n = jpeg_encode_rgb565(cover_copy, COVER_PX, COVER_PX, COVER_PX, 85,
                                      cover_jpeg, sizeof(cover_jpeg));
        if (!n)
            return false;
        cover_jpeg_len = (uint32_t)n;
        cover_jpeg_id = id;
        logf_("cover %07u: %u bytes", (unsigned)id, (unsigned)n);
    }
    *data = cover_jpeg;
    *len = cover_jpeg_len;
    return true;
}

static void on_cover_client(bool up)
{
    logf_(up ? "a car wants covers" : "the car's covers are done");
    los_lock();
    cover_wanted = up;
    los_unlock();
}

static void on_confirm(const char *name, uint32_t code, bool passkey)
{
    logf_(passkey ? "pairing: type %06u on %s" : "pairing: code %06u, confirm on both",
          (unsigned)code, name[0] ? name : "the device");
    los_lock();
    st.confirm = !passkey;
    st.passkey = passkey;
    st.code = code;
    snprintf(st.confirm_name, sizeof(st.confirm_name), "%s", name[0] ? name : "the device");
    los_unlock();
}

static const source_ops ops = {
    on_state, on_found, get_key, put_key, forget_key, on_remember, on_audio_begin,
    on_audio_end, on_audio_read, on_play_status, on_action, on_volume, on_controls,
    on_confirm, on_track, on_cover, on_cover_client,
};

/* ---- the Bluetooth thread ---- */

static void chip_start(void)
{
    los_lock();
    st.state = BT_STARTING;
    snprintf(st.message, sizeof(st.message), "Starting Bluetooth...");
    los_unlock();
    if (!los_bt_up(log_line)) {
        los_lock();
        st.state = BT_FAILED;
        snprintf(st.message, sizeof(st.message), "The Bluetooth chip didn't start");
        st.on = false;
        los_unlock();
        return;
    }
    chip_on = true;
    lbt_addr a;
    char name[32];
    los_lock();
    bool have = nknown > 0;
    if (have) {
        memcpy(a.b, known[0].addr, 6);
        snprintf(name, sizeof(name), "%s", known[0].name);
    }
    los_unlock();
    source_start(have ? &a : NULL, have ? name : "");
}

static void chip_stop(void)
{
    source_stop();                 /* pauses, says goodbye */
    for (int i = 0; i < 20; i++) {
        los_bt_poll();
        los_sleep_ms(10);
    }
    output_bluetooth(0);
    los_bt_down();
    chip_on = false;
    los_lock();
    st.state = BT_OFF;
    snprintf(st.message, sizeof(st.message), "Off");
    vol_synced = false;
    los_unlock();
}

/* Disconnect and wait for it (up to 4 s), so another can connect */
static void hang_up(void)
{
    source_disconnect();
    for (int t = 0; t < 400 && source_get_info()->state >= SOURCE_CONNECTING; t++) {
        los_bt_poll();
        los_sleep_ms(10);
    }
}

/* Connect to these headphones, leaving any others first */
static void connect_to(const lbt_addr *a, const char *name)
{
    const source_info *i = source_get_info();
    if (i->state >= SOURCE_CONNECTED && !memcmp(i->peer.b, a->b, 6))
        return;                    /* already them */
    if (i->state >= SOURCE_CONNECTING)
        hang_up();
    los_lock();
    snprintf(st.peer, sizeof(st.peer), "%s", name[0] ? name : "Headphones");
    los_unlock();
    source_connect(a, name);
}

static void handle_commands(void)
{
    los_lock();
    unsigned c = cmds;
    cmds = 0;
    int index = connect_index;
    los_unlock();
    if (c & CMD_OFF) {
        if (chip_on)
            chip_stop();
        return;
    }
    static bool visible_now;
    if ((c & CMD_ON) && !chip_on) {
        chip_start();
        visible_now = false;           /* a fresh start: not visible */
    }
    if (!chip_on)
        return;
    los_lock();
    bool want = visible_wanted;
    los_unlock();
    if (want != visible_now) {
        visible_now = want;
        source_set_visible(want);
        logf_(want ? "visible to searches as %s" : "not visible to searches", LOCAL_NAME);
    }
    if (c & (CMD_CONFIRM_YES | CMD_CONFIRM_NO)) {
        logf_((c & CMD_CONFIRM_YES) ? "pairing: confirmed" : "pairing: refused");
        source_confirm((c & CMD_CONFIRM_YES) != 0);
    }
    if (c & CMD_SCAN) {
        los_lock();
        st.ndevices = 0;
        los_unlock();
        /* lbt searches only without a link: leave the headphones first */
        if (source_get_info()->state >= SOURCE_CONNECTED)
            hang_up();
        if (!source_scan(10))
            logf_("couldn't start the search (state %d)", (int)source_get_info()->state);
    }
    if (c & CMD_CONNECT) {
        lbt_addr a;
        char name[32];
        los_lock();
        bool ok = index >= 0 ? index < st.ndevices : nknown > 0;
        if (ok && index >= 0) {
            memcpy(a.b, st.devices[index].addr, 6);
            snprintf(name, sizeof(name), "%s", st.devices[index].name);
        } else if (ok) {
            memcpy(a.b, known[0].addr, 6);
            snprintf(name, sizeof(name), "%s", known[0].name);
        }
        los_unlock();
        if (ok)
            connect_to(&a, name);
    }
    if (c & CMD_CONNECT_KNOWN) {
        lbt_addr a;
        char name[32];
        los_lock();
        int k = known_index;
        bool ok = k >= 0 && k < nknown;
        if (ok) {
            memcpy(a.b, known[k].addr, 6);
            snprintf(name, sizeof(name), "%s", known[k].name);
        }
        los_unlock();
        if (ok)
            connect_to(&a, name);
    }
    if (c & CMD_FORGET) {
        los_lock();
        int k = forget_index;
        bool ok = k >= 0 && k < nknown;
        uint8_t addr[6];
        char name[32] = "";
        if (ok) {
            memcpy(addr, known[k].addr, 6);
            snprintf(name, sizeof(name), "%s", known[k].name);
        }
        los_unlock();
        if (ok) {
            const source_info *i = source_get_info();
            if (i->state >= SOURCE_CONNECTING && !memcmp(i->peer.b, addr, 6))
                hang_up();
            lbt_addr a;
            memcpy(a.b, addr, 6);
            forget_key(&a);
            los_lock();
            int at = find_known(addr);
            if (at >= 0)
                known_remove(at);
            publish_known();
            st.peer_known = -1;
            los_unlock();
            logf_("forgot %s", name);
            save_store();
        }
    }
    if (c & CMD_DISCONNECT)
        source_disconnect();
}

static void bt_thread(void)
{
    los_lock();
    st.peer_known = -1;
    los_unlock();
    source_init(&ops, LOCAL_NAME);
    load_store();
    uint32_t next_flush = 0;
    for (;;) {
        los_lock();
        unsigned c = cmds;
        int out = vol_synced && vol_out != vol_theirs ? vol_out : -1;
        if (out >= 0)
            vol_theirs = out;
        los_unlock();
        if (c)
            handle_commands();
        uint32_t t = lbt_platform_ms();
        if ((int32_t)(t - next_flush) >= 0) {
            flush_log();
            next_flush = t + 5000;
        }
        if (!chip_on) {
            los_sleep_ms(20);
            continue;
        }
        if (out >= 0)
            source_set_volume((uint8_t)out);
        los_bt_poll();

        /* while streaming, how it's going, every 10 s */
        static uint32_t next_report, last_packets, last_silent;
        static int most_free;    /* the most chip buffers free in this report's 10 s */
        int total, free_now = hci_acl_free(&total);
        if (free_now > most_free)
            most_free = free_now;
        const source_info *i = source_get_info();
        uint32_t now = lbt_platform_ms();
        if (i->state == SOURCE_STREAMING && (int32_t)(now - next_report) >= 0) {
            if (next_report)
                logf_("10 s: %u packets, %u silent, bitpool %d, chip buffers %d/%d",
                      (unsigned)(i->packets - last_packets),
                      (unsigned)(i->frames_silent - last_silent), i->bitpool, most_free, total);
            most_free = 0;
            last_packets = i->packets;
            last_silent = i->frames_silent;
            next_report = now + 10000;
        } else if (i->state != SOURCE_STREAMING) {
            next_report = 0;
        }
        los_sleep_ms(1);
    }
}

/* ---- the UI's side ---- */

static void post(unsigned c)
{
    los_lock();
    cmds |= c;
    los_unlock();
}

void bluetooth_init(void)
{
    static bool started;
    if (!started) {
        started = true;
        los_thread_start(LOS_THREAD_BLUETOOTH, bt_thread);
    }
}

void bluetooth_power(bool on)
{
    los_lock();
    st.on = on;
    if (on && st.state == BT_FAILED)
        st.state = BT_OFF;
    los_unlock();
    post(on ? CMD_ON : CMD_OFF);
}

void bluetooth_scan(void) { post(CMD_SCAN); }

void bluetooth_confirm(bool yes)
{
    los_lock();
    st.confirm = st.passkey = false;
    cmds |= yes ? CMD_CONFIRM_YES : CMD_CONFIRM_NO;
    los_unlock();
}

void bluetooth_visible(bool on)
{
    los_lock();
    if (visible_wanted != on) {
        visible_wanted = on;
        cmds |= CMD_VISIBLE;          /* the thread acts on commands only */
    }
    los_unlock();
}

void bluetooth_connect(int device)
{
    los_lock();
    connect_index = device;
    los_unlock();
    post(CMD_CONNECT);
}

void bluetooth_disconnect(void) { post(CMD_DISCONNECT); }

void bluetooth_connect_known(int i)
{
    los_lock();
    known_index = i;
    los_unlock();
    post(CMD_CONNECT_KNOWN);
}

void bluetooth_forget(int i)
{
    los_lock();
    forget_index = i;
    los_unlock();
    post(CMD_FORGET);
}

void bluetooth_status(bt_status *out)
{
    los_lock();
    *out = st;
    los_unlock();
}

void bluetooth_now_playing(const char *title, const char *artist, const char *album,
                           int number, int total, uint32_t length_ms, uint32_t position_ms,
                           uint32_t serial, uint32_t cover_id)
{
    los_lock();
    now_cover = title ? cover_id : 0;
    now_playing.loaded = title != NULL;
    if (title) {
        snprintf(now_playing.title, sizeof(now_playing.title), "%s", title);
        snprintf(now_playing.artist, sizeof(now_playing.artist), "%s", artist ? artist : "");
        snprintf(now_playing.album, sizeof(now_playing.album), "%s", album ? album : "");
    }
    now_playing.number = number > 0 ? (uint32_t)number : 0;
    now_playing.total = total > 0 ? (uint32_t)total : 0;
    now_playing.length_ms = length_ms;
    now_playing.position_ms = position_ms;
    now_playing.serial = serial;
    los_unlock();
}

bool bluetooth_wants_cover(void)
{
    los_lock();
    bool w = cover_wanted;
    los_unlock();
    return w;
}

void bluetooth_cover(const uint16_t *px, int stride, uint32_t cover_id)
{
    los_lock();
    for (int y = 0; y < COVER_PX; y++)
        memcpy(cover_px + y * COVER_PX, px + y * stride, COVER_PX * sizeof(uint16_t));
    cover_px_id = cover_id;
    los_unlock();
}

void bluetooth_play_status(int state)
{
    los_lock();
    play_state = state;
    los_unlock();
}

int bluetooth_take_action(void)
{
    los_lock();
    int a = action_pending;
    action_pending = 0;
    los_unlock();
    return a;
}

static int to_abs(int db)
{
    if (db <= vol_min)
        return 0;
    if (db >= vol_max)
        return 127;
    return ((db - vol_min) * 127 + (vol_max - vol_min) / 2) / (vol_max - vol_min);
}

void bluetooth_set_volume(int db, int min_db, int max_db)
{
    los_lock();
    vol_min = min_db;
    vol_max = max_db;
    vol_out = to_abs(db);
    los_unlock();
}

bool bluetooth_take_volume(int *db)
{
    los_lock();
    int v = vol_in;
    vol_in = -1;
    bool synced = vol_synced;
    los_unlock();
    if (v < 0 || !synced)
        return false;
    /* to the nearest 2 dB step of the player's range */
    int d = vol_min + (v * (vol_max - vol_min) + 63) / 127;
    d = vol_max - ((vol_max - d + 1) / 2) * 2;
    *db = d < vol_min ? vol_min : d > vol_max ? vol_max : d;
    los_lock();
    vol_out = v;                /* what they have: nothing to send back */
    los_unlock();
    return true;
}

bool bluetooth_has_volume(void)
{
    los_lock();
    bool s = vol_synced && st.state >= BT_CONNECTED;
    los_unlock();
    return s;
}
