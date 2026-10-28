/*
 * lbt GAP: finding, connecting to and pairing with devices
 * (Bluetooth Core, Vol 3 Part C; security in Vol 2 Part F and Vol 3 Part C 5).
 *
 * - controller set-up after HCI start: event masks, Secure Simple Pairing,
 *   name, class of device, page scan
 * - inquiry with names (from extended inquiry results or remote name
 *   requests)
 * - outgoing and incoming ACL connections
 * - security: authenticate with a stored link key, or pair with Secure
 *   Simple Pairing: "just works" with headphones (no screen), a code to
 *   confirm on both screens with a car or a phone (ops->confirm, answered
 *   with gap_confirm), or a legacy PIN of 0000; then encrypt. New keys go
 *   to the platform to keep.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_GAP_H
#define LBT_GAP_H

#include "lbt_util.h"

#define GAP_NAME_MAX   64
#define GAP_MAX_FOUND  16

/* Class of device for a music player: Audio service, Audio/Video major
 * class, "portable audio" minor class */
#define GAP_COD_PORTABLE_AUDIO 0x20041c

typedef struct {
    /* controller set up (ok) or not */
    void (*ready)(bool ok);
    /* a device answered an inquiry; name may be "" (a name may follow) */
    void (*found)(const lbt_addr *addr, const char *name, uint32_t cod, int8_t rssi);
    /* a name arrived for a device found earlier */
    void (*named)(const lbt_addr *addr, const char *name);
    void (*inquiry_done)(void);
    /* ACL link up (status 0) or the connection attempt failed */
    void (*connected)(uint16_t handle, const lbt_addr *addr, uint8_t status);
    /* the link is authenticated and encrypted (status 0), or that failed */
    void (*secured)(uint16_t handle, uint8_t status);
    void (*disconnected)(uint16_t handle, uint8_t reason);
    /* link keys: look one up, keep a new one, forget a bad one */
    bool (*get_key)(const lbt_addr *addr, uint8_t key[16]);
    void (*put_key)(const lbt_addr *addr, const uint8_t key[16], uint8_t type);
    void (*forget_key)(const lbt_addr *addr);
    /* pairing with a device that has a screen: show code (6 digits) and
     * answer with gap_confirm; passkey: they type it, nothing to answer.
     * NULL: yes to every code */
    void (*confirm)(const lbt_addr *addr, uint32_t code, bool passkey);
} gap_ops;

void gap_init(const gap_ops *ops, const char *name, uint32_t cod);

/* Start HCI and set the controller up; ops->ready when done */
void gap_start(void);

bool gap_inquiry(int seconds);
void gap_inquiry_cancel(void);

bool gap_connect(const lbt_addr *addr);
/* Stop a connection attempt: its Connection Complete then comes with an
 * error (0x02, Unknown Connection Identifier) */
void gap_connect_cancel(const lbt_addr *addr);
void gap_disconnect(uint16_t handle, uint8_t reason);

/* Authenticate (pairing if needed) and encrypt; ops->secured when done */
bool gap_secure(uint16_t handle);

/* Page scan on (others can connect to us) or off */
void gap_set_connectable(bool on);
/* Found by other devices' searches (a car's "add device"), as well as
 * reachable: while the player's search screen is open */
void gap_set_discoverable(bool on);

bool gap_is_secure(uint16_t handle);

/* The code shown matched (yes) or not */
void gap_confirm(const lbt_addr *addr, bool yes);

#endif
