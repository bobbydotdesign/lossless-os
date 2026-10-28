/*
 * lbt host tests: HCI.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../lbt_platform.h"

static bool started_ok, started_called;
static void on_started(bool ok) { started_called = true; started_ok = ok; }

TEST(startup_sequence)
{
    started_called = false;
    hci_start(on_started);
    CHECK_BYTES(fake_take(), 0x03, 0x0c, 0x00);            /* Reset */
    CHECK(fake_take() == NULL);                             /* one at a time */
    fake_command_complete(HCI_RESET, 1, (const uint8_t[]){ 0 }, 1);
    CHECK_BYTES(fake_take(), 0x01, 0x10, 0x00);            /* Read Local Version */
    fake_command_complete(HCI_READ_LOCAL_VERSION, 1,
        (const uint8_t[]){ 0, 8, 0, 0, 8, 0x0f, 0, 0x22, 0x09 }, 9);
    CHECK_BYTES(fake_take(), 0x09, 0x10, 0x00);            /* Read BD_ADDR */
    fake_command_complete(HCI_READ_BD_ADDR, 1,
        (const uint8_t[]){ 0, 0x02, 0x00, 0x00, 0xcc, 0xbb, 0xaa }, 7);
    CHECK_BYTES(fake_take(), 0x05, 0x10, 0x00);            /* Read Buffer Size */
    CHECK(!started_called);
    fake_command_complete(HCI_READ_BUFFER_SIZE, 1,
        (const uint8_t[]){ 0, 0xfd, 0x03, 64, 8, 0, 0, 0 }, 8);
    CHECK(started_called && started_ok);
    const lbt_addr *a = hci_local_addr();
    CHECK(a->b[0] == 0x02 && a->b[5] == 0xaa && a->b[1] == 0x00);
    CHECK(fake_sent[0].type == LBT_H4_COMMAND);
}

TEST(startup_reports_failure)
{
    started_called = false;
    hci_start(on_started);
    fake_command_complete(HCI_RESET, 1, (const uint8_t[]){ 0x0c }, 1);
    fake_command_complete(HCI_READ_LOCAL_VERSION, 1, (const uint8_t[]){ 0 }, 1);
    fake_command_complete(HCI_READ_BD_ADDR, 1, (const uint8_t[]){ 0 }, 1);
    fake_command_complete(HCI_READ_BUFFER_SIZE, 1, (const uint8_t[]){ 0 }, 1);
    CHECK(started_called && !started_ok);
}

static int done_calls;
static uint8_t done_status;
static uint16_t done_opcode;
static const uint8_t *done_ret;
static void on_done(uint16_t opcode, uint8_t status, const uint8_t *ret,
                    uint8_t ret_len, void *ctx)
{
    (void)ret_len; (void)ctx;
    done_calls++;
    done_opcode = opcode;
    done_status = status;
    done_ret = ret;
}

TEST(commands_wait_for_credits)
{
    done_calls = 0;
    CHECK(hci_command(HCI_WRITE_SCAN_ENABLE, (const uint8_t[]){ 0x02 }, 1, on_done, NULL));
    CHECK(hci_command(HCI_WRITE_PAGE_TIMEOUT, (const uint8_t[]){ 0x00, 0x20 }, 2, on_done, NULL));
    CHECK_BYTES(fake_take(), 0x1a, 0x0c, 0x01, 0x02);
    CHECK(fake_take() == NULL);          /* waits for a credit */
    fake_command_complete(HCI_WRITE_SCAN_ENABLE, 1, (const uint8_t[]){ 0 }, 1);
    CHECK(done_calls == 1 && done_opcode == HCI_WRITE_SCAN_ENABLE && done_status == 0);
    CHECK_BYTES(fake_take(), 0x18, 0x0c, 0x02, 0x00, 0x20);
    fake_command_complete(HCI_WRITE_PAGE_TIMEOUT, 1, (const uint8_t[]){ 0 }, 1);
    CHECK(done_calls == 2);
}

TEST(command_status_answers_async_commands)
{
    done_calls = 0;
    hci_command(HCI_INQUIRY, (const uint8_t[]){ 0x33, 0x8b, 0x9e, 8, 0 }, 5, on_done, NULL);
    fake_take();
    fake_command_status(HCI_INQUIRY, 0x00, 1);
    CHECK(done_calls == 1 && done_opcode == HCI_INQUIRY && done_ret == NULL);
}

TEST(command_times_out)
{
    done_calls = 0;
    hci_command(HCI_WRITE_SCAN_ENABLE, (const uint8_t[]){ 0x02 }, 1, on_done, NULL);
    hci_command(HCI_WRITE_CLASS_OF_DEVICE, (const uint8_t[]){ 0x1c, 0x04, 0x24 }, 3, on_done, NULL);
    fake_take();
    fake_now_ms += HCI_CMD_TIMEOUT_MS + 1;
    hci_tick();
    CHECK(done_calls == 1 && done_status == HCI_STATUS_TIMEOUT);
    CHECK_BYTES(fake_take(), 0x24, 0x0c, 0x03, 0x1c, 0x04, 0x24);   /* next one goes */
}

TEST(controller_nop_after_reset_is_ignored)
{
    done_calls = 0;
    fake_command_complete(0x0000, 1, NULL, 0);
    CHECK(done_calls == 0);
}

static uint8_t last_event;
static int events_seen;
static void on_event(uint8_t code, const uint8_t *p, uint8_t len)
{
    (void)p; (void)len;
    last_event = code;
    events_seen++;
}

TEST(events_split_across_reads)
{
    events_seen = 0;
    hci_add_event_handler(on_event);
    /* Inquiry Complete, fed one byte at a time */
    const uint8_t bytes[] = { LBT_H4_EVENT, 0x01, 0x01, 0x00 };
    for (size_t i = 0; i < sizeof(bytes); i++)
        hci_receive(&bytes[i], 1);
    CHECK(events_seen == 1 && last_event == HCI_EV_INQUIRY_COMPLETE);
    /* two events in one read */
    const uint8_t two[] = { 4, 0x01, 1, 0, 4, 0x10, 1, 0x42 };
    hci_receive(two, sizeof(two));
    CHECK(events_seen == 3 && last_event == HCI_EV_HARDWARE_ERROR);
}

static const uint8_t airpods[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static int ready_calls;
static void on_ready(void) { ready_calls++; }

TEST(acl_out_split_and_paced_by_credits)
{
    fake_start(27, 2);          /* tiny buffers, two at a time */
    fake_connect(0x0b, airpods);
    hci_set_ready_handler(on_ready);
    ready_calls = 0;

    uint8_t pdu[60];
    for (int i = 0; i < 60; i++)
        pdu[i] = (uint8_t)i;
    CHECK(hci_fragments_for(60) == 3);
    CHECK(hci_send_pdu(0x0b, pdu, 60));

    struct sent_packet *p = fake_take();
    CHECK(p && p->type == LBT_H4_ACL && p->len == 4 + 27);
    CHECK(p && lbt_get16(p->data) == 0x000b);        /* first fragment */
    CHECK(p && lbt_get16(p->data + 2) == 27 && p->data[4] == 0);
    p = fake_take();
    CHECK(p && lbt_get16(p->data) == 0x100b);        /* continuing */
    CHECK(p && p->data[4] == 27);
    CHECK(fake_take() == NULL);                       /* out of credits */

    fake_completed(0x0b, 1);
    p = fake_take();
    CHECK(p && lbt_get16(p->data) == 0x100b && lbt_get16(p->data + 2) == 6);
    CHECK(p && p->data[4] == 54);
    CHECK(ready_calls == 1);
}

TEST(acl_out_refused_when_full)
{
    fake_start(27, 0);          /* no credits: everything queues */
    fake_connect(0x0b, airpods);
    uint8_t pdu[27 * HCI_TX_BUFFERS] = { 0 };
    CHECK(hci_send_pdu(0x0b, pdu, sizeof(pdu)));
    CHECK(hci_tx_free() == 0);
    CHECK(!hci_send_pdu(0x0b, pdu, 1));
    CHECK(!hci_send_pdu(0x0c, pdu, 1));   /* no such connection */
}

static uint8_t got_pdu[HCI_PDU_MAX];
static uint16_t got_len, got_handle;
static int got_count;
static void on_pdu(uint16_t handle, const uint8_t *pdu, uint16_t len)
{
    got_handle = handle;
    got_len = len;
    got_count++;
    memcpy(got_pdu, pdu, len);
}

TEST(acl_in_joined_into_pdus)
{
    fake_start(1021, 8);
    fake_connect(0x0b, airpods);
    hci_set_pdu_handler(on_pdu);
    got_count = 0;

    /* L2CAP PDU of 4 + 10 bytes, arriving as 6 + 8 */
    const uint8_t a[] = { 10, 0, 0x40, 0x00, 1, 2 };
    const uint8_t b[] = { 3, 4, 5, 6, 7, 8, 9, 10 };
    fake_acl(0x0b, 2, a, sizeof(a));
    CHECK(got_count == 0);
    fake_acl(0x0b, 1, b, sizeof(b));
    CHECK(got_count == 1 && got_handle == 0x0b && got_len == 14);
    CHECK(got_pdu[4] == 1 && got_pdu[13] == 10);

    /* a continuation with no start is ignored */
    fake_acl(0x0b, 1, b, sizeof(b));
    CHECK(got_count == 1);
    /* unknown connection: ignored */
    fake_acl(0x0c, 2, a, sizeof(a));
    CHECK(got_count == 1);
}

TEST(disconnect_returns_credits)
{
    fake_start(27, 1);
    fake_connect(0x0b, airpods);
    uint8_t pdu[40] = { 0 };
    CHECK(hci_send_pdu(0x0b, pdu, 40));   /* 2 fragments, 1 credit */
    CHECK(fake_take() != NULL);
    CHECK(fake_take() == NULL);
    fake_event(HCI_EV_DISCONNECTION_COMPLETE, (const uint8_t[]){ 0, 0x0b, 0, 0x13 }, 4);
    CHECK(!hci_is_connected(0x0b));
    /* the queued fragment was dropped, and the credit came back */
    fake_connect(0x0c, airpods);
    CHECK(hci_send_pdu(0x0c, pdu, 10));
    struct sent_packet *p = fake_take();
    CHECK(p && lbt_get16(p->data) == 0x000c);
}

int main(void)
{
    RUN(startup_sequence);
    RUN(startup_reports_failure);
    RUN(commands_wait_for_credits);
    RUN(command_status_answers_async_commands);
    RUN(command_times_out);
    RUN(controller_nop_after_reset_is_ignored);
    RUN(events_split_across_reads);
    RUN(acl_out_split_and_paced_by_credits);
    RUN(acl_out_refused_when_full);
    RUN(acl_in_joined_into_pdus);
    RUN(disconnect_returns_credits);
    printf(test_failures ? "\n%d FAILED\n" : "\nall HCI tests passed\n", test_failures);
    return test_failures != 0;
}
