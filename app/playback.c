/*
 * The playback engine (playback.h).
 *
 * Gapless: the UI says which file comes next. When the playing file's last
 * sample is decoded, the next one is opened and decoding carries straight
 * on into the same output, if it has the same rate, depth and channels (a
 * different rate means the DAC has to change: then the old file plays out
 * and the UI starts the next one, with a short gap). The UI learns the
 * new song began when the output reaches the frame where it starts.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "playback.h"
#include "source.h"
#include "los_sys.h"
#include "output.h"

#define CHUNK_FRAMES 2048

enum request { REQ_NONE, REQ_PLAY, REQ_STOP, REQ_SEEK };

/* Shared with the UI, under los_lock */
static struct {
    enum request req;
    char path[512];
    bool paused;
    bool ended;          /* a file played to its end; the UI takes it */
    bool failed;
    unsigned rate;
    uint64_t seek_frame;    /* REQ_SEEK: where to */
    int64_t base;           /* the file's frame at the output's start (after a seek) */
    uint32_t start_ms;      /* REQ_PLAY: from here into the file */
    char next_path[512];    /* the file after this one, for gapless; "" none */
    bool idle;              /* the audio thread has no file open */
    bool boundary_set;      /* the next file started at output frame 'boundary' */
    uint64_t boundary;
} shared;

/* The audio thread's own */
static los_source src;
static bool playing;          /* a source is open and feeding */
static bool draining;         /* the source ended; waiting for the last samples */
static bool paused_now;
static uint64_t written;
static char cur_path[512];    /* the file playing, to open again after its end */
static char prev_path[512];   /* the file before a gapless change, until it's heard */
static int32_t chunk[CHUNK_FRAMES * 2];
static size_t chunk_frames, chunk_used;

static void finish(bool ended)
{
    source_close(&src);
    playing = draining = false;
    los_lock();
    if (ended)
        shared.ended = true;
    los_unlock();
}

static void take_requests(void)
{
    char path[512];
    los_lock();
    enum request req = shared.req;
    shared.req = REQ_NONE;
    bool paused = shared.paused;
    if (req == REQ_PLAY)
        memcpy(path, shared.path, sizeof(path));
    uint64_t seek_to = shared.seek_frame;
    uint32_t start_ms = shared.start_ms;
    los_unlock();

    bool back = false;
    if (req != REQ_NONE) {
        los_lock();
        if (req == REQ_SEEK && shared.boundary_set) {
            /* the next file was decoding but isn't heard yet: the seek is in
             * the one before, and the next comes after it again */
            back = true;
            memcpy(shared.next_path, cur_path, sizeof(shared.next_path));
        }
        shared.boundary_set = false;
        los_unlock();
    }
    if (back) {
        memcpy(cur_path, prev_path, sizeof(cur_path));
        if (playing)
            source_close(&src);
        playing = false;
    }
    if (req == REQ_SEEK && cur_path[0]) {
        /* drop what's queued, go there, start the output again */
        if (playing || draining || back)
            output_stop();
        bool ok = playing || source_open(&src, cur_path) == 0;
        ok = ok && source_seek(&src, seek_to) &&
             output_start(src.rate, src.bits, src.channels);
        chunk_frames = chunk_used = 0;
        written = 0;
        playing = ok;
        draining = false;
        paused_now = false;      /* set again below if paused */
        los_lock();
        shared.base = ok ? (int64_t)seek_to : 0;
        shared.ended = false;
        los_unlock();
    }

    if (req == REQ_STOP || req == REQ_PLAY) {
        if (playing || draining)
            output_stop();
        if (playing)
            source_close(&src);
        playing = draining = false;
        chunk_frames = chunk_used = 0;
    }
    if (req == REQ_PLAY) {
        uint64_t from = 0;
        bool ok = source_open(&src, path) == 0;
        if (ok && start_ms) {
            from = (uint64_t)start_ms * src.rate / 1000;
            ok = source_seek(&src, from);
        }
        ok = ok && output_start(src.rate, src.bits, src.channels);
        los_lock();
        shared.failed = !ok;
        shared.rate = ok ? src.rate : 0;
        los_unlock();
        if (ok) {
            playing = true;
            written = 0;
            paused_now = false;
            memcpy(cur_path, path, sizeof(cur_path));
            los_lock();
            shared.base = (int64_t)from;
            los_unlock();
        } else {
            source_close(&src);
        }
    }
    if ((playing || draining) && paused != paused_now) {
        output_pause(paused);
        paused_now = paused;
    }
    los_lock();
    shared.idle = !playing && !draining && shared.req == REQ_NONE;
    los_unlock();
}

/* The playing file ended: open the next one in its place, if there is one
 * and it fits the output as it is. True if it did. */
static bool next_file(void)
{
    char next[512];
    los_lock();
    memcpy(next, shared.next_path, sizeof(next));
    los_unlock();
    if (!next[0])
        return false;
    unsigned rate = src.rate, bits = src.bits, channels = src.channels;
    source_close(&src);
    if (source_open(&src, next) != 0)
        return false;
    if (src.rate != rate || src.bits != bits || src.channels != channels) {
        source_close(&src);           /* the UI starts it, after this one plays out */
        return false;
    }
    memcpy(prev_path, cur_path, sizeof(prev_path));
    memcpy(cur_path, next, sizeof(cur_path));
    los_lock();
    shared.boundary = written;
    shared.boundary_set = true;
    shared.next_path[0] = '\0';     /* the UI says what comes after */
    los_unlock();
    return true;
}

/* One pass: decode a chunk if needed, hand over what the output takes.
 * Returns true if it did something (else the thread naps). */
static bool feed(void)
{
    if (paused_now)
        return false;
    if (draining) {
        output_write(NULL, 0);               /* what the EQ still holds */
        if (output_played() >= written)
            finish(true);
        return false;
    }
    if (!playing)
        return false;
    if (chunk_used >= chunk_frames) {
        int n = source_read(&src, chunk, CHUNK_FRAMES);
        if (n <= 0 && next_file())
            n = source_read(&src, chunk, CHUNK_FRAMES);
        if (n <= 0) {
            source_close(&src);
            playing = false;
            draining = true;
            return false;
        }
        chunk_frames = (size_t)n;
        chunk_used = 0;
    }
    size_t took = output_write(chunk + chunk_used * src.channels, chunk_frames - chunk_used);
    chunk_used += took;
    written += took;
    return took > 0;
}

static void audio_thread(void)
{
    for (;;) {
        take_requests();
        if (!feed())
            los_sleep_ms(5);
    }
}

void playback_init(void)
{
    static bool started;
    if (!started) {
        started = true;
        los_thread_start(LOS_THREAD_AUDIO, audio_thread);
    }
}

void playback_play(const char *path)
{
    playback_play_from(path, 0);
}

void playback_play_from(const char *path, uint32_t ms)
{
    los_lock();
    shared.req = REQ_PLAY;
    shared.start_ms = ms;
    strncpy(shared.path, path, sizeof(shared.path) - 1);
    shared.paused = false;
    shared.ended = false;
    shared.rate = 0;
    shared.boundary_set = false;
    shared.next_path[0] = '\0';
    los_unlock();
}

void playback_pause(bool paused)
{
    los_lock();
    shared.paused = paused;
    los_unlock();
}

void playback_stop(void)
{
    los_lock();
    shared.req = REQ_STOP;
    shared.ended = false;
    shared.boundary_set = false;
    shared.next_path[0] = '\0';
    shared.rate = 0;         /* no position until something plays again */
    los_unlock();
}

uint32_t playback_position_ms(void)
{
    los_lock();
    unsigned rate = shared.rate;
    los_unlock();
    los_lock();
    int64_t base = shared.base;
    bool set = shared.boundary_set;
    uint64_t boundary = shared.boundary;
    los_unlock();
    uint64_t played = output_played();
    int64_t at = set && played >= boundary ? (int64_t)(played - boundary) : base + (int64_t)played;
    return rate && at > 0 ? (uint32_t)((uint64_t)at * 1000 / rate) : 0;
}

void playback_seek_ms(uint32_t ms)
{
    los_lock();
    if (shared.rate) {
        shared.req = REQ_SEEK;
        shared.seek_frame = (uint64_t)ms * shared.rate / 1000;
    }
    los_unlock();
}

void playback_set_next(const char *path)
{
    los_lock();
    /* the next file already started (not heard yet): the UI's idea of what
     * follows is a song behind; it sets it again once it moves on */
    if (!shared.boundary_set)
        snprintf(shared.next_path, sizeof(shared.next_path), "%s", path ? path : "");
    los_unlock();
}

bool playback_took_gapless(void)
{
    los_lock();
    bool set = shared.boundary_set;
    uint64_t boundary = shared.boundary;
    los_unlock();
    if (!set || output_played() < boundary)
        return false;
    los_lock();
    shared.boundary_set = false;
    shared.base = -(int64_t)boundary;     /* positions count from the new song */
    los_unlock();
    return true;
}

bool playback_took_end(void)
{
    los_lock();
    bool e = shared.ended;
    shared.ended = false;
    los_unlock();
    return e;
}

bool playback_idle(void)
{
    los_lock();
    bool idle = shared.idle && shared.req == REQ_NONE;
    los_unlock();
    return idle;
}

bool playback_failed(void)
{
    los_lock();
    bool f = shared.failed;
    los_unlock();
    return f;
}
