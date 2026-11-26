/*
 * Themes (skins in the code): the player's look. Four come built in
 * (Graphite, Gallery, Backlight and Classic); anyone can add more as small text
 * files in /Themes on the card (docs/themes.md has the format).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_SKINS_H
#define LOS_SKINS_H

#include <stdbool.h>
#include "tokens.h"

/* What a skin may set beyond the palette: the selected row and the title
 * bar (colours, gradients), chevrons, a split Home and Albums as Covers.
 * Unset ones keep the usual look. */
typedef struct {
    bool has_select, has_select_fg, has_header, has_header_fg;
    bool has_select_grad, has_header_grad;
    uint32_t select, select_fg, header, header_fg;
    uint32_t select_grad, header_grad;   /* the bar's bottom colour: a gradient */
    bool chevrons;        /* a > on rows that open something */
    bool split_home;      /* Home and Music: the list on the left, a cover on the right */
    bool albums_covers;   /* Music > Albums opens as Covers */
    bool classic;         /* layout = classic: the thin title bar, short rows and screens of a
                           * classic pocket player */
    bool gallery;         /* layout = gallery: covers first, tinted by the cover in front, and a
                           * menu that slides in from the left */
} los_skin_extra;

#define SKINS_MAX 24
#define SKIN_NAME 24

/* Finds the skins: the built-in ones, then /Themes on the card. Returns how
 * many there are. */
int skins_scan(void);
int skins_count(void);
const char *skin_name(int i);
/* The skin's colours, and whether it's a dark one; false if a card skin
 * can't be read any more (the built-in Graphite is given then) */
bool skin_palette(int i, los_palette *out, bool *dark, los_skin_extra *extra);
/* The skin called 'name' (any case); -1 if there's none */
int skin_find(const char *name);

/* Reads a skin file's text over 'p' (already holding the base colours):
 * "name = #RRGGBB" lines, "dark = yes/no", "#" comments. Returns how many
 * colours it set; *dark is set if the file says. For tests too. */
int skin_parse(const char *text, los_palette *p, bool *dark, bool *said_dark, los_skin_extra *extra);

#endif
