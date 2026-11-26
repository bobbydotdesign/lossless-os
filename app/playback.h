/*
 * Lossless OS: the playback engine. Decodes on its own thread (so screens
 * and covers can't interrupt the sound) and hands samples to the platform
 * unchanged: bit-perfect, at the file's own rate.
 *
 * The UI calls these from its thread; they only leave requests for the
 * audio thread, so none of them wait on the card.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOS_PLAYBACK_H
#define LOS_PLAYBACK_H

#include <stdbool.h>
#include <stdint.h>

/* Start the audio thread (once) */
void playback_init(void);

/* Play a file (path without a drive letter) from the start */
void playback_play(const char *path);
/* The same, starting ms into the file (resuming after a restart) */
void playback_play_from(const char *path, uint32_t ms);
void playback_pause(bool paused);
void playback_stop(void);

/* Jump to this point of the playing file (to its last moment at most) */
void playback_seek_ms(uint32_t ms);

/* Where the playing file is, in milliseconds heard */
uint32_t playback_position_ms(void);

/* The file to play after this one without a gap (NULL or "" for none).
 * Set it again after each change of song or play mode. */
void playback_set_next(const char *path);
/* True once each time the next file has begun to be heard (gaplessly):
 * the UI moves on to that song, already playing */
bool playback_took_gapless(void);

/* True once each time a file has played to its end */
bool playback_took_end(void);

/* True if the last file couldn't be played (format, damage) */
bool playback_failed(void);
/* No file open on the audio thread (after playback_stop, once it's taken) */
bool playback_idle(void);

#endif
