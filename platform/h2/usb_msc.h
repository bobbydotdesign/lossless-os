/*
 * The player as a USB drive (usb_msc.c): USB mass storage, bulk-only
 * transport, SCSI commands, the card as the one drive.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdbool.h>

/* On the USB thread: one command (waits for it), or returns at once if
 * the bus was reset or the cable came out */
void msc_run(void);
/* The host reset the drive (a bus reset, or the class's own reset) */
void msc_reset(void);

/* Give the card to the computer (the file system is unmounted) or take it
 * back; it writes to it only if 'can_write'. Any thread. */
void msc_share(bool on, bool can_write);
bool msc_shared(void);
/* The computer ejected the card */
bool msc_ejected(void);

#endif
