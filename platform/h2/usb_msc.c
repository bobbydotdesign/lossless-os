/*
 * The player as a USB drive (usb_msc.h). Bulk-only transport: the computer
 * sends a command block wrapper (CBW: a SCSI command and how much data
 * goes which way), the data moves, and the drive answers with a status
 * wrapper (CSW). The card is the one drive, read and written in 512-byte
 * blocks through Zephyr's disk access ("SD").
 *
 * Sources: USB Mass Storage Class, Bulk-Only Transport 1.0; SCSI Primary
 * and Block Commands (the few commands computers use with USB drives).
 *
 * Written to only if the card passed the player's own write check at
 * start (los_writes_ok); otherwise the drive says it's write-protected,
 * and the computer mounts it read-only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include "h2_usb.h"
#include "usb_msc.h"

#define DISK "SD"
#define BLOCK 512
#define BUF_BLOCKS 128                      /* 64 KB at a time */

static uint8_t buf[BUF_BLOCKS * BLOCK] __aligned(32);
static uint8_t cbw[512] __aligned(4);
K_MUTEX_DEFINE(msc_lock);                   /* the card: sharing it, using it */

static volatile bool shared, ejected, writable;
static bool attention;                      /* "the medium changed", to say once */
static uint32_t blocks;
static uint8_t sense_key, sense_asc, sense_ascq;

enum { CMD_TEST_UNIT_READY = 0x00, CMD_REQUEST_SENSE = 0x03, CMD_INQUIRY = 0x12,
       CMD_MODE_SENSE6 = 0x1a, CMD_START_STOP = 0x1b, CMD_PREVENT_ALLOW = 0x1e,
       CMD_READ_FORMAT_CAPACITIES = 0x23, CMD_READ_CAPACITY10 = 0x25, CMD_READ10 = 0x28,
       CMD_WRITE10 = 0x2a, CMD_VERIFY10 = 0x2f, CMD_SYNC_CACHE10 = 0x35,
       CMD_MODE_SENSE10 = 0x5a };

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static void put_be32(uint8_t *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = (uint8_t)v; }
static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void put_le32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }

static void sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
	sense_key = key;
	sense_asc = asc;
	sense_ascq = ascq;
}

void msc_share(bool on, bool can_write)
{
	k_mutex_lock(&msc_lock, K_FOREVER);
	if (on && !shared) {
		writable = can_write;
		blocks = 0;
		if (disk_access_init(DISK) == 0 &&
		    disk_access_ioctl(DISK, DISK_IOCTL_GET_SECTOR_COUNT, &blocks) != 0)
			blocks = 0;
		ejected = false;
		attention = true;
		shared = true;
	} else if (!on) {
		shared = false;
	}
	k_mutex_unlock(&msc_lock);
}

bool msc_shared(void) { return shared; }
bool msc_ejected(void) { return ejected; }

void msc_reset(void)
{
	/* nothing in flight survives a reset; the next CBW starts afresh */
}

/* The card is there for the computer; if not, the sense says why */
static bool ready(void)
{
	if (!shared || ejected || !blocks) {
		sense(0x02, 0x3a, 0x00);                /* not ready: medium not present */
		return false;
	}
	if (attention) {
		attention = false;
		sense(0x06, 0x28, 0x00);                /* unit attention: medium changed */
		return false;
	}
	return true;
}

/* Data to the computer: at most what it asked for. Less than that, ending
 * on a whole packet, can't be told apart from "more to come", so the
 * endpoint is stalled to end it. Returns what wasn't sent (the residue). */
static uint32_t send(const void *data, uint32_t n, uint32_t asked)
{
	if (n > asked)
		n = asked;
	if (n && !usb_bulk_in(data, n))
		return asked;
	if (n < asked && (n == 0 || n % usb_bulk_mps() == 0))
		usb_bulk_stall_in();
	return asked - n;
}

/* Data from the computer we won't use */
static void discard(uint32_t n)
{
	while (n) {
		uint32_t take = n < sizeof(buf) ? n : sizeof(buf);
		int got = usb_bulk_out(buf, take);

		if (got <= 0)
			return;
		n -= (uint32_t)got;
	}
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, uint32_t asked, bool *ok)
{
	*ok = false;
	if (!ready())
		return send(NULL, 0, asked);
	if (lba >= blocks || count > blocks - lba || count * BLOCK != asked) {
		sense(0x05, 0x21, 0x00);                /* illegal request: block out of range */
		return send(NULL, 0, asked);
	}
	uint32_t done = 0;

	while (done < count) {
		uint32_t n = count - done < BUF_BLOCKS ? count - done : BUF_BLOCKS;

		k_mutex_lock(&msc_lock, K_FOREVER);
		int err = shared ? disk_access_read(DISK, buf, lba + done, n) : -1;

		k_mutex_unlock(&msc_lock);
		if (err) {
			sense(0x03, 0x11, 0x00);        /* medium error: unrecovered read */
			return send(NULL, 0, asked - done * BLOCK);
		}
		if (!usb_bulk_in(buf, n * BLOCK))
			return asked - done * BLOCK;
		done += n;
	}
	*ok = true;
	return 0;
}

static uint32_t write_blocks(uint32_t lba, uint32_t count, uint32_t given, bool *ok)
{
	*ok = false;
	if (!ready()) {
		discard(given);
		return given;
	}
	if (!writable) {
		sense(0x07, 0x27, 0x00);                /* data protect: write protected */
		discard(given);
		return given;
	}
	if (lba >= blocks || count > blocks - lba || count * BLOCK != given) {
		sense(0x05, 0x21, 0x00);
		discard(given);
		return given;
	}
	uint32_t done = 0;

	while (done < count) {
		uint32_t n = count - done < BUF_BLOCKS ? count - done : BUF_BLOCKS;
		int got = usb_bulk_out(buf, n * BLOCK);

		if (got != (int)(n * BLOCK))
			return given - done * BLOCK;
		k_mutex_lock(&msc_lock, K_FOREVER);
		int err = shared ? disk_access_write(DISK, buf, lba + done, n) : -1;

		k_mutex_unlock(&msc_lock);
		if (err) {
			sense(0x03, 0x0c, 0x00);        /* medium error: write error */
			discard(given - (done + n) * BLOCK);
			return given - done * BLOCK;
		}
		done += n;
	}
	*ok = true;
	return 0;
}

void msc_run(void)
{
	int got = usb_bulk_out(cbw, sizeof(cbw));

	if (got < 0)
		return;
	if (got != 31 || le32(cbw) != 0x43425355u) {
		/* not a command: stall until the computer resets the drive */
		usb_bulk_stall_in();
		return;
	}
	uint32_t tag = le32(cbw + 4), length = le32(cbw + 8);
	bool to_host = cbw[12] & 0x80;
	const uint8_t *cb = cbw + 15;
	uint8_t reply[36];
	uint32_t residue = 0;
	bool ok = true;

	switch (cb[0]) {
	case CMD_TEST_UNIT_READY:
		ok = ready();
		break;
	case CMD_REQUEST_SENSE:
		memset(reply, 0, 18);
		reply[0] = 0x70;
		reply[2] = sense_key;
		reply[7] = 10;
		reply[12] = sense_asc;
		reply[13] = sense_ascq;
		residue = send(reply, 18, length);
		sense(0, 0, 0);
		break;
	case CMD_INQUIRY:
		memset(reply, 0, 36);
		reply[1] = 0x80;                        /* removable */
		reply[2] = 0x04;                        /* SPC-2 */
		reply[3] = 0x02;
		reply[4] = 31;
		memcpy(reply + 8, "Lossless", 8);
		memcpy(reply + 16, "Player card     ", 16);
		memcpy(reply + 32, "1.0 ", 4);
		residue = send(reply, 36, length);
		break;
	case CMD_READ_CAPACITY10:
		if (!(ok = ready())) {
			residue = send(NULL, 0, length);
			break;
		}
		put_be32(reply, blocks - 1);
		put_be32(reply + 4, BLOCK);
		residue = send(reply, 8, length);
		break;
	case CMD_READ_FORMAT_CAPACITIES:
		memset(reply, 0, 12);
		reply[3] = 8;
		put_be32(reply + 4, blocks);
		reply[8] = shared && !ejected && blocks ? 0x02 : 0x03;   /* formatted / none */
		reply[10] = BLOCK >> 8;
		residue = send(reply, 12, length);
		break;
	case CMD_MODE_SENSE6:
		memset(reply, 0, 4);
		reply[0] = 3;
		reply[2] = writable ? 0 : 0x80;         /* write-protected */
		residue = send(reply, 4, length);
		break;
	case CMD_MODE_SENSE10:
		memset(reply, 0, 8);
		reply[1] = 6;
		reply[3] = writable ? 0 : 0x80;
		residue = send(reply, 8, length);
		break;
	case CMD_START_STOP:
		if ((cb[4] & 0x03) == 0x02)             /* eject, don't start */
			ejected = true;
		break;
	case CMD_PREVENT_ALLOW:
	case CMD_VERIFY10:
		break;
	case CMD_SYNC_CACHE10:
		k_mutex_lock(&msc_lock, K_FOREVER);
		if (shared)
			disk_access_ioctl(DISK, DISK_IOCTL_CTRL_SYNC, NULL);
		k_mutex_unlock(&msc_lock);
		break;
	case CMD_READ10:
		residue = read_blocks(be32(cb + 2), (uint32_t)(cb[7] << 8 | cb[8]), length, &ok);
		break;
	case CMD_WRITE10:
		residue = write_blocks(be32(cb + 2), (uint32_t)(cb[7] << 8 | cb[8]), length, &ok);
		break;
	default:
		sense(0x05, 0x20, 0x00);                /* illegal request: invalid command */
		ok = false;
		if (to_host)
			residue = send(NULL, 0, length);
		else {
			discard(length);
			residue = length;
		}
		break;
	}
	uint8_t csw[13];

	put_le32(csw, 0x53425355u);
	put_le32(csw + 4, tag);
	put_le32(csw + 8, residue);
	csw[12] = ok ? 0 : 1;
	usb_bulk_in(csw, 13);
}
