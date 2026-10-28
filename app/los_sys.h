/*
 * Lossless OS: what the platform provides to the audio side of the app:
 * files, audio out, a thread. No LVGL here: the audio thread uses these
 * and nothing from the UI. (los.h has the rest.)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_SYS_H
#define LOS_SYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Files for the audio thread (no LVGL there). Paths without a drive letter. */
void *los_file_open(const char *path);
size_t los_file_read(void *file, void *buf, size_t n);
bool los_file_seek(void *file, uint64_t pos);           /* from the start */
void los_file_close(void *file);
uint64_t los_file_size(void *file);

/* Writing (test 4c). Only inside the card's /.lossless folder: any other
 * path is refused. And only once the start-up check has passed
 * (los_writes_ok); before that, and if it failed, everything is refused.
 * Save writes a whole file safely: to name.tmp, synced, then renamed over
 * the old one (a power cut leaves the old file or the new one; loaders
 * should fall back to name.tmp when name is missing). */
bool los_writes_ok(void);
bool los_file_save(const char *path, const void *data, size_t len);
bool los_file_append(const char *path, const void *data, size_t len);
bool los_file_remove(const char *path);
bool los_mkdir(const char *path);

/* Audio out. Samples are interleaved int32 holding 'bits'-bit values, as
 * decoded: the platform sends them on unchanged (bit-perfect), at 'rate'. */
bool los_audio_start(unsigned rate, unsigned bits, unsigned channels);
size_t los_audio_write(const int32_t *frames, size_t count);   /* frames taken, never blocks */
uint64_t los_audio_played(void);   /* frames heard since start */
void los_audio_pause(bool paused);
void los_audio_stop(void);         /* stop now, drop what's queued */

/* The output's volume now and its range, in dB. The platform applies it
 * (on the player, in the DAC: the samples are never scaled). False if
 * there is no volume control. */
bool los_volume(int *db, int *min_db, int *max_db);
void los_volume_set(int db);
/* Make-up gain on top of the volume, in 0.5 dB steps, applied by the
 * output's own volume (the DAC): what the EQ took off to keep from
 * clipping, given back after it. Not part of the volume shown. */
void los_volume_trim(int half_db);

/* The clock: local time in seconds since 1970-01-01 (no time zone).
 * False if it isn't set (it lost power) or there is none. */
bool los_clock_get(int64_t *seconds);
bool los_clock_set(int64_t seconds);

/* The battery: percent (0..100), charging, and whether USB power is
 * there. False if unknown. */
bool los_battery(int *percent, bool *charging, bool *usb);
/* Switch the player off (after the app has saved what it needs) */
void los_power_off(void);

/* USB: the player as a drive on a computer. A charger never gets past
 * LOS_USB_CABLE. LOS_USB_COMPUTER: a computer has set the player up and
 * wants the card; the app stops using the card (nothing playing, settings
 * saved), then los_usb_share(true) unmounts it and hands it over
 * (LOS_USB_SHARED). LOS_USB_EJECTED: the computer has let go of it. When
 * it's ejected or the cable is out (LOS_USB_NONE), los_usb_share(false)
 * takes it back and mounts it again. */
enum los_usb { LOS_USB_NONE, LOS_USB_CABLE, LOS_USB_COMPUTER, LOS_USB_SHARED, LOS_USB_EJECTED,
               LOS_USB_DAC };
enum los_usb los_usb_state(void);
bool los_usb_share(bool on);
/* USB DAC: from LOS_USB_COMPUTER, the player leaves the bus and comes back
 * as a sound card (LOS_USB_DAC until the cable comes out, or until
 * los_usb_dac(false) makes it a drive again). The computer's audio goes
 * to the DAC as it comes (bit-perfect); nothing else may play meanwhile.
 * los_usb_dac_info: audio coming in now, and its rate and depth. */
bool los_usb_dac(bool on);
bool los_usb_dac_info(unsigned *rate, unsigned *bits);

/* The screen's light, in percent of its normal brightness: 100 normal,
 * lower dims it, 0 off (the lock screen dims, then sleeps) */
void los_backlight(int percent);

/* Root of the card, for data files (no trailing slash) */
const char *los_card_path(void);

/* Bluetooth: the controller, with lbt (lib/lbt) on top in bluetooth.c.
 * Up: power, firmware patch, full speed, each step told to 'log'; false
 * if it didn't come up. Poll: hand lbt what the controller sent
 * (hci_receive) and let it run (source_poll); the Bluetooth thread calls it
 * every millisecond or so. The platform also provides lbt_platform_send,
 * _ms and _log (lib/lbt/lbt_platform.h). */
bool los_bt_up(void (*log)(const char *line));
void los_bt_down(void);
void los_bt_poll(void);

/* The app's threads, a lock shared by all of them, and a sleep */
enum los_thread { LOS_THREAD_AUDIO, LOS_THREAD_BLUETOOTH };
void los_thread_start(enum los_thread which, void (*fn)(void));
void los_lock(void);
void los_unlock(void);
void los_sleep_ms(int ms);

#endif
