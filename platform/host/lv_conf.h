/*
 * LVGL settings for Lossless OS builds that use this file (the host
 * simulator). Zephyr builds set LVGL up through Kconfig (prj.conf) instead.
 * Anything not set here takes LVGL's default.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Same pixel format as the H2's screen */
#define LV_COLOR_DEPTH 16

#ifdef LOS_HOST
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_DIRECT_EXIT 1
#endif

/* Files: fonts and covers are read through LVGL's "A:" drive */
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0

/* Geist straight from its TTF files, and JPEG covers */
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 1
#define LV_TINY_TTF_CACHE_GLYPH_CNT 256
#define LV_USE_TJPGD 1
#define LV_CACHE_DEF_SIZE (8 * 1024 * 1024)
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 32

/* Screenshots for checking the UI */
#define LV_USE_SNAPSHOT 1

#define LV_USE_FLEX 1
#define LV_FONT_MONTSERRAT_14 1

#endif
