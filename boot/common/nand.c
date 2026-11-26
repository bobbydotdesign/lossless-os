/*
 * The player's flash: an SPI NAND with on-die ECC, 2048-byte pages, 64
 * pages a block, on the X1000's SPI flash controller (SFC), one phase per
 * transfer, the data by DMA through a buffer in main memory (which must
 * be running). Two chips are known in the H2 (hardware revision 4):
 * Winbond W25N01GV (ID EF AA 21) and, in some V2.3 players, XTX XT26G01C
 * (ID 0B 11). Everything below works on both, the same commands and
 * layouts, and the waits cover the slower of the two.
 *
 * Controller: Ingenic, X1000 Programming Manual, ch. 10 (registers 10.7,
 * flows 10.8). Flash commands: Winbond W25N01GV datasheet, and XTX
 * XT26G01C datasheet Rev A1.0 (Table 2; Fig 11: its read data has 4 dummy
 * bits and a 12-bit column, the same bytes as Winbond's 16-bit column for
 * columns below 4096; XTX's ECC bits are 7:4): 0xff reset,
 * 0x9f JEDEC ID (8 dummy clocks), 0x0f get feature (status register 0xc0:
 * bit 0 busy, bits 5:4 ECC), 0x13 page data read (8 dummy bits, 16-bit page
 * address), 0x03 read data (16-bit column, 8 dummy clocks); for writing
 * (the bootloader writer only): 0x06 write enable, 0x1f set feature
 * (protection register 0xa0), 0xd8 block erase (8 dummy bits + page),
 * 0x02 program data load (16-bit column, then data), 0x10 program
 * execute (8 dummy bits + page). Writes go out by DMA the other way
 * (SFC_GLB TRAN_DIR, manual 10.7.3.1 and the write flow in 10.8.7).
 *
 * Dummy clocks go out as an extra address byte of zero: with the
 * controller's own dummy setting (TRAN_CONF DMY_BITS) the data phase never
 * started on the H2.
 *
 * Pins PA26-PA31 on function 1, as the player runs them (pin registers
 * read under the current bootloader, 2026-10-31).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nand.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SFC            0xb3440000u
#define S(off)         REG32(SFC + (off))
#define SFC_GLB        0x00
#define SFC_DEV_CONF   0x04
#define SFC_TRAN_CONF0 0x14
#define SFC_TRAN_LEN   0x2c
#define SFC_DEV_ADR0   0x30
#define SFC_DEV_PLUS0  0x48
#define SFC_MEM_ADDR   0x60
#define SFC_TRIG       0x64
#define SFC_SR         0x68
#define SFC_SCR        0x6c
#define SFC_INTC       0x70
#define SFC_CGE        0x78
#define SFC_DR         0x1000

#define GLB_PHASE1     (1u << 3)
#define GLB_THRESHOLD1 (1u << 7)
#define GLB_WRITE      (1u << 13)

#define TC_ADDR(n)     ((uint32_t)(n) << 26)
#define TC_CMD_EN      (1u << 24)
#define TC_DUMMY(n)    ((uint32_t)(n) << 17)
#define TC_DATA_EN     (1u << 16)

#define TRIG_START     (1u << 0)
#define TRIG_STOP      (1u << 1)
#define TRIG_FLUSH     (1u << 2)

#define SR_RECE_REQ    (1u << 2)
#define SR_END         (1u << 4)
#define SR_FIFO_NUM(v) (((v) >> 16) & 0x7f)

#define CPM_CLKGR      0xb0000020u
#define CPM_SSICDR     0xb0000074u
#define CDR_CE         (1u << 29)
#define CDR_BUSY       (1u << 28)
#define CDR_STOP       (1u << 27)

#define GPIO_A(off)    REG32(0xb0010000u + (off))

#define POLLS          5000000u

/* What the last steps saw, for tests: [0] last xfer's result, [1] its SR
 * at the end, [2] FSM, [3] bytes it got, [4] the status register's value */
uint32_t nand_debug[8];

/* One transfer: command, then address (width bytes), dummy clocks, data
 * (len bytes into buf, if any). 0 or < 0. */
/* A DMA bounce buffer in main memory (physical, used through kseg1): the
 * controller writes data there and the CPU copies it out uncached. Main
 * memory must be running. */
#ifdef NAND_BOUNCE_EXTERN
/* Lossless OS: a buffer of its own, its cache lines flushed and never
 * used cached (platform/h2/h2_stock.c) */
extern uint32_t nand_bounce_phys;
#define BOUNCE_PHYS nand_bounce_phys
#else
#define BOUNCE_PHYS 0x00e00000u
#endif
#define BOUNCE      ((volatile uint8_t *)(0xa0000000u | BOUNCE_PHYS))
#define BOUNCE_W    ((volatile uint32_t *)(0xa0000000u | BOUNCE_PHYS))
#define SENTINEL    0x5aa5c33cu

/* One transfer: command, then address (width bytes), dummy clocks, data
 * (len bytes into buf, if any), the data by DMA (manual 10.8.4): with the
 * CPU reading the FIFO (slave mode) transfers stalled now and then on the
 * H2. 0 or < 0. */
int nand_xfer(uint32_t cmd, uint32_t addr, uint32_t width, uint32_t dummy,
	      uint8_t *buf, uint32_t len)
{
	uint32_t polls = 0;

	/* briefly let the controller settle to idle (SR bits 6:5); after the
	 * stop and flush that end every transfer it may not report idle, and
	 * waiting the full timeout made each transfer half a second */
	while ((S(SFC_SR) & (3u << 5)) && ++polls < 2000)
		;
	/* DMA in bursts of 4 words (BURST_MD INCR4), whole bursts only */
	uint32_t need = (len + 3) / 4;                        /* words we use */
	uint32_t words = ((len + 15) / 16) * 4;
	S(SFC_SCR) = 0x1f;
	S(SFC_GLB) = GLB_THRESHOLD1 | GLB_PHASE1 | (1u << 6) | 0u;   /* read, DMA, INCR4 */
	S(SFC_TRAN_CONF0) = TC_CMD_EN | TC_ADDR(width) | TC_DUMMY(dummy) |
			    (len ? TC_DATA_EN : 0) | (cmd & 0xffff);
	S(SFC_DEV_ADR0) = addr;
	S(SFC_DEV_PLUS0) = 0;
	S(SFC_TRAN_LEN) = words * 4;
	S(SFC_MEM_ADDR) = len ? BOUNCE_PHYS : 0;
	if (len) {
		BOUNCE_W[need - 1] = SENTINEL;       /* overwritten when our data is in */
		__asm__ volatile("sync" ::: "memory");   /* out of the write buffer first */
	}
	S(SFC_TRIG) = TRIG_FLUSH;
	S(SFC_TRIG) = TRIG_START;

	for (polls = 0; !(S(SFC_SR) & SR_END); polls++) {
		if (polls > POLLS) {
			nand_debug[0] = 0xe2;
			nand_debug[1] = S(SFC_SR);
			nand_debug[2] = S(0x74);
			S(SFC_TRIG) = TRIG_STOP;
			S(SFC_TRIG) = TRIG_FLUSH;
			return -2;
		}
	}
	S(SFC_SCR) = 1u << 4;                                 /* clear END */
	/* "done" is the end of the flash transfer; the DMA may still be
	 * writing the last of the data to memory. Wait for idle (SR bits 6:5)
	 * before using it: stopping the controller right after "done" cut
	 * data short now and then. */
	for (polls = 0; (S(SFC_SR) & (3u << 5)) && polls < POLLS; polls++)
		;
	/* "done" and "idle" can come before the DMA's last writes reach
	 * memory (a short read once returned the previous read's data): wait
	 * until the last word is no longer the marker. Data that happens to
	 * equal it just costs the short timeout. */
	if (len)
		for (polls = 0; BOUNCE_W[need - 1] == SENTINEL && polls < 200000; polls++)
			;
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t i = 0; i < len; i++)
		buf[i] = BOUNCE[i];
	nand_debug[0] = 0;
	nand_debug[1] = S(SFC_SR);
	return 0;
}

/*
 * Waits on the X1000's timer, counter 2 at 24 MHz / 4 = 6 MHz (as Lossless
 * OS runs it: soc/ingenic/x1000/ost_timer.c; docs/h2-hardware.md, "Timer").
 * A counting loop ran several times faster from cached main memory than
 * from on-chip memory, so the flash's load time wasn't waited out. The
 * flash's status register (0x0f get feature) never completed through this
 * controller, so waits use the datasheet's maximum times instead of
 * polling for ready.
 */
#define OST_CTRL       0xb2000000u
#define OST_ENABLE     0xb2000004u
#define OST_CLEAR      0xb2000008u
#define OST_2CNTL      0xb2000020u
#define OST_ENABLE_SET 0xb2000034u
#define OST_ENABLE_CLR 0xb2000038u

static void timer_start(void)
{
	REG32(CPM_CLKGR) &= ~(1u << 20);                   /* OST clock on */
	if (!(REG32(OST_ENABLE) & 2u) || ((REG32(OST_CTRL) >> 2) & 3u) != 1u) {
		REG32(OST_ENABLE_CLR) = 2u;
		REG32(OST_CTRL) = (REG32(OST_CTRL) & ~(3u << 2)) | (1u << 2);   /* /4 */
		REG32(OST_CLEAR) = 2u;
		REG32(OST_ENABLE_SET) = 2u;
	}
}

static void wait_us(uint32_t us)
{
	uint32_t start = REG32(OST_2CNTL);

	while (REG32(OST_2CNTL) - start < us * 6u)
		;
}

int nand_init(uint32_t *id)
{
	uint8_t b[4] = {0};

	/* pins PA26-PA31 to the controller: INT 0, MSK 0, PAT1 0, PAT0 1 */
	uint32_t pins = 0x3fu << 26;
	GPIO_A(0x18) = pins;
	GPIO_A(0x28) = pins;
	GPIO_A(0x38) = pins;
	GPIO_A(0x44) = pins;

	/* clock: SCLK_A (1008 MHz) / 42 = 24 MHz into the controller */
	REG32(CPM_CLKGR) &= ~(1u << 2);
	REG32(CPM_SSICDR) = (REG32(CPM_SSICDR) & ~((1u << 31) | CDR_STOP | 0xffu)) | CDR_CE | 41u;
	for (uint32_t i = 0; i < 1000000 && (REG32(CPM_SSICDR) & CDR_BUSY); i++)
		;
	REG32(CPM_SSICDR) &= ~CDR_CE;

	timer_start();
	S(SFC_DEV_CONF) = 0x27;     /* reset value: CE, HOLD, WP idle high */
	S(SFC_INTC) = 0x1f;         /* no interrupts */
	/* Manual 10.7.3.16: "please disable all the clock gate function
	 * (clear all bits of SFC_CGE)". All on at reset; left on, the
	 * controller's FIFO and DMA stalled now and then. */
	S(SFC_CGE) = 0;

	if (nand_xfer(0xff, 0, 0, 0, 0, 0))              /* reset */
		return -1;
	wait_us(1000);                                   /* tRST: 500 us at most (both) */
	if (nand_xfer(0x9f, 0, 1, 0, b, 3))         /* JEDEC ID: the dummy byte as an address */
		return -3;
	*id = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
	return 0;
}

int nand_unlock(void)
{
	/* set feature 0x1f: the register and its value go out as a two-byte
	 * address (0xa0, then 0x00), so no data phase is needed */
	return nand_xfer(0x1f, 0xa0u << 8 | 0x00u, 2, 0, 0, 0);
}

/* One transfer out: command, address (width bytes), then len bytes of
 * data from buf by DMA (len a multiple of 16) */
static int xfer_out(uint32_t cmd, uint32_t addr, uint32_t width, const uint8_t *buf, uint32_t len)
{
	uint32_t polls = 0;

	while ((S(SFC_SR) & (3u << 5)) && ++polls < 2000)
		;
	for (uint32_t i = 0; i < len; i++)
		BOUNCE[i] = buf[i];
	__asm__ volatile("sync" ::: "memory");
	S(SFC_SCR) = 0x1f;
	S(SFC_GLB) = GLB_THRESHOLD1 | GLB_PHASE1 | GLB_WRITE | (1u << 6);   /* write, DMA, INCR4 */
	S(SFC_TRAN_CONF0) = TC_CMD_EN | TC_ADDR(width) | (len ? TC_DATA_EN : 0) | (cmd & 0xffff);
	S(SFC_DEV_ADR0) = addr;
	S(SFC_DEV_PLUS0) = 0;
	S(SFC_TRAN_LEN) = len;
	S(SFC_MEM_ADDR) = BOUNCE_PHYS;
	S(SFC_TRIG) = TRIG_FLUSH;
	S(SFC_TRIG) = TRIG_START;
	for (polls = 0; !(S(SFC_SR) & SR_END); polls++) {
		if (polls > POLLS) {
			S(SFC_TRIG) = TRIG_STOP;
			S(SFC_TRIG) = TRIG_FLUSH;
			return -2;
		}
	}
	S(SFC_SCR) = 1u << 4;                                 /* clear END */
	for (polls = 0; (S(SFC_SR) & (3u << 5)) && polls < POLLS; polls++)
		;
	return 0;
}

int nand_erase_block(uint32_t page)
{
	if (nand_xfer(0x06, 0, 0, 0, 0, 0))                    /* write enable */
		return -1;
	if (nand_xfer(0xd8, page & 0xffff, 3, 0, 0, 0))        /* block erase */
		return -2;
	wait_us(12000);                                        /* tBE: 10 ms at most (both) */
	return 0;
}

int nand_program_page(uint32_t page, const uint8_t *data)
{
	if (nand_xfer(0x06, 0, 0, 0, 0, 0))                    /* write enable */
		return -1;
	if (xfer_out(0x02, 0, 2, data, NAND_PAGE))             /* load, from column 0 */
		return -2;
	if (nand_xfer(0x10, page & 0xffff, 3, 0, 0, 0))        /* program execute */
		return -3;
	wait_us(1700);                                         /* tPP: 700 us at most, XTX 1400 */
	return 0;
}

/* len bytes from byte offset col of page into buf; *ecc: the status
 * register's ECC bits (0 clean, 1 corrected, 2/3 not). 0 or < 0. */
int nand_read_page(uint32_t page, uint32_t col, uint8_t *buf, uint32_t len, uint32_t *ecc)
{
	if (nand_xfer(0x13, page & 0xffff, 3, 0, 0, 0))  /* page data read: 8 dummy bits + page */
		return -1;
	wait_us(350);                                    /* tRD with ECC: 60 us at most, XTX 280 */
	if (ecc)
		*ecc = 0xff;                                 /* unknown: no status reads */
	if (nand_xfer(0x03, col << 8, 3, 0, buf, len))   /* read data: column, then the dummy byte */
		return -3;
	return 0;
}

/* A block is bad if the first byte of its first page's spare area (column
 * 2048) isn't 0xff (the usual SPI NAND marking, W25N01GV datasheet). */
int nand_block_bad(uint32_t block)
{
	uint8_t m[4];

	if (nand_read_page(block * NAND_BLOCK_PAGES, NAND_PAGE, m, 4, 0))
		return 1;
	return m[0] != 0xff;
}

int nand_read(uint32_t start, uint32_t end, uint8_t *buf, uint32_t len)
{
	const uint32_t block_bytes = NAND_PAGE * NAND_BLOCK_PAGES;
	uint32_t at = start, block = ~0u;

	while (len) {
		if (at >= end)
			return -1;                      /* past the partition */
		if (at / block_bytes != block) {
			block = at / block_bytes;
			if (nand_block_bad(block)) {
				at = (block + 1) * block_bytes;   /* skip it */
				continue;
			}
		}
		uint32_t page = at / NAND_PAGE, col = at % NAND_PAGE;
		uint32_t n = NAND_PAGE - col;
		if (n > len)
			n = len;
		if (nand_read_page(page, col, buf, n, 0))
			return -2;
		buf += n;
		at += n;
		len -= n;
	}
	return 0;
}

