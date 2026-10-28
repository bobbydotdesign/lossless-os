/*
 * lbt host tests: simulated headphones behind a simulated controller, for
 * whole-stack tests of the source layer.
 *
 * The controller answers commands like the H2's Broadcom chip, holds 8
 * ACL buffers and frees them at radio speed. The headphones behave like
 * the AirPods Pro 3 in the recording: pairing, AVDTP sink, AVRCP buttons
 * and absolute volume. They play what arrives through a jitter buffer, so
 * the tests can count dropouts.
 *
 * Time only moves in sim_run(); the stack's clock ticks in 10 ms steps
 * like the H2's.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_TEST_SIM_H
#define LBT_TEST_SIM_H

#include <stdio.h>
#include <stdlib.h>
#include "../lbt_util.h"

extern int test_failures;
extern uint32_t sim_now;   /* ms */

struct sim_headphones {
    /* set up by the test */
    lbt_addr addr;
    char name[32];
    bool in_range;            /* within radio range */
    bool awake;               /* out of the case */
    bool pairing_mode;        /* answer searches, pair with anyone */
    bool has_key;             /* they remember us */
    uint8_t key[16];
    bool page_player;         /* they connect to the player when they wake */
    bool lead_audio;          /* they set up AVDTP themselves */
    uint8_t volume;           /* 0..127 */
    int busy_drops;           /* hang up this many audio set-ups: busy with a phone */

    /* what happened */
    int pairings;             /* new keys made */
    int connections;
    int sig_channels;         /* AVDTP signaling channels opened */
    bool connected, encrypted, configured, open, streaming;
    uint8_t player_status;    /* last play status the player told them */
    int volume_sets;          /* absolute volume commands from the player */
    uint32_t frames, samples; /* SBC received */
    uint32_t seq_errors, ts_errors, bad_frames;
    /* playback through the jitter buffer */
    bool playing;
    int32_t buffered;         /* samples waiting */
    int32_t buffered_min, buffered_max;
    uint32_t underrun_ms, underruns;
    int rate;
};

extern struct sim_headphones hp;

/* controller side */
extern int sim_flow_errors;       /* ACL sent with no free buffer */
extern int sim_pages;             /* connection attempts by the player */
extern bool sim_page_scan;        /* the player can be reached */

void sim_reset(void);
/* Advance time; the stack is polled every 'poll_ms' (10 like the H2) */
void sim_run(uint32_t ms);
extern uint32_t sim_poll_ms;
/* Hold the stack up for this long once (an SD card write, say) */
void sim_stall(uint32_t ms);

/* things the headphones do */
void sim_press(uint8_t op);           /* a button: press, then release */
void sim_swipe_volume(uint8_t v);     /* their volume changes */
void sim_suspend(void);               /* an AirPod out: they suspend */
void sim_out_of_range(void);          /* the link drops after 5 s */
void sim_back_in_range(void);
void sim_case_close(void);            /* they hang up and sleep */
void sim_case_open(void);

#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    test_failures++; } } while (0)

#define TEST(name) static void name(void)
extern const char *sim_only;   /* run just this test (argv[1]) */
#define RUN(name) do { if (!sim_only || !strcmp(sim_only, #name)) { \
    printf("%s\n", #name); name(); } } while (0)

#endif
