/*
 * The H2's screen while the bootloader's LCD set-up is still in charge:
 * we draw into draw_buf and copy it to the frame the LCD controller keeps
 * sending to the panel. Also the screen log (everything Zephyr prints) and
 * the "Stopped" screen for fatal errors.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_SCREEN_H
#define H2_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

/* Physical address of the frame, 0 if it wasn't found */
uint32_t screen_frame(void);

/* Copy draw_buf to the screen */
void screen_show(void);

/* The last two complete lines printed (older first) */
void screen_log_tail(const char **older, const char **newest);

/* The backlight as a plain on/off pin (for when there's no screen) */
void screen_backlight_blink(int times, uint32_t loops);

#endif
