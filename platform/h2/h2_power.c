/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "h2_power.h"
#include "x1000_i2c.h"
#include "h2_usb.h"
#include "los_sys.h"

#include <zephyr/kernel.h>
#include <stdio.h>

#define BUS          2
#define ADDR         0x34

#define REG_CHIP_ID  0x03
#define ID_AXP192    0x03
#define ID_AXP2101   0x4a

/* AXP2101 */
#define A2101_COMM_CONFIG  0x10   /* bit 0: switch off */
#define A2101_VBAT_H       0x34   /* battery voltage, 14 bits, 1 mV each */
#define A2101_BATT_PERCENT 0xa4

/* AXP192 */
#define A192_OFF_CTRL      0x32   /* bit 7: switch off */

static enum h2_pmu pmu;

/* Change some bits of a register, leaving the others as they are */
static int set_bits(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t v;
	int err = x1000_i2c_read(BUS, ADDR, reg, &v, 1);

	return err ? err : x1000_i2c_write(BUS, ADDR, reg, (uint8_t)((v & ~mask) | val));
}

/* The charger. Found at power-on: 100 mA charge current and a 500 mA
 * USB input limit, so a full charge took about 10 hours (power report,
 * H2, 2026-10-30). HiFi Walker: a 1500 mAh battery, about 3 hours to
 * full from 5 V / 2 A, so 700 mA (under half the capacity per hour).
 * The input limit follows the source: 500 mA on a computer, what the
 * player asks it for (h2_usb.c, its configuration: bus powered, 500 mA),
 * and also while it isn't known yet; the chip's own 1.5 A on a charger.
 * Where a source gives less, the chip lowers the charge current itself as
 * the input sags to 4.36 V (VINDPM, left as found). Full voltage left as
 * found. X-Powers AXP2101 datasheet V1.0: REG 16 bits 2:0 (001b 500 mA,
 * 100b 1500 mA), REG 62 bits 4:0 (01101b 700 mA). */
#define A2101_INPUT_LIMIT  0x16
#define A2101_ICC          0x62
#define INPUT_500MA        0x01
#define INPUT_1500MA       0x04
#define ICC_700MA          0x0d

static int input_set = -1;

/* From los_battery, so on the one thread that uses this bus */
static void input_follow(void)
{
	int want = h2_usb_source() == H2_USB_CHARGER ? INPUT_1500MA : INPUT_500MA;

	if (want != input_set && !set_bits(A2101_INPUT_LIMIT, 0x07, (uint8_t)want)) {
		input_set = want;
	}
}

static void charger_setup(void)
{
	if (!set_bits(A2101_INPUT_LIMIT, 0x07, INPUT_500MA)) {
		input_set = INPUT_500MA;
	}
	set_bits(A2101_ICC, 0x1f, ICC_700MA);
}

enum h2_pmu h2_power_init(uint8_t *chip_id)
{
	uint8_t id = 0;

	*chip_id = 0;
	if (!x1000_i2c_init(BUS) || x1000_i2c_read(BUS, ADDR, REG_CHIP_ID, &id, 1)) {
		return pmu = PMU_NONE;
	}
	*chip_id = id;
	pmu = id == ID_AXP2101 ? PMU_AXP2101 : id == ID_AXP192 ? PMU_AXP192 : PMU_UNKNOWN;
	if (pmu == PMU_AXP2101) {
		charger_setup();
	}
	return pmu;
}

int h2_power_battery_mv(void)
{
	uint8_t v[2];

	if (pmu != PMU_AXP2101 || x1000_i2c_read(BUS, ADDR, A2101_VBAT_H, v, 2)) {
		return -1;
	}
	return (v[0] & 0x3f) << 8 | v[1];
}

int h2_power_battery_percent(void)
{
	uint8_t p;

	if (pmu != PMU_AXP2101 || x1000_i2c_read(BUS, ADDR, A2101_BATT_PERCENT, &p, 1)) {
		return -1;
	}
	return p <= 100 ? p : -1;
}

/* For the app (los_sys.h): AXP2101 register 0x01 bits 6:5 = 1 while
 * charging; register 0x00 bit 5 = USB power present */
/* ---- the power report: the charger's settings and state, once a minute
 * for the last four hours, written to the card when it goes to the
 * computer. Read only. Registers and their meaning: X-Powers' AXP2101
 * (in Zephyr's charger and fuel gauge drivers, from its datasheet): 0x00
 * bit 3 battery present, bit 5 USB power; 0x01 bits 2:0 charging phase;
 * 0x18 bit 1 charger on, bit 3 gauge on; 0x61 pre-charge, 25 mA steps;
 * 0x62 charge current; 0x63 bits 3:0 end-of-charge current, 25 mA steps,
 * bit 4 on; 0x64 full voltage; 0x15 bits 3:0 input voltage limit,
 * 3.88 V + 80 mV steps; 0x16 bits 2:0 input current limit (X-Powers
 * AXP2101 datasheet V1.0). */
#define PWR_N        240
#define PWR_MS       60000

static const uint8_t pwr_regs[] = { 0x00, 0x01, 0x15, 0x16, 0x18, 0x61, 0x62, 0x63, 0x64 };

struct pwr {
	uint32_t t;                         /* seconds since power-on */
	int8_t pct;
	int16_t mv;
	uint8_t r[sizeof(pwr_regs)];
};

static struct pwr pwr_log[PWR_N];
static unsigned pwr_n;
static int64_t pwr_next;
static K_MUTEX_DEFINE(pwr_lock);

/* Called from los_battery, so on the one thread that uses this bus */
static void pwr_take(int percent)
{
	struct pwr e = { .t = (uint32_t)(k_uptime_get() / 1000), .pct = (int8_t)percent };

	e.mv = (int16_t)h2_power_battery_mv();
	for (size_t i = 0; i < sizeof(pwr_regs); i++) {
		if (x1000_i2c_read(BUS, ADDR, pwr_regs[i], &e.r[i], 1)) {
			return;
		}
	}
	k_mutex_lock(&pwr_lock, K_FOREVER);
	pwr_log[pwr_n++ % PWR_N] = e;
	k_mutex_unlock(&pwr_lock);
}

static const char *const phases[] = {
	"trickle", "pre-charge", "constant current", "constant voltage (tapering)",
	"done", "not charging", "?", "?",
};
static const uint16_t icc_ma[] = { 0, 0, 0, 0, 100, 125, 150, 175, 200, 300, 400, 500, 600, 700, 800, 900, 1000 };
static const uint16_t in_ma[] = { 100, 500, 900, 1000, 1500, 2000 };
static const uint16_t cv_mv[] = { 0, 4000, 4100, 4200, 4350, 4400 };

static int pwr_line(char *b, size_t n, const struct pwr *e)
{
	const uint8_t *r = e->r;    /* 00 01 15 16 18 61 62 63 64 */
	unsigned icc = r[6] & 0x1f, cv = r[8] & 7;

	return snprintf(b, n,
		"  %4um %3d%% %4d mV | USB %d, battery %d | %s | charger %s, gauge %s"
		" | charge %u mA (62=%02x), pre %u mA, end %u mA%s, full %u mV (64=%02x)"
		" | input limit %u mA (16=%02x), %u mV (15=%02x) | 00=%02x 01=%02x 18=%02x\n",
		(unsigned)(e->t / 60), e->pct, e->mv, (r[0] >> 5) & 1, (r[0] >> 3) & 1,
		phases[r[1] & 7], (r[4] & 2) ? "on" : "off", (r[4] & 8) ? "on" : "off",
		icc < 17 ? icc_ma[icc] : 0, r[6], (r[5] & 0xf) * 25, (r[7] & 0xf) * 25,
		(r[7] & 0x10) ? "" : " (off)", cv < 6 ? cv_mv[cv] : 0, r[8],
		(r[3] & 7) < 6 ? in_ma[r[3] & 7] : 0, r[3], 3880 + (r[2] & 0xf) * 80, r[2],
		r[0], r[1], r[4]);
}

void h2_power_save_report(void)
{
	static char text[PWR_N * 200 + 300];
	static const char path[] = "/SD:/.lossless/logs/power.txt";
	int len;

	len = snprintf(text, sizeof(text),
		"Power, once a minute (minutes since power-on), newest last:\n");
	k_mutex_lock(&pwr_lock, K_FOREVER);
	unsigned first = pwr_n > PWR_N ? pwr_n - PWR_N : 0;
	for (unsigned i = first; i < pwr_n && len < (int)sizeof(text) - 200; i++) {
		len += pwr_line(text + len, sizeof(text) - len, &pwr_log[i % PWR_N]);
	}
	k_mutex_unlock(&pwr_lock);
	los_mkdir("/SD:/.lossless");
	los_mkdir("/SD:/.lossless/logs");
	los_file_save(path, text, (size_t)len);
}

bool los_battery(int *percent, bool *charging, bool *usb)
{
	uint8_t s0, s1;
	int p = h2_power_battery_percent();

	if (p < 0 || x1000_i2c_read(BUS, ADDR, 0x00, &s0, 1) || x1000_i2c_read(BUS, ADDR, 0x01, &s1, 1)) {
		return false;
	}
	input_follow();
	if (k_uptime_get() >= pwr_next) {
		pwr_next = k_uptime_get() + PWR_MS;
		pwr_take(p);
	}
	*percent = p;
	*charging = ((s1 >> 5) & 3) == 1;
	*usb = (s0 & 0x20) != 0;
	return true;
}

/* Set one bit in a register, leaving the others as they are */
static int set_bit(uint8_t reg, uint8_t bit)
{
	uint8_t v;
	int err = x1000_i2c_read(BUS, ADDR, reg, &v, 1);

	return err ? err : x1000_i2c_write(BUS, ADDR, reg, v | bit);
}

int h2_power_off(void)
{
	int err;

	switch (pmu) {
	case PMU_AXP2101: err = set_bit(A2101_COMM_CONFIG, 1u << 0); break;
	case PMU_AXP192:  err = set_bit(A192_OFF_CTRL, 1u << 7); break;
	default:          return -3;
	}
	if (err) {
		return err;
	}
	k_msleep(500);    /* power should be gone by now */
	return -4;
}
