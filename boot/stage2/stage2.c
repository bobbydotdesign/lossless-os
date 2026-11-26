/*
 * Our second stage: from the card, /.lossless/boot/lossless.bin (made by
 * tools/mklosboot.py: a 64-byte header, then the image), checked, loaded
 * and started.
 *
 * The header: "LOSB", version 1, load address, entry point, image size,
 * CRC-32 of the image, OS version. The image must go inside Lossless OS's
 * memory (0x81000000 up to 0x82000000) and match its CRC.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "ff.h"
#include "stage2.h"
#include "nand.h"
#include "inflate.h"
#include "x1000_i2c.h"
#include "x1000_soc.h"
#include "draw.h"
#include <stdbool.h>

/* Lossless OS's boot file (app/bootfile.h, tools/mklosboot.py): the
 * installed one, else the release copied to the card */
#define BOOT_FILE    "/.lossless/boot/lossless.bin"
#define BOOT_PACKAGE "/Lossless/lossless.bin"
#define OS_START     0x81000000u
#define OS_END       0x81e00000u   /* our own code starts here */

void delay_us(uint32_t us)
{
	/* at 1008 MHz, cached: a few cycles a turn; errs long */
	for (volatile uint32_t i = us * 300u; i; i--)
		;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return 0;
	}
}


struct boot_header {
	char magic[4];
	uint32_t version, load, entry, size, crc, os_version;
	uint32_t reserved[9];
};

static FATFS fs;
static FIL file;

/* Data cache back to memory and instruction cache emptied for a range */
static void sync_caches(uint32_t start, uint32_t len)
{
	for (uint32_t a = start & ~31u; a < start + len; a += 32)
		__asm__ volatile("cache 0x15, 0(%0)" : : "r"(a));   /* Hit_Writeback_Inv_D */
	__asm__ volatile("sync");
	for (uint32_t a = start & ~31u; a < start + len; a += 32)
		__asm__ volatile("cache 0x10, 0(%0)" : : "r"(a));   /* Hit_Invalidate_I */
}

/* What to start. MODE_AUTO (a real boot): the keys and the original
 * firmware's update request decide (choose() below). Tests over USB boot
 * may patch another mode in ("MODE", then the mode). */
enum { MODE_AUTO, MODE_NAND_TEST, MODE_PLAYER, MODE_RECOVERY, MODE_PLAYER_DRY, MODE_RETURN, MODE_LOSSLESS, MODE_FLASH };

__attribute__((section(".data.params"), used))
static volatile struct {
	uint32_t magic, mode;
} params = {0x45444f4du, MODE_AUTO};

/* Flash test: the ID, page 0's first 16 bytes (the boot ROM's header),
 * and the original firmware's kernel header (the uImage at 0x300000) */
static void nand_test(void)
{
	uint32_t id = 0, ecc = 0, w[16];

	status.step = 0x10;
	int err = nand_init(&id);
	note(id);
	if (err) {
		status.error = 0x1000u + (uint32_t)-err;
		return;
	}
	status.step = 0x11;
	err = nand_read_page(0, 0, (uint8_t *)w, 16, &ecc);
	if (err) {
		status.error = 0x1100u + (uint32_t)-err;
		return;
	}
	note(ecc);
	for (int i = 0; i < 4; i++)
		note(w[i]);
	status.step = 0x12;
	err = nand_read_page(0x300000u / NAND_PAGE, 0, (uint8_t *)w, 64, &ecc);
	if (err) {
		status.error = 0x1200u + (uint32_t)-err;
		return;
	}
	note(ecc);
	for (int i = 0; i < 16; i++)
		note(w[i]);
	status.step = 0x13;
}

/*
 * The original firmware: its kernel from flash, unpacked, started the way
 * a MIPS Linux kernel expects (a0 = argc, a1 = argv, a2 = envp; argv[0]
 * unused, the command line in argv[1]).
 *
 * Flash layout and command lines: the original firmware's own (read from
 * its kernel's partition table and its bootloader's settings; research
 * notes, b-binaries.md): the player's kernel at 0x300000 (6 MB), its
 * recovery kernel at 0x900000 (7 MB). Each is a legacy uImage: a 64-byte
 * big-endian header (magic 0x27051956, header CRC, time, data size, load,
 * entry, data CRC, os, arch, type, compression), then gzip data.
 */
#define PLAYER_KERNEL   0x300000u
#define PLAYER_END      0x900000u
#define RECOVERY_KERNEL 0x900000u
#define RECOVERY_END    0x1000000u
#define PACKED          0x81000000u    /* the packed kernel, while unpacking */

/* Test builds may add to the player's command line (e.g. " panic=3": a
 * fatal error then restarts the chip, which shows how far it got) */
#ifndef PLAYER_ARGS_EXTRA
#define PLAYER_ARGS_EXTRA ""
#endif

static const char player_args[] =
	"console=ttyS2,115200n8 mem=32M@0x0 no_console_suspend lpj=5009408 ip=off "
	"init=/linuxrc ubi.mtd=4 root=ubi0:rootfs ubi.mtd=5 rootfstype=ubifs "
	"sn_no=00000000000000000000000000000000 bt_mac=xxxxxxxxxxxx wifi_mac=xxxxxxxxxxxx rw"
	PLAYER_ARGS_EXTRA;
static const char recovery_args[] = "console=ttyS2,115200n8 mem=32M@0x0 no_console_suspend lpj=5009408 ip=off";

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Breadcrumbs in the power chip's data buffer (AXP2101 regs 0x04-0x07,
 * cleared only by a complete loss of power): step, error, and two bytes
 * of detail. The second stage can't hand back to the USB boot ROM (its
 * "start" request for main memory doesn't return), so this is how a
 * test reads where it got to: on the next start, before anything else.
 */
static int crumbs_on;

/* the Play and Vol+ pins as choose() read them, kept in the breadcrumbs */
static uint16_t keys_seen;

static void crumb(uint8_t step, uint8_t err, uint16_t detail)
{
	if (!crumbs_on)
		crumbs_on = x1000_i2c_init(2) ? 1 : -1;
	if (crumbs_on < 0)
		return;
	x1000_i2c_write(2, 0x34, 0x04, step);
	x1000_i2c_write(2, 0x34, 0x05, err);
	x1000_i2c_write(2, 0x34, 0x06, (uint8_t)detail);
	x1000_i2c_write(2, 0x34, 0x07, (uint8_t)(detail >> 8));
}

/* For tests: catch an early exception after the jump (see trap_entry in
 * start.S). Breadcrumb: 0xee, exception code | EPC bits 22:20 << 5, EPC
 * bits 19:4. */
void trap_entry(void);

void trap_report(uint32_t cause, uint32_t epc, uint32_t bad)
{
	(void)bad;
	crumbs_on = 0;     /* the I2C controller again, from scratch */
	crumb(0xee, (uint8_t)(((cause >> 2) & 0x1f) | ((epc >> 20) & 7) << 5),
	      (uint16_t)(epc >> 4));
}

#ifdef TRAP_TEST
static void install_trap(void)
{
	uint32_t h = (uint32_t)(uintptr_t)trap_entry;
	static const uint32_t vec[] = {0x80000000u, 0x80000180u, 0x80000200u};

	for (unsigned i = 0; i < 3; i++) {
		volatile uint32_t *v = (volatile uint32_t *)vec[i];
		v[0] = 0x3c1a0000u | (h >> 16);          /* lui k0, hi */
		v[1] = 0x375a0000u | (h & 0xffff);       /* ori k0, k0, lo */
		v[2] = 0x03400008u;                      /* jr k0 */
		v[3] = 0;                                /* nop */
	}
	sync_caches(0x80000000u, 0x220);
	__asm__ volatile(
		"li $t0, 0x80000000\n\tmtc0 $t0, $15, 1\n\t"   /* EBase */
		"mfc0 $t0, $12\n\tli $t1, ~(1 << 22)\n\tand $t0, $t0, $t1\n\t"
		"mtc0 $t0, $12\n\tehb" ::: "t0", "t1");          /* BEV off */
}
#endif

static char *linux_argv[3];

bool h2_lcd_start(void);
static void clocks_for_linux(void);
static void screen_for_linux(void);
static void cpu_for_linux(void);

static void start_linux(uint32_t start, uint32_t end, const char *args, int dry)
{
	uint8_t *packed = (uint8_t *)PACKED;
	uint32_t err;

	status.step = 0x20;    /* flash */
	crumb(0x20, 0, 0);
	uint32_t id;
	if ((err = (uint32_t)-nand_init(&id))) {
		status.error = 0x2000u + err;
		crumb(0x20, (uint8_t)err, 0);
		return;
	}
	note(id);
	if ((err = (uint32_t)-nand_read(start, end, packed, 64))) {
		status.error = 0x2100u + err;
		crumb(0x21, (uint8_t)err, 0);
		return;
	}
	uint32_t size = be32(packed + 12), load = be32(packed + 16), entry = be32(packed + 20);
	uint32_t hcrc = be32(packed + 4), dcrc = be32(packed + 24);
	note(be32(packed));
	note(size);
	note(load);
	note(entry);
	packed[4] = packed[5] = packed[6] = packed[7] = 0;
	if (be32(packed) != 0x27051956u || inflate_crc32(packed, 64) != hcrc ||
	    size > end - start - 64 || packed[31] != 1 /* gzip */ ||
	    load < 0x80000000u || load >= 0x81000000u) {
		status.error = 0x2200;
		crumb(0x22, 1, (uint16_t)(be32(packed) >> 16));
		return;
	}
	crumb(0x23, 0, (uint16_t)(size >> 12));

	status.step = 0x21;    /* the packed kernel */
	if ((err = (uint32_t)-nand_read(start, end, packed, 64 + size))) {
		status.error = 0x2300u + err;
		crumb(0x24, (uint8_t)err, 0);
		return;
	}
	if (inflate_crc32(packed + 64, size) != dcrc) {
		status.error = 0x2400;
		crumb(0x25, 1, 0);
		return;
	}
	crumb(0x26, 0, 0);

	status.step = 0x22;    /* unpack */
	uint32_t got = 0;
	int e = gunzip(packed + 64, size, (uint8_t *)load, PACKED - load, &got);
	note(got);
	if (e) {
		status.error = 0x2500u + (uint32_t)-e;
		crumb(0x27, (uint8_t)-e, (uint16_t)(got >> 12));
		return;
	}
	crumb(0x28, 0, (uint16_t)(got >> 12));

	note(inflate_crc32((const uint8_t *)load, got));
	if (dry) {
		status.step = 0x2f;    /* all but the jump */
		return;
	}
	status.step = 0x23;    /* start it */
	crumb(0x29, 0, keys_seen);
	/* The flash writable for Linux (nand_unlock): the original
	 * firmware's file systems and its updater write it */
	nand_unlock();
	/* The clocks and gates as the kernels expect (clocks_for_linux); the
	 * kernel's timer (OST) and console port (UART2) left on */
	screen_for_linux();
	clocks_for_linux();
	/* Its console port ready, as its command line says (ttyS2, 115200):
	 * UART2 at 0xb0032000, 24 MHz / (13 x 16), 8N1, FIFOs and the port on
	 * (registers as in platform/h2/x1000_uart.c). An early message to a
	 * port that was never enabled would wait forever. */
	{
		const uint32_t u = 0xb0032000u;
		REG(u + 0x04) = 0;            /* IER */
		REG(u + 0x0c) = 0x83;         /* LCR: 8 bits, DLAB */
		REG(u + 0x00) = 13;           /* DLL */
		REG(u + 0x04) = 0;            /* DLH */
		REG(u + 0x0c) = 0x03;         /* LCR: 8N1 */
		REG(u + 0x24) = 16;           /* UMR */
		REG(u + 0x28) = 0;            /* UACR */
		REG(u + 0x10) = 0;            /* MCR */
		REG(u + 0x08) = 0x17;         /* FCR: on, reset FIFOs, FIFO enable */
	}
	linux_argv[0] = 0;                 /* as the other bootloaders: no program name */
	linux_argv[1] = (char *)args;
	linux_argv[2] = 0;
	sync_caches(load, got);
#ifdef TRAP_TEST
	install_trap();
#endif
	cpu_for_linux();
	((void (*)(int, char **, char **, uint32_t))entry)(2, linux_argv, 0, 0);
}

/*
 * The screen as the original firmware expects to find it (this player's
 * firmware relies on the bootloader to have started the panel): the
 * panel awake in the 18-bit colour format it uses (0x3a = 0x66), the
 * controller's frame DMA stopped, its frame and FIFO interrupts off, its
 * clock gated afterwards (clocks_for_linux). The backlight stays off.
 * Panel commands: the panel's own (sleep in 0x10, sleep out 0x11, display
 * off 0x28, on 0x29, pixel format 0x3a).
 */
#define LCD_CTRL      0xb3050030u
#define LCD_MCTRL     0xb30500a4u
#define LCD_MSTATE_R  0xb30500a8u
#define LCD_MDATA     0xb30500acu
#define LCD_MCFG_NEW  0xb30500b8u

static void lcd_idle(void)
{
	for (uint32_t i = 0; i < 200000u && (REG(LCD_MSTATE_R) & 1u); i++)
		;
}

static void lcd_cmd(uint8_t c)
{
	lcd_idle();
	REG(LCD_MDATA) = (1u << 30) | c;
}

static void lcd_param(uint8_t v)
{
	lcd_idle();
	REG(LCD_MDATA) = v;
}

static void screen_for_linux(void)
{
	if (REG(0xb0000020u) & (1u << 23) && !h2_lcd_start())
		return;                                 /* not started, and it didn't start */
	x1000_pin_out(X1000_PC, 25, 0);             /* backlight off */
	REG(LCD_MCTRL) &= ~((1u << 1) | (1u << 2)); /* frame DMA off */
	REG(LCD_CTRL) &= ~(1u << 3);
	lcd_idle();
	REG(LCD_MCFG_NEW) = 0;                      /* back to commands, a byte a cycle */
	lcd_cmd(0x28);
	lcd_cmd(0x10);
	delay_us(5000);
	lcd_cmd(0x3a);
	lcd_param(0x66);
	lcd_cmd(0x11);
	delay_us(120000);
	lcd_cmd(0x29);
	delay_us(20000);
	lcd_idle();
	REG(LCD_CTRL) &= ~((1u << 6) | (1u << 7) | (1u << 10) | (1u << 12) | (1u << 13));
}

/*
 * USB put down cleanly, if it's up (it is when we were started over USB
 * boot): the controller (a DWC2 core at 0xb3500000) off the bus, its PHY
 * clock stopped and its interrupts masked; the PHY suspended and powered
 * down; then the PHY clock gates in OPCR (bit 23) and USBCDR (bit 26)
 * left open, as kernels may expect them.
 */
static void usb_for_linux(void)
{
	if (REG(0xb0000024u) & (1u << 7)) {         /* OPCR: the PHY is awake */
		REG(0xb0000020u) &= ~(1u << 3);         /* the controller's clock, to reach it */
		REG(0xb3500804u) |= 1u << 1;            /* DCTL: soft disconnect */
		REG(0xb3500e00u) |= 1u;                 /* PCGCCTL: stop the PHY clock */
		REG(0xb3500018u) = 0;                   /* GINTMSK: nothing */
	}
	REG(0xb0000024u) &= ~(1u << 7);             /* OPCR: PHY suspended */
	REG(0xb000003cu) |= (1u << 20) | (1u << 21); /* USBPCR: OTG off, SIDDQ */
	REG(0xb0000024u) &= ~(1u << 23);
	REG(0xb0000050u) &= ~(1u << 26);
}

/*
 * The CPU as the kernel should find it: every TLB entry invalid and
 * unique (in kseg0, so it can never match), Wired 0; Status with BEV, ERL,
 * EXL and IE clear; Cause.IV set (the interrupt vector at its own offset).
 */
static void cpu_for_linux(void)
{
	uint32_t c1, entries;

	__asm__ volatile("mfc0 %0, $16, 1" : "=r"(c1));
	entries = ((c1 >> 25) & 0x3fu) + 1u;
	__asm__ volatile("mtc0 $0, $5\n\tmtc0 $0, $6\n\tmtc0 $0, $2\n\tmtc0 $0, $3\n\tehb");
	for (uint32_t i = 0; i < entries; i++) {
		uint32_t hi = 0x80000000u + (i << 13);
		__asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $10\n\tehb\n\ttlbwi\n\tehb" :: "r"(i), "r"(hi));
	}
	/* every L1 line written back and invalidated by index (16 KB each),
	 * and the branch target buffer cleared (Config7 bit 1) */
	for (uint32_t a = 0x80000000u; a < 0x80004000u; a += 32)
		__asm__ volatile("cache 0x01, 0(%0)\n\tcache 0x00, 0(%0)" :: "r"(a));
	__asm__ volatile("sync");
	uint32_t c7;
	__asm__ volatile("mfc0 %0, $16, 7" : "=r"(c7));
	__asm__ volatile("mtc0 %0, $16, 7\n\tehb" :: "r"(c7 | 2u));

	uint32_t st, cause;
	__asm__ volatile("mfc0 %0, $12" : "=r"(st));
	st &= ~((1u << 22) | (1u << 2) | (1u << 1) | 1u);
	__asm__ volatile("mtc0 %0, $12\n\tehb" :: "r"(st));
	__asm__ volatile("mfc0 %0, $13" : "=r"(cause));
	cause |= 1u << 23;
	__asm__ volatile("mtc0 %0, $13\n\tehb" :: "r"(cause));
}

/*
 * The clocks as the original firmware's kernels expect them, set right
 * before the jump: what the factory bootloader runs, and what another
 * bootloader that starts them switches to first (read from both
 * binaries: CPMPCR 0x98000080, DDRCDR from the MPLL / 3, CPCCR
 * 0x9a052210: CPU 1008 MHz from the APLL, L2 504, AHB0 and AHB2 200 from
 * the MPLL, APB 100; the camera, LCD and card clocks parked on the MPLL,
 * stopped, gated). Ours (everything from the APLL, MPLL off) suits
 * Lossless OS. X1000 Programming Manual 11.1.
 */
#define CPM_CPCCR   0xb0000000u
#define CPM_CPMPCR  0xb0000014u
#define CPM_DDRCDR  0xb000002cu
#define CPM_CPCSR   0xb00000d4u

static void wait_reg(uint32_t reg, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0; i < 1000000u && (REG(reg) & mask) != want; i++)
		;
}

static void clocks_for_linux(void)
{
	/* the MPLL at 600 MHz */
	REG(CPM_CPMPCR) = 0x98000080u;
	wait_reg(CPM_CPMPCR, 1u, 1u);

	/* memory from the MPLL / 3 (200 MHz): source bits 31:30 = 2, divider
	 * 3, with its change enable (bit 29) until it's done (busy, bit 28) */
	uint32_t d = (REG(CPM_DDRCDR) & ~((3u << 30) | 0xfu)) | (2u << 30) | 2u;
	REG(CPM_DDRCDR) = d | (1u << 29);
	wait_reg(CPM_DDRCDR, 1u << 28, 0);
	REG(CPM_DDRCDR) = d & ~(1u << 29);

	/* AHB0 and AHB2 onto the MPLL (dividers as they are: 120 MHz), the
	 * switch settled; then the factory dividers, with their change
	 * enables until none is busy */
	REG(CPM_CPCCR) = (REG(CPM_CPCCR) & 0x00ffffffu & ~(7u << 20)) | 0x9a000000u;
	wait_reg(CPM_CPCSR, 7u << 28, 7u << 28);
	REG(CPM_CPCCR) = 0x9a000000u | (7u << 20) | 0x52210u;
	wait_reg(CPM_CPCSR, 7u, 0);
	REG(CPM_CPCCR) = 0x9a052210u;

	/* camera, LCD and card clocks parked: MPLL, stopped, divider 0xfe */
	static const uint32_t cdrs[] = { 0xb000007cu, 0xb0000064u, 0xb0000068u, 0xb00000a4u };
	for (unsigned i = 0; i < sizeof(cdrs) / sizeof(cdrs[0]); i++) {
		REG(cdrs[i]) = (1u << 31) | (1u << 29) | (1u << 27) | 0xfeu;
		wait_reg(cdrs[i], 1u << 28, 0);
		REG(cdrs[i]) &= ~(1u << 29);
	}
	REG(0xb0000060u) &= ~(1u << 30);            /* I2SCDR */
	REG(0xb0000084u) &= ~(1u << 30);            /* PCMCDR */
	/* the gates: everything on but OTG, the card controllers, camera,
	 * LCD, MAC, PCM and the flash controller (the kernels' drivers open
	 * their own) */
	usb_for_linux();
	REG(0xb0000020u) = 0x06c0003cu;

	/* The power switch timing registers (CPM 0x90-0x9c) as other
	 * bootloaders set them (register dump; 0 after reset): 0x00, 0x10,
	 * 0x18, 0x08 */
	REG(0xb0000090u) = 0x00;
	REG(0xb0000094u) = 0x10;
	REG(0xb0000098u) = 0x18;
	REG(0xb000009cu) = 0x08;

	/* 8 KB of on-chip memory at 0xb3422000 as other bootloaders leave it
	 * (they copy flash 0x4000-0x5fff there, zeros in one of them); ours
	 * holds our first stage's leftovers */
	for (uint32_t a = 0xb3422000u; a < 0xb3424000u; a += 4)
		REG(a) = 0;
}

/* Lossless OS from the boot file at path; returns only if it can't */
static void start_lossless(const char *path)
{
	struct boot_header h;
	UINT got;
	FRESULT fr;

	status.step = 2;    /* the boot file */
	fr = f_open(&file, path, FA_READ);
	if (fr) {
		status.error = 0x200u + fr;
		return;
	}
	if (f_read(&file, &h, sizeof(h), &got) || got != sizeof(h)) {
		status.error = 0x300;
		return;
	}
	note(h.load);
	note(h.entry);
	note(h.size);
	note(h.os_version);
	if (memcmp(h.magic, "LOSB", 4) || h.version != 1 || h.load < OS_START ||
	    h.size > OS_END - h.load || h.entry < h.load || h.entry >= h.load + h.size ||
	    f_size(&file) != sizeof(h) + h.size) {
		status.error = 0x301;
		return;
	}

	status.step = 3;    /* load */
	if (f_read(&file, (void *)h.load, h.size, &got) || got != h.size) {
		status.error = 0x400;
		return;
	}
	uint32_t crc = inflate_crc32((const uint8_t *)h.load, h.size);
	note(crc);
	if (crc != h.crc) {
		status.error = 0x401;
		return;
	}

	status.step = 4;    /* start Lossless OS */
	/* The clock gates as in a start known to work (Lossless OS's own
	 * register dump, started over USB boot: CPM_CLKGR 0x076bfde4): what
	 * the boot ROM leaves open differs between starting from flash and
	 * from USB, and a gated device's registers can't be read (X1000
	 * Programming Manual 11.2.2.6). The gates as found go in the
	 * breadcrumbs (0x33: bits 7:0, then 23:8). */
	uint32_t gates = REG(0xb0000020u);
	crumb(0x33, (uint8_t)gates, (uint16_t)(gates >> 8));
	REG(0xb0000020u) = gates & 0x076bfde4u;
	sync_caches(h.load, h.size);
	((void (*)(void))h.entry)();
}

/*
 * When Lossless OS isn't on the card, or doesn't check out: a short note on
 * the screen (the panel started by us: platform/h2/h2_lcd.c; text in the
 * dot-matrix font: platform/h2/draw.c). On battery the player switches off
 * after 30 s; with USB power in (power chip register 0x00 bit 5) it stays
 * on, as switching off would only bring it straight back.
 */
bool h2_lcd_start(void);

#define LCD_DA          0xb3050040u
#define LCD_MCTRL       0xb30500a4u
#define LCD_MSTATE      0xb30500a8u
#define OST_CTRL        0xb2000000u
#define OST_ENABLE      0xb2000004u
#define OST_ENABLE_SET  0xb2000034u
#define OST_2CNTL       0xb2000020u

static void show(void)
{
	/* the frame is the source of the second descriptor (h2_lcd.c) */
	volatile uint32_t *d0 = (volatile uint32_t *)(0xa0000000u | REG(LCD_DA));
	volatile uint32_t *d1 = (volatile uint32_t *)(0xa0000000u | d0[0]);
	volatile uint32_t *dst = (volatile uint32_t *)(0xa0000000u | d1[1]);
	const uint32_t *src = (const uint32_t *)draw_buf;

	for (int i = 0; i < DRAW_W * DRAW_H / 2; i++)
		dst[i] = src[i];
	for (int i = 0; i < 20000000 && (REG(LCD_MSTATE) & 1u); i++)
		;
	REG(LCD_MCTRL) |= (1u << 1) | (1u << 2);     /* DMA start, DMA mode */
}

/* The mark and lines of text, centred, on a black screen; false if the
 * screen didn't start */
static bool screen_lines(const char *const *lines, unsigned n)
{
	const uint32_t bg = 0x000000, fg = 0xffffff;
	static bool started;

	if (!started && !h2_lcd_start())
		return false;
	started = true;
	draw_fill(0, 0, DRAW_W, DRAW_H, bg);
	int mw = draw_mark_width(6, 1);
	draw_mark((DRAW_W - mw) / 2, 20, 6, 1, bg);
	int y = 20 + draw_mark_height(6, 1) + 20;
	for (unsigned i = 0; i < n; i++, y += 20)
		draw_text((DRAW_W - draw_text_width(lines[i], 2)) / 2, y, lines[i], 2, fg, DRAW_W);
	show();
	x1000_pin_out(X1000_PC, 25, 1);              /* backlight, full */
	return true;
}

static void stay_then_off(void);

static void no_os_screen(void)
{
	static const char *const lines[] = {
		"Lossless OS isn't on",
		"the card.",
		"",
		"Copy it to the card",
		"again from your",
		"computer.",
	};

	if (!screen_lines(lines, sizeof(lines) / sizeof(lines[0])))
		return;
	stay_then_off();
}

/* What's on the screen stays. On battery the player switches off after
 * 30 s without USB power, on the hardware timer (counter 2, 24 MHz / 4),
 * checked every half second; with USB power in it stays on. */
static void stay_then_off(void)
{
	REG(0xb0000020u) &= ~(1u << 20);
	REG(OST_CTRL) = (REG(OST_CTRL) & ~(3u << 2)) | (1u << 2);
	REG(OST_ENABLE_SET) = 2u;
	uint32_t t0 = REG(OST_2CNTL), last = t0;
	for (;;) {
		uint32_t now = REG(OST_2CNTL);
		if (now - last < 3000000u)
			continue;
		last = now;
		uint8_t st = 0;
		if (x1000_i2c_read(2, 0x34, 0x00, &st, 1) || (st & (1u << 5))) {
			t0 = now;                            /* USB power in (or no answer) */
			continue;
		}
		if (now - t0 >= 30u * 6000000u) {
			uint8_t v = 0;
			if (!x1000_i2c_read(2, 0x34, 0x10, &v, 1))
				x1000_i2c_write(2, 0x34, 0x10, v | 1u);   /* power chip: off */
			t0 = now;
		}
	}
}

/*
 * The bootloader writer (MODE_FLASH, over USB boot only: our tool loads
 * the image into memory first). In memory at 0xa1000000: "LOSW", the
 * image's length, its CRC-32, a zero word, then the image (out/uboot.bin,
 * tools/mkbootimage.py). Every check first: the CRC, the boot ROM's
 * header and its crc7, our second stage's header. Then the flash's block
 * protection off, and only the blocks the image covers, within the
 * bootloader partition (blocks 0-3, flash 0 to 0x80000), erased and
 * programmed; then all of it read back and compared. The result on the
 * screen and in the breadcrumbs (0x60 done; 0x61-0x6f where it stopped).
 */
#define WRITE_AT     0xa1000000u
#define READBACK_AT  0xa1800000u
#define BLOCK_BYTES  (NAND_PAGE * NAND_BLOCK_PAGES)
#define BOOT_BLOCKS  4u

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t crc7(const uint8_t *p, uint32_t n)
{
	uint8_t c = 0;
	while (n--) {
		uint8_t b = *p++;
		for (int i = 7; i >= 0; i--) {
			uint8_t bit = (uint8_t)(((b >> i) & 1) ^ ((c >> 6) & 1));
			c = (uint8_t)((c << 1) & 0x7f);
			if (bit)
				c ^= 0x09;
		}
	}
	return c;
}

/* A bootloader image fit to write: the boot ROM's header with a right
 * crc7 over its first stage, a size within the bootloader partition. Ours
 * (our second stage's header at 0x4000, its CRC-32 right) or anyone's
 * whose header checks out (the factory's, from HiFi Walker's update). */
static const char *image_check(const uint8_t *img, uint32_t len, int *ours)
{
	static const uint8_t rom[8] = { 6, 5, 4, 3, 2, 0x55, 0xaa, 0x55 };
	uint32_t rlen = len >= 16 ? le32(img + 12) : 0;

	*ours = 0;
	if (len < 0x1000 || len > BOOT_BLOCKS * BLOCK_BYTES)
		return "Image size is wrong";
	if (memcmp(img, rom, 8) || rlen < 0x800 || rlen > 0x3000 || rlen > len ||
	    crc7(img + 0x800, rlen - 0x800) != img[9])
		return "Boot ROM header is wrong";
	if (len >= 0x4040 && !memcmp(img + 0x4000, "LOS2", 4)) {
		uint32_t n = le32(img + 0x4010);
		if (le32(img + 0x4004) != 1 || n > len - 0x4040 ||
		    inflate_crc32(img + 0x4040, n) != le32(img + 0x4014))
			return "Second stage is damaged";
		*ours = 1;
	}
	return 0;
}

/* img into the bootloader partition: only the blocks it covers, erased
 * and programmed, then all of it read back and compared */
static const char *flash_write_image(const uint8_t *img, uint32_t len, uint8_t *step)
{
	static uint8_t page[NAND_PAGE] __attribute__((aligned(16)));

	*step = 0x64;
	uint32_t id;
	if (nand_init(&id) || !NAND_KNOWN(id))
		return "Flash not found";
	if (nand_unlock())
		return "Flash unlock failed";
	uint32_t blocks = (len + BLOCK_BYTES - 1) / BLOCK_BYTES;
	for (uint32_t b = 0; b < blocks; b++) {
		*step = (uint8_t)(0x65 + b);
		if (nand_block_bad(b))
			return "A bad block: nothing more written";
		crumb(*step, 1, (uint16_t)b);
		if (nand_erase_block(b * NAND_BLOCK_PAGES))
			return "Erase failed";
		for (uint32_t pg = 0; pg < NAND_BLOCK_PAGES; pg++) {
			uint32_t at = b * BLOCK_BYTES + pg * NAND_PAGE;
			if (at >= len)
				break;
			for (uint32_t i = 0; i < NAND_PAGE; i++)
				page[i] = at + i < len ? img[at + i] : 0xff;
			if (nand_program_page(b * NAND_BLOCK_PAGES + pg, page))
				return "Program failed";
		}
	}
	*step = 0x6e;
	uint8_t *back = (uint8_t *)READBACK_AT;
	if (nand_read(0, BOOT_BLOCKS * BLOCK_BYTES, back, len))
		return "Read back failed";
	if (memcmp(back, img, len))
		return "Read back differs";
	*step = 0x60;
	return 0;
}

/* MODE_FLASH: the image our tool loaded, with its "LOSW" header */
static const char *flash_write_checked(uint8_t *step)
{
	const uint8_t *p = (const uint8_t *)WRITE_AT, *img = p + 16;
	int ours;

	*step = 0x61;
	if (memcmp(p, "LOSW", 4))
		return "No image in memory";
	uint32_t len = le32(p + 4);
	if (len > BOOT_BLOCKS * BLOCK_BYTES)
		return "Image size is wrong";
	*step = 0x62;
	if (inflate_crc32(img, len) != le32(p + 8))
		return "Image checksum is wrong";
	*step = 0x63;
	const char *err = image_check(img, len, &ours);
	return err ? err : flash_write_image(img, len, step);
}

static void flash_writer(void)
{
	static const char *const writing[] = { "Writing the", "bootloader..." };
	uint8_t step = 0x61;

	screen_lines(writing, 2);
	const char *err = flash_write_checked(&step);
	crumb(step, err ? 1 : 0, 0);
	if (err) {
		const char *const lines[] = { "Not written:", err };
		screen_lines(lines, 2);
	} else {
		static const char *const lines[] = { "Bootloader written", "and checked.", "",
						     "Hold Power to switch", "off, then on again." };
		screen_lines(lines, 5);
	}
	for (;;)
		;
}

/* Before the original firmware's recovery: the screen on (black, the
 * backlight full), as a bootloader's menu leaves it. Its updater shows
 * progress on the screen and, started with the screen off, restarted
 * without installing anything (H2, 2026-11-01). */
static void screen_for_recovery(void)
{
	if (h2_lcd_start())
		x1000_pin_out(X1000_PC, 25, 1);
}

/*
 * Test runs, for finding out what the original firmware's kernels need
 * (they fail when we start them, and the player has no console we can
 * read). A request on the card, /.lossless/test/linux.txt: its first
 * word "player" or "recovery", then words to add to that kernel's
 * command line. Lossless OS restarts into it (app.c); we run it once:
 * CPM_CPSPR's high half 0x7e57 marks "the test has run", so the next
 * start (the watchdog's, or the kernel's own restart, which writes
 * 0x00003535 there) goes to Lossless OS, which saves what the kernel
 * left in memory and removes the request. The hardware watchdog (X1000
 * Programming Manual ch. 15, RTC clock / 64 = 512 Hz) restarts the
 * player if the kernel hangs.
 */
#define TEST_FILE    "/.lossless/test/linux.txt"
#define TEST_MARK    0x7e57u
#define WDT(off)     REG(0xb0002000u + (off))   /* TDR 0, TCER 4, TCNT 8, TCSR 0xc */
#define TCU_TSCR     0xb000203cu                /* bit 16: the watchdog's stop bit cleared */

static void cpspr_high(uint32_t high)
{
	REG(0xb0000038u) = 0x5a5au;                 /* CPM_CPSPPR: unlock */
	REG(0xb0000034u) = high << 16 | (REG(0xb0000034u) & 0xffffu);
	REG(0xb0000038u) = 0xa5a5u;                 /* lock again */
}

static void watchdog_arm(uint32_t seconds)
{
	REG(0xb0000020u) &= ~(1u << 27);            /* the RTC clock it counts */
	WDT(0x4) = 0;
	REG(TCU_TSCR) = 1u << 16;
	WDT(0x8) = 0;
	WDT(0x0) = seconds * 512u > 0xffffu ? 0xffffu : seconds * 512u;
	WDT(0xc) = 0x1a;                            /* RTC clock, / 64 */
	WDT(0x4) = 1;
}

/* A test requested and not run yet: run it (doesn't come back) */
static void test_run(void)
{
	static char req[256], args[512];
	uint32_t cpspr = REG(0xb0000034u);
	UINT got;

	if (f_open(&file, TEST_FILE, FA_READ))
		return;                                 /* no request */
	if ((cpspr >> 16) == TEST_MARK || (cpspr & 0xffffu) == 0x3535u) {
		cpspr_high(0);                          /* it has run: Lossless OS saves the result */
		crumb(0x70, 1, (uint16_t)cpspr);
		return;
	}
	if (f_read(&file, req, sizeof(req) - 1, &got))
		return;
	req[got] = 0;
	int recovery = !memcmp(req, "recovery", 8);
	if (!recovery && memcmp(req, "player", 6))
		return;
	const char *extra = req;
	while (*extra && *extra != '\n')
		extra++;
	const char *base = recovery ? recovery_args : player_args;
	unsigned n = 0;
	while (*base && n < sizeof(args) - 2)
		args[n++] = *base++;
	if (*extra)
		args[n++] = ' ', extra++;
	while (*extra && n < sizeof(args) - 1) {
		char c = *extra++;
		args[n++] = c == '\n' || c == '\r' ? ' ' : c;
	}
	args[n] = 0;

	cpspr_high(TEST_MARK);
	crumb(0x71, (uint8_t)recovery, 0);
	watchdog_arm(60);
	if (recovery)
		screen_for_recovery();
	start_linux(recovery ? RECOVERY_KERNEL : PLAYER_KERNEL, recovery ? RECOVERY_END : PLAYER_END,
		    args, 0);
}

/*
 * At power-on, as the factory bootloader does (its second stage, read
 * from the binary):
 *   the original firmware's requests in the low half of CPM_CPSPR
 *   (0xb0000034; its kernel writes them after unlocking with 0x5a5a at
 *   CPM_CPSPPR, 0xb0000038, then restarts): 0x1a1a recovery, 0xa1a1 its
 *   watchdog's recovery. Either one, or Vol+ held (PA17 low, docs/
 *   h2-hardware.md, "Buttons"): the request cleared, then the original
 *   firmware's recovery, which installs update.upt from the card. This is
 *   also how our bootloader comes off: the factory update file and Vol+.
 *   Anything else in CPM_CPSPR (0x3535: its kernel restarted) is left.
 *   Otherwise Lossless OS from the card: /.lossless/boot/lossless.bin, or
 *   /Lossless/lossless.bin (the release, before it has installed itself);
 *   if neither checks out, a note on the screen.
 */
#define GPIO_A_PIN   0xb0010000u
#define KEY_VOL_UP   (1u << 17)
#define CPM_CPSPR    0xb0000034u
#define CPM_CPSPPR   0xb0000038u

/*
 * Vol+ with an update file on the card (/update.upt): the bootloader it
 * holds, written by us. The file is an ISO 9660 image (ECMA-119: the
 * primary volume descriptor at 0x8000, "CD001"; the root directory's
 * extent and size at its offset 156 + 2 and + 10; each directory record:
 * length, extent at + 2, size at + 10, name length at + 32, name at + 33,
 * records never crossing a 2048-byte sector) holding UBOOT.BIN. Ours (our
 * own update) or anyone's whose boot ROM header checks out: the factory
 * bootloader is in HiFi Walker's own update for the player, so writing it
 * back is how our bootloader comes off. The result on the screen, then a
 * restart (the watchdog). Returns if there's no such file or no
 * bootloader in it; nothing is written then.
 */
#define UPDATE_FILE  "/update.upt"
#define ISO_SECTOR   2048u

static int iso_name_is(const uint8_t *name, unsigned n, const char *want)
{
	unsigned i = 0;
	for (; i < n && want[i]; i++) {
		char c = (char)name[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		if (c != want[i])
			return 0;
	}
	return !want[i] && (i == n || name[i] == ';');
}

static int iso_read(uint32_t at, void *buf, uint32_t n)
{
	UINT got;
	return f_lseek(&file, at) || f_read(&file, buf, n, &got) || got != n;
}

/* Enough power to write the flash: USB power in (power chip register 0x00
 * bit 5), or the battery at 30% or more (register 0xa4, the fuel gauge's
 * percentage, as Lossless OS reads it). No answer, or a reading that makes
 * no sense: not enough. A write cut short leaves a player that only starts
 * over USB boot. */
#define MIN_BATTERY 30

static int power_ok(void)
{
	uint8_t st = 0, pct = 0;

	if (x1000_i2c_read(2, 0x34, 0x00, &st, 1))
		return 0;
	if (st & (1u << 5))
		return 1;
	if (x1000_i2c_read(2, 0x34, 0xa4, &pct, 1))
		return 0;
	return pct >= MIN_BATTERY && pct <= 100;
}

static void update_from_card(void)
{
	static uint8_t sec[ISO_SECTOR] __attribute__((aligned(4)));
	uint8_t *img = (uint8_t *)WRITE_AT;
	uint32_t at = 0, len = 0;

	if (f_open(&file, UPDATE_FILE, FA_READ))
		return;
	if (iso_read(16 * ISO_SECTOR, sec, ISO_SECTOR) || sec[0] != 1 || memcmp(sec + 1, "CD001", 5))
		return;
	uint32_t dir = le32(sec + 156 + 2) * ISO_SECTOR, dir_len = le32(sec + 156 + 10);
	if (dir_len > 16 * ISO_SECTOR)
		return;
	for (uint32_t off = 0; off < dir_len && !len; off += ISO_SECTOR) {
		if (iso_read(dir + off, sec, ISO_SECTOR))
			return;
		for (uint32_t r = 0; r + 33 < ISO_SECTOR && sec[r];) {
			uint8_t rl = sec[r], nl = sec[r + 32];
			if (rl < 34 || r + rl > ISO_SECTOR)
				break;
			if (r + 33 + nl <= ISO_SECTOR && iso_name_is(sec + r + 33, nl, "UBOOT.BIN")) {
				at = le32(sec + r + 2) * ISO_SECTOR;
				len = le32(sec + r + 10);
				break;
			}
			r += rl;
		}
	}
	if (!len)
		return;                                  /* no bootloader in it */

	static const char *const reading[] = { "Installing the", "bootloader..." };
	screen_lines(reading, 2);
	uint8_t step = 0x61;
	int ours = 0;
	const char *err = len > BOOT_BLOCKS * BLOCK_BYTES ? "Image size is wrong" : 0;
	if (!err && iso_read(at, img, len))
		err = "The update file couldn't be read";
	if (!err)
		err = image_check(img, len, &ours);
	if (!err && !power_ok()) {
		static const char *const lines[] = {
			"Not installed: the", "battery is low.", "",
			"Charge it or plug it", "into USB power, then", "try again.", "",
			"Nothing was changed.",
		};
		crumb(0x6f, 1, 0);                      /* battery low: nothing written */
		screen_lines(lines, 8);
		stay_then_off();
	}
	if (!err)
		err = flash_write_image(img, len, &step);
	crumb(step, err ? 1 : (uint8_t)(0x10 | ours), 0);
	if (err && step < 0x65) {
		/* stopped before anything was erased */
		const char *const lines[] = { "Not installed:", err, "", "Nothing was changed." };
		screen_lines(lines, 4);
		stay_then_off();
	}
	if (err) {
		const char *const lines[] = { "Install failed:", err, "", "Rescue steps:", "github.com/", "bobbydotdesign/", "lossless-os" };
		screen_lines(lines, 7);
		stay_then_off();
	}
	if (ours) {
		static const char *const lines[] = { "Lossless OS's", "bootloader is", "installed.", "", "Restarting..." };
		screen_lines(lines, 5);
	} else {
		static const char *const lines[] = { "The original", "bootloader is back.", "", "Restarting..." };
		screen_lines(lines, 4);
	}
	delay_us(3000000);
	watchdog_arm(1);
	for (;;)
		;
}

static void choose(void)
{
	uint32_t pins = REG(GPIO_A_PIN);
	uint32_t request = REG(CPM_CPSPR) & 0xffffu;
	int update = request == 0x1a1au || request == 0xa1a1u;

	note(pins);
	note(REG(CPM_CPSPR));
	keys_seen = (uint16_t)((pins >> 16) & 3u);   /* bit 0 Play, bit 1 Vol+: 1 = up */
	if (update || !(pins & KEY_VOL_UP)) {
		for (int i = 0; i < 8 && (REG(CPM_CPSPR) & 0xffffu); i++) {
			REG(CPM_CPSPPR) = 0x5a5au;            /* unlock */
			REG(CPM_CPSPR) &= ~0xffffu;           /* request taken */
			REG(CPM_CPSPPR) = 0xa5a5u;            /* lock again */
		}
		crumb(0x40, update ? 1 : 2, keys_seen);
		if (!f_mount(&fs, "", 1))
			update_from_card();                  /* a bootloader on the card: ours or the factory's */
		screen_for_recovery();
		start_linux(RECOVERY_KERNEL, RECOVERY_END, recovery_args, 0);
		return;
	}
	status.step = 1;    /* card and file system */
	crumb(0x30, 0, keys_seen);
	FRESULT fr = f_mount(&fs, "", 1);
	if (fr) {
		status.error = 0x100u + fr;
	} else {
		test_run();                              /* a requested test, once */
		start_lossless(BOOT_FILE);
		crumb(0x31, (uint8_t)(status.error >> 8), (uint16_t)status.error);
		status.error = 0;
		start_lossless(BOOT_PACKAGE);
	}
	crumb(0x42, (uint8_t)(status.error >> 8), (uint16_t)status.error);
	no_os_screen();
}

void stage2_main(void)
{
	status.magic = STATUS_MAGIC;
	status.error = 0;
	status.n = 0;

	if (params.mode == MODE_FLASH) {
		flash_writer();
		return;
	}
	if (params.mode == MODE_NAND_TEST) {
		nand_test();
		return;
	}
	if (params.mode == MODE_RETURN) {
		status.step = 0x50;
		crumb(0x50, 0, 0x1234);
		for (;;)
			;
	}
	if (params.mode == MODE_PLAYER_DRY) {
		start_linux(PLAYER_KERNEL, PLAYER_END, player_args, 1);
		crumb(0x2f, (uint8_t)status.error, 0);
		for (;;)
			;
	}
	if (params.mode == MODE_PLAYER) {
		start_linux(PLAYER_KERNEL, PLAYER_END, player_args, 0);
		return;
	}
	if (params.mode == MODE_RECOVERY) {
		screen_for_recovery();
		start_linux(RECOVERY_KERNEL, RECOVERY_END, recovery_args, 0);
		return;
	}
	if (params.mode == MODE_LOSSLESS) {
		if (!f_mount(&fs, "", 1))
			start_lossless(BOOT_FILE);
		return;
	}

	choose();
}
