/*
 * The player's original firmware, read only (stockfw.c): the Bluetooth
 * chip's patch out of its system partition.
 *
 * The chip needs its maker's patch at every start, and its licence lets
 * nobody else hand it out; every player carries one in its original
 * firmware, which installing Lossless OS leaves in place. So the player
 * copies its own (install.c), and nothing needs downloading.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_STOCKFW_H
#define LOS_STOCKFW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The patch into buf (cap bytes): found through the file system's index
 * (well under a second), or, with thorough, by reading the whole
 * partition when that fails (half a minute). Its size, or 0. note (n
 * bytes) says the file's name in the original firmware, or what went
 * wrong. */
size_t stockfw_bt_patch(uint8_t *buf, size_t cap, bool thorough, char *note, size_t n);

/* A patch for the chip: its records (2-byte command, 1-byte length,
 * data) fill it exactly, all "write to the chip's memory" and the last
 * one "start". */
bool stockfw_hcd_ok(const uint8_t *p, size_t n);

#endif
