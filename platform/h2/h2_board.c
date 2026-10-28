/*
 * The HiFi Walker H2 under the Lossless app: the card, the buttons, power.
 *
 * Before the app starts, a check screen (drawn directly, like tests 2 and 3):
 * the card mounted read-only, its size, a checksum of a test file put there
 * from the Mac (every byte read right?), the read speed, and how much music
 * it found. Then the app takes over the screen.
 *
 * Keys: wheel = up/down, Play = enter, Back = back, Menu = home,
 * Prev/Next = previous/next. Holding Power 2 s switches off.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <ff.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "board.h"
#include "draw.h"
#include "ui.h"
#include "screen.h"
#include "h2_buttons.h"
#include "h2_power.h"
#include "h2_usb.h"
#include "h2_audio.h"
#include "los.h"
#include "x1000_msc.h"

#define MOUNT       "/SD:"
#define CHECK_BIN   MOUNT "/.lossless/test/check.bin"
#define CHECK_TXT   MOUNT "/.lossless/test/check.txt"
#define OFF_HOLD_MS 2000
#define LOCK_TAP_MS 600      /* Power pressed shorter than this: lock key */
#define WHEEL_STEPS 2

enum { ROW_CARD, ROW_FILES, ROW_CHECK, ROW_SPEED, ROW_MUSIC, ROW_FONTS, ROW_SOUND, ROW_WRITES, ROWS };
static const char *const labels[ROWS] = {
	"Card", "Files", "Check", "MB/s", "Music", "Fonts", "Sound", "Writes",
};

#define VALUE_X  92
#define ROW_STEP 20

static char text[96];

static void row(int r, uint32_t colour)
{
	int y = UI_TOP + r * ROW_STEP;

	draw_fill(VALUE_X, y, DRAW_W - VALUE_X, UI_TEXT_H, UI->bg);
	draw_text(VALUE_X, y, text, UI_TEXT, colour, DRAW_W - 4);
	screen_show();
}

/* Two lines at the bottom: what the SD driver has seen, then the status */
static void footer(const char *s, uint32_t colour)
{
	const struct device *sdhc = DEVICE_DT_GET(DT_NODELABEL(sdhc0));
	struct x1000_msc_stats st;
	char sd[64];
	uint32_t sd_colour = UI->faint;

	x1000_msc_get_stats(sdhc, &st);
	if (st.errors) {
		snprintf(sd, sizeof(sd), "SD: %u errors, CMD%u %d %x", st.errors,
			 st.last_opcode, st.last_err, st.last_flags);
		sd_colour = UI->danger;
		printk("sd: %u cmds, %u errors, last CMD%u err %d iflag %08x stat %08x\n",
		       st.commands, st.errors, st.last_opcode, st.last_err,
		       st.last_flags, st.last_stat);
	} else {
		snprintf(sd, sizeof(sd), "SD: %u commands, no errors", st.commands);
	}
	draw_fill(0, 200, DRAW_W, DRAW_H - 200, UI->bg);
	draw_fill(0, 203, DRAW_W, 1, UI->line);
	draw_text(UI_MARGIN, 207, sd, UI_TEXT, sd_colour, DRAW_W - 4);
	draw_text(UI_MARGIN, 224, s, UI_TEXT, colour, DRAW_W - 4);
	screen_show();
}

static FATFS fat;
static struct fs_mount_t card = {
	.type = FS_FATFS,
	.mnt_point = MOUNT,
	.fs_data = &fat,
	.storage_dev = (void *)"SD",
	/* never formatted; written only in /.lossless, and only once
	 * write_check() passed (platform/zephyr/sys.c) */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

static void tenths(char *out, size_t n, uint64_t bytes, uint64_t unit, const char *name)
{
	uint64_t t = bytes * 10 / unit;

	snprintf(out, n, "%u.%u %s", (unsigned)(t / 10), (unsigned)(t % 10), name);
}

static bool mount_card(void)
{
	int err = fs_mount(&card);

	if (err) {
		snprintf(text, sizeof(text), "not mounted (%d)", err);
		row(ROW_CARD, UI->danger);
		printk("mount: %d\n", err);
		return false;
	}

	uint32_t sectors = 0, size = 0;
	char total[24], free_[32];
	struct fs_statvfs st;

	disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
	disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &size);
	tenths(total, sizeof(total), (uint64_t)sectors * size, 1000000000ull, "GB");
	snprintf(text, sizeof(text), "%s", total);
	row(ROW_CARD, UI->fg);

	const char *kind = fat.fs_type == FS_EXFAT ? "exFAT" : fat.fs_type == FS_FAT32 ?
			   "FAT32" : fat.fs_type == FS_FAT16 ? "FAT16" : "FAT";

	if (fs_statvfs(MOUNT, &st) == 0) {
		tenths(free_, sizeof(free_), (uint64_t)st.f_bfree * st.f_frsize,
		       1000000000ull, "GB free");
		snprintf(text, sizeof(text), "%s, %s", kind, free_);
	} else {
		/* FatFs can't count free space when mounted read-only */
		snprintf(text, sizeof(text), "%s, read-only", kind);
	}
	row(ROW_FILES, UI->fg);
	return true;
}

/* The test file: its CRC-32 and size are in check.txt, written on the Mac */
static uint8_t buf[32 * 1024];

/* Straight from the card, no files: 2 MB in 64-block reads, from 32 MB in */
static uint32_t raw_speed_tenths(void)
{
	uint32_t start = k_cycle_get_32();

	for (uint32_t s = 65536; s < 65536 + 4096; s += 64) {
		if (disk_access_read("SD", buf, s, 64)) {
			return 0;
		}
	}
	uint64_t us = (uint64_t)(k_cycle_get_32() - start) * 1000000 / sys_clock_hw_cycles_per_sec();

	return us ? (uint32_t)(2ull * 1024 * 1024 * 10 / us) : 0;   /* MB/s x 10 */
}

static void check_file(void)
{
	struct fs_file_t f;
	unsigned long want_size = 0, want_crc = 0;

	fs_file_t_init(&f);
	if (fs_open(&f, CHECK_TXT, FS_O_READ) == 0) {
		char line[48] = {0};

		fs_read(&f, line, sizeof(line) - 1);
		fs_close(&f);
		want_size = strtoul(line, NULL, 10);
		char *sp = strchr(line, ' ');
		want_crc = sp ? strtoul(sp + 1, NULL, 16) : 0;
	}
	if (!want_size || fs_open(&f, CHECK_BIN, FS_O_READ)) {
		snprintf(text, sizeof(text), "no test file");
		row(ROW_CHECK, UI->faint);
		snprintf(text, sizeof(text), "not measured");
		row(ROW_SPEED, UI->faint);
		return;
	}

	snprintf(text, sizeof(text), "reading...");
	row(ROW_CHECK, UI->faint);

	uint32_t crc = 0, start = k_cycle_get_32();
	uint64_t got = 0;
	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
		crc = crc32_ieee_update(crc, buf, (size_t)n);
		got += (uint64_t)n;
	}
	uint32_t cycles = k_cycle_get_32() - start;
	fs_close(&f);

	if (n < 0) {
		snprintf(text, sizeof(text), "read error %d", (int)n);
		row(ROW_CHECK, UI->danger);
	} else if (got == want_size && crc == want_crc) {
		tenths(text, sizeof(text), got, 1000000, "MB, every byte ok");
		row(ROW_CHECK, UI->ok);
	} else {
		snprintf(text, sizeof(text), "WRONG: crc %08x", crc);
		row(ROW_CHECK, UI->danger);
		printk("check: %llu bytes crc %08x, want %lu %08lx\n",
		       (unsigned long long)got, crc, want_size, want_crc);
	}

	uint64_t us = (uint64_t)cycles * 1000000 / sys_clock_hw_cycles_per_sec();
	uint32_t file = us ? (uint32_t)(got * 10 / us) : 0;     /* MB/s x 10 */
	uint32_t raw = raw_speed_tenths();

	snprintf(text, sizeof(text), "card %u.%u, file %u.%u", raw / 10, raw % 10,
		 file / 10, file % 10);
	row(ROW_SPEED, UI->fg);
}

/* Artists are the folders in /Music, albums the folders in those */
static void count_music(void)
{
	static struct fs_dirent e;
	struct fs_dir_t top, sub;
	int artists = 0, albums = 0;
	char path[300];

	fs_dir_t_init(&top);
	if (fs_opendir(&top, MOUNT "/Music")) {
		snprintf(text, sizeof(text), "no /Music folder");
		row(ROW_MUSIC, UI->danger);
		return;
	}
	static char names[64][256];
	int pending = 0;

	/* Collect a batch of artist names first: one folder open at a time */
	for (;;) {
		while (pending < 64 && fs_readdir(&top, &e) == 0 && e.name[0]) {
			if (e.type == FS_DIR_ENTRY_DIR && e.name[0] != '.') {
				snprintf(names[pending++], sizeof(names[0]), "%s", e.name);
			}
		}
		if (!pending) {
			break;
		}
		for (int i = 0; i < pending; i++) {
			artists++;
			snprintf(path, sizeof(path), MOUNT "/Music/%.255s", names[i]);
			fs_dir_t_init(&sub);
			if (fs_opendir(&sub, path) == 0) {
				while (fs_readdir(&sub, &e) == 0 && e.name[0]) {
					albums += e.type == FS_DIR_ENTRY_DIR && e.name[0] != '.';
				}
				fs_closedir(&sub);
			}
		}
		pending = 0;
	}
	fs_closedir(&top);
	snprintf(text, sizeof(text), "%d albums", albums);
	printk("music: %d artists, %d albums\n", artists, albums);
	row(ROW_MUSIC, artists ? UI->fg : UI->danger);
}

static void check_fonts(void)
{
	struct fs_dirent e;
	int n = 0;
	struct fs_dir_t d;

	fs_dir_t_init(&d);
	if (fs_opendir(&d, MOUNT "/.lossless/fonts") == 0) {
		while (fs_readdir(&d, &e) == 0 && e.name[0]) {
			n += strstr(e.name, ".ttf") != NULL;
		}
		fs_closedir(&d);
	}
	snprintf(text, sizeof(text), n ? "%d on the card" : "missing: built-in font", n);
	row(ROW_FONTS, n ? UI->fg : UI->warn);
}

/* A short quiet beep (1 kHz at -20 dBFS, with the DAC at its starting
 * -40 dB) through the whole path, timing how fast the DMA takes samples:
 * that is the real I2S rate, so it checks the clock settings too */
static const int16_t sine_1k[48] = { 0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249, 3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428, 0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249, -3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1639, -1254, -848, -428 };

static struct h2_audio_state snd;      /* caught mid-beep, for page 2 */
static uint32_t snd_hz;

static void check_sound(void)
{
	static int32_t buf[480 * 2];
	bool caught = false;

	snprintf(text, sizeof(text), "beep...");
	row(ROW_SOUND, UI->faint);
	if (!los_audio_start(48000, 16, 2)) {
		snprintf(text, sizeof(text), "couldn't start");
		row(ROW_SOUND, UI->danger);
		return;
	}
	for (int i = 0; i < 480; i++) {
		buf[2 * i] = buf[2 * i + 1] = sine_1k[i % 48];
	}
	int64_t start = k_uptime_get(), t0 = 0;
	uint64_t p0 = 0;
	while (k_uptime_get() - start < 1200) {
		los_audio_write(buf, 480);
		uint64_t p = los_audio_played();
		if (!t0 && p > 4800) {           /* measure after the first 0.1 s */
			t0 = k_uptime_get();
			p0 = p;
		}
		if (!caught && k_uptime_get() - start > 600) {
			h2_audio_state_get(&snd);
			caught = true;
		}
		k_msleep(5);
	}
	uint64_t p1 = los_audio_played();
	int64_t t1 = k_uptime_get();
	los_audio_stop();

	if (!t0 || t1 <= t0 || p1 <= p0) {
		snprintf(text, sizeof(text), "DMA didn't run");
		row(ROW_SOUND, UI->danger);
		printk("sound: played %llu frames\n", (unsigned long long)p1);
		return;
	}
	uint32_t hz = (uint32_t)((p1 - p0) * 1000 / (uint64_t)(t1 - t0));
	snd_hz = hz;
	snprintf(text, sizeof(text), "beep, %u.%u kHz", hz / 1000, hz % 1000 / 100);
	row(ROW_SOUND, hz > 47000 && hz < 49000 ? UI->ok : UI->warn);
}

/* Page 2: what the sound hardware said during the beep */
static void __attribute__((unused)) sound_page(void)
{
	char l[10][40];
	int n = 0;

	ui_header("test 5: sound check");
	if (snd.dac_reg0 < 0) {
		snprintf(l[n++], 40, "DAC: no answer on I2C");
	} else {
		snprintf(l[n++], 40, "DAC r0 %02x r10 %02x vol %02x", snd.dac_reg0, snd.dac_reg10,
			 snd.dac_vol);
	}
	snprintf(l[n++], 40, "clk %u/%u of %u MHz", snd.m, snd.n, snd.sclk / 1000000);
	snprintf(l[n++], 40, "rate %u Hz, div %u", snd_hz, snd.i2sdiv);
	snprintf(l[n++], 40, "cfg %08x i2s %08x", snd.aic_cfg, snd.aic_i2scr);
	snprintf(l[n++], 40, "ccr %08x sr %08x", snd.aic_ccr, snd.aic_sr);
	snprintf(l[n++], 40, "cdr %08x ctl %08x", snd.i2scdr, snd.dma_ctrl);
	snprintf(l[n++], 40, "dma cs %08x", snd.dma_cs);
	snprintf(l[n++], 40, "sa %08x da %08x", snd.dma_sa, snd.dma_da);
	snprintf(l[n++], 40, "pins B %08x %s", snd.pins_b, snd.running ? "run" : "stop");
	for (int i = 0; i < n; i++) {
		draw_text(UI_MARGIN, UI_TOP - 4 + i * 18, l[i], UI_TEXT, UI->fg, DRAW_W - 4);
	}
	draw_fill(0, 221, DRAW_W, 1, UI->line);
	draw_text(UI_MARGIN, 224, "Any button starts the app", UI_TEXT, UI->muted, DRAW_W - 4);
	screen_show();
	for (int i = 0; i < 9; i++) {
		printk("%s\n", l[i]);
	}
}

static void wait_button(int ms)
{
	int64_t until = k_uptime_get() + ms;

	while (k_uptime_get() < until && !h2_buttons_held()) {
		k_msleep(20);
	}
	while (h2_buttons_held()) {
		k_msleep(20);
	}
}

/* ---- writing to the card (test 4c) ---- */

void los_writes_enable(bool on);      /* platform/zephyr/sys.c */

#define WCHECK_DIR   MOUNT "/.lossless/test"
#define WCHECK_FILE  WCHECK_DIR "/write-check.bin"
#define WCHECK_BYTES (40 * 1024)      /* more than one 32 KB cluster */
#define WCHECK_MORE  4096

static uint8_t wpattern[WCHECK_BYTES + WCHECK_MORE];
static char write_status[48] = "not checked";

/* The whole file as written? */
static bool read_back(size_t len)
{
	void *f = los_file_open(WCHECK_FILE);

	if (!f) {
		return false;
	}
	bool ok = los_file_size(f) == len;
	for (size_t at = 0; ok && at < len;) {
		size_t n = len - at < sizeof(buf) ? len - at : sizeof(buf);

		ok = los_file_read(f, buf, n) == n && !memcmp(buf, wpattern + at, n);
		at += n;
	}
	los_file_close(f);
	return ok;
}

/* Write a file (spanning clusters), read it back, add to it, read it
 * again, delete it: what the app needs. Writes stay on only if all of it
 * worked. Only ever in /.lossless/test. */
/* A line under the start-up logo (nothing else is on screen then) */
static void boot_line(const char *t, uint32_t colour)
{
	draw_fill(0, 200, DRAW_W, UI_TEXT_H + 4, 0x000000);
	draw_text(UI_MARGIN, 202, t, UI_TEXT, colour, DRAW_W - 4);
	screen_show();
}

static bool write_check(void)
{
	uint32_t seed = k_cycle_get_32() | 1;
	int64_t t0 = k_uptime_get();
	const char *step = "folder";

	for (size_t i = 0; i < sizeof(wpattern); i++) {
		seed = seed * 1664525u + 1013904223u;
		wpattern[i] = (uint8_t)(seed >> 24);
	}
	los_writes_enable(true);
	/* no line while it runs: it takes moments, and a line that comes and
	 * goes at once only flashes at start-up; a failure says so below */
	bool ok = los_mkdir(MOUNT "/.lossless") && los_mkdir(WCHECK_DIR);
	if (ok) {
		step = "save";
		ok = los_file_save(WCHECK_FILE, wpattern, WCHECK_BYTES);
	}
	if (ok) {
		step = "read back";
		ok = read_back(WCHECK_BYTES);
	}
	if (ok) {
		step = "append";
		ok = los_file_append(WCHECK_FILE, wpattern + WCHECK_BYTES, WCHECK_MORE) &&
		     read_back(WCHECK_BYTES + WCHECK_MORE);
	}
	if (ok) {
		step = "delete";
		struct fs_dirent st;

		ok = los_file_remove(WCHECK_FILE) && fs_stat(WCHECK_FILE, &st) == -ENOENT;
	}
	los_writes_enable(ok);
	if (ok) {
		snprintf(write_status, sizeof(write_status), "on (%u ms)", (unsigned)(k_uptime_get() - t0));
	} else {
		/* what the SD controller last saw, for a photo */
		const struct device *sdhc = DEVICE_DT_GET(DT_NODELABEL(sdhc0));
		struct x1000_msc_stats st;

		x1000_msc_get_stats(sdhc, &st);
		snprintf(write_status, sizeof(write_status), "off: %s failed", step);
		snprintf(text, sizeof(text), "Read-only: %s failed, CMD%u err %d flags %08x stat %08x",
			 step, (unsigned)st.last_opcode, st.last_err, (unsigned)st.last_flags,
			 (unsigned)st.last_stat);
		boot_line(text, 0xd65a4a);
		k_msleep(5000);
	}
	printk("writes %s\n", write_status);
	return ok;
}

const char *h2_write_status(void)
{
	return write_status;
}

/* The check screen: the card, the test file, the library, fonts, sound */
static void check_screen(void)
{
	ui_header("Lossless OS: checks");
	for (int r = 0; r < ROWS; r++) {
		draw_text(UI_MARGIN, UI_TOP + r * ROW_STEP, labels[r], UI_TEXT, UI->muted, VALUE_X);
		snprintf(text, sizeof(text), "...");
		row(r, UI->faint);
	}
	footer("Reading the card", UI->faint);

	if (mount_card()) {
		check_file();
		count_music();
		check_fonts();
		check_sound();
		bool w = write_check();
		snprintf(text, sizeof(text), "%s", write_status);
		row(ROW_WRITES, w ? UI->ok : UI->danger);
	}
	footer("Any button: the app", UI->muted);
	wait_button(30000);
}

static void cpu_probe(void);

void los_board_start(void)
{
	uint8_t id;

	h2_power_init(&id);
	h2_buttons_start();

	/* Menu held while switching on: the check screen. Otherwise straight
	 * to the app, unless the card won't mount (then the check screen says
	 * why). */
	k_msleep(20);
	uint32_t held = h2_buttons_held();
	if (held & BIT(BTN_MENU)) {
		check_screen();
		return;
	}
	int err = fs_mount(&card);
	if (err) {
		printk("mount: %d\n", err);
		check_screen();
		return;
	}
	if (held & BIT(BTN_BACK)) {
		/* Back held while switching on: no writing this time */
		snprintf(write_status, sizeof(write_status), "off: Back held at start");
		h2_usb_start();
		return;
	}
	if (write_check()) {
		h2_usb_probe();
		cpu_probe();
	}
	h2_usb_start();
}

/* Once: the CPU's identity, whether it has a floating-point unit (Config1
 * bit 0), and how fast floating point is in software, in
 * /.lossless/logs/cpu.txt (which decoders can run, and how). */
#define MFC0(reg, sel) ({ uint32_t v_; \
	__asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r"(v_)); v_; })

static void cpu_probe(void)
{
	static const char path[] = "/SD:/.lossless/logs/cpu.txt";
	static char text[300];
	void *f = los_file_open(path);

	if (f) {
		los_file_close(f);
		return;
	}
	uint32_t prid = MFC0(15, 0), config = MFC0(16, 0), config1 = MFC0(16, 1);
	/* a million multiply-adds in software floating point */
	volatile float a = 1.0001f, b = 0.9999f, c = 0.5f;
	int64_t t0 = k_uptime_get();

	for (int i = 0; i < 1000000; i++)
		a = a * b + c;
	int64_t ms = k_uptime_get() - t0;
	int n = snprintf(text, sizeof(text),
			 "CPU probe\nPRId %08x\nConfig %08x\nConfig1 %08x\n"
			 "FPU: %s\nsoftware float, 1M multiply-adds: %d ms\n",
			 prid, config, config1, (config1 & 1) ? "yes" : "no", (int)ms);
	los_mkdir("/SD:/.lossless");
	los_mkdir("/SD:/.lossless/logs");
	los_file_save(path, text, (size_t)n);
}

const char *los_player_name(void)
{
	return "HiFi Walker H2";
}

/* The card to a computer and back (h2_usb.c). Writing stops while it's
 * away, and comes back only if it worked at start. */
bool h2_card_mount(bool on)
{
	static bool writes_were_ok, away;

	if (!on) {
		if (away)
			return true;
		writes_were_ok = los_writes_ok();
		los_writes_enable(false);
		int err = fs_unmount(&card);

		if (err) {
			printk("unmount: %d\n", err);
			los_writes_enable(writes_were_ok);
			return false;
		}
		away = true;
		return true;
	}
	if (!away)
		return true;
	int err = fs_mount(&card);

	if (err) {
		printk("mount: %d\n", err);
		return false;
	}
	away = false;
	los_writes_enable(writes_were_ok);
	return true;
}

/* Keys */

static const struct { enum h2_button button; uint32_t key; } keymap[] = {
	{BTN_PLAY, LV_KEY_ENTER}, {BTN_BACK, LV_KEY_ESC}, {BTN_MENU, LV_KEY_HOME},
	{BTN_PREV, LV_KEY_LEFT}, {BTN_NEXT, LV_KEY_RIGHT},
};

static void switch_off(void)
{
	los_app_before_off();      /* settings still to save */
	int tile = BOOT_MARK_TILE, gap = BOOT_MARK_GAP;
	int w = draw_mark_width(tile, gap), h = draw_mark_height(tile, gap);

	draw_fill(0, 0, DRAW_W, DRAW_H, 0x000000);
	draw_mark((DRAW_W - w) / 2, (DRAW_H - h) / 2, tile, gap, 0x000000);
	screen_show();
	k_msleep(300);
	h2_power_off();   /* returns only if the power chip didn't do it */
}

void los_power_off(void)
{
	switch_off();
}

bool los_board_key(uint32_t *key, bool *pressed, bool *more)
{
	static uint32_t was_held;
	static int32_t wheel_done;
	static bool wheel_down;      /* reported a wheel press, release next */
	static int64_t power_since;
	static bool armed;           /* Power seen released since start */
	static bool lock_down;       /* reported a lock press, release next */

	uint32_t held = h2_buttons_held();
	int64_t now = k_uptime_get();

	if (lock_down) {
		lock_down = false;
		*key = LOS_KEY_LOCK;
		*pressed = false;
		*more = true;
		return true;
	}

	/* Power: a tap locks or unlocks the keys, a hold switches off */
	if (!(held & BIT(BTN_POWER))) {
		bool tap = armed && power_since && now - power_since < LOCK_TAP_MS;
		armed = true;
		power_since = 0;
		if (tap) {
			lock_down = true;
			*key = LOS_KEY_LOCK;
			*pressed = true;
			*more = true;
			return true;
		}
	} else if (armed) {
		if (!power_since) {
			power_since = now;
		} else if (now - power_since >= OFF_HOLD_MS) {
			switch_off();
		}
	}

	/* Volume: 2 dB a press, to the app (it sets the DAC, or the
	 * headphones' own volume over Bluetooth; samples are never scaled) */
	static uint32_t vol_was;
	uint32_t vol = held & (BIT(BTN_VOL_UP) | BIT(BTN_VOL_DOWN));
	uint32_t pressed_now = vol & ~vol_was;
	vol_was = vol;
	if (pressed_now & BIT(BTN_VOL_UP)) {
		los_app_volume(+2);
	}
	if (pressed_now & BIT(BTN_VOL_DOWN)) {
		los_app_volume(-2);
	}

	/* Wheel: every WHEEL_STEPS steps is one press and release of up or
	 * down (one step per row was too quick to stop on the row wanted;
	 * the existing firmware also moves once per two steps here) */
	int32_t steps = h2_wheel_steps() / WHEEL_STEPS;

	if (wheel_down) {
		wheel_down = false;
		*pressed = false;
		*more = steps != wheel_done;
		return true;
	}
	if (steps != wheel_done) {
		*key = steps > wheel_done ? LV_KEY_DOWN : LV_KEY_UP;
		wheel_done += steps > wheel_done ? 1 : -1;
		*pressed = true;
		*more = true;
		wheel_down = true;
		return true;
	}

	/* Buttons: report a change */
	for (size_t i = 0; i < ARRAY_SIZE(keymap); i++) {
		uint32_t bit = BIT(keymap[i].button);

		if ((held ^ was_held) & bit) {
			was_held = (was_held & ~bit) | (held & bit);
			*key = keymap[i].key;
			*pressed = held & bit;
			*more = true;
			return true;
		}
	}
	return false;
}
