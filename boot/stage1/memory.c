/*
 * Memory: the X1000's 32 MB of in-package LPDDR (256 Mbit x16 Mobile DDR:
 * 13-bit rows, 9-bit columns, 4 banks), at 201.6 MHz (APLL 1008 / 5).
 *
 * Derivation and sources: research/own-bootloader/ddr.md (private notes).
 *   Controller: X1000 Programming Manual ch. 9 (fields, 9.3.1 init order).
 *   Memory interface (PHY): a standard DDR PHY utility block, documented
 *   bit by bit in Rockchip's RK30xx TRM, ch. 13.
 *   Timings: JEDEC Mobile DDR and public 256 Mbit LPDDR datasheets (-5
 *   grade), at tCK = 4.96 ns, rounded up.
 * Measured on the player, used where nothing can be derived: CAS latency
 * 4, DCFG bits 5:2, the controller's address-map base (DMMAP0 0x20fe),
 * CPM_DRCG bits 6:4, the PHY's DLL bias trims (DLLGCR), PGCR's ZQ clock
 * select, DXCCR bit 14.
 *
 * The PHY powers the chip up itself (200 us, precharge, 2 refreshes, MR,
 * EMR) and trains the read strobe gate; all its DLLs run in bypass, as
 * the manual says below 9.4.3's limit.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "stage1.h"

/* The waits below are generous (100 us where the documents ask for 50 ns
 * to 1 us): with the minimums, reads failed now and then; every passing
 * run had long waits. */

/* Test builds stop early and hand back: 1 before the PHY's own start-up,
 * 2 after it, 3 after one word in memory, 4 after the address lines. */
#ifndef STOP_AT
#define STOP_AT 0
#endif

#define CLKGR     0xb0000020u
#define DDRCDR    0xb000002cu
#define CPM_DRCG  0xb00000d0u
#define PHYRST    0xb3012080u

#define DDRC(o)   (0xb34f0000u + (o))
#define DSTATUS   DDRC(0x00)
#define DCFG      DDRC(0x04)
#define DCTRL     DDRC(0x08)
#define DREFCNT   DDRC(0x18)
#define DMMAP0    DDRC(0x24)
#define DMMAP1    DDRC(0x28)
#define DTIMING(n) DDRC(0x60 + 4 * ((n) - 1))

#define PHY(o)    (0xb3011000u + (o))
#define PIR       PHY(0x004)
#define PGCR      PHY(0x008)
#define PGSR      PHY(0x00c)
#define DLLGCR    PHY(0x010)
#define ACDLLCR   PHY(0x014)
#define PTR0      PHY(0x018)
#define PTR1      PHY(0x01c)
#define ACIOCR    PHY(0x024)
#define DXCCR     PHY(0x028)
#define DSGCR     PHY(0x02c)
#define DCR       PHY(0x030)
#define DTPR(n)   PHY(0x034 + 4 * (n))
#define MR(n)     PHY(0x040 + 4 * (n))
#define ODTCR     PHY(0x050)
#define DTAR      PHY(0x054)
#define ZQ0SR0    PHY(0x188)
#define DXGCR(n)  PHY(0x1c0 + 0x40 * (n))
#define DXGSR0(n) PHY(0x1c4 + 0x40 * (n))
#define DXDQSTR(n) PHY(0x1d4 + 0x40 * (n))

#define PIR_INIT     (1u << 0)
#define PIR_ZCAL     (1u << 3)
#define PIR_ITMSRST  (1u << 4)
#define PIR_DRAMINIT (1u << 6)
#define PIR_QSTRN    (1u << 7)
#define PIR_DLLBYP   (1u << 17)
#define PIR_LOCKBYP  (1u << 29)

#define PGSR_IDONE   (1u << 0)
#define PGSR_ERRORS  ((1u << 5) | (1u << 6) | (1u << 7))

struct ddr_settings {
	uint32_t dcfg, dtiming[6], drefcnt;
	uint32_t ptr0, ptr1, dtpr[3], mr0, mr2;
};

/* Ours, derived (ddr.md section 3, column A) */
static const struct ddr_settings derived = {
	/* LPDDR, 13-bit rows, 9-bit columns, 4 banks, 16 bits, CS0; bits 5:2
	 * = 1011b as measured: the manual calls them unused, but with 0 the
	 * first read from memory never finished */
	.dcfg = 0x0006896c,
	.dtiming = {0x02050401, 0x02090404, 0x2704030d, 0x07790121, 0xff070200, 0x070c0505},
	.drefcnt = 0x002f0003,  /* refresh every 7.62 us */
	.ptr0 = 0x0021024b, .ptr1 = 0x02889d80,
	.dtpr = {0x1a69444a, 0x00100090, 0x10010819},
	.mr0 = 0x42,            /* burst 4, sequential, CAS latency 4 */
	.mr2 = 0x80,            /* EMR: three-quarter drive, full array */
};

#ifdef MEMORY_SET
/* Test builds: the player's measured values, alone or in part, to find
 * which of ours is wrong. 1: all measured. 2: ours with the controller's
 * timings measured. 3: ours with the PHY's timings and mode registers
 * measured. */
static const struct ddr_settings measured = {
	.dcfg = 0x0a46896c,
	.dtiming = {0x02050501, 0x04090404, 0x2704030d, 0x0b7a0251, 0xff090200, 0x0a0a0202},
	.drefcnt = 0x002f1e03,
	.ptr0 = 0x0021000a, .ptr1 = 0x00a09c40,
	.dtpr = {0x1a69444a, 0x00180090, 0x1ff99428},
	.mr0 = 0x42, .mr2 = 0x98,
};
static struct ddr_settings ddr;

static void pick_settings(void)
{
	ddr = derived;
	if (MEMORY_SET == 1)
		ddr = measured;
	if (MEMORY_SET == 2)
		for (int i = 0; i < 6; i++)
			ddr.dtiming[i] = measured.dtiming[i];
	if (MEMORY_SET == 3) {
		ddr.ptr0 = measured.ptr0;
		ddr.ptr1 = measured.ptr1;
		for (int i = 0; i < 3; i++)
			ddr.dtpr[i] = measured.dtpr[i];
		ddr.mr2 = measured.mr2;
	}
	if (MEMORY_SET >= 4 && MEMORY_SET <= 6)
		ddr = measured;
	note(0x5e700000u | MEMORY_SET);
}
#else
#define ddr derived
static void pick_settings(void) {}
#endif

static int wait_bits(uint32_t reg, uint32_t mask, uint32_t want, uint32_t tries)
{
	while (tries--) {
		if ((REG(reg) & mask) == want)
			return 0;
	}
	return -1;
}

/* 0 or an error code; notes PHY results in the status block */
int memory_init(void)
{
	pick_settings();
#ifdef TUNE
	return 0;    /* gate tests run after a set-up: memory is running */
#endif

	/* 0. Ungate the memory controller's clock (CLKGR bit 31; manual
	 * 11.2.2.6: while gated its registers can't be accessed). The ROM
	 * leaves it gated in USB boot mode (CLKGR read 0x87fbfff4). */
	note(REG(CLKGR));
	REG(CLKGR) &= ~(1u << 31);

	/* 1. The memory clock: APLL / 5, with the change enable, then off.
	 * Skipped if it's already that (a second run over USB boot: the
	 * change never finishes while memory is running). */
#if defined(MEMORY_SET) && MEMORY_SET == 5
	const uint32_t div = 5;    /* test: 1008 / 6 = 168 MHz */
#else
	const uint32_t div = 4;    /* 1008 / 5 = 201.6 MHz */
#endif
	if ((REG(DDRCDR) & 0xc000000fu) != (0x40000000u | div)) {
		REG(DDRCDR) = 0x64000000u | div;
		if (wait_bits(DDRCDR, 1u << 28, 0, 1000000))
			return 1;
		REG(DDRCDR) = 0x44000000u | div;
		delay_us(100);
	}

#ifdef DIAG
	/* Diagnostic: the controller's registers as the chip starts, before
	 * we write any (APB group 0x00-0x8c, AHB 0x54-0x5c, 0xb4-0xbc) */
	for (uint32_t o = 0; o <= 0x8c; o += 4)
		note(REG(0xb3012000u + o));
	for (uint32_t o = 0x54; o <= 0x5c; o += 4)
		note(REG(DDRC(o)));
	for (uint32_t o = 0xb4; o <= 0xbc; o += 4)
		note(REG(DDRC(o)));
	note(REG(DCTRL));
#endif
	/* 2. Reset pulse for the controller and PHY */
	REG(PHYRST) = 0x01e00001u;
	delay_us(100);
	REG(PHYRST) = 0;
	delay_us(100);

	/* 3. DLLs into bypass (manual 9.4.3; ACDLLCR bit 31 per the PHY
	 * docs), holding them in reset meanwhile */
	REG(CPM_DRCG) = 0x73u;
	delay_us(100);
	REG(PIR) = PIR_DLLBYP | PIR_LOCKBYP;
	REG(ACDLLCR) = 0x80000000u;
	/* The PHY's DLL bias trims, as measured on the player (SBIAS 0x3f,
	 * reset 0x37): analog values no document gives. With the reset value,
	 * byte lane 1's strobe delay is off and about 0.3% of its reads flip
	 * a bit at 201.6 MHz; with 0x3f, none. */
	REG(DLLGCR) = 0x03f37000u;
	delay_us(100);
	REG(CPM_DRCG) = 0x71u;
	delay_us(100);

	/* 4. Controller (manual 9.3.1 steps 4-7) */
	REG(DCFG) = ddr.dcfg;
	for (int i = 0; i < 6; i++)
		REG(DTIMING(i + 1)) = ddr.dtiming[i];
	REG(DMMAP0) = 0x000020feu;
	REG(DMMAP1) = 0x0000ff00u;

	/* 5. PHY: LPDDR mode, CMOS I/O, timings, mode registers */
	REG(DCR) = 0;
	REG(PGCR) = 0x01042e03u;
	REG(ACIOCR) = 0x30400813u;
	REG(DXCCR) = 0x00004912u;
	REG(DSGCR) = 0xfa00000fu;
	REG(PTR0) = ddr.ptr0;
	REG(PTR1) = ddr.ptr1;
	for (int i = 0; i < 3; i++)
		REG(DTPR(i)) = ddr.dtpr[i];
	REG(MR(0)) = ddr.mr0;
	REG(MR(1)) = 0;
	REG(MR(2)) = ddr.mr2;
	REG(MR(3)) = 0;
	REG(ODTCR) = 0;
	REG(DTAR) = 0x00150000u;
	REG(DXGCR(0)) = 0x881u;
	REG(DXGCR(1)) = 0x881u;

#if STOP_AT == 1
	note(REG(PGSR));
	note(REG(DCFG));
	note(REG(DSTATUS));
	return 0x80;
#endif
	/* 6. The PHY calibrates, powers the chip up and trains */
	REG(PIR) = PIR_INIT | PIR_ZCAL | PIR_ITMSRST | PIR_DRAMINIT | PIR_QSTRN | PIR_DLLBYP;
	delay_us(100);
	int timeout = wait_bits(PGSR, PGSR_IDONE, PGSR_IDONE, 50000000);
	note(REG(PGSR));
	note(REG(ZQ0SR0));
	note(REG(DXGSR0(0)));
	note(REG(DXGSR0(1)));
	note(REG(DXDQSTR(0)));
	note(REG(DXDQSTR(1)));
	if (timeout)
		return 2;
	if (REG(PGSR) & PGSR_ERRORS)
		return 3;
#if STOP_AT == 2
	return 0x80;
#endif

	/* 7. CKE to the controller, then refresh; then a moment before the
	 * first access */
	REG(DCTRL) = 0x2u;
	REG(DREFCNT) = ddr.drefcnt;
	delay_us(1000);
	note(REG(DSTATUS));
	return 0;
}

/*
 * Memory test through the uncached window (0xa0000000): the data lines,
 * then every address line, then all 32 MB. 0, or an error code with the
 * failing address and value noted.
 */
#define RAM ((volatile uint32_t *)0xa0000000u)
#define WORDS (32u * 1024 * 1024 / 4)

static int fail(uint32_t i, uint32_t want, uint32_t got)
{
	note(0xa0000000u + 4 * i);
	note(want);
	note(got);
	return -1;
}

#if defined(DIAG) || defined(TUNE)
/* 4096 distinct words, each read back four times: how many reads were
 * wrong, which bits flipped (OR of all differences), and the first 4 */
static uint32_t pattern(uint32_t i)
{
	return (i * 0x9e3779b9u) ^ (i << 24) ^ 0x01020304u;
}

static void diag_test(void)
{
	const uint32_t words = 4096;
	for (uint32_t i = 0; i < words; i++)
		RAM[i] = pattern(i);
	uint32_t bad = 0, flips = 0;
	for (int pass = 0; pass < 4; pass++)
		for (uint32_t i = 0; i < words; i++) {
			uint32_t got = RAM[i], want = pattern(i);
			if (got != want) {
				if (bad < 4) {
					note(i);
					note(want);
					note(got);
					note(pass);
				}
				bad++;
				flips |= got ^ want;
			}
		}
	note(0xbad00000u | bad);
	note(flips);
}
#endif

#ifdef TUNE
/* One setting of byte lane 1's read gate (DX1DQSTR: R0DGSL 2:0, R0DGPS
 * 13:12, DQSDLY 22:20) per run, with memory left running: the computer
 * patches the setting in ("TUNE", mask, value), this counts wrong reads of
 * 4096 words read twice, and puts the trained value back.
 * Notes: DX1DQSTR as found, then as tried, then wrong reads. */
__attribute__((section(".data.params"), used))
static volatile struct {
	uint32_t magic, mask, value, write, addr;
} tune_params = {0x454e5554u, 0, 0, 1, 0};

static uint32_t lane1_errors(void)
{
	uint32_t bad = 0;
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < 4096; i++)
			if (RAM[i] != pattern(i))
				bad++;
	return bad;
}

static void tune(void)
{
	const uint32_t reg = tune_params.addr ? tune_params.addr : DXDQSTR(1), found = REG(reg);
	note(found);
	if (tune_params.write)
		for (uint32_t i = 0; i < 4096; i++)
			RAM[i] = pattern(i);
	REG(reg) = (found & ~tune_params.mask) | tune_params.value;
	note(REG(reg));
	note(lane1_errors());
	if (!tune_params.addr)
		REG(reg) = found;    /* gate tests put lane 1 back; others stay */
}
#endif

int memory_test(void)
{
#ifdef TUNE
	tune();
	return 0x80;
#endif
#ifdef DIAG
	diag_test();
	return 0x80;
#endif
#if STOP_AT == 3
	RAM[0] = 0x12345678u;
	RAM[1] = 0x9abcdef0u;
	note(RAM[0]);
	note(RAM[1]);
	return RAM[0] == 0x12345678u && RAM[1] == 0x9abcdef0u ? 0x80 : 4;
#endif
	/* data lines: walking one, walking zero */
	for (int b = 0; b < 32; b++) {
		uint32_t v = 1u << b;
		RAM[0] = v;
		RAM[1] = ~v;
		if (RAM[0] != v)
			return fail(0, v, RAM[0]) ? 1 : 0;
		RAM[0] = ~v;
		if (RAM[0] != ~v)
			return fail(0, ~v, RAM[0]) ? 1 : 0;
	}
	/* address lines: a distinct word at every power of two */
	RAM[0] = 0xa5a5a5a5u;
	for (uint32_t i = 1; i < WORDS; i <<= 1)
		RAM[i] = i;
	if (RAM[0] != 0xa5a5a5a5u)
		return fail(0, 0xa5a5a5a5u, RAM[0]) ? 2 : 0;
	for (uint32_t i = 1; i < WORDS; i <<= 1)
		if (RAM[i] != i)
			return fail(i, i, RAM[i]) ? 2 : 0;
#if STOP_AT == 4
	return 0x80;
#endif
	/* everything (test builds: the first FILL_MB): a pattern that differs
	 * at every word */
#ifdef FILL_MB
	const uint32_t words = FILL_MB * 1024u * 1024 / 4;
#else
	const uint32_t words = WORDS;
#endif
	for (uint32_t i = 0; i < words; i++)
		RAM[i] = i * 0x9e3779b9u ^ 0x5a5a5a5au;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t want = i * 0x9e3779b9u ^ 0x5a5a5a5au;
		if (RAM[i] != want)
			return fail(i, want, RAM[i]) ? 3 : 0;
	}
	return 0;
}
