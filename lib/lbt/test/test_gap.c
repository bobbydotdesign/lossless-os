/*
 * lbt host tests: GAP (set-up, inquiry, connections, pairing).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../gap.h"
#include "../lbt_platform.h"

/* AirPods, as in the recording (address on the wire, least significant first) */
static const uint8_t airpods[6] = { 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa };
static const uint8_t key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static int ready_calls, found_calls, named_calls, connected_calls, secured_calls;
static int disconnected_calls, put_calls, forget_calls;
static bool ready_ok, have_key;
static uint8_t secured_status, connected_status, put_type;
static uint16_t connected_handle;
static char last_name[GAP_NAME_MAX];
static uint8_t stored[16];

static void on_ready(bool ok) { ready_calls++; ready_ok = ok; }
static void on_found(const lbt_addr *a, const char *name, uint32_t cod, int8_t rssi)
{
    (void)a; (void)cod; (void)rssi;
    found_calls++;
    strcpy(last_name, name);
}
static void on_named(const lbt_addr *a, const char *name) { (void)a; named_calls++; strcpy(last_name, name); }
static void on_inquiry_done(void) {}
static void on_connected(uint16_t h, const lbt_addr *a, uint8_t st)
{
    (void)a;
    connected_calls++;
    connected_handle = h;
    connected_status = st;
}
static void on_secured(uint16_t h, uint8_t st) { (void)h; secured_calls++; secured_status = st; }
static void on_disconnected(uint16_t h, uint8_t r) { (void)h; (void)r; disconnected_calls++; }
static bool get_key(const lbt_addr *a, uint8_t k[16]) { (void)a; if (have_key) memcpy(k, key, 16); return have_key; }
static void put_key(const lbt_addr *a, const uint8_t k[16], uint8_t type)
{
    (void)a;
    put_calls++;
    put_type = type;
    memcpy(stored, k, 16);
}
static void forget_key(const lbt_addr *a) { (void)a; forget_calls++; have_key = false; }
static int confirm_calls;
static uint32_t confirm_code;
static bool confirm_passkey;
static void on_confirm(const lbt_addr *a, uint32_t code, bool passkey)
{
    (void)a;
    confirm_calls++;
    confirm_code = code;
    confirm_passkey = passkey;
}

static const gap_ops ops = {
    on_ready, on_found, on_named, on_inquiry_done, on_connected, on_secured,
    on_disconnected, get_key, put_key, forget_key, on_confirm,
};

static void setup(void)
{
    ready_calls = found_calls = named_calls = connected_calls = secured_calls = 0;
    disconnected_calls = put_calls = forget_calls = 0;
    have_key = false;
    gap_init(&ops, "lossless H2", GAP_COD_PORTABLE_AUDIO);
}

/* Answer every command the stack sent with success, in order */
static void complete_all(void)
{
    struct sent_packet *p;
    while ((p = fake_take()) != NULL)
        if (p->type == LBT_H4_COMMAND)
            fake_command_complete(lbt_get16(p->data), 1, (const uint8_t[]){ 0 }, 1);
}

/* Take sent packets until a command with this opcode; NULL if none.
 * Like a controller, it answers each command it passes (Command Status),
 * so the stack can send the next one. */
static struct sent_packet *next_cmd(uint16_t opcode)
{
    struct sent_packet *p;
    while ((p = fake_take()) != NULL) {
        if (p->type != LBT_H4_COMMAND)
            continue;
        uint16_t op = lbt_get16(p->data);
        fake_command_status(op, 0, 1);
        if (op == opcode)
            return p;
    }
    return NULL;
}

TEST(controller_setup)
{
    setup();
    fake_start(1021, 8);        /* HCI is up; now GAP's own set-up */
    gap_start();                /* (runs HCI start again, then set-up) */
    fake_command_complete(HCI_RESET, 1, (const uint8_t[]){ 0 }, 1);
    fake_command_complete(HCI_READ_LOCAL_VERSION, 1, (const uint8_t[]){ 0, 8, 0, 0, 8, 0x0f, 0, 0x22, 9 }, 9);
    fake_command_complete(HCI_READ_BD_ADDR, 1, (const uint8_t[]){ 0, 1, 2, 3, 4, 5, 6 }, 7);
    fake_command_complete(HCI_READ_BUFFER_SIZE, 1, (const uint8_t[]){ 0, 0xfd, 3, 64, 8, 0, 0, 0 }, 8);
    /* skip HCI's four start-up commands */
    for (int i = 0; i < 4; i++)
        fake_take();
    struct sent_packet *p = fake_take();
    CHECK_BYTES(p, 0x01, 0x0c, 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f);  /* event mask */
    fake_command_complete(HCI_SET_EVENT_MASK, 1, (const uint8_t[]){ 0 }, 1);
    CHECK_BYTES(fake_take(), 0x56, 0x0c, 0x01, 0x01);                                   /* SSP on */
    fake_command_complete(HCI_WRITE_SIMPLE_PAIRING_MODE, 1, (const uint8_t[]){ 0 }, 1);
    complete_all();
    for (int i = 0; i < 12 && ready_calls == 0; i++)
        complete_all();
    CHECK(ready_calls == 1 && ready_ok);
    /* class of device and name were written (seen in fake_sent) */
    bool cod = false, name = false;
    for (int i = 0; i < fake_sent_count; i++) {
        if (lbt_get16(fake_sent[i].data) == HCI_WRITE_CLASS_OF_DEVICE)
            cod = lbt_get24(fake_sent[i].data + 3) == GAP_COD_PORTABLE_AUDIO;
        if (lbt_get16(fake_sent[i].data) == HCI_WRITE_LOCAL_NAME)
            name = !memcmp(fake_sent[i].data + 3, "lossless H2", 12);
    }
    CHECK(cod && name);
}

TEST(inquiry_names_from_eir)
{
    setup();
    fake_start(1021, 8);
    CHECK(gap_inquiry(10));
    CHECK_BYTES(next_cmd(HCI_INQUIRY), 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, 0x08, 0x00);
    /* an extended inquiry result with a complete name */
    uint8_t ev[255] = { 1 };
    memcpy(ev + 1, airpods, 6);
    ev[7] = 1;                              /* page scan rep mode */
    lbt_put24(ev + 9, 0x240418);            /* headphones */
    lbt_put16(ev + 12, 0x1234);
    ev[14] = (uint8_t)-60;
    const char nm[] = "Example Headphones";
    ev[15] = (uint8_t)(sizeof(nm));         /* length incl. type byte */
    ev[16] = 0x09;
    memcpy(ev + 17, nm, sizeof(nm) - 1);
    fake_event(HCI_EV_EXTENDED_INQUIRY_RESULT, ev, 255);
    CHECK(found_calls == 1 && !strcmp(last_name, nm));
    /* done: no name lookups needed */
    fake_event(HCI_EV_INQUIRY_COMPLETE, (const uint8_t[]){ 0 }, 1);
    CHECK(next_cmd(HCI_REMOTE_NAME_REQUEST) == NULL);
}

TEST(inquiry_asks_for_missing_names)
{
    setup();
    fake_start(1021, 8);
    gap_inquiry(5);
    fake_command_status(HCI_INQUIRY, 0, 1);
    /* inquiry result with RSSI, no name */
    uint8_t ev[15] = { 1 };
    memcpy(ev + 1, airpods, 6);
    ev[7] = 2;
    lbt_put24(ev + 9, 0x240418);
    lbt_put16(ev + 12, 0x0042);
    ev[14] = (uint8_t)-70;
    fake_event(HCI_EV_INQUIRY_RESULT_WITH_RSSI, ev, 15);
    CHECK(found_calls == 1 && last_name[0] == '\0');
    fake_event(HCI_EV_INQUIRY_COMPLETE, (const uint8_t[]){ 0 }, 1);
    struct sent_packet *p = next_cmd(HCI_REMOTE_NAME_REQUEST);
    CHECK(p && !memcmp(p->data + 3, airpods, 6) && p->data[9] == 2 &&
          lbt_get16(p->data + 11) == (0x0042 | 0x8000));
    uint8_t nm[255] = { 0 };
    memcpy(nm + 1, airpods, 6);
    memcpy(nm + 7, "AirPods", 7);
    fake_event(HCI_EV_REMOTE_NAME_REQUEST_COMPLETE, nm, 255);
    CHECK(named_calls == 1 && !strcmp(last_name, "AirPods"));
}

/* Connect to the AirPods, as in the recording: handle 0x000c */
static void connect_airpods(void)
{
    lbt_addr a;
    memcpy(a.b, airpods, 6);
    CHECK(gap_connect(&a));
    struct sent_packet *p = next_cmd(HCI_CREATE_CONNECTION);
    /* the recording's bytes: address, packet types 0xcc18, ..., role switch */
    CHECK_BYTES(p, 0x05, 0x04, 0x0d, 0x01, 0x00, 0x00, 0xcc, 0xbb, 0xaa,
                0x18, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x01);
    fake_connect(0x000c, airpods);
}

TEST(reconnect_with_stored_key)
{
    setup();
    fake_start(1021, 8);
    connect_airpods();
    CHECK(connected_calls == 1 && connected_handle == 0x000c && connected_status == 0);
    have_key = true;
    CHECK(gap_secure(0x000c));
    CHECK_BYTES(next_cmd(HCI_AUTHENTICATION_REQUESTED), 0x11, 0x04, 0x02, 0x0c, 0x00);
    fake_event(HCI_EV_LINK_KEY_REQUEST, airpods, 6);
    struct sent_packet *p = next_cmd(HCI_LINK_KEY_REQUEST_REPLY);
    CHECK(p && p->len == 3 + 22 && !memcmp(p->data + 3, airpods, 6) && !memcmp(p->data + 9, key, 16));
    fake_event(HCI_EV_AUTHENTICATION_COMPLETE, (const uint8_t[]){ 0, 0x0c, 0 }, 3);
    CHECK_BYTES(next_cmd(HCI_SET_CONNECTION_ENCRYPTION), 0x13, 0x04, 0x03, 0x0c, 0x00, 0x01);
    CHECK(secured_calls == 0);
    fake_event(HCI_EV_ENCRYPTION_CHANGE, (const uint8_t[]){ 0, 0x0c, 0, 2 }, 4);
    CHECK(secured_calls == 1 && secured_status == 0 && gap_is_secure(0x000c));
}

TEST(pair_new_headphones_just_works)
{
    setup();
    fake_start(1021, 8);
    connect_airpods();
    gap_secure(0x000c);
    fake_event(HCI_EV_LINK_KEY_REQUEST, airpods, 6);
    CHECK(next_cmd(HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY) != NULL);
    fake_event(HCI_EV_IO_CAPABILITY_REQUEST, airpods, 6);
    struct sent_packet *p = next_cmd(HCI_IO_CAPABILITY_REQUEST_REPLY);
    CHECK(p && p->data[9] == 0x01 && p->data[10] == 0 && p->data[11] == 0x04);  /* screen, yes / no */
    uint8_t io[9];
    memcpy(io, airpods, 6);
    io[6] = 0x03; io[7] = 0; io[8] = 0x04;                       /* they: no screen, no buttons */
    fake_event(HCI_EV_IO_CAPABILITY_RESPONSE, io, 9);
    uint8_t conf[10];
    memcpy(conf, airpods, 6);
    lbt_put32(conf + 6, 123456);
    fake_event(HCI_EV_USER_CONFIRMATION_REQUEST, conf, 10);
    p = next_cmd(HCI_USER_CONFIRMATION_REQUEST_REPLY);
    CHECK(p && !memcmp(p->data + 3, airpods, 6));
    CHECK(confirm_calls == 0);                                   /* nothing to show: just works */
    uint8_t note[23];
    memcpy(note, airpods, 6);
    memcpy(note + 6, key, 16);
    note[22] = 0x05;   /* authenticated? no: unauthenticated P-256 is 7; any type */
    fake_event(HCI_EV_LINK_KEY_NOTIFICATION, note, 23);
    CHECK(put_calls == 1 && put_type == 0x05 && !memcmp(stored, key, 16));
    fake_event(HCI_EV_AUTHENTICATION_COMPLETE, (const uint8_t[]){ 0, 0x0c, 0 }, 3);
    fake_event(HCI_EV_ENCRYPTION_CHANGE, (const uint8_t[]){ 0, 0x0c, 0, 1 }, 4);
    CHECK(secured_calls == 1 && secured_status == 0);
}

TEST(forgotten_key_pairs_again)
{
    setup();
    fake_start(1021, 8);
    connect_airpods();
    have_key = true;
    gap_secure(0x000c);
    fake_event(HCI_EV_LINK_KEY_REQUEST, airpods, 6);
    CHECK(next_cmd(HCI_LINK_KEY_REQUEST_REPLY) != NULL);
    /* the headphones were reset: key missing */
    fake_event(HCI_EV_AUTHENTICATION_COMPLETE, (const uint8_t[]){ 0x06, 0x0c, 0 }, 3);
    CHECK(forget_calls == 1 && secured_calls == 0);
    CHECK(next_cmd(HCI_AUTHENTICATION_REQUESTED) != NULL);   /* tries again */
    fake_event(HCI_EV_LINK_KEY_REQUEST, airpods, 6);
    CHECK(next_cmd(HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY) != NULL);
    /* and if that fails too, it says so */
    fake_event(HCI_EV_AUTHENTICATION_COMPLETE, (const uint8_t[]){ 0x05, 0x0c, 0 }, 3);
    CHECK(secured_calls == 1 && secured_status == 0x05);
}

TEST(legacy_pin_and_incoming_connection)
{
    setup();
    fake_start(1021, 8);
    uint8_t req[10];
    memcpy(req, airpods, 6);
    lbt_put24(req + 6, 0x240404);
    req[9] = 0x01;
    fake_event(HCI_EV_CONNECTION_REQUEST, req, 10);
    struct sent_packet *p = next_cmd(HCI_ACCEPT_CONNECTION_REQUEST);
    CHECK(p && !memcmp(p->data + 3, airpods, 6) && p->data[9] == 0x01);
    fake_event(HCI_EV_PIN_CODE_REQUEST, airpods, 6);
    p = next_cmd(HCI_PIN_CODE_REQUEST_REPLY);
    CHECK(p && p->data[9] == 4 && !memcmp(p->data + 10, "0000", 4));
}

TEST(failed_connection_reported)
{
    setup();
    fake_start(1021, 8);
    lbt_addr a;
    memcpy(a.b, airpods, 6);
    gap_connect(&a);
    fake_command_status(HCI_CREATE_CONNECTION, 0, 1);
    uint8_t cc[11] = { 0x04 };   /* page timeout */
    memcpy(cc + 3, airpods, 6);
    cc[9] = 1;
    fake_event(HCI_EV_CONNECTION_COMPLETE, cc, 11);
    CHECK(connected_calls == 1 && connected_status == 0x04 && connected_handle == 0);
}

/* Found by searches while asked (a car adds the player from its own
 * screen), reachable either way */
TEST(visible_to_searches)
{
    setup();
    fake_start(1021, 8);
    gap_set_discoverable(true);
    CHECK_BYTES(next_cmd(HCI_WRITE_SCAN_ENABLE), 0x1a, 0x0c, 0x01, 0x03);    /* inquiry + page */
    gap_set_discoverable(false);
    CHECK_BYTES(next_cmd(HCI_WRITE_SCAN_ENABLE), 0x1a, 0x0c, 0x01, 0x02);    /* page only */
    gap_set_connectable(false);
    CHECK_BYTES(next_cmd(HCI_WRITE_SCAN_ENABLE), 0x1a, 0x0c, 0x01, 0x00);
}

/* A car with a screen: both show the code, the user says yes or no */
static const uint8_t car[6] = { 0x19, 0x95, 0x00, 0x11, 0x22, 0x33 };

static void car_asks(uint8_t their_io)
{
    setup();
    confirm_calls = 0;
    fake_start(1021, 8);
    uint8_t io[9];
    memcpy(io, car, 6);
    io[6] = their_io; io[7] = 0; io[8] = 0x05;                   /* MITM wanted */
    fake_event(HCI_EV_IO_CAPABILITY_RESPONSE, io, 9);
    uint8_t conf[10];
    memcpy(conf, car, 6);
    lbt_put32(conf + 6, 42);
    fake_event(HCI_EV_USER_CONFIRMATION_REQUEST, conf, 10);
}

TEST(car_pairing_shows_the_code)
{
    car_asks(0x01);
    CHECK(confirm_calls == 1 && confirm_code == 42 && !confirm_passkey);
    CHECK(next_cmd(HCI_USER_CONFIRMATION_REQUEST_REPLY) == NULL);    /* waits for the user */
    lbt_addr a;
    memcpy(a.b, car, 6);
    gap_confirm(&a, true);
    struct sent_packet *p = next_cmd(HCI_USER_CONFIRMATION_REQUEST_REPLY);
    CHECK(p && !memcmp(p->data + 3, car, 6));
}

TEST(car_pairing_refused_by_the_user)
{
    car_asks(0x01);
    lbt_addr a;
    memcpy(a.b, car, 6);
    gap_confirm(&a, false);
    CHECK(next_cmd(HCI_OP(0x01, 0x002d)) != NULL);                    /* negative reply */
}

TEST(keyboard_types_our_passkey)
{
    setup();
    confirm_calls = 0;
    fake_start(1021, 8);
    uint8_t n[10];
    memcpy(n, car, 6);
    lbt_put32(n + 6, 123456);
    fake_event(0x3b, n, 10);                                     /* passkey notification */
    CHECK(confirm_calls == 1 && confirm_code == 123456 && confirm_passkey);
}

int main(void)
{
    RUN(controller_setup);
    RUN(inquiry_names_from_eir);
    RUN(inquiry_asks_for_missing_names);
    RUN(reconnect_with_stored_key);
    RUN(pair_new_headphones_just_works);
    RUN(forgotten_key_pairs_again);
    RUN(legacy_pin_and_incoming_connection);
    RUN(failed_connection_reported);
    RUN(visible_to_searches);
    RUN(car_pairing_shows_the_code);
    RUN(car_pairing_refused_by_the_user);
    RUN(keyboard_types_our_passkey);
    printf(test_failures ? "\n%d FAILED\n" : "\nall GAP tests passed\n", test_failures);
    return test_failures != 0;
}
