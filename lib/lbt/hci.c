/*
 * lbt HCI: commands, events and ACL data (Bluetooth Core, Vol 4 Part E,
 * and Vol 4 Part A for the H4 UART transport).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hci.h"
#include "lbt_platform.h"

/* ---- state ------------------------------------------------------------------ */

struct hci_cmd {
    uint16_t opcode;
    uint8_t len;
    uint8_t params[255];
    hci_cmd_done_fn done;
    void *ctx;
};

/* Waiting to be sent, in order */
static struct hci_cmd cmd_queue[HCI_CMD_QUEUE];
static int cmd_head, cmd_count;

/* Sent, waiting for Command Complete or Command Status */
#define HCI_IN_FLIGHT 4
static struct {
    bool used;
    uint16_t opcode;
    hci_cmd_done_fn done;
    void *ctx;
    uint32_t sent_ms;
} in_flight[HCI_IN_FLIGHT];

static int cmd_credits;    /* commands the controller can take now */

struct hci_conn {
    bool used;
    uint16_t handle;
    lbt_addr addr;
    int unacked;           /* fragments sent, not yet reported completed */
    uint16_t rx_len;       /* bytes of the PDU being joined */
    uint16_t rx_expected;  /* its full length (0: none in progress) */
    uint8_t rx[HCI_PDU_MAX];
};
static struct hci_conn conns[HCI_MAX_CONNECTIONS];

/* Outgoing ACL fragments, in order */
static struct {
    uint16_t handle;
    uint16_t len;          /* packet length: 4-byte header + data */
    uint8_t packet[4 + HCI_ACL_MAX];
} tx[HCI_TX_BUFFERS];
static int tx_head, tx_count;

static int acl_credits;    /* controller ACL buffers free */
static int acl_total;      /* how many it has */
static uint16_t acl_len = 0;  /* controller's ACL data length (0: unknown) */

static lbt_addr local_addr;

#define HCI_EVENT_HANDLERS 4
static hci_event_fn event_handlers[HCI_EVENT_HANDLERS];
static hci_pdu_fn pdu_handler;
static hci_ready_fn ready_handler;

/* H4 receive state */
static uint8_t rx_type;
static uint16_t rx_have, rx_need;
static bool rx_header_done;
static uint8_t rx_buf[4 + HCI_PDU_MAX];
static bool rx_discard;

/* ---- helpers ------------------------------------------------------------------ */

static struct hci_conn *find_conn(uint16_t handle)
{
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (conns[i].used && conns[i].handle == handle)
            return &conns[i];
    return NULL;
}

static void notify_ready(void)
{
    if (ready_handler)
        ready_handler();
}

/* ---- commands ------------------------------------------------------------------ */

static void send_commands(void)
{
    while (cmd_credits > 0 && cmd_count > 0) {
        int slot = -1;
        for (int i = 0; i < HCI_IN_FLIGHT; i++)
            if (!in_flight[i].used) {
                slot = i;
                break;
            }
        if (slot < 0)
            return;

        struct hci_cmd *c = &cmd_queue[cmd_head];
        uint8_t packet[3 + 255];
        lbt_put16(packet, c->opcode);
        packet[2] = c->len;
        memcpy(packet + 3, c->params, c->len);

        in_flight[slot].used = true;
        in_flight[slot].opcode = c->opcode;
        in_flight[slot].done = c->done;
        in_flight[slot].ctx = c->ctx;
        in_flight[slot].sent_ms = lbt_platform_ms();

        cmd_head = (cmd_head + 1) % HCI_CMD_QUEUE;
        cmd_count--;
        cmd_credits--;
        lbt_platform_send(LBT_H4_COMMAND, packet, 3 + c->len);
    }
}

bool hci_command(uint16_t opcode, const uint8_t *params, uint8_t len,
                 hci_cmd_done_fn done, void *ctx)
{
    if (cmd_count >= HCI_CMD_QUEUE)
        return false;
    struct hci_cmd *c = &cmd_queue[(cmd_head + cmd_count) % HCI_CMD_QUEUE];
    c->opcode = opcode;
    c->len = len;
    if (len)
        memcpy(c->params, params, len);
    c->done = done;
    c->ctx = ctx;
    cmd_count++;
    send_commands();
    return true;
}

/* The oldest command in flight with this opcode answered */
static void command_answered(uint16_t opcode, uint8_t status,
                             const uint8_t *ret, uint8_t ret_len)
{
    int found = -1;
    for (int i = 0; i < HCI_IN_FLIGHT; i++) {
        if (in_flight[i].used && in_flight[i].opcode == opcode &&
            (found < 0 ||
             (int32_t)(in_flight[i].sent_ms - in_flight[found].sent_ms) < 0))
            found = i;
    }
    if (found < 0)
        return;   /* e.g. the controller's own NOP after reset */
    hci_cmd_done_fn done = in_flight[found].done;
    void *ctx = in_flight[found].ctx;
    in_flight[found].used = false;
    if (done)
        done(opcode, status, ret, ret_len, ctx);
}

void hci_tick(void)
{
    uint32_t now = lbt_platform_ms();
    for (int i = 0; i < HCI_IN_FLIGHT; i++) {
        if (in_flight[i].used &&
            now - in_flight[i].sent_ms > HCI_CMD_TIMEOUT_MS) {
            uint16_t opcode = in_flight[i].opcode;
            hci_cmd_done_fn done = in_flight[i].done;
            void *ctx = in_flight[i].ctx;
            in_flight[i].used = false;
            lbt_platform_log("hci: command %04x timed out", opcode);
            /* assume the controller can take another one */
            if (cmd_credits == 0)
                cmd_credits = 1;
            if (done)
                done(opcode, HCI_STATUS_TIMEOUT, NULL, 0, ctx);
        }
    }
    send_commands();
}

/* ---- ACL out --------------------------------------------------------------------- */

static uint16_t fragment_size(void)
{
    uint16_t n = acl_len ? acl_len : 27;   /* 27: smallest the spec allows */
    return n > HCI_ACL_MAX ? HCI_ACL_MAX : n;
}

int hci_fragments_for(uint16_t len)
{
    uint16_t f = fragment_size();
    return (len + f - 1) / f;
}

int hci_acl_free(int *total)
{
    if (total)
        *total = acl_total;
    return acl_credits;
}

int hci_tx_free(void)
{
    return HCI_TX_BUFFERS - tx_count;
}

static void send_acl(void)
{
    while (acl_credits > 0 && tx_count > 0) {
        struct hci_conn *c = find_conn(tx[tx_head].handle);
        if (c) {
            c->unacked++;
            acl_credits--;
            lbt_platform_send(LBT_H4_ACL, tx[tx_head].packet, tx[tx_head].len);
        }
        /* else the connection went away: drop it */
        tx_head = (tx_head + 1) % HCI_TX_BUFFERS;
        tx_count--;
    }
}

bool hci_send_pdu(uint16_t handle, const uint8_t *pdu, uint16_t len)
{
    if (!find_conn(handle) || len == 0)
        return false;
    uint16_t f = fragment_size();
    int need = (len + f - 1) / f;
    if (need > hci_tx_free())
        return false;

    for (uint16_t off = 0; off < len; off += f) {
        uint16_t n = len - off < f ? len - off : f;
        int slot = (tx_head + tx_count) % HCI_TX_BUFFERS;
        /* packet boundary: 00 first non-flushable fragment, 01 continuing */
        uint16_t pb = off == 0 ? 0x0000 : 0x1000;
        lbt_put16(tx[slot].packet, (handle & 0x0fff) | pb);
        lbt_put16(tx[slot].packet + 2, n);
        memcpy(tx[slot].packet + 4, pdu + off, n);
        tx[slot].handle = handle;
        tx[slot].len = 4 + n;
        tx_count++;
    }
    send_acl();
    return true;
}

/* ---- ACL in ---------------------------------------------------------------------- */

static void acl_received(const uint8_t *packet, uint16_t len)
{
    if (len < 4)
        return;
    uint16_t hdr = lbt_get16(packet);
    uint16_t handle = hdr & 0x0fff;
    uint8_t pb = (hdr >> 12) & 0x3;
    uint16_t dlen = lbt_get16(packet + 2);
    const uint8_t *data = packet + 4;
    struct hci_conn *c = find_conn(handle);

    if (!c || dlen + 4 > len)
        return;

    if (pb == 0x2 || pb == 0x0) {
        /* start of a PDU: its basic header says how long it is */
        c->rx_len = 0;
        c->rx_expected = 0;
        if (dlen < 4)
            return;
        uint16_t total = lbt_get16(data) + 4;
        if (total > HCI_PDU_MAX) {
            lbt_platform_log("hci: dropped a %u-byte PDU (too long)", total);
            return;
        }
        c->rx_expected = total;
    } else if (pb != 0x1 || c->rx_expected == 0) {
        return;   /* continuation without a start */
    }

    if (c->rx_len + dlen > c->rx_expected) {
        lbt_platform_log("hci: PDU longer than its header said; dropped");
        c->rx_expected = 0;
        return;
    }
    memcpy(c->rx + c->rx_len, data, dlen);
    c->rx_len += dlen;
    if (c->rx_len == c->rx_expected) {
        c->rx_expected = 0;
        if (pdu_handler)
            pdu_handler(handle, c->rx, c->rx_len);
    }
}

/* ---- events ---------------------------------------------------------------------- */

static void event_received(const uint8_t *packet, uint16_t len)
{
    if (len < 2)
        return;
    uint8_t code = packet[0];
    uint8_t plen = packet[1];
    const uint8_t *p = packet + 2;
    if (plen + 2 > len)
        return;

    switch (code) {
    case HCI_EV_COMMAND_COMPLETE:
        if (plen >= 3) {
            cmd_credits = p[0];
            uint16_t opcode = lbt_get16(p + 1);
            if (plen >= 4)
                command_answered(opcode, p[3], p + 4, plen - 4);
            else
                command_answered(opcode, 0, NULL, 0);
        }
        break;

    case HCI_EV_COMMAND_STATUS:
        if (plen >= 4) {
            cmd_credits = p[1];
            command_answered(lbt_get16(p + 2), p[0], NULL, 0);
        }
        break;

    case HCI_EV_NUMBER_OF_COMPLETED_PACKETS:
        if (plen >= 1) {
            int n = p[0];
            for (int i = 0; i < n && 1 + 4 * (i + 1) <= plen; i++) {
                uint16_t handle = lbt_get16(p + 1 + 4 * i) & 0x0fff;
                uint16_t done = lbt_get16(p + 3 + 4 * i);
                struct hci_conn *c = find_conn(handle);
                if (c) {
                    if (done > c->unacked)
                        done = c->unacked;
                    c->unacked -= done;
                    acl_credits += done;
                }
            }
            send_acl();
            notify_ready();
        }
        break;

    case HCI_EV_CONNECTION_COMPLETE:
        /* status, handle, address, link type (1 = ACL), encryption */
        if (plen >= 11 && p[0] == 0 && p[9] == 0x01) {
            for (int i = 0; i < HCI_MAX_CONNECTIONS; i++) {
                if (!conns[i].used) {
                    memset(&conns[i], 0, sizeof(conns[i]));
                    conns[i].used = true;
                    conns[i].handle = lbt_get16(p + 1) & 0x0fff;
                    memcpy(conns[i].addr.b, p + 3, 6);
                    break;
                }
            }
        }
        break;

    case HCI_EV_DISCONNECTION_COMPLETE:
        if (plen >= 4 && p[0] == 0) {
            struct hci_conn *c = find_conn(lbt_get16(p + 1) & 0x0fff);
            if (c) {
                /* its packets in the controller are gone: credits back */
                acl_credits += c->unacked;
                c->used = false;
            }
            send_acl();
        }
        break;

    case HCI_EV_HARDWARE_ERROR:
        lbt_platform_log("hci: controller hardware error 0x%02x",
                         plen ? p[0] : 0);
        break;

    default:
        break;
    }

    for (int i = 0; i < HCI_EVENT_HANDLERS; i++)
        if (event_handlers[i])
            event_handlers[i](code, p, plen);

    send_commands();
}

/* ---- H4 framing --------------------------------------------------------------------- */

static hci_trace_fn trace;

void hci_set_trace(hci_trace_fn fn)
{
    trace = fn;
}

static void packet_received(void)
{
    if (rx_discard)
        return;
    if (trace)
        trace(rx_type, rx_buf, (uint16_t)rx_have);
    if (rx_type == LBT_H4_EVENT)
        event_received(rx_buf, rx_have);
    else if (rx_type == LBT_H4_ACL)
        acl_received(rx_buf, rx_have);
}

void hci_receive(const uint8_t *data, size_t len)
{
    while (len) {
        if (rx_type == 0) {
            rx_type = *data++;
            len--;
            rx_have = 0;
            rx_header_done = false;
            rx_discard = false;
            if (rx_type == LBT_H4_EVENT)
                rx_need = 2;
            else if (rx_type == LBT_H4_ACL)
                rx_need = 4;
            else if (rx_type == LBT_H4_SCO)
                rx_need = 3;
            else {
                /* lost sync: skip bytes until a packet type we know */
                lbt_platform_log("hci: unexpected byte 0x%02x", rx_type);
                rx_type = 0;
            }
            continue;
        }

        size_t take = rx_need - rx_have;
        if (take > len)
            take = len;
        if (!rx_discard)
            memcpy(rx_buf + rx_have, data, take);
        rx_have += take;
        data += take;
        len -= take;
        if (rx_have < rx_need)
            continue;

        if (!rx_header_done) {
            rx_header_done = true;
            uint16_t payload;
            if (rx_type == LBT_H4_EVENT)
                payload = rx_buf[1];
            else if (rx_type == LBT_H4_ACL)
                payload = lbt_get16(rx_buf + 2);
            else
                payload = rx_buf[2];
            rx_need += payload;
            if (rx_need > sizeof(rx_buf) || rx_type == LBT_H4_SCO)
                rx_discard = true;   /* read past it */
            if (payload)
                continue;
        }
        packet_received();
        rx_type = 0;
    }
}

/* ---- startup ---------------------------------------------------------------------- */

static hci_started_fn started_cb;
static bool start_ok;

static void start_step(uint16_t opcode, uint8_t status, const uint8_t *ret,
                       uint8_t ret_len, void *ctx);

static void start_next(int step)
{
    static const uint16_t steps[] = {
        HCI_RESET, HCI_READ_LOCAL_VERSION, HCI_READ_BD_ADDR,
        HCI_READ_BUFFER_SIZE,
    };
    if (step >= (int)LBT_ARRAYLEN(steps)) {
        if (started_cb)
            started_cb(start_ok);
        return;
    }
    hci_command(steps[step], NULL, 0, start_step, (void *)(intptr_t)(step + 1));
}

static void start_step(uint16_t opcode, uint8_t status, const uint8_t *ret,
                       uint8_t ret_len, void *ctx)
{
    if (status != 0) {
        lbt_platform_log("hci: startup command %04x failed (0x%02x)",
                         opcode, status);
        start_ok = false;
    }
    switch (opcode) {
    case HCI_RESET:
        /* the controller forgets everything: so do we */
        acl_credits = 0;
        break;
    case HCI_READ_LOCAL_VERSION:
        if (ret_len >= 8)
            lbt_platform_log("hci: version %u.%04x, LMP %u.%04x, maker %u",
                             ret[0], lbt_get16(ret + 1), ret[3],
                             lbt_get16(ret + 6), lbt_get16(ret + 4));
        break;
    case HCI_READ_BD_ADDR:
        if (ret_len >= 6)
            memcpy(local_addr.b, ret, 6);
        break;
    case HCI_READ_BUFFER_SIZE:
        /* ACL length (2), SCO length (1), ACL count (2), SCO count (2) */
        if (ret_len >= 7) {
            acl_len = lbt_get16(ret);
            acl_credits = lbt_get16(ret + 3);
            acl_total = acl_credits;
            lbt_platform_log("hci: controller takes %u ACL packets of %u bytes",
                             acl_credits, acl_len);
        }
        break;
    default:
        break;
    }
    start_next((int)(intptr_t)ctx);
}

void hci_start(hci_started_fn started)
{
    started_cb = started;
    start_ok = true;
    start_next(0);
}

/* ---- setup ---------------------------------------------------------------------- */

void hci_init(void)
{
    memset(in_flight, 0, sizeof(in_flight));
    memset(conns, 0, sizeof(conns));
    memset(event_handlers, 0, sizeof(event_handlers));
    cmd_head = cmd_count = 0;
    tx_head = tx_count = 0;
    cmd_credits = 1;
    acl_credits = 0;
    acl_len = 0;
    rx_type = 0;
    pdu_handler = NULL;
    ready_handler = NULL;
    started_cb = NULL;
}

bool hci_add_event_handler(hci_event_fn fn)
{
    for (int i = 0; i < HCI_EVENT_HANDLERS; i++)
        if (!event_handlers[i]) {
            event_handlers[i] = fn;
            return true;
        }
    return false;
}

void hci_set_pdu_handler(hci_pdu_fn fn)
{
    pdu_handler = fn;
}

void hci_set_ready_handler(hci_ready_fn fn)
{
    ready_handler = fn;
}

const lbt_addr *hci_local_addr(void)
{
    return &local_addr;
}

bool hci_is_connected(uint16_t handle)
{
    return find_conn(handle) != NULL;
}

bool hci_connection_addr(uint16_t handle, lbt_addr *addr)
{
    struct hci_conn *c = find_conn(handle);
    if (!c)
        return false;
    *addr = c->addr;
    return true;
}
