/*
 * What a board adds to Lossless OS on Zephyr. Each hook has a default in
 * platform/zephyr/main.c (an emulator: no card, no buttons); a board's own
 * file replaces the ones it needs (platform/h2/h2_board.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_BOARD_H
#define LOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Before the app: mount the card and anything else the board needs */
void los_board_start(void);

/* Keys for the app, as LVGL keys (LV_KEY_UP, LV_KEY_ENTER, ...). Returns
 * true with *key and *pressed when there is something to report; *more
 * asks to be called again straight away (a queue of wheel steps). */
bool los_board_key(uint32_t *key, bool *pressed, bool *more);

#endif
