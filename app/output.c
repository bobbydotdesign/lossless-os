/*
 * The output switch (output.h).
 *
 * Switching happens on the audio thread, at its next write: what the old
 * output still had queued is dropped (up to its buffer, a second or so on
 * the DAC), and the new one starts from there.
 *
 * The EQ turns its output down by its highest boost so nothing clips; the
 * DAC's volume gives that back (los_volume_trim), so what isn't boosted
 * stays as loud as before. The music queued for the DAC was made with the
 * old settings, so a change of trim waits until the DAC reaches the first
 * frame made with the new ones.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>
#include "output.h"
#include "resample.h"
#include "eq.h"
#include "los_sys.h"

#define RING_FRAMES 16384               /* about 0.37 s at 44.1 kHz; a power of 2 */
#define PIECE       256                 /* input frames per resampler call */

/* The filter table: the biggest is 147 phases x 320 taps (192 kHz to 44.1) */
static int32_t coef_store[147 * RESAMPLE_MAX_TAPS];

static void *coef_alloc(size_t n) { return n <= sizeof(coef_store) ? coef_store : NULL; }

enum { TO_DAC, TO_BT };

/* Set by the Bluetooth thread, taken by the audio thread (los_lock) */
static int bt_rate_wanted;

/* The audio thread's (read by others under los_lock) */
static int target = TO_DAC;
static bool started, paused;
static unsigned rate, bits, channels;
static unsigned out_bits;               /* to the output: bits as they are, or 24 with EQ */
static uint64_t base;                   /* frames heard on earlier outputs */

/* EQ: asked for by the UI (los_lock), followed on the audio thread */
static struct { bool on; int gains[EQ_BANDS]; unsigned seq; } eq_wanted;
static unsigned eq_seq;
static bool eq_on;
static eq_state eq;
#define EQBUF_FRAMES 2048
static int32_t eqbuf[EQBUF_FRAMES * 2];
static size_t eq_len, eq_used;          /* frames in eqbuf, frames sent of them */

/* The DAC's make-up gain for the EQ (0.5 dB steps), and a change waiting
 * for the DAC to reach the frame it goes with */
static uint64_t dac_out;                /* frames given to the DAC since it started */
static int trim_now = -1, trim_next;
static uint64_t trim_at;
static bool trim_pending;

static int eq_trim(void)
{
    return eq_on && target != TO_BT ? (int)lroundf(eq.headroom_db * 2) : 0;
}

static void trim_set(int half_db)
{
    trim_pending = false;
    if (half_db != trim_now) {
        trim_now = half_db;
        los_volume_trim(half_db);
    }
}

/* Once the DAC has played up to the new settings */
static void trim_follow(void)
{
    if (trim_pending && los_audio_played() >= trim_at)
        trim_set(trim_next);
}

/* Bluetooth */
static int bt_rate;
static resampler rs;
static bool rs_ok;
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t wpos, rpos;    /* frames, counting up; audio / BT thread */
static uint32_t rpos_at_start;
static uint64_t bt_in;                  /* frames given to the resampler */
static volatile bool bt_paused;
static int16_t piece_out[(PIECE * 6 + 2) * 2];

static uint64_t bt_heard(void)
{
    /* all of it, once the headphones have taken everything (exact, so
     * playback sees the end of a song) */
    if (rpos == wpos)
        return bt_in;
    uint32_t got = rpos - rpos_at_start;
    uint64_t heard = bt_rate ? (uint64_t)got * rate / (unsigned)bt_rate : 0;
    return heard < bt_in ? heard : bt_in;
}

static void bt_open(void)
{
    rpos_at_start = wpos = rpos;
    bt_in = 0;
    /* the table lives in coef_store: no freeing, and the same rates as
     * last time reuse it */
    rs_ok = resample_init(&rs, rate, out_bits, channels, (unsigned)bt_rate, coef_alloc);
}

static void bt_close(void)
{
    rs_ok = false;
    wpos = rpos;
}

/* Start on the current target; 'from' frames already heard */
static bool open_target(void)
{
    if (target == TO_BT) {
        bt_paused = paused;
        bt_open();
        return rs_ok;
    }
    if (!los_audio_start(rate, out_bits, channels))
        return false;
    if (paused)
        los_audio_pause(true);
    dac_out = 0;
    trim_now = -1;                     /* the DAC role may have reset it */
    trim_set(eq_trim());               /* nothing queued yet: now */
    return true;
}

static void close_target(void)
{
    if (target == TO_BT)
        bt_close();
    else
        los_audio_stop();
}

static uint64_t target_heard(void)
{
    return target == TO_BT ? bt_heard() : los_audio_played();
}

/* A switch the Bluetooth thread asked for */
static void follow_bluetooth(void)
{
    los_lock();
    int want_rate = bt_rate_wanted;
    los_unlock();
    int want = want_rate > 0 ? TO_BT : TO_DAC;
    if (want == target && (want == TO_DAC || want_rate == bt_rate))
        return;
    if (started) {
        uint64_t heard = target_heard();
        close_target();
        los_lock();
        base += heard;
        target = want;
        bt_rate = want_rate;
        los_unlock();
        open_target();
    } else {
        los_lock();
        target = want;
        bt_rate = want_rate;
        los_unlock();
    }
}

/* An EQ change the UI asked for: new filters, or (on / off) the output
 * started again, since what goes to it changes from as-is to 24 bits */
static void follow_eq(void)
{
    los_lock();
    unsigned seq = eq_wanted.seq;
    bool on = eq_wanted.on;
    int gains[EQ_BANDS];
    memcpy(gains, eq_wanted.gains, sizeof(gains));
    los_unlock();
    if (seq == eq_seq)
        return;
    eq_seq = seq;
    if (on != eq_on && started) {
        uint64_t heard = target_heard();
        close_target();
        los_lock();
        base += heard;
        los_unlock();
        eq_on = on;
        out_bits = on ? 24 : bits;
        eq_len = eq_used = 0;
        if (on)
            eq_setup(&eq, gains, rate, channels);
        open_target();
        return;
    }
    eq_on = on;
    if (on && started) {
        eq_setup(&eq, gains, rate, channels);
        if (target != TO_BT) {
            /* the new trim goes with the first frame made from here on:
             * after what's queued and what the EQ already made */
            trim_next = eq_trim();
            trim_at = dac_out + (eq_len - eq_used);
            trim_pending = true;
        }
    }
}

void output_set_eq(bool on, const int gains[EQ_BANDS])
{
    los_lock();
    eq_wanted.on = on;
    memcpy(eq_wanted.gains, gains, sizeof(eq_wanted.gains));
    eq_wanted.seq++;
    los_unlock();
}

bool output_start(unsigned r, unsigned b, unsigned ch)
{
    follow_bluetooth();
    if (started)
        output_stop();
    eq_seq--;                       /* take the EQ as it is now (not started: no restart) */
    follow_eq();
    rate = r;
    bits = b;
    channels = ch;
    paused = false;
    out_bits = eq_on ? 24 : bits;
    eq_len = eq_used = 0;
    if (eq_on) {
        los_lock();
        int gains[EQ_BANDS];
        memcpy(gains, eq_wanted.gains, sizeof(gains));
        los_unlock();
        eq_setup(&eq, gains, rate, channels);
    }
    los_lock();
    base = 0;
    started = true;
    los_unlock();
    if (!open_target()) {
        started = false;
        return false;
    }
    return true;
}

static size_t bt_write(const int32_t *frames, size_t count)
{
    size_t done = 0;
    if (!rs_ok)
        return 0;
    while (done < count) {
        size_t n = count - done < PIECE ? count - done : PIECE;
        uint32_t room = RING_FRAMES - (wpos - rpos);
        if (room < resample_max_out(&rs, n))
            break;
        size_t made = resample_run(&rs, frames + done * channels, n, piece_out);
        uint32_t w = wpos;
        for (size_t i = 0; i < made; i++, w++) {
            uint32_t at = (w & (RING_FRAMES - 1)) * 2;
            ring[at] = piece_out[i * 2];
            ring[at + 1] = piece_out[i * 2 + 1];
        }
        los_lock();
        bt_in += n;
        los_unlock();
        __atomic_store_n(&wpos, w, __ATOMIC_RELEASE);
        done += n;
    }
    return done;
}

static size_t target_write(const int32_t *frames, size_t count)
{
    if (target == TO_BT)
        return paused ? 0 : bt_write(frames, count);
    size_t took = los_audio_write(frames, count);
    dac_out += took;
    return took;
}

size_t output_write(const int32_t *frames, size_t count)
{
    follow_bluetooth();
    follow_eq();
    trim_follow();
    if (!started)
        return 0;
    if (!eq_on)
        return frames ? target_write(frames, count) : 0;
    /* through the EQ: what the output didn't take yet goes first */
    if (eq_used < eq_len) {
        eq_used += target_write(eqbuf + eq_used * channels, eq_len - eq_used);
        if (eq_used < eq_len)
            return 0;
    }
    if (!frames || !count)
        return 0;
    size_t n = count < EQBUF_FRAMES ? count : EQBUF_FRAMES;
    eq_run(&eq, frames, eqbuf, (unsigned)n, bits);
    eq_len = n;
    eq_used = target_write(eqbuf, n);
    return n;
}

uint64_t output_played(void)
{
    los_lock();
    uint64_t heard = started ? base + target_heard() : 0;
    los_unlock();
    return heard;
}

void output_pause(bool p)
{
    paused = p;
    if (!started)
        return;
    if (target == TO_BT)
        bt_paused = p;
    else
        los_audio_pause(p);
}

void output_stop(void)
{
    if (!started)
        return;
    close_target();
    eq_len = eq_used = 0;
    los_lock();
    started = false;
    base = 0;
    los_unlock();
}

/* ---- the Bluetooth thread ---- */

void output_bluetooth(int r)
{
    los_lock();
    bt_rate_wanted = r;
    los_unlock();
}

bool output_bluetooth_read(int16_t *pcm, int frames)
{
    uint32_t w = __atomic_load_n(&wpos, __ATOMIC_ACQUIRE), r = rpos;
    uint32_t have = w - r;
    if (bt_paused || target != TO_BT || !have) {
        memset(pcm, 0, (size_t)frames * 2 * sizeof(int16_t));
        return false;
    }
    uint32_t n = have < (uint32_t)frames ? have : (uint32_t)frames;
    for (uint32_t i = 0; i < n; i++, r++) {
        uint32_t at = (r & (RING_FRAMES - 1)) * 2;
        pcm[i * 2] = ring[at];
        pcm[i * 2 + 1] = ring[at + 1];
    }
    if (n < (uint32_t)frames)
        memset(pcm + n * 2, 0, (size_t)(frames - (int)n) * 2 * sizeof(int16_t));
    __atomic_store_n(&rpos, r, __ATOMIC_RELEASE);
    return true;
}

bool output_is_bluetooth(void)
{
    los_lock();
    bool bt = bt_rate_wanted > 0;
    los_unlock();
    return bt;
}
