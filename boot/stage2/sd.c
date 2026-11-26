/*
 * The card, for reading only: the X1000's card controller (MSC0) polled
 * through its FIFO, and the SD start-up sequence.
 *
 * Controller registers and clock: docs/h2-hardware.md, "SD card
 * controller", and our Lossless OS driver (drivers/sdhc/x1000_msc.c).
 * Card protocol: SD Association, Physical Layer Simplified Specification:
 * CMD0, CMD8 (0x1aa), ACMD41 until ready (high capacity asked for), CMD2,
 * CMD3 (the card's address), CMD7 (select), ACMD6 (4 data lines), CMD16
 * (512-byte blocks, older cards), then CMD17/CMD18 reads and CMD12.
 *
 * The first stage has powered the card (PB10) and given the controller
 * its pins (PA20-25).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "stage2.h"

#define MSC              0xb3450000u
#define MSC_CTRL         0x00
#define MSC_STAT         0x04
#define MSC_CLKRT        0x08
#define MSC_CMDAT        0x0c
#define MSC_BLKLEN       0x18
#define MSC_NOB          0x1c
#define MSC_IMASK        0x24
#define MSC_IFLAG        0x28
#define MSC_CMD          0x2c
#define MSC_ARG          0x30
#define MSC_RES          0x34
#define MSC_RXFIFO       0x38
#define MSC_LPM          0x40
#define MSC_DMAC         0x44
#define MSC_CTRL2        0x58
#define M(off)           REG(MSC + (off))

#define CTRL_START_OP    (1u << 2)
#define CTRL_RESET       (1u << 3)
#define STAT_FIFO_EMPTY  (1u << 6)
#define STAT_CLOCK_EN    (1u << 8)
#define STAT_RESETTING   (1u << 15)

#define RESP_NONE        0u
#define RESP_SHORT       1u
#define RESP_LONG        2u
#define RESP_NOCRC       3u
#define CMDAT_DATA_EN    (1u << 3)
#define CMDAT_BUSY       (1u << 6)
#define CMDAT_INIT       (1u << 7)
#define CMDAT_4BIT       (2u << 9)

#define F_DATA_DONE      (1u << 0)
#define F_PROG_DONE      (1u << 1)
#define F_END_CMD_RES    (1u << 2)
#define F_TIMEOUT_READ   (1u << 8)
#define F_TIMEOUT_RES    (1u << 9)
#define F_CRC_READ       (1u << 11)
#define F_CRC_RES        (1u << 12)

#define CPM_CCR          0xb0000000u
#define CPM_APCR         0xb0000010u
#define CPM_CLKGR        0xb0000020u
#define CPM_MSC0CDR      0xb0000068u
#define CDR_CE           (1u << 29)
#define CDR_BUSY         (1u << 28)

#define LPM_ENABLE       (1u << 0)
#define LPM_TIMING_MASK  (0xfu << 28)

static uint32_t bus;          /* CMDAT data-line bits */
static uint32_t init_once;    /* 80 clocks before the first command */
static uint32_t rca;
static int high_capacity;     /* blocks addressed by number, not byte */

/* Polls: generous counts, each poll a register read (well over 10 ns) */
#define POLLS_1S 100000000u

static uint32_t sclk_a_hz(void)
{
	uint32_t a = REG(CPM_APCR);
	uint32_t hz = 24000000u * ((a >> 24 & 0x7f) + 1) / ((a >> 18 & 0x1f) + 1);

	return (REG(CPM_CCR) >> 30) == 2 ? hz >> (a >> 16 & 3) : 24000000u;
}

/* The card clock: MSC0CDR divides SCLK_A / 2, then CLKRT by 2^n */
static void set_clock(uint32_t hz)
{
	uint32_t src = sclk_a_hz();
	uint32_t want = hz > 50000000u ? hz : 50000000u;
	uint32_t div = (src / 2 + want - 1) / want;

	if (div < 1)
		div = 1;
	if (div > 256)
		div = 256;
	for (uint32_t i = 0; i < 1000000 && (M(MSC_STAT) & STAT_CLOCK_EN); i++)
		;
	REG(CPM_MSC0CDR) = (REG(CPM_MSC0CDR) & ~(1u << 31 | 0xffu)) | CDR_CE | (div - 1);
	for (uint32_t i = 0; i < 1000000 && (REG(CPM_MSC0CDR) & CDR_BUSY); i++)
		;
	REG(CPM_MSC0CDR) &= ~CDR_CE;

	uint32_t base = src / (2 * div), shift = 0;

	while (shift < 7 && (base >> shift) > hz)
		shift++;
	M(MSC_CLKRT) = shift;
	/* default speed: drive on the falling edge, sample on the rising */
	M(MSC_LPM) = (M(MSC_LPM) & ~LPM_TIMING_MASK) | LPM_ENABLE;
	M(MSC_CTRL2) &= ~7u;
}

static int wait_flags(uint32_t want, uint32_t errors, uint32_t *flags)
{
	for (uint32_t i = 0; i < POLLS_1S; i++) {
		uint32_t f = M(MSC_IFLAG);
		if (f & (want | errors)) {
			*flags = f;
			return (f & errors) ? -1 : 0;
		}
	}
	*flags = M(MSC_IFLAG);
	return -2;
}

/* The response comes out of RES 16 bits at a time, most significant
 * first; a short one is 48 bits, of which the 32 in the middle count */
static uint32_t short_response(void)
{
	uint32_t r = M(MSC_RES) & 0xffff, w = r << 24;

	r = M(MSC_RES) & 0xffff;
	w |= r << 8;
	r = M(MSC_RES) & 0xffff;
	return w | (r & 0xff);
}

/* One command; data (blocks of 512 into buf) if nblocks. 0 or < 0. */
static int command(uint32_t cmd, uint32_t arg, uint32_t resp, uint32_t *r,
		   uint8_t *buf, uint32_t nblocks)
{
	uint32_t cmdat = bus | resp, flags;

	if (init_once) {
		cmdat |= CMDAT_INIT;
		init_once = 0;
	}
	if (nblocks) {
		cmdat |= CMDAT_DATA_EN;
		M(MSC_NOB) = nblocks;
		M(MSC_BLKLEN) = 512;
	} else {
		M(MSC_NOB) = 0;
		M(MSC_BLKLEN) = 0;
	}
	M(MSC_IFLAG) = 0xffffffffu;
	M(MSC_IMASK) = 0xffffffffu;
	M(MSC_CMD) = cmd;
	M(MSC_ARG) = arg;
	M(MSC_CMDAT) = cmdat;
	M(MSC_CTRL) = CTRL_START_OP;

	if (wait_flags(F_END_CMD_RES, F_TIMEOUT_RES | F_CRC_RES, &flags))
		return -1;
	if (resp != RESP_NONE && resp != RESP_LONG && r)
		*r = short_response();
	if (resp == RESP_LONG)
		for (int i = 0; i < 8; i++)
			(void)M(MSC_RES);      /* CID: not needed */

	if (nblocks) {
		uint32_t left = nblocks * 512 / 4;
		uint32_t *dst = (uint32_t *)buf;
		uint32_t polls = 0;

		while (left) {
			if (M(MSC_STAT) & STAT_FIFO_EMPTY) {
				uint32_t f = M(MSC_IFLAG);
				if (f & (F_TIMEOUT_READ | F_CRC_READ))
					return -3;
				if (++polls > POLLS_1S)
					return -4;
				continue;
			}
			uint32_t w = M(MSC_RXFIFO);
			if (((uintptr_t)dst & 3) == 0) {
				*dst++ = w;
			} else {
				uint8_t *b = (uint8_t *)dst;
				b[0] = w; b[1] = w >> 8; b[2] = w >> 16; b[3] = w >> 24;
				dst++;
			}
			left--;
		}
		if (wait_flags(F_DATA_DONE, F_TIMEOUT_READ | F_CRC_READ, &flags))
			return -5;
	} else if (cmdat & CMDAT_BUSY) {
		if (wait_flags(F_PROG_DONE, 0, &flags))
			return -6;
	}
	return 0;
}

static int app_command(uint32_t cmd, uint32_t arg, uint32_t resp, uint32_t *r)
{
	uint32_t r1;

	if (command(55, rca << 16, RESP_SHORT, &r1, 0, 0))
		return -1;
	return command(cmd, arg, resp, r, 0, 0);
}

int sd_init(void)
{
	uint32_t r;

	REG(CPM_CLKGR) &= ~(1u << 4);    /* MSC0 clock on */
	M(MSC_CTRL) = CTRL_RESET;
	delay_us(100);
	M(MSC_CTRL) = 0;
	for (uint32_t i = 0; i < 1000000 && (M(MSC_STAT) & STAT_RESETTING); i++)
		;
	M(MSC_IMASK) = 0xffffffffu;
	M(MSC_IFLAG) = 0xffffffffu;
	M(MSC_DMAC) = 0;
	bus = 0;
	rca = 0;
	init_once = 1;
	set_clock(300000);               /* start-up: under 400 kHz */
	delay_us(1000);

	if (command(0, 0, RESP_NONE, 0, 0, 0))                  /* CMD0: idle */
		return 1;
	int v2 = command(8, 0x1aa, RESP_SHORT, &r, 0, 0) == 0 && (r & 0xfff) == 0x1aa;

	for (int i = 0;; i++) {                                 /* ACMD41 */
		if (app_command(41, (v2 ? 1u << 30 : 0) | 0x00ff8000u, RESP_NOCRC, &r))
			return 2;
		if (r & (1u << 31))
			break;
		if (i > 1000)
			return 3;
		delay_us(1000);
	}
	high_capacity = (r >> 30) & 1;

	if (command(2, 0, RESP_LONG, 0, 0, 0))                  /* CMD2: CID */
		return 4;
	if (command(3, 0, RESP_SHORT, &r, 0, 0))                /* CMD3: address */
		return 5;
	rca = r >> 16;

	set_clock(25000000);                                    /* default speed */
	if (command(7, rca << 16, RESP_SHORT | CMDAT_BUSY, &r, 0, 0))   /* select */
		return 6;
	if (app_command(6, 2, RESP_SHORT, &r) == 0)             /* 4 data lines */
		bus = CMDAT_4BIT;
	if (!high_capacity && command(16, 512, RESP_SHORT, &r, 0, 0))
		return 7;
	note(0x5d000000u | (uint32_t)high_capacity << 16 | (bus ? 4 : 1));
	return 0;
}

/* count blocks of 512 from block lba into buf. 0 or < 0. */
int sd_read(uint8_t *buf, uint32_t lba, uint32_t count)
{
	uint32_t r, addr = high_capacity ? lba : lba * 512;

	if (count == 1)
		return command(17, addr, RESP_SHORT, &r, buf, 1);
	int err = command(18, addr, RESP_SHORT, &r, buf, count);
	int stop = command(12, 0, RESP_SHORT | CMDAT_BUSY, &r, 0, 0);
	return err ? err : stop;
}
