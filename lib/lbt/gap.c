/*
 * lbt GAP: controller set-up, inquiry, connections and security.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gap.h"
#include "hci.h"
#include "lbt_platform.h"

#define HCI_USER_PASSKEY_REQUEST_NEGATIVE_REPLY HCI_OP(0x01, 0x002f)
#define HCI_WRITE_SECURE_CONNECTIONS_HOST       HCI_OP(0x03, 0x007a)
#define HCI_EV_USER_PASSKEY_REQUEST             0x34
#define HCI_EV_USER_PASSKEY_NOTIFICATION        0x3b
#define HCI_USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY HCI_OP(0x01, 0x002d)

/* IO capabilities (Core Vol 4 Part E 7.7.41) */
#define IO_DISPLAY_ONLY        0x00
#define IO_DISPLAY_YES_NO      0x01
#define IO_KEYBOARD_ONLY       0x02
#define IO_NO_INPUT_NO_OUTPUT  0x03
#define AUTH_GENERAL_BONDING   0x04   /* MITM not ours to require: they may */

#define STATUS_KEY_MISSING     0x06   /* PIN or key missing */
#define STATUS_AUTH_FAILURE    0x05

static const gap_ops *ops;
static char local_name[GAP_NAME_MAX];
static uint32_t local_cod;
static bool connectable;
static bool discoverable;   /* answers searches (inquiry scan) */

/* The pairing under way: what the other side said it can do */
static struct {
    lbt_addr addr;
    uint8_t io;                 /* their IO capability, or 0xff if not heard */
    uint8_t auth;               /* their authentication requirements */
} pairing = { .io = 0xff };

/* ---- devices found by inquiry ------------------------------------------------------ */

static struct found {
    bool used;
    lbt_addr addr;
    uint8_t psrm;           /* page scan repetition mode */
    uint16_t clock_offset;
    bool named;
} found[GAP_MAX_FOUND];

static bool inquiring;
static int name_pending = -1;   /* index into found[] being named */

static struct found *find_found(const lbt_addr *a)
{
    for (int i = 0; i < GAP_MAX_FOUND; i++)
        if (found[i].used && lbt_addr_equal(&found[i].addr, a))
            return &found[i];
    return NULL;
}

static struct found *add_found(const lbt_addr *a)
{
    struct found *f = find_found(a);
    if (f)
        return f;
    for (int i = 0; i < GAP_MAX_FOUND; i++)
        if (!found[i].used) {
            memset(&found[i], 0, sizeof(found[i]));
            found[i].used = true;
            found[i].addr = *a;
            return &found[i];
        }
    return NULL;
}

/* ---- connections ------------------------------------------------------------------- */

static struct conn {
    bool used;
    uint16_t handle;
    lbt_addr addr;
    bool securing;
    bool used_stored_key;
    bool retried;
    bool authenticated;
    bool secure;
} conns[2];

static struct conn *conn_by_handle(uint16_t h)
{
    for (int i = 0; i < 2; i++)
        if (conns[i].used && conns[i].handle == h)
            return &conns[i];
    return NULL;
}

static struct conn *conn_by_addr(const lbt_addr *a)
{
    for (int i = 0; i < 2; i++)
        if (conns[i].used && lbt_addr_equal(&conns[i].addr, a))
            return &conns[i];
    return NULL;
}

/* ---- set-up ------------------------------------------------------------------------ */

static int setup_step;
static bool setup_ok;

static void setup_next(void);

static void setup_done(uint16_t opcode, uint8_t status, const uint8_t *ret,
                       uint8_t ret_len, void *ctx)
{
    (void)ret; (void)ret_len;
    bool essential = (intptr_t)ctx != 0;
    if (status) {
        lbt_platform_log("gap: set-up command %04x failed (0x%02x)%s", opcode,
                         status, essential ? "" : ", carrying on");
        if (essential)
            setup_ok = false;
    }
    setup_next();
}

static void setup_cmd(uint16_t opcode, const uint8_t *p, uint8_t len, bool essential)
{
    hci_command(opcode, p, len, setup_done, (void *)(intptr_t)essential);
}

static void setup_next(void)
{
    uint8_t p[248];
    switch (setup_step++) {
    case 0: {   /* every event, including Secure Simple Pairing's */
        static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f };
        setup_cmd(HCI_SET_EVENT_MASK, mask, 8, true);
        break;
    }
    case 1:
        p[0] = 1;
        setup_cmd(HCI_WRITE_SIMPLE_PAIRING_MODE, p, 1, true);
        break;
    case 2:
        /* keys made with Secure Connections need it on to work again */
        p[0] = 1;
        setup_cmd(HCI_WRITE_SECURE_CONNECTIONS_HOST, p, 1, false);
        break;
    case 3:
        p[0] = 2;   /* extended inquiry results (they carry names) */
        setup_cmd(HCI_WRITE_INQUIRY_MODE, p, 1, false);
        break;
    case 4:
        lbt_put24(p, local_cod);
        setup_cmd(HCI_WRITE_CLASS_OF_DEVICE, p, 3, false);
        break;
    case 5:
        memset(p, 0, 248);
        memcpy(p, local_name, strlen(local_name));
        setup_cmd(HCI_WRITE_LOCAL_NAME, p, 248, false);
        break;
    case 6: {   /* the name in our inquiry response too */
        size_t n = strlen(local_name);
        memset(p, 0, 241);
        p[0] = 0;               /* FEC not required */
        p[1] = (uint8_t)(n + 1);
        p[2] = 0x09;            /* complete local name */
        memcpy(p + 3, local_name, n);
        setup_cmd(HCI_WRITE_EXTENDED_INQUIRY_RESPONSE, p, 241, false);
        break;
    }
    case 7:
        lbt_put16(p, 0x0000);   /* no role switch or sniff by default */
        setup_cmd(HCI_WRITE_DEFAULT_LINK_POLICY, p, 2, false);
        break;
    case 8:
        lbt_put16(p, 0x6000);   /* 15.4 s to reach a device when paging */
        setup_cmd(HCI_WRITE_PAGE_TIMEOUT, p, 2, false);
        break;
    case 9:
        p[0] = (connectable ? 0x02 : 0x00) |   /* page scan: reconnects */
               (discoverable ? 0x01 : 0x00);   /* inquiry scan: found by searches */
        setup_cmd(HCI_WRITE_SCAN_ENABLE, p, 1, true);
        break;
    default:
        if (ops->ready)
            ops->ready(setup_ok);
        break;
    }
}

static void hci_started(bool ok)
{
    if (!ok) {
        if (ops->ready)
            ops->ready(false);
        return;
    }
    setup_step = 0;
    setup_ok = true;
    setup_next();
}

/* ---- names ------------------------------------------------------------------------- */

static void request_next_name(void);

static void name_status(uint16_t opcode, uint8_t status, const uint8_t *ret,
                        uint8_t ret_len, void *ctx)
{
    (void)opcode; (void)ret; (void)ret_len; (void)ctx;
    if (status) {   /* couldn't even ask: move on */
        if (name_pending >= 0)
            found[name_pending].named = true;
        name_pending = -1;
        request_next_name();
    }
}

static void request_next_name(void)
{
    if (name_pending >= 0 || inquiring)
        return;
    for (int i = 0; i < GAP_MAX_FOUND; i++) {
        if (found[i].used && !found[i].named) {
            uint8_t p[10];
            memcpy(p, found[i].addr.b, 6);
            p[6] = found[i].psrm;
            p[7] = 0;
            lbt_put16(p + 8, found[i].clock_offset | 0x8000);
            name_pending = i;
            hci_command(HCI_REMOTE_NAME_REQUEST, p, 10, name_status, NULL);
            return;
        }
    }
}

/* The name inside extended inquiry response data, if any */
static bool eir_name(const uint8_t *eir, int len, char *name)
{
    for (int i = 0; i + 1 < len; ) {
        uint8_t l = eir[i];
        if (l == 0 || i + 1 + l > len)
            break;
        uint8_t type = eir[i + 1];
        if (type == 0x09 || type == 0x08) {
            int n = l - 1 < GAP_NAME_MAX - 1 ? l - 1 : GAP_NAME_MAX - 1;
            memcpy(name, eir + i + 2, n);
            name[n] = '\0';
            return true;
        }
        i += 1 + l;
    }
    return false;
}

static void inquiry_result(const lbt_addr *a, uint8_t psrm, uint32_t cod,
                           uint16_t clock, int8_t rssi, const char *name)
{
    struct found *f = add_found(a);
    if (f) {
        f->psrm = psrm;
        f->clock_offset = clock & 0x7fff;
        if (name && name[0])
            f->named = true;
    }
    if (ops->found)
        ops->found(a, name ? name : "", cod, rssi);
}

/* ---- security ------------------------------------------------------------------------ */

static void auth_requested(struct conn *c)
{
    uint8_t p[2];
    lbt_put16(p, c->handle);
    c->securing = true;
    c->used_stored_key = false;
    hci_command(HCI_AUTHENTICATION_REQUESTED, p, 2, NULL, NULL);
}

static void secure_failed(struct conn *c, uint8_t status)
{
    c->securing = false;
    if (ops->secured)
        ops->secured(c->handle, status);
}

/* ---- events -------------------------------------------------------------------------- */

static void event(uint8_t code, const uint8_t *p, uint8_t len)
{
    lbt_addr a;
    switch (code) {
    case HCI_EV_INQUIRY_RESULT: {
        int n = len ? p[0] : 0;
        for (int i = 0; i < n && 1 + 14 * n <= len; i++) {
            memcpy(a.b, p + 1 + 6 * i, 6);
            uint8_t psrm = p[1 + 6 * n + i];
            uint32_t cod = lbt_get24(p + 1 + 9 * n + 3 * i);
            uint16_t clock = lbt_get16(p + 1 + 12 * n + 2 * i);
            inquiry_result(&a, psrm, cod, clock, 0, NULL);
        }
        break;
    }
    case HCI_EV_INQUIRY_RESULT_WITH_RSSI: {
        int n = len ? p[0] : 0;
        for (int i = 0; i < n && 1 + 14 * n <= len; i++) {
            memcpy(a.b, p + 1 + 6 * i, 6);
            uint8_t psrm = p[1 + 6 * n + i];
            uint32_t cod = lbt_get24(p + 1 + 8 * n + 3 * i);
            uint16_t clock = lbt_get16(p + 1 + 11 * n + 2 * i);
            int8_t rssi = (int8_t)p[1 + 13 * n + i];
            inquiry_result(&a, psrm, cod, clock, rssi, NULL);
        }
        break;
    }
    case HCI_EV_EXTENDED_INQUIRY_RESULT:
        if (len >= 15) {
            char name[GAP_NAME_MAX] = "";
            memcpy(a.b, p + 1, 6);
            eir_name(p + 15, len - 15, name);
            inquiry_result(&a, p[7], lbt_get24(p + 9), lbt_get16(p + 12),
                           (int8_t)p[14], name);
        }
        break;
    case HCI_EV_INQUIRY_COMPLETE:
        inquiring = false;
        if (ops->inquiry_done)
            ops->inquiry_done();
        request_next_name();
        break;
    case HCI_EV_REMOTE_NAME_REQUEST_COMPLETE:
        if (len >= 7) {
            memcpy(a.b, p + 1, 6);
            struct found *f = find_found(&a);
            if (f)
                f->named = true;
            if (p[0] == 0 && ops->named) {
                char name[GAP_NAME_MAX];
                int n = 0;
                while (n < GAP_NAME_MAX - 1 && 7 + n < len && p[7 + n])
                    n++;
                memcpy(name, p + 7, n);
                name[n] = '\0';
                ops->named(&a, name);
            }
            name_pending = -1;
            request_next_name();
        }
        break;

    case HCI_EV_CONNECTION_REQUEST:
        if (len >= 10 && p[9] == 0x01) {   /* ACL */
            uint8_t r[7];
            memcpy(r, p, 6);
            r[6] = 0x01;   /* stay peripheral */
            hci_command(HCI_ACCEPT_CONNECTION_REQUEST, r, 7, NULL, NULL);
        }
        break;
    case HCI_EV_CONNECTION_COMPLETE:
        if (len >= 11 && p[9] == 0x01) {
            uint16_t h = lbt_get16(p + 1) & 0x0fff;
            memcpy(a.b, p + 3, 6);
            if (p[0] == 0) {
                for (int i = 0; i < 2; i++)
                    if (!conns[i].used) {
                        memset(&conns[i], 0, sizeof(conns[i]));
                        conns[i].used = true;
                        conns[i].handle = h;
                        conns[i].addr = a;
                        break;
                    }
            }
            if (ops->connected)
                ops->connected(p[0] == 0 ? h : 0, &a, p[0]);
        }
        break;
    case HCI_EV_DISCONNECTION_COMPLETE:
        if (len >= 4 && p[0] == 0) {
            uint16_t h = lbt_get16(p + 1) & 0x0fff;
            struct conn *c = conn_by_handle(h);
            if (c)
                c->used = false;
            if (ops->disconnected)
                ops->disconnected(h, p[3]);
        }
        break;

    case HCI_EV_LINK_KEY_REQUEST:
        if (len >= 6) {
            uint8_t r[22];
            memcpy(r, p, 6);
            memcpy(a.b, p, 6);
            struct conn *c = conn_by_addr(&a);
            if (ops->get_key && ops->get_key(&a, r + 6)) {
                if (c)
                    c->used_stored_key = true;
                hci_command(HCI_LINK_KEY_REQUEST_REPLY, r, 22, NULL, NULL);
            } else {
                hci_command(HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY, r, 6, NULL, NULL);
            }
        }
        break;
    case HCI_EV_LINK_KEY_NOTIFICATION:
        if (len >= 23 && ops->put_key) {
            memcpy(a.b, p, 6);
            ops->put_key(&a, p + 6, p[22]);
        }
        break;
    case HCI_EV_PIN_CODE_REQUEST:
        if (len >= 6) {   /* old devices: the usual 0000 */
            uint8_t r[23] = { 0 };
            memcpy(r, p, 6);
            r[6] = 4;
            memcpy(r + 7, "0000", 4);
            hci_command(HCI_PIN_CODE_REQUEST_REPLY, r, 23, NULL, NULL);
        }
        break;
    case HCI_EV_IO_CAPABILITY_REQUEST:
        if (len >= 6) {
            uint8_t r[9];
            memcpy(r, p, 6);
            /* a screen and yes / no (Play / Back): with headphones, which
             * have neither, pairing is "just works" as before; with a car
             * or a phone both show a code to confirm, as many of them
             * insist (Core Vol 3 Part C 5.2.2.6) */
            r[6] = IO_DISPLAY_YES_NO;
            r[7] = 0;   /* no out-of-band data */
            r[8] = AUTH_GENERAL_BONDING;
            hci_command(HCI_IO_CAPABILITY_REQUEST_REPLY, r, 9, NULL, NULL);
        }
        break;
    case HCI_EV_IO_CAPABILITY_RESPONSE:
        if (len >= 9) {
            memcpy(pairing.addr.b, p, 6);
            pairing.io = p[6];
            pairing.auth = p[8];
            lbt_platform_log("gap: they pair as io 0x%02x, auth 0x%02x", p[6], p[8]);
        }
        break;
    case HCI_EV_USER_CONFIRMATION_REQUEST:
        if (len >= 10) {
            lbt_addr who;
            memcpy(who.b, p, 6);
            uint32_t code = lbt_get32(p + 6);
            /* a code to compare only if they have a screen too; with
             * nothing to show it on ("just works"), yes at once */
            bool theirs_shows = lbt_addr_equal(&who, &pairing.addr) &&
                                (pairing.io == IO_DISPLAY_YES_NO || pairing.io == IO_DISPLAY_ONLY);
            if (theirs_shows && ops->confirm) {
                lbt_platform_log("gap: code %06u to confirm", (unsigned)(code % 1000000));
                ops->confirm(&who, code % 1000000, false);
            } else {
                hci_command(HCI_USER_CONFIRMATION_REQUEST_REPLY, p, 6, NULL, NULL);
            }
        }
        break;
    case HCI_EV_USER_PASSKEY_NOTIFICATION:
        if (len >= 10 && ops->confirm) {   /* they have a keyboard: they type it */
            lbt_addr who;
            memcpy(who.b, p, 6);
            ops->confirm(&who, lbt_get32(p + 6) % 1000000, true);
        }
        break;
    case HCI_EV_USER_PASSKEY_REQUEST:
        if (len >= 6)   /* no keyboard */
            hci_command(HCI_USER_PASSKEY_REQUEST_NEGATIVE_REPLY, p, 6, NULL, NULL);
        break;
    case HCI_EV_SIMPLE_PAIRING_COMPLETE:
        if (len >= 1 && p[0])
            lbt_platform_log("gap: pairing failed (0x%02x)", p[0]);
        pairing.io = 0xff;
        break;

    case HCI_EV_AUTHENTICATION_COMPLETE:
        if (len >= 3) {
            struct conn *c = conn_by_handle(lbt_get16(p + 1) & 0x0fff);
            if (!c || !c->securing)
                break;
            if (p[0] == 0) {
                uint8_t r[3];
                c->authenticated = true;
                lbt_put16(r, c->handle);
                r[2] = 1;
                hci_command(HCI_SET_CONNECTION_ENCRYPTION, r, 3, NULL, NULL);
            } else if (c->used_stored_key && !c->retried &&
                       (p[0] == STATUS_KEY_MISSING || p[0] == STATUS_AUTH_FAILURE)) {
                /* the headphones forgot us: drop the old key and pair again */
                lbt_platform_log("gap: stored key refused (0x%02x), pairing again", p[0]);
                if (ops->forget_key)
                    ops->forget_key(&c->addr);
                c->retried = true;
                auth_requested(c);
            } else {
                secure_failed(c, p[0]);
            }
        }
        break;
    case HCI_EV_ENCRYPTION_CHANGE:
        if (len >= 4) {
            struct conn *c = conn_by_handle(lbt_get16(p + 1) & 0x0fff);
            if (!c)
                break;
            if (p[0] == 0 && p[3]) {
                c->secure = true;
                if (c->securing) {
                    c->securing = false;
                    if (ops->secured)
                        ops->secured(c->handle, 0);
                }
            } else if (c->securing) {
                secure_failed(c, p[0] ? p[0] : 0xff);
            } else {
                c->secure = false;
            }
        }
        break;
    default:
        break;
    }
}

/* ---- API --------------------------------------------------------------------------------- */

void gap_init(const gap_ops *o, const char *name, uint32_t cod)
{
    ops = o;
    local_cod = cod;
    strncpy(local_name, name, GAP_NAME_MAX - 1);
    local_name[GAP_NAME_MAX - 1] = '\0';
    connectable = true;
    discoverable = false;
    inquiring = false;
    name_pending = -1;
    memset(found, 0, sizeof(found));
    memset(conns, 0, sizeof(conns));
    hci_add_event_handler(event);
}

void gap_start(void)
{
    hci_start(hci_started);
}

static void inquiry_status(uint16_t opcode, uint8_t status, const uint8_t *ret,
                           uint8_t ret_len, void *ctx)
{
    (void)opcode; (void)ret; (void)ret_len; (void)ctx;
    if (status) {
        inquiring = false;
        if (ops->inquiry_done)
            ops->inquiry_done();
    }
}

bool gap_inquiry(int seconds)
{
    if (inquiring)
        return false;
    int units = (seconds * 100 + 127) / 128;   /* 1.28 s units */
    if (units < 1) units = 1;
    if (units > 0x30) units = 0x30;
    uint8_t p[5] = { 0x33, 0x8b, 0x9e, (uint8_t)units, 0 };   /* general inquiry */
    memset(found, 0, sizeof(found));
    name_pending = -1;
    inquiring = true;
    return hci_command(HCI_INQUIRY, p, 5, inquiry_status, NULL);
}

void gap_inquiry_cancel(void)
{
    if (inquiring)
        hci_command(HCI_INQUIRY_CANCEL, NULL, 0, NULL, NULL);
    inquiring = false;
}

static void connect_status(uint16_t opcode, uint8_t status, const uint8_t *ret,
                           uint8_t ret_len, void *ctx)
{
    (void)opcode; (void)ret; (void)ret_len;
    if (status && ops->connected)
        ops->connected(0, (const lbt_addr *)ctx, status);
}

void gap_connect_cancel(const lbt_addr *addr)
{
    hci_command(HCI_CREATE_CONNECTION_CANCEL, addr->b, 6, NULL, NULL);
}

bool gap_connect(const lbt_addr *addr)
{
    static lbt_addr target;
    uint8_t p[13];
    struct found *f = find_found(addr);
    target = *addr;
    memcpy(p, addr->b, 6);
    lbt_put16(p + 6, 0xcc18);   /* DM1/DH1/DM3/DH3/DM5/DH5 */
    p[8] = f ? f->psrm : 0;
    p[9] = 0;
    lbt_put16(p + 10, f ? (f->clock_offset | 0x8000) : 0);
    p[12] = 1;                  /* allow role switch */
    return hci_command(HCI_CREATE_CONNECTION, p, 13, connect_status, &target);
}

void gap_disconnect(uint16_t handle, uint8_t reason)
{
    uint8_t p[3];
    lbt_put16(p, handle);
    p[2] = reason;
    hci_command(HCI_DISCONNECT, p, 3, NULL, NULL);
}

bool gap_secure(uint16_t handle)
{
    struct conn *c = conn_by_handle(handle);
    if (!c)
        return false;
    if (c->secure) {
        if (ops->secured)
            ops->secured(handle, 0);
        return true;
    }
    c->retried = false;
    auth_requested(c);
    return true;
}

static void write_scan(void)
{
    uint8_t p = (connectable ? 0x02 : 0x00) | (discoverable ? 0x01 : 0x00);
    hci_command(HCI_WRITE_SCAN_ENABLE, &p, 1, NULL, NULL);
}

void gap_set_connectable(bool on)
{
    connectable = on;
    write_scan();
}

void gap_confirm(const lbt_addr *addr, bool yes)
{
    hci_command(yes ? HCI_USER_CONFIRMATION_REQUEST_REPLY : HCI_USER_CONFIRMATION_REQUEST_NEGATIVE_REPLY,
                addr->b, 6, NULL, NULL);
}

void gap_set_discoverable(bool on)
{
    discoverable = on;
    write_scan();
}

bool gap_is_secure(uint16_t handle)
{
    struct conn *c = conn_by_handle(handle);
    return c && c->secure;
}
