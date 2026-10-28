/*
 * USB on the H2 (h2_usb.c): clock and PHY, the cable, and the controller
 * as a device. usb_msc.c is the drive on top.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_USB_H
#define H2_USB_H

#include <stdbool.h>
#include <stdint.h>

void h2_usb_power(bool on);
/* A USB cable is in (computer or charger) */
bool h2_usb_cable(void);
/* Once: notes the controller's registers in /.lossless/logs/usb.txt */
void h2_usb_probe(void);
/* The USB thread: watches the cable, runs the device while it's in */
void h2_usb_start(void);

/* The card's file system, for handing the card to a computer and back
 * (h2_board.c). True if it's done. */
bool h2_card_mount(bool on);

/* For the drive (usb_msc.c), on the USB thread. Bulk transfers on
 * endpoint 1, waiting until done; false (or -1) if the bus was reset or
 * the cable came out meanwhile: then the drive starts over. */
void usb_service(void);
unsigned usb_bulk_mps(void);
bool usb_bulk_in(const void *buf, uint32_t len);
int usb_bulk_out(void *buf, uint32_t len);          /* bytes received */
/* Stall bulk IN, and wait until the host clears it */
bool usb_bulk_stall_in(void);

/* For the sound card (usb_dac.c), at interrupt time: words out of the
 * receive FIFO (NULL drops them); endpoint 1 as isochronous OUT (the audio,
 * packets up to 'out_mps') and IN (the feedback, 4 bytes), every
 * microframe */
void usb_fifo_read(uint32_t *dst, unsigned words);
void usb_iso_open(unsigned out_mps);
void usb_iso_close(void);
void usb_iso_out_arm(void);
void usb_iso_fb_send(uint32_t value);

#endif
