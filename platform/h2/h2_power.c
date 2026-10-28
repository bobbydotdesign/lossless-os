/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "h2_power.h"
#include "x1000_i2c.h"
#include "los_sys.h"

#include <zephyr/kernel.h>

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

enum h2_pmu h2_power_init(uint8_t *chip_id)
{
	uint8_t id = 0;

	*chip_id = 0;
	if (!x1000_i2c_init(BUS) || x1000_i2c_read(BUS, ADDR, REG_CHIP_ID, &id, 1)) {
		return pmu = PMU_NONE;
	}
	*chip_id = id;
	pmu = id == ID_AXP2101 ? PMU_AXP2101 : id == ID_AXP192 ? PMU_AXP192 : PMU_UNKNOWN;
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
bool los_battery(int *percent, bool *charging, bool *usb)
{
	uint8_t s0, s1;
	int p = h2_power_battery_percent();

	if (p < 0 || x1000_i2c_read(BUS, ADDR, 0x00, &s0, 1) || x1000_i2c_read(BUS, ADDR, 0x01, &s1, 1)) {
		return false;
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
