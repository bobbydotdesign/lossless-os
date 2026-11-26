/*
 * Power: the supplies from the AXP2101 power chip (I2C bus 2, 0x34) that
 * the player runs with. Under the boot ROM only some are on (DCDC 1, 2, 4
 * and one regulator); the player's firmware turns on all five converters
 * and every regulator, at the voltages the chip already holds. Lossless OS
 * relies on that (screen, sound, Bluetooth), and memory reads are marginal
 * at full speed without it.
 *
 * Registers (X-Powers AXP2101 datasheet; values as on the running player,
 * research notes step 2): 0x80 converter enables, bits 4:0 = DCDC5..1;
 * 0x90 and 0x91 regulator enables.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "stage1.h"
#include "x1000_i2c.h"

#define AXP 0x34

static int i2c_ready;

/* Breadcrumbs before the power steps (from flash: to see how far a start
 * got); again after the clocks change, as the bus's timing follows them */
#ifdef FLASH_BOOT
void crumbs_on(void)
{
	i2c_ready = x1000_i2c_init(2) != 0;
}

/* A breadcrumb with three bytes of detail (power chip regs 0x04-0x07) */
void crumb4(uint8_t step, uint8_t a, uint8_t b, uint8_t c)
{
	if (!i2c_ready)
		return;
	x1000_i2c_write(2, AXP, 0x04, step);
	x1000_i2c_write(2, AXP, 0x05, a);
	x1000_i2c_write(2, AXP, 0x06, b);
	x1000_i2c_write(2, AXP, 0x07, c);
}
#endif

void crumb(uint8_t step)
{
	if (i2c_ready)
		x1000_i2c_write(2, AXP, 0x04, step);
}

static int set_reg(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t v;
	if (x1000_i2c_read(2, AXP, reg, &v, 1))
		return -1;
	note(reg << 8 | v);
	return x1000_i2c_write(2, AXP, reg, (uint8_t)((v & ~mask) | val));
}

#ifdef POWER_PARAMS
/* Test builds: the writes come from the computer, patched into the image
 * here before it's sent ("PWRS", count, then reg, mask, value per entry). */
__attribute__((section(".data.params"), used))
static volatile struct {
	uint32_t magic, count;
	uint8_t w[16][3];
} params = {0x53525750u, 0, {{0}}};

int power_init(void)
{
	if (!x1000_i2c_init(2))
		return 1;
	for (uint32_t i = 0; i < params.count && i < 16; i++) {
		if (set_reg(params.w[i][0], params.w[i][1], params.w[i][2]))
			return 2;
		delay_us(20000);
		uint8_t v;
		if (x1000_i2c_read(2, AXP, params.w[i][0], &v, 1))
			return 3;
		note(0xb0000000u | (uint32_t)params.w[i][0] << 8 | v);
	}
	return 0;
}
#else
/*
 * One supply at a time, in the order that works on the player, with time
 * to settle (all at once, or 20 ms apart, and the power chip cuts out):
 *   DLDO2's setting to 0.55 V first, as on the running player: switched on
 *   at the ROM's 1.25 V the player freezes;
 *   CPUSLDO ("CPUs / reference of DDR"), DC4SW/DLDO2 (from DCDC4, the
 *   memory supply);
 *   DCDC3, DCDC5;
 *   ALDO2-4, BLDO1-2, DLDO1.
 */
static const uint8_t steps[][4] = {    /* reg, mask, value, ms to wait */
	{0x9a, 0x1f, 0x01, 1},
	{0x90, 0x40, 0x40, 50}, {0x91, 0x01, 0x01, 100},
	{0x80, 0x04, 0x04, 150}, {0x80, 0x10, 0x10, 150},
	{0x90, 0x02, 0x02, 50}, {0x90, 0x04, 0x04, 50}, {0x90, 0x08, 0x08, 50},
	{0x90, 0x10, 0x10, 50}, {0x90, 0x20, 0x20, 50}, {0x90, 0x80, 0x80, 50},
	/* charge current 700 mA as Lossless OS sets it (power chip default
	 * 100 mA: long sessions over USB boot drained the battery) */
	{0x62, 0x1f, 0x0d, 1},
};

int power_init(void)
{
	if (!x1000_i2c_init(2))
		return 1;
	i2c_ready = 1;
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		crumb((uint8_t)(0x10 + i));
		if (set_reg(steps[i][0], steps[i][1], steps[i][2]))
			return 2;
		delay_us(1000u * steps[i][3]);
	}
	uint8_t v[3];
	if (x1000_i2c_read(2, AXP, 0x80, v, 1) || x1000_i2c_read(2, AXP, 0x90, v + 1, 2))
		return 3;
	note(0xa0000000u | v[0] << 16 | v[1] << 8 | v[2]);
	return 0;
}
#endif
