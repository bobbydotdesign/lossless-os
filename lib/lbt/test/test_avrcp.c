/*
 * lbt host tests: AVRCP, using the AirPods Pro 3's messages from the
 * recording (with our transaction labels).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chan.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../avrcp.h"

static const uint8_t airpods[6] = { 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa };
static int connected_calls, disconnected_calls, button_calls, volume_calls;
static bool connected_ok, last_pressed;
static uint8_t last_op, last_volume, status = AVRCP_PAUSED;

static void on_connected(bool ok) { connected_calls++; connected_ok = ok; }
static void on_button(uint8_t op, bool pressed) { button_calls++; last_op = op; last_pressed = pressed; }
static void on_volume(uint8_t v) { volume_calls++; last_volume = v; }
static void on_disconnected(void) { disconnected_calls++; }
static uint8_t play_status(void) { return status; }
static avrcp_track song;              /* .loaded false: nothing selected */
static void on_track(avrcp_track *t) { *t = song; }
static const avrcp_ops ops = { on_connected, on_button, on_volume, on_disconnected, play_status,
                               on_track };

static uint16_t cid;

static void in(const uint8_t *d, uint16_t len) { chan_in(cid, d, len); }
#define IN(...) in((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

static void setup(void)
{
    fake_start(1021, 8);
    l2cap_init();
    avrcp_init(&ops);
    fake_connect(chan_handle, airpods);
    connected_calls = disconnected_calls = button_calls = volume_calls = 0;
    status = AVRCP_PAUSED;
    memset(&song, 0, sizeof(song));
}

/* Open the control channel and learn they take absolute volume, as recorded */
static void connect_like_the_recording(void)
{
    CHECK(avrcp_connect(chan_handle));
    cid = chan_accept_ours(0x230d);
    CHECK(connected_calls == 1 && connected_ok);
    /* GetCapabilities (events), as in the recording */
    CHECK_BYTES(chan_next_data(), 0x00, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x10, 0x00, 0x00, 0x01, 0x03);
    /* they take events 01, 02 and 0d (volume) */
    IN(0x02, 0x11, 0x0e, 0x0c, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x05,
       0x03, 0x03, 0x01, 0x02, 0x0d);
    CHECK(avrcp_has_absolute_volume());
    /* we register for their volume changes */
    CHECK_BYTES(chan_next_data(), 0x10, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x31, 0x00, 0x00, 0x05, 0x0d, 0x00, 0x00, 0x00, 0x00);
    /* interim: they're at 95 */
    IN(0x12, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x5f);
    CHECK(volume_calls == 1 && last_volume == 95);
}

TEST(volume_swipes_on_the_airpods)
{
    setup();
    connect_like_the_recording();
    /* a swipe down: changed to 87, and we register again with a new label */
    IN(0x12, 0x11, 0x0e, 0x0d, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x57);
    CHECK(volume_calls == 2 && last_volume == 87);
    struct sent_packet *p = chan_next_data();
    CHECK(p && (p->data[0] >> 4) == 2 && p->data[9] == 0x31 && p->data[13] == 0x0d);
    /* their interim for the new registration repeats it */
    IN(0x22, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x57);
    CHECK(volume_calls == 3 && last_volume == 87);
}

TEST(player_volume_to_the_airpods)
{
    setup();
    connect_like_the_recording();
    CHECK(avrcp_set_volume(0x5f));
    CHECK_BYTES(chan_next_data(), 0x20, 0x11, 0x0e, 0x00, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x50, 0x00, 0x00, 0x01, 0x5f);
    /* quick presses while that's out: only the latest goes next */
    CHECK(avrcp_set_volume(0x43));
    CHECK(avrcp_set_volume(0x47));
    CHECK(chan_next_data() == NULL);
    IN(0x22, 0x11, 0x0e, 0x09, 0x48, 0x00, 0x00, 0x19, 0x58, 0x50, 0x00, 0x00, 0x01, 0x5f);
    struct sent_packet *p = chan_next_data();
    CHECK(p && p->data[9] == 0x50 && p->data[13] == 0x47);
}

TEST(buttons_like_the_recording)
{
    setup();
    connect_like_the_recording();
    /* play pressed, then released: we accept each, echoing the operation */
    IN(0x60, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x44, 0x00);
    CHECK_BYTES(chan_next_data(), 0x62, 0x11, 0x0e, 0x09, 0x48, 0x7c, 0x44, 0x00);
    CHECK(button_calls == 1 && last_op == AVRCP_OP_PLAY && last_pressed);
    IN(0x70, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0xc4, 0x00);
    CHECK_BYTES(chan_next_data(), 0x72, 0x11, 0x0e, 0x09, 0x48, 0x7c, 0xc4, 0x00);
    CHECK(button_calls == 2 && last_op == AVRCP_OP_PLAY && !last_pressed);
    /* pause, forward, backward */
    IN(0xa0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x46, 0x00);
    chan_next_data();
    CHECK(last_op == AVRCP_OP_PAUSE);
    IN(0xb0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x4b, 0x00);
    chan_next_data();
    CHECK(last_op == AVRCP_OP_FORWARD);
    IN(0xc0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x4c, 0x00);
    chan_next_data();
    CHECK(last_op == AVRCP_OP_BACKWARD && button_calls == 5);
    /* a button we don't do: not implemented, no callback */
    IN(0xd0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x20, 0x00);   /* "0" key */
    CHECK_BYTES(chan_next_data(), 0xd2, 0x11, 0x0e, 0x08, 0x48, 0x7c, 0x20, 0x00);
    CHECK(button_calls == 5);
}

TEST(airpods_ask_about_us)
{
    setup();
    connect_like_the_recording();
    /* their GetCapabilities: events 01, 02 and 05 (play status, track,
     * position) */
    IN(0x30, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x01, 0x03);
    CHECK_BYTES(chan_next_data(), 0x32, 0x11, 0x0e, 0x0c, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x10, 0x00, 0x00, 0x05, 0x03, 0x03, 0x01, 0x02, 0x05);
    /* they register for our play status: interim "paused" */
    IN(0x40, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x05,
       0x01, 0x00, 0x00, 0x00, 0x00);
    CHECK_BYTES(chan_next_data(), 0x42, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x31, 0x00, 0x00, 0x02, 0x01, 0x02);
    /* we start playing: changed, on their label */
    avrcp_play_status_changed(AVRCP_PLAYING);
    CHECK_BYTES(chan_next_data(), 0x42, 0x11, 0x0e, 0x0d, 0x48, 0x00, 0x00, 0x19, 0x58,
                0x31, 0x00, 0x00, 0x02, 0x01, 0x01);
    /* nothing more until they register again */
    avrcp_play_status_changed(AVRCP_PAUSED);
    CHECK(chan_next_data() == NULL);
    /* play status */
    status = AVRCP_PLAYING;
    IN(0x50, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x30, 0x00, 0x00, 0x00);
    struct sent_packet *p = chan_next_data();
    CHECK(p && p->data[3] == 0x0c && lbt_get16be(p->data + 11) == 9 && p->data[21] == AVRCP_PLAYING);
    /* the song's details: none selected, so none */
    IN(0x60, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00, 0x00, 0x09,
       0, 0, 0, 0, 0, 0, 0, 0, 0);
    p = chan_next_data();
    CHECK(p && p->data[3] == 0x0c && p->data[9] == 0x20 && p->data[13] == 0x00);
    /* unit info */
    IN(0x70, 0x11, 0x0e, 0x01, 0xff, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff);
    p = chan_next_data();
    CHECK(p && p->data[3] == 0x0c && p->data[5] == 0x30 && p->data[7] == 0x48);
}

TEST(headphones_without_absolute_volume)
{
    setup();
    avrcp_connect(chan_handle);
    cid = chan_accept_ours(0x230d);
    chan_next_data();
    IN(0x02, 0x11, 0x0e, 0x0c, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x03,
       0x03, 0x01, 0x01);                         /* play status only */
    CHECK(!avrcp_has_absolute_volume());
    CHECK(chan_next_data() == NULL);               /* no registration */
    CHECK(!avrcp_set_volume(50));
}

TEST(they_open_the_channel_and_close_it)
{
    setup();
    cid = chan_open_theirs(0x0017, 0x0060);
    CHECK(connected_calls == 1 && connected_ok);
    struct sent_packet *p = chan_next_data();      /* we still ask their capabilities */
    CHECK(p && p->data[9] == 0x10);
    /* another profile's id: we say it isn't ours */
    IN(0x80, 0x11, 0x0f, 0x01, 0x48, 0x00);
    CHECK_BYTES(chan_next_data(), 0x83, 0x11, 0x0f);
    /* link drops */
    fake_event(0x05, (const uint8_t[]){ 0, 0x0c, 0, 0x13 }, 4);
    CHECK(disconnected_calls == 1);
}

static void a_song(void);

/* A car opens its own control channel after ours: theirs is used, ours
 * closes, and it reads the song on its own */
TEST(car_uses_its_own_control_channel)
{
    setup();
    connect_like_the_recording();
    a_song();
    uint16_t ours = cid;
    cid = chan_open_theirs(0x0017, 0x0070);
    struct sent_packet *p = chan_next();                          /* ours goes */
    CHECK(p && p->data[4] == 0x06 && lbt_get16(p->data + 10) == ours);
    p = chan_next_data();                                         /* their capabilities, on theirs */
    CHECK(p && p->data[9] == 0x10);
    CHECK(connected_calls == 1 && disconnected_calls == 0);
    IN(0x60, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00, 0x00, 0x09,
       0, 0, 0, 0, 0, 0, 0, 0, 0);
    p = chan_next_data();
    CHECK(p && p->data[9] == 0x20 && p->data[13] == 6);
    /* ours closing tells nobody it's gone */
    chan_signal_in(0x07, p ? 0 : 0, (const uint8_t[]){ 0x0d, 0x23, 0x41, 0x00 }, 4);
    CHECK(disconnected_calls == 0);
}

static void a_song(void)
{
    song.loaded = true;
    song.serial = 7;
    strcpy(song.title, "So What");
    strcpy(song.artist, "Miles Davis");
    strcpy(song.album, "Kind of Blue");
    song.number = 1;
    song.total = 5;
    song.length_ms = 562000;
    song.position_ms = 61000;
}

/* Attribute i of a GetElementAttributes answer: its id, and its text */
static bool attr_at(const struct sent_packet *p, int i, uint32_t *id, char *text)
{
    int at = 14;
    for (int k = 0; k <= i; k++) {
        if ((size_t)at + 8 > (size_t)p->len)
            return false;
        uint16_t n = lbt_get16be(p->data + at + 6);
        if (k == i) {
            *id = lbt_get32be(p->data + at);
            memcpy(text, p->data + at + 8, n);
            text[n] = 0;
            return lbt_get16be(p->data + at + 4) == 0x006a;   /* UTF-8 */
        }
        at += 8 + n;
    }
    return false;
}

/* A car asks for everything about the song playing */
TEST(car_reads_the_song)
{
    setup();
    connect_like_the_recording();
    a_song();
    IN(0x60, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00, 0x00, 0x09,
       0, 0, 0, 0, 0, 0, 0, 0, 0);
    struct sent_packet *p = chan_next_data();
    CHECK(p && p->data[3] == 0x0c && p->data[9] == 0x20 && p->data[13] == 6);
    uint32_t id;
    char text[128];
    CHECK(p && attr_at(p, 0, &id, text) && id == 1 && !strcmp(text, "So What"));
    CHECK(p && attr_at(p, 1, &id, text) && id == 2 && !strcmp(text, "Miles Davis"));
    CHECK(p && attr_at(p, 2, &id, text) && id == 3 && !strcmp(text, "Kind of Blue"));
    CHECK(p && attr_at(p, 3, &id, text) && id == 4 && !strcmp(text, "1"));
    CHECK(p && attr_at(p, 4, &id, text) && id == 5 && !strcmp(text, "5"));
    CHECK(p && attr_at(p, 5, &id, text) && id == 7 && !strcmp(text, "562000"));
    CHECK(p && lbt_get16be(p->data + 11) == p->len - 13);              /* the length is right */
}

/* With a car connected for covers, the cover's handle comes too */
TEST(car_reads_the_cover_handle)
{
    setup();
    connect_like_the_recording();
    a_song();
    strcpy(song.cover, "0000001");
    IN(0x60, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00, 0x00, 0x09,
       0, 0, 0, 0, 0, 0, 0, 0, 0);
    struct sent_packet *p = chan_next_data();
    uint32_t id;
    char text[128];
    CHECK(p && p->data[13] == 7);
    CHECK(p && attr_at(p, 6, &id, text) && id == 8 && !strcmp(text, "0000001"));
}

/* Only some: title and playing time */
TEST(car_reads_some_of_it)
{
    setup();
    connect_like_the_recording();
    a_song();
    IN(0x60, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00, 0x00, 0x11,
       0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 7);
    struct sent_packet *p = chan_next_data();
    uint32_t id;
    char text[128];
    CHECK(p && p->data[13] == 2);
    CHECK(p && attr_at(p, 0, &id, text) && id == 1 && !strcmp(text, "So What"));
    CHECK(p && attr_at(p, 1, &id, text) && id == 7 && !strcmp(text, "562000"));
}

/* Length and position */
TEST(car_reads_where_it_is)
{
    setup();
    connect_like_the_recording();
    a_song();
    status = AVRCP_PLAYING;
    IN(0x50, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x30, 0x00, 0x00, 0x00);
    struct sent_packet *p = chan_next_data();
    CHECK(p && lbt_get32be(p->data + 13) == 562000 && lbt_get32be(p->data + 17) == 61000 &&
          p->data[21] == AVRCP_PLAYING);
}

/* A new song: they hear of it (and of its position), then ask again */
TEST(car_hears_of_a_new_song)
{
    setup();
    connect_like_the_recording();
    a_song();
    IN(0x40, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x05,
       0x02, 0x00, 0x00, 0x00, 0x00);
    struct sent_packet *p = chan_next_data();
    CHECK(p && p->data[3] == 0x0f && p->data[13] == 0x02 && p->data[14] == 0 && p->data[21] == 0);
    avrcp_tick();
    CHECK(chan_next_data() == NULL);                             /* the same song */
    song.serial = 8;
    strcpy(song.title, "Freddie Freeloader");
    avrcp_tick();
    p = chan_next_data();
    CHECK(p && (p->data[0] >> 4) == 4 && p->data[3] == 0x0d && p->data[13] == 0x02);
    avrcp_tick();
    CHECK(chan_next_data() == NULL);                             /* until they register again */
}

/* Where the song is, every second they asked for, while it plays */
TEST(car_follows_the_position)
{
    setup();
    connect_like_the_recording();
    a_song();
    avrcp_play_status_changed(AVRCP_PLAYING);
    IN(0x40, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x05,
       0x05, 0x00, 0x00, 0x00, 0x01);
    struct sent_packet *p = chan_next_data();
    CHECK(p && p->data[3] == 0x0f && p->data[13] == 0x05 && lbt_get32be(p->data + 14) == 61000);
    avrcp_tick();
    CHECK(chan_next_data() == NULL);
    fake_now_ms += 1001;
    song.position_ms = 62000;
    avrcp_tick();
    p = chan_next_data();
    CHECK(p && p->data[3] == 0x0d && p->data[13] == 0x05 && lbt_get32be(p->data + 14) == 62000);
}

int main(void)
{
    RUN(volume_swipes_on_the_airpods);
    RUN(player_volume_to_the_airpods);
    RUN(buttons_like_the_recording);
    RUN(airpods_ask_about_us);
    RUN(headphones_without_absolute_volume);
    RUN(they_open_the_channel_and_close_it);
    RUN(car_reads_the_song);
    RUN(car_uses_its_own_control_channel);
    RUN(car_reads_the_cover_handle);
    RUN(car_reads_some_of_it);
    RUN(car_reads_where_it_is);
    RUN(car_hears_of_a_new_song);
    RUN(car_follows_the_position);
    printf(test_failures ? "\n%d FAILED\n" : "\nall AVRCP tests passed\n", test_failures);
    return test_failures != 0;
}
