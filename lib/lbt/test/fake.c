/*
 * lbt host tests: the fake controller and the platform functions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include "fake.h"
#include "../lbt_platform.h"
#include "../hci.h"

struct sent_packet fake_sent[FAKE_MAX_SENT];
int fake_sent_count;
static int fake_taken;
uint32_t fake_now_ms;
int test_failures;
static bool verbose_log;

void lbt_platform_send(uint8_t h4_type, const uint8_t *packet, size_t len)
{
    if (fake_sent_count >= FAKE_MAX_SENT) {
        printf("  FAIL: too many packets sent\n");
        test_failures++;
        return;
    }
    struct sent_packet *s = &fake_sent[fake_sent_count++];
    s->type = h4_type;
    s->len = len < sizeof(s->data) ? len : sizeof(s->data);
    memcpy(s->data, packet, s->len);
}

uint32_t lbt_platform_ms(void)
{
    return fake_now_ms;
}

void lbt_platform_log(const char *fmt, ...)
{
    if (!verbose_log && !getenv("LBT_LOG"))
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void fake_reset(void)
{
    fake_sent_count = 0;
    fake_taken = 0;
    fake_now_ms = 1000;
    hci_init();
}

struct sent_packet *fake_take(void)
{
    if (fake_taken >= fake_sent_count)
        return NULL;
    return &fake_sent[fake_taken++];
}

void fake_event(uint8_t code, const uint8_t *params, uint8_t len)
{
    uint8_t buf[2 + 255 + 1];
    buf[0] = LBT_H4_EVENT;
    buf[1] = code;
    buf[2] = len;
    memcpy(buf + 3, params, len);
    hci_receive(buf, 3 + len);
}

void fake_command_complete(uint16_t opcode, uint8_t credits,
                           const uint8_t *ret, uint8_t ret_len)
{
    uint8_t p[3 + 255];
    p[0] = credits;
    lbt_put16(p + 1, opcode);
    memcpy(p + 3, ret, ret_len);
    fake_event(HCI_EV_COMMAND_COMPLETE, p, 3 + ret_len);
}

void fake_command_status(uint16_t opcode, uint8_t status, uint8_t credits)
{
    uint8_t p[4] = { status, credits, 0, 0 };
    lbt_put16(p + 2, opcode);
    fake_event(HCI_EV_COMMAND_STATUS, p, 4);
}

void fake_acl(uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len)
{
    uint8_t buf[5 + 1100];
    buf[0] = LBT_H4_ACL;
    lbt_put16(buf + 1, (handle & 0x0fff) | ((uint16_t)pb << 12));
    lbt_put16(buf + 3, len);
    memcpy(buf + 5, data, len);
    hci_receive(buf, 5 + len);
}

void fake_completed(uint16_t handle, uint16_t count)
{
    uint8_t p[5] = { 1 };
    lbt_put16(p + 1, handle);
    lbt_put16(p + 3, count);
    fake_event(HCI_EV_NUMBER_OF_COMPLETED_PACKETS, p, 5);
}

void fake_connect(uint16_t handle, const uint8_t addr[6])
{
    uint8_t p[11] = { 0 };
    lbt_put16(p + 1, handle);
    memcpy(p + 3, addr, 6);
    p[9] = 0x01;   /* ACL */
    fake_event(HCI_EV_CONNECTION_COMPLETE, p, 11);
}

static bool started_flag;
static void on_started(bool ok) { started_flag = ok; }

void fake_start(uint16_t acl_len, uint16_t acl_count)
{
    static const uint8_t ok[] = { 0x00 };
    static const uint8_t version[] = { 0x00, 0x08, 0x00, 0x00, 0x08, 0x0f, 0x00, 0x22, 0x09 };
    static const uint8_t addr[] = { 0x00, 0x02, 0x00, 0x00, 0xcc, 0xbb, 0xaa };
    uint8_t bufsize[8] = { 0x00 };
    lbt_put16(bufsize + 1, acl_len);
    bufsize[3] = 64;
    lbt_put16(bufsize + 4, acl_count);

    started_flag = false;
    hci_start(on_started);
    fake_command_complete(HCI_RESET, 1, ok, 1);
    fake_command_complete(HCI_READ_LOCAL_VERSION, 1, version, sizeof(version));
    fake_command_complete(HCI_READ_BD_ADDR, 1, addr, sizeof(addr));
    fake_command_complete(HCI_READ_BUFFER_SIZE, 1, bufsize, sizeof(bufsize));
    if (!started_flag) {
        printf("  FAIL: startup didn't finish\n");
        test_failures++;
    }
    fake_sent_count = 0;
    fake_taken = 0;
}
