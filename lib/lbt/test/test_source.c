/*
 * lbt host tests: the whole stack through the source layer, against the
 * simulated AirPods (sim.c). Everyday use: first sound, buttons, pausing,
 * pairing from scratch, walking out of range, the case, an hour of music.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sim.h"
#include "../source.h"
#include "../avrcp.h"

/* ---- a pretend player ----------------------------------------------------------------- */

static struct {
    bool playing;
    int pauses, resumes, nexts, previous, volume_steps;
    int volume_reports, firsts;
    uint8_t volume;
    bool controls;
    enum source_state state;
    char message[64];
    int remembered;
    bool have_key;
    uint8_t key[16];
    int keys_stored, keys_forgotten;
    int found;
    char found_name[32];
    lbt_addr found_addr;
    bool audio_on;
    int audio_begins, audio_ends, rate;
    int16_t phase;
} pl;

static void on_state(enum source_state st, const char *msg)
{
    pl.state = st;
    strncpy(pl.message, msg, sizeof(pl.message) - 1);
}
static void on_found(const lbt_addr *a, const char *name, uint32_t cod, int8_t rssi)
{
    (void)cod; (void)rssi;
    pl.found++;
    pl.found_addr = *a;
    if (name[0])
        strncpy(pl.found_name, name, sizeof(pl.found_name) - 1);
}
static bool get_key(const lbt_addr *a, uint8_t k[16])
{
    (void)a;
    if (pl.have_key)
        memcpy(k, pl.key, 16);
    return pl.have_key;
}
static void put_key(const lbt_addr *a, const uint8_t k[16], uint8_t type)
{
    (void)a; (void)type;
    pl.have_key = true;
    memcpy(pl.key, k, 16);
    pl.keys_stored++;
}
static void forget_key(const lbt_addr *a) { (void)a; pl.have_key = false; pl.keys_forgotten++; }
static void remember(const lbt_addr *a, const char *name) { (void)a; (void)name; pl.remembered++; }
static void audio_begin(int rate) { pl.audio_on = true; pl.audio_begins++; pl.rate = rate; }
static void audio_end(void) { pl.audio_on = false; pl.audio_ends++; }
static bool audio_read(int16_t *pcm, int frames)
{
    if (!pl.playing)
        return false;
    for (int i = 0; i < frames; i++) {   /* a quiet saw tooth */
        pl.phase = (int16_t)(pl.phase + 300);
        pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(pl.phase / 4);
    }
    return true;
}
static uint8_t play_status(void) { return pl.playing ? SOURCE_PLAYING : SOURCE_PAUSED; }
static void action(enum source_action a)
{
    switch (a) {
    case SOURCE_PAUSE:    pl.playing = false; pl.pauses++; break;
    case SOURCE_RESUME:   pl.playing = true; pl.resumes++; break;
    case SOURCE_NEXT:     pl.nexts++; break;
    case SOURCE_PREVIOUS: pl.previous++; break;
    default:              pl.volume_steps++; break;
    }
}
static void on_volume(uint8_t v, bool first)
{
    pl.volume_reports++;
    if (first) {
        pl.firsts++;
        source_set_volume(pl.volume);   /* the player's volume wins */
    } else {
        pl.volume = v;
    }
}
static void on_controls(bool up) { pl.controls = up; }

static const source_ops ops = {
    on_state, on_found, get_key, put_key, forget_key, remember, audio_begin, audio_end,
    audio_read, play_status, action, on_volume, on_controls,
    NULL,                                  /* simulated AirPods: no screen, no code */
    NULL,                                  /* nor a screen for the song */
    NULL, NULL,                            /* nor its cover */
};

/* ---- set-ups ---------------------------------------------------------------------------- */

static void fresh(void)
{
    sim_reset();
    memset(&pl, 0, sizeof(pl));
    pl.volume = 60;
    source_init(&ops, "lossless H2");
}

/* The H2 today: paired with the AirPods, both remember the key */
static void paired(void)
{
    fresh();
    pl.have_key = hp.has_key = true;
    for (int i = 0; i < 16; i++)
        pl.key[i] = hp.key[i] = (uint8_t)(0xa0 + i);
}

static void power_on(bool with_last)
{
    source_start(with_last ? &hp.addr : NULL, "AirPods Pro");
}

/* Power on, reconnect and play: the path from the first test on the H2 */
static void listening(void)
{
    paired();
    power_on(true);
    sim_run(3000);
    pl.playing = true;
    sim_run(2000);
}

/* ---- tests ---------------------------------------------------------------------------- */

TEST(first_sound_like_the_h2)
{
    paired();
    power_on(true);
    sim_run(3000);
    CHECK(hp.connections == 1 && hp.encrypted && hp.pairings == 0);
    CHECK(pl.state == SOURCE_CONNECTED && pl.remembered == 1);
    CHECK(pl.audio_on && pl.rate == 44100);
    CHECK(pl.controls && pl.firsts == 1 && hp.volume == pl.volume);
    CHECK(hp.sig_channels == 1 && hp.open && !hp.streaming);   /* nothing playing yet */

    pl.playing = true;
    sim_run(10000);
    CHECK(pl.state == SOURCE_STREAMING && hp.streaming);
    CHECK(hp.player_status == SOURCE_PLAYING);
    const source_info *i = source_get_info();
    CHECK(i->bitpool == 53 && i->frame_bytes == 119 && i->frames_per_packet == 8);
    /* about 10 s of audio, all of it heard */
    CHECK(hp.samples > 44100 * 9 && hp.samples < 44100 * 11);
    CHECK(hp.underruns == 0 && hp.seq_errors == 0 && hp.ts_errors == 0 && hp.bad_frames == 0);
    CHECK(sim_flow_errors == 0);
}

TEST(airpods_play_press_while_playing_pauses)
{
    listening();
    sim_press(AVRCP_OP_PLAY);   /* what the AirPods sent on the H2 */
    sim_run(500);
    CHECK(!pl.playing && pl.pauses == 1);
    sim_press(AVRCP_OP_PLAY);
    sim_run(500);
    CHECK(pl.playing && pl.resumes == 1);
    sim_press(AVRCP_OP_PAUSE);
    sim_run(500);
    CHECK(!pl.playing && pl.pauses == 2);
    sim_press(AVRCP_OP_PAUSE);  /* "pause" while paused: they mean play */
    sim_run(500);
    CHECK(pl.playing && pl.resumes == 2);
    sim_press(AVRCP_OP_FORWARD);
    sim_press(AVRCP_OP_BACKWARD);
    sim_run(500);
    CHECK(pl.nexts == 1 && pl.previous == 1);
    sim_press(AVRCP_OP_VOLUME_UP);   /* they have absolute volume: ignored */
    sim_run(500);
    CHECK(pl.volume_steps == 0);
}

TEST(play_right_after_connecting_is_a_resume)
{
    paired();
    pl.playing = true;
    power_on(true);
    sim_run(2000);   /* controls just came up */
    CHECK(pl.controls);
    sim_press(AVRCP_OP_PLAY);
    sim_run(300);
    CHECK(pl.playing && pl.pauses == 0);
}

TEST(volume_both_ways)
{
    listening();
    sim_swipe_volume(100);
    sim_run(300);
    CHECK(pl.volume == 100 && pl.firsts == 1);
    sim_swipe_volume(90);
    sim_run(300);
    CHECK(pl.volume == 90);
    source_set_volume(40);
    sim_run(300);
    CHECK(hp.volume == 40);
}

TEST(pause_on_the_player_then_play)
{
    listening();
    pl.playing = false;
    sim_run(5000);   /* a short pause: the stream stays up */
    CHECK(hp.streaming && hp.player_status == SOURCE_PAUSED);
    pl.playing = true;
    sim_run(2000);
    CHECK(hp.streaming && hp.underruns == 0);

    pl.playing = false;
    sim_run(SOURCE_IDLE_MS + 2000);   /* a long one: suspended */
    CHECK(!hp.streaming && pl.state == SOURCE_CONNECTED);
    CHECK(pl.pauses == 0);           /* our own suspend doesn't touch the player */
    uint32_t before = hp.samples;
    pl.playing = true;
    sim_run(3000);
    CHECK(hp.streaming && pl.state == SOURCE_STREAMING);
    CHECK(hp.samples - before > 44100 * 2);
    CHECK(hp.underruns == 0);
}

TEST(airpod_out_pauses_the_player)
{
    listening();
    sim_suspend();
    sim_run(300);
    CHECK(!pl.playing && pl.pauses == 1 && !hp.streaming);
    pl.playing = true;   /* back in: they press play, or the player does */
    sim_run(1000);
    CHECK(hp.streaming);
}

TEST(pair_new_headphones_from_scratch)
{
    fresh();
    hp.pairing_mode = true;
    power_on(false);
    sim_run(1000);
    CHECK(pl.state == SOURCE_READY && sim_pages == 0);
    CHECK(source_scan(5));
    sim_run(3000);
    CHECK(pl.found >= 1 && !strcmp(pl.found_name, "AirPods Pro"));
    source_connect(&pl.found_addr, pl.found_name);
    sim_run(3000);
    CHECK(hp.pairings == 1 && pl.keys_stored == 1 && !memcmp(pl.key, hp.key, 16));
    CHECK(pl.state == SOURCE_CONNECTED && pl.remembered == 1);
    pl.playing = true;
    sim_run(3000);
    CHECK(hp.streaming && hp.samples > 44100);

    /* power off and on: back with the new key, no pairing */
    hp.pairing_mode = false;
    source_stop();
    sim_run(500);
    CHECK(!hp.connected && !pl.playing);   /* turning Bluetooth off pauses */
    power_on(true);
    pl.playing = true;
    sim_run(4000);
    CHECK(hp.pairings == 1 && hp.connections == 2 && hp.streaming);
}

/* Just after start-up the player is still looking for the last headphones
 * (gone, here) when a search is asked for: it must still find the new
 * ones, and stop looking for the old */
TEST(search_while_looking_for_the_last_headphones)
{
    fresh();
    hp.pairing_mode = true;
    lbt_addr gone = { { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } };
    source_start(&gone, "Old headphones");
    sim_run(1500);
    CHECK(pl.state == SOURCE_CONNECTING && sim_pages >= 1);
    CHECK(source_scan(5));
    sim_run(15000);
    CHECK(pl.found >= 1 && !strcmp(pl.found_name, "AirPods Pro"));
    int pages = sim_pages;
    sim_run(30000);
    CHECK(sim_pages == pages);                 /* not looking for the old ones */
    source_connect(&pl.found_addr, pl.found_name);
    sim_run(3000);
    CHECK(hp.pairings == 1 && pl.state == SOURCE_CONNECTED);
}

TEST(headphones_forgot_us)
{
    paired();
    hp.has_key = false;      /* reset, or paired with something else since */
    hp.pairing_mode = true;
    power_on(true);
    sim_run(4000);
    CHECK(pl.keys_forgotten == 1 && pl.keys_stored == 1 && hp.pairings == 1);
    CHECK(pl.state == SOURCE_CONNECTED);
}

TEST(headphones_forgot_us_and_wont_pair)
{
    paired();
    hp.has_key = false;
    hp.pairing_mode = false;
    hp.key[0] ^= 1;          /* they have someone else's key */
    hp.has_key = true;
    power_on(true);
    sim_run(4000);
    /* Bluetooth itself is fine: ready, with the reason */
    CHECK(pl.state == SOURCE_READY && !hp.connected);
    CHECK(strstr(pl.message, "Pairing failed") != NULL);
}

/* The H2, 27 Oct: found the AirPods after 5 minutes of looking, but they
 * hung up mid set-up ("low resources": a phone had them). Try again. */
TEST(airpods_busy_with_the_phone_then_free)
{
    paired();
    hp.busy_drops = 1;
    pl.playing = true;
    power_on(true);
    sim_run(20000);
    CHECK(hp.connections == 2);
    CHECK(pl.state == SOURCE_STREAMING && hp.streaming);
}

/* ...and if they stay busy, stop after a few tries, Bluetooth still on */
TEST(airpods_stay_busy)
{
    paired();
    hp.busy_drops = 100;
    power_on(true);
    sim_run(120000);
    CHECK(hp.connections == 1 + SOURCE_SETUP_TRIES);
    CHECK(pl.state == SOURCE_READY && !hp.connected);
    CHECK(strstr(pl.message, "in use") != NULL);
}

TEST(walk_out_of_range_and_back)
{
    listening();
    sim_out_of_range();
    sim_run(6000);
    CHECK(!hp.connected && !pl.playing && pl.pauses == 1);   /* paused, not playing to nobody */
    CHECK(pl.state == SOURCE_CONNECTING && !pl.audio_on);
    int pages = sim_pages;
    sim_run(40000);          /* away for a while: it keeps looking */
    CHECK(sim_pages > pages && !hp.connected);
    sim_back_in_range();
    sim_run(20000);          /* within a try or so */
    CHECK(hp.connected && pl.state == SOURCE_CONNECTED && pl.audio_on);
    pl.playing = true;
    sim_run(2000);
    CHECK(hp.streaming && sim_flow_errors == 0);
}

TEST(gone_for_good_stops_looking)
{
    listening();
    sim_out_of_range();
    sim_run(SOURCE_SEEK_MS + 30000);
    CHECK(pl.state == SOURCE_READY && !strcmp(pl.message, "Headphones not found"));
    int pages = sim_pages;
    sim_run(60000);
    CHECK(sim_pages == pages);
}

TEST(airpods_in_the_case_at_power_on)
{
    paired();
    hp.awake = false;
    power_on(true);
    sim_run(60000);          /* a minute later they come out */
    CHECK(!hp.connected && pl.state == SOURCE_CONNECTING);
    sim_case_open();
    sim_run(20000);
    CHECK(hp.connected && pl.state == SOURCE_CONNECTED);
}

TEST(case_closed_then_opened)
{
    listening();
    sim_case_close();
    sim_run(1000);
    CHECK(!pl.playing && pl.pauses == 1 && pl.state == SOURCE_READY);
    int pages = sim_pages;
    sim_run(30000);
    CHECK(sim_pages == pages);   /* they hung up on purpose: no chasing */
    /* the AirPods connect to the player themselves when the case opens */
    hp.page_player = true;
    sim_case_open();
    sim_run(4000);
    CHECK(hp.connected && pl.state == SOURCE_CONNECTED && hp.sig_channels == 2);
}

TEST(headphones_connect_and_set_up_audio)
{
    paired();
    hp.awake = false;
    power_on(false);         /* not looking for anyone */
    sim_run(1000);
    hp.page_player = true;
    hp.lead_audio = true;
    sim_case_open();
    sim_run(4000);
    CHECK(hp.connected && hp.open && pl.state == SOURCE_CONNECTED);
    CHECK(hp.sig_channels == 1);   /* theirs only: we didn't open a second */
    pl.playing = true;
    sim_run(2000);
    CHECK(hp.streaming && hp.samples > 0);
}

TEST(disconnect_from_the_menu)
{
    listening();
    source_disconnect();
    sim_run(2000);
    CHECK(!hp.connected && pl.state == SOURCE_READY && !pl.audio_on);
    int pages = sim_pages;
    sim_run(30000);
    CHECK(sim_pages == pages);
}

TEST(bluetooth_off_and_on_three_times)
{
    listening();
    for (int n = 0; n < 3; n++) {
        source_stop();
        sim_run(500);
        CHECK(!hp.connected && pl.state == SOURCE_OFF && !pl.audio_on && !pl.playing);
        power_on(true);
        pl.playing = true;
        sim_run(4000);
        CHECK(hp.streaming && pl.state == SOURCE_STREAMING);
    }
    CHECK(hp.pairings == 0 && sim_flow_errors == 0);
}

TEST(an_hour_of_music)
{
    listening();
    uint32_t start = hp.samples;
    for (int minute = 0; minute < 60; minute++) {
        for (int s = 0; s < 60; s++) {
            if (s % 7 == 0)
                sim_stall(120);   /* the player busy now and then (disk, UI) */
            sim_run(1000);
        }
    }
    uint32_t got = hp.samples - start;
    uint32_t want = 44100u * 3600u;
    CHECK(got > want - want / 1000 && got < want + want / 1000);
    CHECK(hp.underruns == 0 && hp.seq_errors == 0 && hp.ts_errors == 0);
    CHECK(hp.buffered_max < 44100 / 2);   /* latency stays low */
    CHECK(sim_flow_errors == 0 && hp.bad_frames == 0);
    printf("  %u samples in an hour (%+d), buffer %d..%d ms\n", got, (int)(got - want),
           hp.buffered_min * 1000 / 44100, hp.buffered_max * 1000 / 44100);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        sim_only = argv[1];
    RUN(first_sound_like_the_h2);
    RUN(airpods_play_press_while_playing_pauses);
    RUN(play_right_after_connecting_is_a_resume);
    RUN(volume_both_ways);
    RUN(pause_on_the_player_then_play);
    RUN(airpod_out_pauses_the_player);
    RUN(pair_new_headphones_from_scratch);
    RUN(search_while_looking_for_the_last_headphones);
    RUN(headphones_forgot_us);
    RUN(headphones_forgot_us_and_wont_pair);
    RUN(airpods_busy_with_the_phone_then_free);
    RUN(airpods_stay_busy);
    RUN(walk_out_of_range_and_back);
    RUN(gone_for_good_stops_looking);
    RUN(airpods_in_the_case_at_power_on);
    RUN(case_closed_then_opened);
    RUN(headphones_connect_and_set_up_audio);
    RUN(disconnect_from_the_menu);
    RUN(bluetooth_off_and_on_three_times);
    RUN(an_hour_of_music);
    printf(test_failures ? "\n%d FAILED\n" : "\nall source tests passed\n", test_failures);
    return test_failures != 0;
}
