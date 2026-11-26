/*
 * The start log: how long each step of a start took, in
 * /.lossless/logs/start.txt. Marks are kept in memory and written once,
 * about 15 seconds in, so the log doesn't slow the start it measures.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_STARTLOG_H
#define LOS_STARTLOG_H

/* Note that a step finished now (any thread). printf-style. */
void startlog_mark(const char *fmt, ...);

/* Write the marks so far to the card, once (the UI thread) */
void startlog_write(void);

#endif
