/*
 * Lossless OS: what the app needs from the platform, and how the platform
 * starts the app. The app itself (app/) knows nothing about Zephyr, the
 * Mac or any player: each platform (platform/<name>/) implements this.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_API_H
#define LOS_API_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "los_sys.h"

/* ---- provided by the platform ------------------------------------------------ */

/* Folder with the bundled fonts (no trailing slash) */
const char *los_assets_path(void);

/* Where the library looks for music, every folder under it (no trailing
 * slash): the whole card on players */
const char *los_music_path(void);

/* The player's name, as people know it ("HiFi Walker H2") */
const char *los_player_name(void);

/* Bytes used and total on the music card; false if unknown */
bool los_storage(uint64_t *used, uint64_t *total);

/* A tap of the player's lock button (Power on the H2): the app locks and
 * unlocks the keys. Not an LVGL key; passed through the keypad like one. */
#define LOS_KEY_LOCK 0x1000

/* ---- provided by the app ------------------------------------------------------- */

/* The player's volume buttons: louder (+) or quieter (-) by 'db'. The app
 * sends it to the output in use (the DAC, or Bluetooth headphones). */
void los_app_volume(int db);

/* The player is about to switch off: save what needs saving */
void los_app_before_off(void);

/* Build the UI on 'display'. Button presses arrive through 'keypad'
 * (LVGL keys: up, down, left = previous, right = next, enter = play or
 * select, esc = back, home = menu). */
void los_app_start(lv_display_t *display, lv_indev_t *keypad);

#endif
