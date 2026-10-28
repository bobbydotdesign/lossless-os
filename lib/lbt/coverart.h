/*
 * lbt cover art: the song's cover for a car's screen (AVRCP 1.6 section
 * 5.14): a BIP image pull (BIP 1.2, cover art) over OBEX (GOEP 2.0) over
 * L2CAP in Enhanced Retransmission Mode.
 *
 * The car opens our OBEX channel (COVERART_PSM, named in the AVRCP target's
 * SDP record) and connects to the cover art service; the song's attributes
 * then carry an image handle ("0000001"), and the car asks for that
 * handle's image, its thumbnail (both a 200 x 200 JPEG here) or its
 * properties. One client at a time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LBT_COVERART_H
#define LBT_COVERART_H

#include "lbt_util.h"

#define COVERART_PSM     0x1005   /* dynamic: odd, upper byte even */
#define COVERART_HANDLE  8        /* "0000001" and its terminator */

typedef struct {
    /* The JPEG for a handle: data and length, kept until the next call.
     * False if there's none (the car hears "not found"). */
    bool (*image)(const char *handle, const uint8_t **data, uint32_t *len);
    /* a car connected to the cover art service, or went */
    void (*connected)(bool up);
} coverart_ops;

void coverart_init(const coverart_ops *ops);

/* A car is connected for covers: handles mean something to it */
bool coverart_connected(void);

/* Every couple of hundred ms: an answer that had to wait goes */
void coverart_tick(void);

#endif
