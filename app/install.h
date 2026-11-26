/*
 * Lossless OS installing itself as what our bootloader starts at power-on
 * (bootfile.h), from the release copied to the card (/Lossless/lossless.bin).
 * Nothing here writes the player's flash: our bootloader gets there
 * through the original firmware's recovery (update.upt and Vol+), and
 * comes off the same way, with the factory update file.
 *
 * The rules (docs/install.md):
 * - The boot file lives in /.lossless/boot. Ours is written beside it as
 *   lossless.bin.new, read back and compared, checked, and renamed in last;
 *   the version it replaces stays as lossless.bin.old. A swap cut short is
 *   finished (or undone) at the next start.
 * - The first start, from the release (the bootloader falls back to
 *   /Lossless/lossless.bin while /.lossless/boot has none), installs
 *   without asking. Our bootloader's update.upt goes once the bootloader
 *   in it is the one in the flash (it has done its job); never anyone
 *   else's update file.
 * - Updates (a newer release on the card) ask first, and not below 30%
 *   battery unless on USB power.
 * - Three starts in a row that never got going put the version before back.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_INSTALL_H
#define LOS_INSTALL_H

#include <stdbool.h>
#include "bootfile.h"

enum install_offer { INSTALL_NOTHING, INSTALL_UPDATE };

typedef struct {
    bootfile_info current;   /* /.lossless/boot/lossless.bin */
    bootfile_info previous;  /* its .old: the version before */
    bootfile_info package;   /* /Lossless/lossless.bin */
} install_state;

/* Looks (reads only) */
void install_look(install_state *s);
enum install_offer install_offer(const install_state *s);

/* NULL when done, else what stopped it (for the screen) */
const char *install_run(void);

/* At start, before anything else: a swap cut short is finished or undone,
 * the first start from the release installs, our update.upt goes once its
 * bootloader is in, and the start counter goes up. True if it put the version before back
 * after three failed starts (the screen says so, then the player switches
 * off). */
bool install_at_start(void);
/* Lossless OS got going (a key was pressed, or it ran a while), or is
 * switching off on purpose: the counter goes back to 0 */
void install_started_ok(void);

/* The Bluetooth chip's patch on the card (LOS_BT_PATCH): if it isn't
 * there, or isn't a patch, the player's own is copied from its original
 * firmware (stockfw.h). At start (install_at_start) it tries once, through
 * the file system's index; thorough (turning Bluetooth on) tries every
 * time, and reads the whole partition if it must. True if the card has a
 * patch now. */
bool install_bt_patch(bool thorough);

#endif
