/*
 * Zephyr's side of los_sys.h: files through Zephyr's file system, the
 * audio thread, a lock. The audio output here is silent (it takes samples
 * in real time); a board with sound replaces los_audio_* (platform/h2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "los_sys.h"

#ifdef CONFIG_FILE_SYSTEM
#include <zephyr/fs/fs.h>

/* A few files open at once: the audio thread's and the library's */
#define FILES 4
static struct fs_file_t files[FILES];
static bool in_use[FILES];

void *los_file_open(const char *path)
{
    unsigned key = irq_lock();
    int i;
    for (i = 0; i < FILES && in_use[i]; i++) {
    }
    if (i == FILES) {
        irq_unlock(key);
        return NULL;
    }
    in_use[i] = true;
    irq_unlock(key);

    fs_file_t_init(&files[i]);
    if (fs_open(&files[i], path, FS_O_READ)) {
        in_use[i] = false;
        return NULL;
    }
    return &files[i];
}

size_t los_file_read(void *f, void *buf, size_t n)
{
    ssize_t got = fs_read(f, buf, n);
    return got > 0 ? (size_t)got : 0;
}

bool los_file_seek(void *f, uint64_t pos)
{
    return fs_seek(f, (off_t)pos, FS_SEEK_SET) == 0;
}

uint64_t los_file_size(void *f)
{
    off_t at = fs_tell(f);
    if (fs_seek(f, 0, FS_SEEK_END))
        return 0;
    off_t size = fs_tell(f);
    fs_seek(f, at, FS_SEEK_SET);
    return size > 0 ? (uint64_t)size : 0;
}

void los_file_close(void *f)
{
    fs_close(f);
    in_use[(struct fs_file_t *)f - files] = false;
}

/* ---- writing: only in /.lossless, only once the start-up check passed ---- */

#define WRITE_DIR "/SD:/.lossless"
static bool writes_on;

void los_writes_enable(bool on) { writes_on = on; }
bool los_writes_ok(void) { return writes_on; }

static bool allowed(const char *path)
{
    size_t n = strlen(WRITE_DIR);
    return writes_on && !strncmp(path, WRITE_DIR, n) && (path[n] == '\0' || path[n] == '/') &&
           !strstr(path, "..");
}

static bool write_file(const char *path, const void *data, size_t len, bool append)
{
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | (append ? FS_O_APPEND : 0)))
        return false;
    bool ok = append || fs_truncate(&f, 0) == 0;
    if (ok && len)
        ok = fs_write(&f, data, len) == (ssize_t)len;
    if (ok)
        ok = fs_sync(&f) == 0;
    return fs_close(&f) == 0 && ok;
}

bool los_file_save(const char *path, const void *data, size_t len)
{
    char tmp[200];
    if (!allowed(path) || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return false;
    if (!write_file(tmp, data, len, false))
        return false;
    int err = fs_unlink(path);
    if (err && err != -ENOENT)
        return false;
    return fs_rename(tmp, path) == 0;
}

bool los_file_append(const char *path, const void *data, size_t len)
{
    return allowed(path) && write_file(path, data, len, true);
}

bool los_file_remove(const char *path)
{
    if (!allowed(path))
        return false;
    int err = fs_unlink(path);
    return !err || err == -ENOENT;
}

#ifdef CONFIG_FS_FATFS_HAS_RTC
/* FatFs asks for the time when it writes: the player's clock, packed the
 * FAT way (years from 1980, month, day, hour, minute, seconds / 2). Not
 * set: 1 January 2025, as before. */
#include "calendar.h"
uint32_t get_fattime(void);
uint32_t get_fattime(void)
{
    int64_t now;
    los_date d = { 2025, 1, 1, 0, 0, 0, 0 };
    if (los_clock_get(&now))
        d = date_from_seconds(now);
    if (d.year < 1980 || d.year > 2107)
        d.year = 2025;
    return (uint32_t)(d.year - 1980) << 25 | (uint32_t)d.month << 21 | (uint32_t)d.day << 16 |
           (uint32_t)d.hour << 11 | (uint32_t)d.minute << 5 | (uint32_t)(d.second / 2);
}
#endif

bool los_mkdir(const char *path)
{
    if (!allowed(path))
        return false;
    int err = fs_mkdir(path);
    return !err || err == -EEXIST;
}
#else
void *los_file_open(const char *path) { (void)path; return NULL; }
size_t los_file_read(void *f, void *buf, size_t n) { (void)f; (void)buf; (void)n; return 0; }
bool los_file_seek(void *f, uint64_t pos) { (void)f; (void)pos; return false; }
void los_file_close(void *f) { (void)f; }
uint64_t los_file_size(void *f) { (void)f; return 0; }
void los_writes_enable(bool on) { (void)on; }
bool los_writes_ok(void) { return false; }
bool los_file_save(const char *p, const void *d, size_t n) { (void)p; (void)d; (void)n; return false; }
bool los_file_append(const char *p, const void *d, size_t n) { (void)p; (void)d; (void)n; return false; }
bool los_file_remove(const char *p) { (void)p; return false; }
bool los_mkdir(const char *p) { (void)p; return false; }
#endif

/* ---- thread, lock ------------------------------------------------------------------ */

/* Above the UI (main runs at CONFIG_MAIN_THREAD_PRIORITY, a lower one), so
 * drawing never starves the sound */
#define AUDIO_PRIORITY 2
/* Bluetooth above both: it sends audio packets on time and is asleep
 * between them */
#define BT_PRIORITY 1
K_THREAD_STACK_DEFINE(audio_stack, 49152);   /* libopus keeps its arrays on the stack */
K_THREAD_STACK_DEFINE(bt_stack, 16384);
static struct k_thread audio_thread, bt_thread;
K_MUTEX_DEFINE(los_mutex);

static void thread_entry(void *fn, void *b, void *c)
{
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    ((void (*)(void))fn)();
}

void los_thread_start(enum los_thread which, void (*fn)(void))
{
    if (which == LOS_THREAD_AUDIO) {
        k_thread_create(&audio_thread, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
                        thread_entry, (void *)fn, NULL, NULL, AUDIO_PRIORITY, 0, K_NO_WAIT);
        k_thread_name_set(&audio_thread, "audio");
    } else {
        k_thread_create(&bt_thread, bt_stack, K_THREAD_STACK_SIZEOF(bt_stack),
                        thread_entry, (void *)fn, NULL, NULL, BT_PRIORITY, 0, K_NO_WAIT);
        k_thread_name_set(&bt_thread, "bluetooth");
    }
}

void los_lock(void) { k_mutex_lock(&los_mutex, K_FOREVER); }
void los_unlock(void) { k_mutex_unlock(&los_mutex); }
void los_sleep_ms(int ms) { k_msleep(ms); }

/* ---- silent audio (boards without sound) ------------------------------------------ */

static unsigned silent_rate;
static uint64_t silent_written;
static int64_t silent_start_ms, silent_paused_ms, silent_paused_at;
static bool silent_paused;

__weak bool los_audio_start(unsigned rate, unsigned bits, unsigned channels)
{
    (void)bits;
    (void)channels;
    silent_rate = rate;
    silent_written = 0;
    silent_start_ms = k_uptime_get();
    silent_paused_ms = 0;
    silent_paused = false;
    return true;
}

__weak uint64_t los_audio_played(void)
{
    int64_t now = silent_paused ? silent_paused_at : k_uptime_get();
    uint64_t heard = (uint64_t)(now - silent_start_ms - silent_paused_ms) * silent_rate / 1000;
    return heard < silent_written ? heard : silent_written;
}

__weak size_t los_audio_write(const int32_t *frames, size_t count)
{
    (void)frames;
    if (silent_paused)
        return 0;
    uint64_t cap = silent_rate / 4, ahead = silent_written - los_audio_played();
    size_t room = ahead < cap ? (size_t)(cap - ahead) : 0;
    if (count > room)
        count = room;
    silent_written += count;
    return count;
}

__weak void los_audio_pause(bool paused)
{
    if (paused == silent_paused)
        return;
    silent_paused = paused;
    if (paused)
        silent_paused_at = k_uptime_get();
    else
        silent_paused_ms += k_uptime_get() - silent_paused_at;
}

__weak void los_audio_stop(void)
{
    silent_written = 0;
}

__weak void los_volume_set(int db) { (void)db; }
__weak void los_volume_trim(int half_db) { (void)half_db; }

const char *los_card_path(void) { return "/SD:"; }

__weak void los_backlight(int percent) { (void)percent; }
__weak bool los_battery(int *percent, bool *charging, bool *usb)
{
    (void)percent; (void)charging; (void)usb;
    return false;
}
__weak void los_power_off(void) {}

/* Boards without USB device support */
__weak enum los_usb los_usb_state(void) { return LOS_USB_NONE; }
__weak bool los_usb_share(bool on) { return !on; }
__weak bool los_usb_dac(bool on) { return !on; }
__weak bool los_usb_dac_info(unsigned *rate, unsigned *bits) { (void)rate; (void)bits; return false; }

/* Boards without a clock */
__weak bool los_clock_get(int64_t *seconds) { (void)seconds; return false; }
__weak bool los_clock_set(int64_t seconds) { (void)seconds; return false; }

/* Boards without Bluetooth */
__weak bool los_bt_up(void (*log)(const char *line))
{
    log("no Bluetooth on this board");
    return false;
}
__weak void los_bt_down(void) {}
__weak void los_bt_poll(void) {}
__weak void lbt_platform_send(uint8_t h4_type, const uint8_t *packet, size_t len)
{
    (void)h4_type; (void)packet; (void)len;
}
__weak uint32_t lbt_platform_ms(void) { return k_uptime_get_32(); }
__weak void lbt_platform_log(const char *fmt, ...) { (void)fmt; }

__weak bool los_volume(int *db, int *min_db, int *max_db)
{
    (void)db; (void)min_db; (void)max_db;
    return false;
}
