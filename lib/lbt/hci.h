/*
 * lbt HCI: talking to the controller (Bluetooth Core, Vol 4 Part E).
 *
 * - commands, queued and sent as the controller has room for them, each
 *   with a callback for its Command Complete or Command Status
 * - events, handed to registered handlers after HCI's own bookkeeping
 * - ACL data: outgoing L2CAP PDUs are split to the controller's buffer
 *   size and sent as it returns credits; incoming fragments are joined
 *   into whole PDUs per connection
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_HCI_H
#define LBT_HCI_H

#include "lbt_util.h"

/* ---- sizes ---------------------------------------------------------------- */

#define HCI_MAX_CONNECTIONS 2
#define HCI_ACL_MAX         1021  /* largest ACL fragment we send or take */
#define HCI_PDU_MAX         1100  /* largest incoming L2CAP PDU we join */
#define HCI_TX_BUFFERS      10    /* outgoing ACL fragments we can hold */
#define HCI_CMD_QUEUE       12
#define HCI_CMD_TIMEOUT_MS  5000

/* ---- opcodes -------------------------------------------------------------- */

#define HCI_OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | (ocf)))

/* Link control */
#define HCI_INQUIRY                         HCI_OP(0x01, 0x0001)
#define HCI_INQUIRY_CANCEL                  HCI_OP(0x01, 0x0002)
#define HCI_CREATE_CONNECTION               HCI_OP(0x01, 0x0005)
#define HCI_CREATE_CONNECTION_CANCEL        HCI_OP(0x01, 0x0008)
#define HCI_DISCONNECT                      HCI_OP(0x01, 0x0006)
#define HCI_ACCEPT_CONNECTION_REQUEST       HCI_OP(0x01, 0x0009)
#define HCI_REJECT_CONNECTION_REQUEST       HCI_OP(0x01, 0x000a)
#define HCI_LINK_KEY_REQUEST_REPLY          HCI_OP(0x01, 0x000b)
#define HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY HCI_OP(0x01, 0x000c)
#define HCI_PIN_CODE_REQUEST_REPLY          HCI_OP(0x01, 0x000d)
#define HCI_PIN_CODE_REQUEST_NEGATIVE_REPLY HCI_OP(0x01, 0x000e)
#define HCI_AUTHENTICATION_REQUESTED        HCI_OP(0x01, 0x0011)
#define HCI_SET_CONNECTION_ENCRYPTION       HCI_OP(0x01, 0x0013)
#define HCI_REMOTE_NAME_REQUEST             HCI_OP(0x01, 0x0019)
#define HCI_REMOTE_NAME_REQUEST_CANCEL      HCI_OP(0x01, 0x001a)
#define HCI_IO_CAPABILITY_REQUEST_REPLY     HCI_OP(0x01, 0x002b)
#define HCI_USER_CONFIRMATION_REQUEST_REPLY HCI_OP(0x01, 0x002c)
#define HCI_USER_CONFIRMATION_NEGATIVE_REPLY HCI_OP(0x01, 0x002d)
#define HCI_IO_CAPABILITY_REQUEST_NEGATIVE_REPLY HCI_OP(0x01, 0x0034)

/* Link policy */
#define HCI_SWITCH_ROLE                     HCI_OP(0x02, 0x000b)
#define HCI_WRITE_LINK_POLICY_SETTINGS      HCI_OP(0x02, 0x000d)
#define HCI_WRITE_DEFAULT_LINK_POLICY       HCI_OP(0x02, 0x000f)

/* Controller and baseband */
#define HCI_SET_EVENT_MASK                  HCI_OP(0x03, 0x0001)
#define HCI_RESET                           HCI_OP(0x03, 0x0003)
#define HCI_WRITE_LOCAL_NAME                HCI_OP(0x03, 0x0013)
#define HCI_WRITE_PAGE_TIMEOUT              HCI_OP(0x03, 0x0018)
#define HCI_WRITE_SCAN_ENABLE               HCI_OP(0x03, 0x001a)
#define HCI_WRITE_CLASS_OF_DEVICE           HCI_OP(0x03, 0x0024)
#define HCI_WRITE_INQUIRY_MODE              HCI_OP(0x03, 0x0045)
#define HCI_WRITE_EXTENDED_INQUIRY_RESPONSE HCI_OP(0x03, 0x0052)
#define HCI_WRITE_SIMPLE_PAIRING_MODE       HCI_OP(0x03, 0x0056)

/* Informational */
#define HCI_READ_LOCAL_VERSION              HCI_OP(0x04, 0x0001)
#define HCI_READ_LOCAL_FEATURES             HCI_OP(0x04, 0x0003)
#define HCI_READ_BUFFER_SIZE                HCI_OP(0x04, 0x0005)
#define HCI_READ_BD_ADDR                    HCI_OP(0x04, 0x0009)

/* ---- events --------------------------------------------------------------- */

#define HCI_EV_INQUIRY_COMPLETE             0x01
#define HCI_EV_INQUIRY_RESULT               0x02
#define HCI_EV_CONNECTION_COMPLETE          0x03
#define HCI_EV_CONNECTION_REQUEST           0x04
#define HCI_EV_DISCONNECTION_COMPLETE       0x05
#define HCI_EV_AUTHENTICATION_COMPLETE      0x06
#define HCI_EV_REMOTE_NAME_REQUEST_COMPLETE 0x07
#define HCI_EV_ENCRYPTION_CHANGE            0x08
#define HCI_EV_COMMAND_COMPLETE             0x0e
#define HCI_EV_COMMAND_STATUS               0x0f
#define HCI_EV_HARDWARE_ERROR               0x10
#define HCI_EV_ROLE_CHANGE                  0x12
#define HCI_EV_NUMBER_OF_COMPLETED_PACKETS  0x13
#define HCI_EV_MODE_CHANGE                  0x14
#define HCI_EV_PIN_CODE_REQUEST             0x16
#define HCI_EV_LINK_KEY_REQUEST             0x17
#define HCI_EV_LINK_KEY_NOTIFICATION        0x18
#define HCI_EV_INQUIRY_RESULT_WITH_RSSI     0x22
#define HCI_EV_EXTENDED_INQUIRY_RESULT      0x2f
#define HCI_EV_ENCRYPTION_KEY_REFRESH_COMPLETE 0x30
#define HCI_EV_IO_CAPABILITY_REQUEST        0x31
#define HCI_EV_IO_CAPABILITY_RESPONSE       0x32
#define HCI_EV_USER_CONFIRMATION_REQUEST    0x33
#define HCI_EV_SIMPLE_PAIRING_COMPLETE      0x36

/* Status passed to command callbacks when the controller never answered */
#define HCI_STATUS_TIMEOUT 0xff

/* ---- API ------------------------------------------------------------------ */

/* Command Complete (ret = return parameters after the status byte) or
 * Command Status (ret = NULL, ret_len = 0). */
typedef void (*hci_cmd_done_fn)(uint16_t opcode, uint8_t status,
                                const uint8_t *ret, uint8_t ret_len, void *ctx);

/* Every event (code, parameters), after HCI's own handling */
typedef void (*hci_event_fn)(uint8_t code, const uint8_t *params, uint8_t len);

/* A whole incoming L2CAP PDU (basic header included) */
typedef void (*hci_pdu_fn)(uint16_t handle, const uint8_t *pdu, uint16_t len);

/* Called when outgoing ACL room frees up */
typedef void (*hci_ready_fn)(void);

/* Called once the startup commands are done (ok = all succeeded) */
typedef void (*hci_started_fn)(bool ok);

void hci_init(void);

/* Reset the controller and read its address and buffer sizes. The chip's
 * patch must already be loaded (see bt_raw.c). */
void hci_start(hci_started_fn started);

/* Feed bytes from the UART, in any chunks. */
void hci_receive(const uint8_t *data, size_t len);

/* Call every few hundred ms: command timeouts. */
void hci_tick(void);

/* Queue a command. False if the queue is full or params too long. */
bool hci_command(uint16_t opcode, const uint8_t *params, uint8_t len,
                 hci_cmd_done_fn done, void *ctx);

bool hci_add_event_handler(hci_event_fn fn);
void hci_set_pdu_handler(hci_pdu_fn fn);
void hci_set_ready_handler(hci_ready_fn fn);

/* Every whole packet from the controller (H4 type, packet), for a recording */
typedef void (*hci_trace_fn)(uint8_t h4_type, const uint8_t *packet, uint16_t len);
void hci_set_trace(hci_trace_fn fn);

/* Queue an L2CAP PDU for a connection. False if there isn't room for all
 * its fragments right now (try again from the ready handler). */
bool hci_send_pdu(uint16_t handle, const uint8_t *pdu, uint16_t len);

/* Free outgoing fragment buffers, and fragments a PDU of len needs */
int hci_tx_free(void);
/* The controller's ACL buffers free now, and how many it has */
int hci_acl_free(int *total);
int hci_fragments_for(uint16_t len);

const lbt_addr *hci_local_addr(void);
bool hci_is_connected(uint16_t handle);
bool hci_connection_addr(uint16_t handle, lbt_addr *addr);

#endif
