/*
 * lbt host tests: replay a real session recorded on the H2 (AirPods Pro 3,
 * traces/airpods-pro3-session1.pklg) into HCI and L2CAP.
 *
 * Everything the controller sent (events and incoming ACL data) is fed
 * back in odd-sized chunks. Checks: every event reaches the handlers,
 * framing never loses sync, and every whole incoming L2CAP PDU comes out
 * of the reassembly intact. ACL packets longer than 96 bytes were cut
 * when recorded, so those (and fragments after them) are skipped.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake.h"
#include "../hci.h"
#include "../l2cap.h"
#include "../lbt_platform.h"

#define TRACE "traces/airpods-pro3-session1.pklg"

static int events_seen, pdus_seen, signal_pdus;
static void on_event(uint8_t code, const uint8_t *p, uint8_t len)
{
    (void)code; (void)p; (void)len;
    events_seen++;
}

static void opened(l2cap_chan *c, uint16_t r) { (void)c; (void)r; }
static void data(l2cap_chan *c, const uint8_t *d, uint16_t l) { (void)c; (void)d; (void)l; }
static void closed(l2cap_chan *c) { (void)c; }
static const l2cap_ops ops = { opened, data, closed };

/* count PDUs on their way from HCI to L2CAP */
static hci_pdu_fn l2cap_pdu;
static uint16_t last_pdu_cid;
static void count_pdu(uint16_t handle, const uint8_t *pdu, uint16_t len)
{
    pdus_seen++;
    last_pdu_cid = lbt_get16(pdu + 2);
    if (last_pdu_cid == 0x0001)
        signal_pdus++;
    CHECK(lbt_get16(pdu) + 4 == len);   /* length matches its header */
    l2cap_pdu(handle, pdu, len);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* Feed bytes in chunks of 1, 2, 3 ... 7 to exercise the framing */
static void feed(const uint8_t *data, size_t len)
{
    static size_t chunk = 1;
    while (len) {
        size_t n = chunk < len ? chunk : len;
        hci_receive(data, n);
        data += n;
        len -= n;
        chunk = chunk % 7 + 1;
    }
}

TEST(replay_airpods_session)
{
    FILE *f = fopen(TRACE, "rb");
    if (!f) {
        printf("  (skipped: %s not found)\n", TRACE);
        return;
    }
    static uint8_t buf[1 << 20];
    size_t size = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    fake_start(1021, 8);
    l2cap_init();
    l2cap_listen(L2CAP_PSM_SDP, &ops, 672);
    l2cap_listen(L2CAP_PSM_AVCTP, &ops, 672);
    l2cap_listen(L2CAP_PSM_AVDTP, &ops, 1000);
    events_seen = pdus_seen = signal_pdus = 0;
    hci_add_event_handler(on_event);
    /* sit between HCI and L2CAP to count whole PDUs */
    extern void l2cap_pdu_received_for_tests(uint16_t, const uint8_t *, uint16_t);
    l2cap_pdu = l2cap_pdu_received_for_tests;
    hci_set_pdu_handler(count_pdu);

    int events_fed = 0, whole_pdus = 0, skipped = 0;
    bool skipping = false;
    for (size_t i = 0; i + 13 <= size; ) {
        uint32_t n = be32(buf + i);
        uint8_t type = buf[i + 12];
        const uint8_t *pkt = buf + i + 13;
        uint32_t plen = n - 9;
        if (i + 4 + n > size)
            break;
        i += 4 + n;

        if (type == 1) {                    /* event */
            uint8_t h4 = LBT_H4_EVENT;
            hci_receive(&h4, 1);
            feed(pkt, plen);
            events_fed++;
        } else if (type == 3 && plen >= 4) { /* ACL in */
            uint16_t hdr = lbt_get16(pkt), dlen = lbt_get16(pkt + 2);
            uint8_t pb = (hdr >> 12) & 3;
            bool whole = plen == 4u + dlen;
            if (pb != 1)
                skipping = !whole;
            if (skipping || !whole) {
                skipped++;
                continue;
            }
            if (pb != 1 && dlen >= 4 && lbt_get16(pkt + 4) + 4 == dlen)
                whole_pdus++;   /* a PDU in one fragment */
            uint8_t h4 = LBT_H4_ACL;
            hci_receive(&h4, 1);
            feed(pkt, plen);
        }
    }
    printf("  %d events, %d whole PDUs (%d signaling), %d cut packets skipped\n",
           events_seen, pdus_seen, signal_pdus, skipped);
    CHECK(events_seen == events_fed);
    CHECK(pdus_seen >= whole_pdus);
    CHECK(signal_pdus > 10);
    /* the session ends with the link going away */
    CHECK(!hci_is_connected(0x000c));
}

int main(void)
{
    RUN(replay_airpods_session);
    printf(test_failures ? "\n%d FAILED\n" : "\nreplay test passed\n", test_failures);
    return test_failures != 0;
}
