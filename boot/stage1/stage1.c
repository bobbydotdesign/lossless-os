/*
 * Our first stage, step by step. Over USB boot it hands back to the ROM
 * when done, and the status block says how far it got.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "stage1.h"
#if defined(NAND_TEST) || defined(FLASH_BOOT)
#include "nand.h"
#endif
#if defined(PMU_READ) || defined(POWER_ON)
#include "x1000_i2c.h"
#endif

void delay_us(uint32_t us)
{
	/* About 59 turns a microsecond at 1008 MHz, running from on-chip
	 * memory (measured: a million microseconds' worth at 1008 a
	 * microsecond took 17.0 s). 64 errs long; at the ROM's 576 MHz,
	 * before the clocks are set, delays run about twice as long. */
	for (volatile uint32_t i = us * 64u; i; i--)
		;
}

/* GPIO port n's registers (docs/h2-hardware.md, "GPIO"): INT, MSK, PAT1,
 * PAT0 at 0x10/0x20/0x30/0x40, each with set +4 and clear +8 */
#define GPIO(port, off) REG(0xb0010000u + (port) * 0x100u + (off))

/*
 * The card slot, as the player runs it (pin registers read under the
 * current bootloader, 2026-10-31): card power PB10 a plain output driven
 * high, then the card controller's pins PA20-PA25 on function 1 (INT 0,
 * MSK 0, PAT1 0, PAT0 1). Lossless OS's card driver expects both.
 */
static void card_slot(void)
{
	uint32_t pb10 = 1u << 10, msc = 0x3fu << 20;

	GPIO(1, 0x18) = pb10;      /* INT clear */
	GPIO(1, 0x24) = pb10;      /* MSK set: plain GPIO */
	GPIO(1, 0x38) = pb10;      /* PAT1 clear: output */
	GPIO(1, 0x44) = pb10;      /* PAT0 set: high, card powered */
	delay_us(10000);
	GPIO(0, 0x18) = msc;       /* INT clear */
	GPIO(0, 0x28) = msc;       /* MSK clear: function */
	GPIO(0, 0x38) = msc;       /* PAT1 clear */
	GPIO(0, 0x44) = msc;       /* PAT0 set: function 1 */
}

#ifdef FLASH_BOOT
/*
 * The second stage, from flash: at 0x4000, with a copy at 0x20000 (the
 * next block). Each is a 64-byte header ("LOS2", version 1, load address,
 * entry, size, CRC-32 of the image; tools/mkbootimage.py), then the
 * image, which goes to its load address through the uncached window
 * (caches aren't set up yet; the second stage does that first) and is
 * started there.
 */
static const uint32_t stage2_at[] = {0x4000u, 0x24000u};

static int same4(const uint8_t *a, const char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32_bits(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static void start_stage2(void)
{
	uint32_t id;

	status.step = 5;
	crumb(0x50);
	if (nand_init(&id)) {
		crumb(0x5e);
		for (;;)
			;
	}
	for (unsigned i = 0; i < 2; i++) {
		uint8_t h[64];
		if (nand_read(stage2_at[i], 0x80000u, h, 64) || !same4(h, "LOS2") ||
		    le32(h + 4) != 1)
			continue;
		uint32_t load = le32(h + 8), entry = le32(h + 12), size = le32(h + 16);
		if (load < 0x80000000u || size > 0x40000u)
			continue;
		uint8_t *dst = (uint8_t *)(load | 0x20000000u);          /* uncached */
		if (nand_read(stage2_at[i] + 64, 0x80000u, dst, size) ||
		    crc32_bits(dst, size) != le32(h + 20))
			continue;
		crumb((uint8_t)(0x51 + i));
		((void (*)(void))(entry | 0x20000000u))();
	}
	crumb(0x5f);         /* neither copy checks out */
	for (;;)
		;
}
#endif

void stage1_main(void)
{
	status.magic = STATUS_MAGIC;
	status.error = 0;
	status.n = 0;

	/* The watchdog (manual ch. 15, 0xb0002000): off under the ROM in USB
	 * boot mode (TCER read 0); make sure of it from flash too. */
	*(volatile uint8_t *)0xb0002004u = 0;

#ifdef NAND_TEST
	/* Flash test: the ID, page 0's first 16 bytes (the boot ROM's header)
	 * and the original firmware's kernel header (the uImage at 0x300000) */
	{
		uint32_t id = 0, ecc = 0, w[16];
		status.step = 0x70;
		int e = clocks_init();
		if (e) {
			status.error = 0x7000u + (uint32_t)e;
			return;
		}
		e = nand_init(&id);
		note(id);
		if (e) {
			note(nand_debug[0]);
			note(nand_debug[1]);
			status.error = 0x7100u + (uint32_t)-e;
			return;
		}
		status.step = 0x71;
		e = nand_read_page(0, 0, (uint8_t *)w, 16, &ecc);
		if (e) {
			status.error = 0x7200u + (uint32_t)-e;
			return;
		}
		note(ecc);
		for (int i = 0; i < 4; i++)
			note(w[i]);
		status.step = 0x72;
#ifdef NAND_HEADERS
		/* our second stage's two headers (0x4000, 0x24000): which one is
		 * in flash (its size and CRC-32); into on-chip memory, no DDR */
		for (int i = 0; i < 2; i++) {
			e = nand_read(i ? 0x24000u : 0x4000u, 0x80000u, (uint8_t *)w, 64);
			if (e) {
				status.error = 0x7600u + (uint32_t)-e;
				return;
			}
			for (int k = 0; k < 6; k++)
				note(w[k]);
		}
		status.step = 0x73;
		return;
#endif
		/* the whole kernel, twice: its header, then 2.4 MB into memory
		 * (uncached at 0xa1000000), CRC-32 against the header's */
		for (int pass = 0; pass < 2; pass++) {
			uint8_t *k = (uint8_t *)0xa1000000u;
			e = nand_read(0x300000u, 0x900000u, k, 64);
			if (e) {
				status.error = 0x7300u + (uint32_t)-e;
				return;
			}
			uint32_t size = (uint32_t)k[12] << 24 | (uint32_t)k[13] << 16 | (uint32_t)k[14] << 8 | k[15];
			uint32_t dcrc = (uint32_t)k[24] << 24 | (uint32_t)k[25] << 16 | (uint32_t)k[26] << 8 | k[27];
			note((uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | k[3]);
			note(size);
			if (size > 0x600000u) {
				status.error = 0x7400;
				return;
			}
			e = nand_read(0x300000u, 0x900000u, k, 64 + size);
			if (e) {
				status.error = 0x7500u + (uint32_t)-e;
				return;
			}
			uint32_t c = 0xffffffffu;
			for (uint32_t i = 0; i < size; i++) {
				c ^= k[64 + i];
				for (int b = 0; b < 8; b++)
					c = (c >> 1) ^ (0xedb88320u & -(c & 1));
			}
			note(~c);
			note(dcrc);
		}
		status.step = 0x73;
		return;
	}
#endif
#ifdef DELAYTEST
	/* delay_us(1 s), to time against the computer */
	status.step = 0x60;
	delay_us(1000000);
	status.step = 0x61;
	return;
#endif
#ifdef REGS
	/* Every register the player's own dump has, after our set-up, for a
	 * side-by-side: CPM, controller AHB and APB groups, PHY */
	status.step = 0x40;
	static const uint32_t ranges[][2] = {
		{0xb0000000u, 0xb00000fcu}, {0xb34f0000u, 0xb34f007cu}, {0xb34f009cu, 0xb34f00bcu},
		{0xb34f0100u, 0xb34f0118u}, {0xb34f0304u, 0xb34f030cu}, {0xb3012000u, 0xb301208cu},
		{0xb3011000u, 0xb301105cu}, {0xb3011180u, 0xb301118cu}, {0xb30111c0u, 0xb30111d4u},
		{0xb3011200u, 0xb3011214u},
	};
	for (unsigned r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++)
		for (uint32_t a = ranges[r][0]; a <= ranges[r][1]; a += 4)
			note(REG(a));
	status.step = 0x41;
	return;
#endif
#ifdef REGS2
	/* The controller's AHB-group registers read three ways: plainly, with
	 * a sync before, and twice in a row (keeping the second) */
	status.step = 0x50;
	static const uint32_t offs[] = {0x04, 0x08, 0x18, 0x24, 0x28, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74};
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
		uint32_t a = 0xb34f0000u + offs[i];
		note(REG(a));
		__asm__ volatile("sync" ::: "memory");
		note(REG(a));
		(void)REG(a);
		note(REG(a));
	}
	status.step = 0x51;
	return;
#endif
#ifdef TOUCH
	/* Read one word of memory (uncached) and the controller's state */
	status.step = 0x30;
	note(REG(0xb0000020u));                         /* CLKGR */
	note(REG(0xb34f0000u));                         /* DSTATUS */
	note(REG(0xb301206cu));                         /* DDRC_STATUS */
	note(*(volatile uint32_t *)0xa0000000u);
	status.step = 0x31;
	return;
#endif
#ifdef FRAME_ONLY
	status.step = 0x0f;    /* the frame alone: entry, stack, status, hand-back */
	note(0x12345678u);
	return;
#endif
#ifdef FLASH_BOOT
	/* started from flash: the clocks as the ROM left them (CPCCR,
	 * CPAPCR, CPMPCR top bytes) */
	crumbs_on();
	crumb4(0x01, (uint8_t)(REG(0xb0000000u) >> 24), (uint8_t)(REG(0xb0000010u) >> 24),
	       (uint8_t)(REG(0xb0000014u) >> 24));
#endif
	status.step = 1;    /* clocks */
	int err = clocks_init();
	if (err) {
		status.error = 0x100u + (uint32_t)err;
#ifdef FLASH_BOOT
		crumbs_on();
		crumb4(0x0e, (uint8_t)err, (uint8_t)(REG(0xb0000000u) >> 24),
		       (uint8_t)(REG(0xb0000010u) >> 24));
		for (;;)
			;
#endif
		return;
	}
#ifdef FLASH_BOOT
	crumbs_on();
	crumb(0x02);         /* clocks set */
#endif

#ifdef PMU_READ
	/* The power chip's 256 registers (AXP2101, I2C bus 2, address 0x34),
	 * four to a word */
	status.step = 0x10;
	if (!x1000_i2c_init(2)) {
		status.error = 0x1001;
		return;
	}
	for (int reg = 0; reg < 256; reg += 4) {
		uint8_t b[4];
		if (x1000_i2c_read(2, 0x34, (uint8_t)reg, b, 4)) {
			status.error = 0x1100u + (uint32_t)reg;
			return;
		}
		note((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
		     (uint32_t)b[3] << 24);
	}
	status.step = 0x11;
	return;
#endif
#ifdef POWER_ON
	status.step = 0x20;    /* power */
	err = power_init();
	if (err) {
		status.error = 0x2000u + (uint32_t)err;
		return;
	}
#ifdef POWER_ONLY
	status.step = 0x21;
	return;
#endif
#endif
	crumb(0x20);
	status.step = 2;    /* memory */
	err = memory_init();
	if (err) {
		status.error = 0x200u + (uint32_t)err;
		return;
	}

	crumb(0x30);
	status.step = 3;    /* memory test */
	err = memory_test();
	if (err) {
		status.error = 0x300u + (uint32_t)err;
		return;
	}

	crumb(0x40);
	card_slot();
	status.step = 4;    /* done */
#ifdef FLASH_BOOT
	start_stage2();      /* doesn't come back */
#endif
}

/* The compiler may call this for struct copies */
void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}
