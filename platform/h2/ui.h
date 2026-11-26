/*
 * What every H2 test screen shares: the theme's colours, the header (mark,
 * "Lossless OS" and the test's name) and the "Stopped" screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_UI_H
#define H2_UI_H

#include "tokens.h"

#define UI (&los_graphite)

#define UI_MARGIN  16
#define UI_TEXT    2              /* dot size of normal text, in pixels */
#define UI_TEXT_H  (8 * UI_TEXT)
#define UI_TOP     56             /* first line below the header */

/* Clear the screen and draw the header with the test's name */
void ui_header(const char *title);

/* Dark red screen: "Stopped", why, and up to 9 lines of detail */
void ui_stopped(const char *why, const char *const *lines, int count);

#endif
