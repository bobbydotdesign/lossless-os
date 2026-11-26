/*
 * lbt host tests: a fake controller. Records what the stack sends and
 * lets tests feed events and ACL data back.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_TEST_FAKE_H
#define LBT_TEST_FAKE_H

#include <stdio.h>
#include <stdlib.h>
#include "../lbt_util.h"

#define FAKE_MAX_SENT 64

struct sent_packet {
    uint8_t type;
    size_t len;
    uint8_t data[1100];
};

extern struct sent_packet fake_sent[FAKE_MAX_SENT];
extern int fake_sent_count;
extern uint32_t fake_now_ms;
extern int test_failures;

void fake_reset(void);
/* Feed an event: code, then parameters */
void fake_event(uint8_t code, const uint8_t *params, uint8_t len);
void fake_command_complete(uint16_t opcode, uint8_t credits,
                           const uint8_t *ret, uint8_t ret_len);
void fake_command_status(uint16_t opcode, uint8_t status, uint8_t credits);
void fake_acl(uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len);
void fake_completed(uint16_t handle, uint16_t count);
void fake_connect(uint16_t handle, const uint8_t addr[6]);
/* Run hci_start() with a controller that takes 'acl_count' packets of
 * 'acl_len' bytes */
void fake_start(uint16_t acl_len, uint16_t acl_count);

/* Take the oldest sent packet (NULL if none) */
struct sent_packet *fake_take(void);

#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    test_failures++; } } while (0)

#define CHECK_BYTES(pkt, ...) do { \
    const uint8_t want_[] = { __VA_ARGS__ }; \
    struct sent_packet *p_ = (pkt); \
    if (!p_) { printf("  FAIL %s:%d: nothing sent\n", __FILE__, __LINE__); \
        test_failures++; break; } \
    if (p_->len != sizeof(want_) || memcmp(p_->data, want_, sizeof(want_))) { \
        printf("  FAIL %s:%d: sent", __FILE__, __LINE__); \
        for (size_t i_ = 0; i_ < p_->len; i_++) printf(" %02x", p_->data[i_]); \
        printf("\n        wanted"); \
        for (size_t i_ = 0; i_ < sizeof(want_); i_++) printf(" %02x", want_[i_]); \
        printf("\n"); test_failures++; } } while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { printf("%s\n", #name); fake_reset(); name(); } while (0)

#endif
