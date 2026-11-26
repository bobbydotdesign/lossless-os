/*
 * The simulator's Bluetooth: lbt against simulated AirPods (lbt's own test
 * controller and headphones, lib/lbt/test/sim.c), in real time. They're in
 * pairing mode, so Search finds them and Connect pairs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <SDL.h>
#include <string.h>
#include "los_sys.h"
#include "../../lib/lbt/test/sim.h"

/* sim.h's test hooks */
int test_failures;
const char *sim_only;

static uint32_t last_ms;

bool los_bt_up(void (*log)(const char *line))
{
    sim_reset();
    strcpy(hp.name, "AirPods Pro (simulated)");
    hp.pairing_mode = true;
    hp.volume = 64;
    last_ms = SDL_GetTicks();
    log("simulated controller and AirPods");
    return true;
}

void los_bt_down(void) {}

void los_bt_poll(void)
{
    uint32_t now = SDL_GetTicks(), ms = now - last_ms;
    if (ms > 200)
        ms = 200;     /* after a pause in the debugger, say */
    if (ms) {
        sim_run(ms);
        last_ms = now;
    }
}
