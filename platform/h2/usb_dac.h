/*
 * The player as a USB DAC (usb_dac.c): USB Audio Class 2, bit-perfect,
 * asynchronous (the player's clock decides).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_DAC_H
#define USB_DAC_H

#include <stdbool.h>
#include <stdint.h>

/* Its descriptors: device (1), configuration (2), qualifier (6) */
const uint8_t *dac_descriptor(unsigned kind, unsigned *len);

/* From h2_usb.c, at interrupt time */
int dac_request(const uint8_t *setup, uint8_t *buf, unsigned len);
void dac_set_interface(int alt);
int dac_get_interface(void);
void dac_rx(unsigned bytes);             /* audio waiting in the receive FIFO */
void dac_out_done(void);                 /* an audio packet came in */
void dac_fb_done(void);                  /* a feedback packet went out */
void dac_reset(void);

/* On the USB thread, every millisecond or so: the audio to the DAC */
void dac_pump(void);
/* Leaving DAC mode: the DAC stops */
void dac_end(void);

/* Playing now, and at what rate and depth */
bool dac_status(unsigned *rate, unsigned *bits);
int dac_debug(char *buf, unsigned n);    /* counters, for bringing it up */

#endif
