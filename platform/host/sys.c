/*
 * The simulator's side of los_sys.h: files with stdio, sound through SDL,
 * an SDL thread. The Mac may resample to its output device; on the player
 * the samples go to the DAC unchanged.
 *
 * Without a sound device (or with LOS_NO_AUDIO set, as in scripted runs)
 * a silent output takes samples in real time, so playback still moves on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include "los_sys.h"

/* ---- files -------------------------------------------------------------------------- */

void *los_file_open(const char *path) { return fopen(path, "rb"); }
size_t los_file_read(void *f, void *buf, size_t n) { return fread(buf, 1, n, f); }
bool los_file_seek(void *f, uint64_t pos) { return fseeko(f, (off_t)pos, SEEK_SET) == 0; }
void los_file_close(void *f) { fclose(f); }
/* Writing, as on the player: only in <card>/.lossless */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
const char *los_card_path(void);

bool los_writes_ok(void) { return true; }

static bool allowed(const char *path)
{
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/.lossless", los_card_path());
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '\0' || path[n] == '/') && !strstr(path, "..");
}

static bool write_file(const char *path, const void *data, size_t len, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (!f)
        return false;
    bool ok = !len || fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    return fclose(f) == 0 && ok;
}

bool los_file_save(const char *path, const void *data, size_t len)
{
    char tmp[1200];
    if (!allowed(path))
        return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    return write_file(tmp, data, len, "wb") && rename(tmp, path) == 0;
}

bool los_file_append(const char *path, const void *data, size_t len)
{
    return allowed(path) && write_file(path, data, len, "ab");
}

bool los_file_remove(const char *path)
{
    return allowed(path) && (remove(path) == 0 || errno == ENOENT);
}

bool los_mkdir(const char *path)
{
    return allowed(path) && (mkdir(path, 0755) == 0 || errno == EEXIST);
}

uint64_t los_file_size(void *f)
{
    off_t at = ftello(f);
    fseeko(f, 0, SEEK_END);
    off_t size = ftello(f);
    fseeko(f, at, SEEK_SET);
    return size > 0 ? (uint64_t)size : 0;
}

/* ---- thread, lock ------------------------------------------------------------------ */

static SDL_mutex *lock;

static int thread_main(void *fn)
{
    ((void (*)(void))fn)();
    return 0;
}

void los_thread_start(enum los_thread which, void (*fn)(void))
{
    if (!lock)
        lock = SDL_CreateMutex();
    SDL_CreateThread(thread_main, which == LOS_THREAD_AUDIO ? "audio" : "bluetooth", (void *)fn);
}

void los_lock(void)
{
    if (!lock)
        lock = SDL_CreateMutex();
    SDL_LockMutex(lock);
}

void los_unlock(void) { SDL_UnlockMutex(lock); }
void los_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }

const char *los_card_path(void);   /* main.c */

void los_backlight(int percent) { (void)percent; }   /* the app dims what it draws too */

bool los_battery(int *percent, bool *charging, bool *usb)
{
    const char *e = getenv("LOS_BATTERY");     /* to try the low-battery path */
    *percent = e ? atoi(e) : 76;
    *charging = *usb = false;
    return true;
}

/* USB, pretended: los_sim_usb(0) no cable, 1 a charger, 2 a computer,
 * 3 the computer ejects the card (scripts: "usb N"; the window: U) */
static int sim_usb;
static bool sim_shared, sim_ejected, sim_dac;

void los_sim_usb(int s)
{
    if (s == 3)
        sim_ejected = true;
    else
        sim_usb = s;
    if (s == 0)
        sim_dac = false;
}

enum los_usb los_usb_state(void)
{
    if (sim_usb == 0)
        return LOS_USB_NONE;
    if (sim_usb == 1)
        return LOS_USB_CABLE;
    if (sim_dac)
        return LOS_USB_DAC;
    if (sim_shared)
        return sim_ejected ? LOS_USB_EJECTED : LOS_USB_SHARED;
    return LOS_USB_COMPUTER;
}

/* As a USB DAC, pretended: the computer plays 96 kHz, 24-bit */
bool los_usb_dac(bool on)
{
    if (on && los_usb_state() != LOS_USB_COMPUTER)
        return false;
    sim_dac = on;
    return true;
}

bool los_usb_dac_info(unsigned *rate, unsigned *bits)
{
    *rate = 96000;
    *bits = 24;
    return sim_dac;
}

bool los_usb_share(bool on)
{
    if (on && los_usb_state() != LOS_USB_COMPUTER)
        return false;
    sim_shared = on;
    sim_ejected = false;
    return true;
}

void los_power_off(void)
{
    printf("switched off\n");
    exit(0);
}

/* ---- clock: the Mac's local time, plus whatever was set ------------------------------ */

#include <time.h>
static int64_t clock_offset;

bool los_clock_get(int64_t *seconds)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    *seconds = (int64_t)now + lt.tm_gmtoff + clock_offset;
    return true;
}

bool los_clock_set(int64_t seconds)
{
    int64_t now;
    clock_offset = 0;
    los_clock_get(&now);
    clock_offset = seconds - now;
    return true;
}

/* ---- sound ------------------------------------------------------------------------- */

/* how far ahead the sim's DAC queue runs; LOS_QUEUE_MS changes it (tests) */
static unsigned queue_ms(void)
{
    static unsigned ms;
    if (!ms) {
        const char *e = getenv("LOS_QUEUE_MS");
        ms = e && atoi(e) > 0 ? (unsigned)atoi(e) : 250;
    }
    return ms;
}
#define QUEUE_MS queue_ms()

static SDL_AudioDeviceID dev;
static unsigned rate, bits, channels;
static volatile uint64_t written;
static bool silent;                 /* no device: count time instead */
static uint32_t silent_start;
static uint64_t silent_paused_frames;
static bool paused;

/* Volume, as the player's (2 dB steps, -74 to -2 dB). The player sets it
 * in the DAC; here the samples are scaled, as the Mac has no other way. */
#define VOL_MIN (-74)
#define VOL_MAX (-2)
static volatile int volume_db = -20;
static volatile int32_t gain_q16 = 6554;       /* 10^(-20/20) in 1/65536 */

static int trim_half;                   /* los_volume_trim */

static void host_gain(void)
{
    double db = volume_db + trim_half / 2.0;
    gain_q16 = (int32_t)(SDL_pow(10.0, (db > VOL_MAX ? VOL_MAX : db) / 20.0) * 65536.0);
}

void host_volume_step(int db)
{
    int v = volume_db + db;
    v = v < VOL_MIN ? VOL_MIN : v > VOL_MAX ? VOL_MAX : v;
    volume_db = v;
    host_gain();
}

void los_volume_trim(int half_db)
{
    trim_half = half_db > 0 ? half_db : 0;
    host_gain();
    if (getenv("LOS_CAPTURE"))
        fprintf(stderr, "trim: %+.1f dB at %llu frames heard\n", trim_half / 2.0,
                (unsigned long long)los_audio_played());
}

void los_volume_set(int db)
{
    host_volume_step(db - volume_db);
}

bool los_volume(int *db, int *min_db, int *max_db)
{
    *db = volume_db;
    *min_db = VOL_MIN;
    *max_db = VOL_MAX;
    return true;
}

/* LOS_CAPTURE=file: everything the DAC is given, as raw 32-bit samples,
 * with a line on stderr each time the output starts (for the gapless test) */
static FILE *capture;

static void capture_start(void)
{
    const char *f = getenv("LOS_CAPTURE");
    if (f && !capture)
        capture = fopen(f, "wb");
    if (capture)
        fprintf(stderr, "capture: output start %u Hz %u bits at byte %ld\n", rate, bits, ftell(capture));
}

bool los_audio_start(unsigned r, unsigned b, unsigned ch)
{
    los_audio_stop();
    rate = r;
    bits = b;
    channels = ch;
    written = 0;
    paused = false;
    silent = getenv("LOS_NO_AUDIO") != NULL;
    if (!silent && !SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        silent = true;
    if (!silent) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = (int)r;
        want.format = AUDIO_S32SYS;
        want.channels = (Uint8)ch;
        want.samples = 1024;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!dev)
            silent = true;
        else
            SDL_PauseAudioDevice(dev, 0);
    }
    silent_start = SDL_GetTicks();
    silent_paused_frames = 0;
    capture_start();
    return true;
}

static uint64_t queued_frames(void)
{
    return dev ? SDL_GetQueuedAudioSize(dev) / (4 * channels) : 0;
}

uint64_t los_audio_played(void)
{
    if (silent) {
        uint64_t t = (uint64_t)(SDL_GetTicks() - silent_start) * rate / 1000;
        uint64_t heard = t > silent_paused_frames ? t - silent_paused_frames : 0;
        return heard < written ? heard : written;
    }
    uint64_t q = queued_frames();
    return written > q ? written - q : 0;
}

size_t los_audio_write(const int32_t *frames, size_t count)
{
    if (paused)
        return 0;
    uint64_t room;
    if (silent) {
        uint64_t heard = los_audio_played();
        uint64_t ahead = written - heard, cap = (uint64_t)rate * QUEUE_MS / 1000;
        room = ahead < cap ? cap - ahead : 0;
    } else {
        uint64_t q = queued_frames(), cap = (uint64_t)rate * QUEUE_MS / 1000;
        room = q < cap ? cap - q : 0;
    }
    if (count > room)
        count = (size_t)room;
    if (count && !silent) {
        static int32_t buf[4096 * 2];
        size_t done = 0;
        while (done < count) {
            size_t n = count - done > 4096 ? 4096 : count - done;
            for (size_t i = 0; i < n * channels; i++)
                buf[i] = (int32_t)(((int64_t)(int32_t)((uint32_t)frames[done * channels + i]
                                                        << (32 - bits)) * gain_q16) >> 16);
            SDL_QueueAudio(dev, buf, (Uint32)(n * channels * 4));
            done += n;
        }
    }
    if (count && capture) {
        fwrite(frames, sizeof(int32_t) * channels, count, capture);
        fflush(capture);
    }
    written += count;
    return count;
}

void los_audio_pause(bool p)
{
    static uint32_t paused_at;
    if (p == paused)
        return;
    paused = p;
    if (dev)
        SDL_PauseAudioDevice(dev, p);
    if (silent) {
        if (p)
            paused_at = SDL_GetTicks();
        else
            silent_paused_frames += (uint64_t)(SDL_GetTicks() - paused_at) * rate / 1000;
    }
}

void los_audio_stop(void)
{
    if (dev) {
        SDL_ClearQueuedAudio(dev);
        SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    written = 0;
}
