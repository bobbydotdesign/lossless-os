/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "x1000_i2c.h"

#include <zephyr/kernel.h>
#include <stdbool.h>

#define REG(a)          (*(volatile uint32_t *)(a))
#define I2C(bus, off)   REG(0xb0050000u + (uint32_t)(bus) * 0x1000u + (off))

#define I2C_CON         0x00
#define I2C_TAR         0x04
#define I2C_DC          0x10      /* data and command */
#define I2C_SHCNT       0x14      /* standard speed: high / low counts */
#define I2C_SLCNT       0x18
#define I2C_RINTST      0x34      /* raw interrupt status */
#define I2C_CTXABT      0x54      /* read to clear an abort */
#define I2C_ENABLE      0x6c
#define I2C_STATUS      0x70
#define I2C_RXFLR       0x78      /* bytes waiting in the receive FIFO */
#define I2C_SDAHD       0x7c
#define I2C_SDASU       0x94
#define I2C_ENBST       0x9c

#define CON_MASTER      (1u << 0)
#define CON_SPEED_100K  (1u << 1)
#define CON_RESTART     (1u << 5)
#define CON_SLAVE_OFF   (1u << 6)
#define DC_READ         (1u << 8)
#define DC_STOP         (1u << 9)
#define RINTST_TXABT    (1u << 6)
#define STATUS_TFE      (1u << 2)     /* transmit FIFO empty */
#define STATUS_MSTACT   (1u << 5)     /* master busy */

#define FREQ_HZ         100000u

#define CPM_CCR         0xb0000000u
#define CPM_APCR        0xb0000010u
#define CPM_MPCR        0xb0000014u
#define CPM_CLKGR       0xb0000020u
#define CLKGR_I2C(bus)  (1u << (7 + (bus)))

#define GPIO(port, off) REG(0xb0010000u + (port) * 0x100u + (off))
#define GPIO_INT_CLR    0x18
#define GPIO_MSK_CLR    0x28
#define GPIO_PAT1_CLR   0x38
#define GPIO_PAT1_SET   0x34
#define GPIO_PAT0_SET   0x44
#define GPIO_PAT0_CLR   0x48

static uint32_t pll_hz(uint32_t reg, uint32_t on_bit)
{
	uint32_t v = REG(reg);

	if (!(v & on_bit)) {
		return 0;
	}
	uint64_t hz = 24000000ull * ((v >> 24 & 0x7f) + 1) / ((v >> 18 & 0x1f) + 1);
	return (uint32_t)(hz >> (v >> 16 & 3));
}

/* The peripheral clock: CCR's H2 source (1 SCLK_A, 2 MPLL) / (PDIV + 1) */
static uint32_t pclk_hz(void)
{
	uint32_t ccr = REG(CPM_CCR), src;

	switch (ccr >> 24 & 3) {
	case 1:
		src = (ccr >> 30) == 1 ? 24000000u :
		      (ccr >> 30) == 2 ? pll_hz(CPM_APCR, 1u << 10) : 0;
		break;
	case 2:
		src = pll_hz(CPM_MPCR, 1u << 0);
		break;
	default:
		src = 0;
	}
	return src / ((ccr >> 16 & 0xf) + 1);
}

/* Give a pin to a peripheral: function n is INT 0, MSK 0, PAT1/PAT0 = n */
static void pin_function(int port, int pin, int fn)
{
	uint32_t bit = 1u << pin;

	GPIO(port, GPIO_INT_CLR) = bit;
	GPIO(port, GPIO_MSK_CLR) = bit;
	GPIO(port, (fn & 2) ? GPIO_PAT1_SET : GPIO_PAT1_CLR) = bit;
	GPIO(port, (fn & 1) ? GPIO_PAT0_SET : GPIO_PAT0_CLR) = bit;
}

static bool wait_until(int bus, uint32_t off, uint32_t mask, uint32_t want)
{
	int64_t end = k_uptime_get() + 20;

	while ((I2C(bus, off) & mask) != want) {
		if (k_uptime_get() > end) {
			return false;
		}
	}
	return true;
}

uint32_t x1000_i2c_init(int bus)
{
	uint32_t pclk = pclk_hz();

	if (!pclk || bus < 0 || bus > 2) {
		return 0;
	}
	if (bus == 2) {
		pin_function(3, 0, 1);    /* PD0, PD1: I2C2 (power chip) */
		pin_function(3, 1, 1);
	} else if (bus == 1) {
		pin_function(2, 26, 0);   /* PC26, PC27: I2C1 (DAC) */
		pin_function(2, 27, 0);
	}
	REG(CPM_CLKGR) &= ~CLKGR_I2C(bus);

	I2C(bus, I2C_ENABLE) = 0;
	wait_until(bus, I2C_ENBST, 1, 0);

	uint32_t su = pclk / (FREQ_HZ * 8), hd = pclk / (FREQ_HZ * 12);
	uint32_t half = pclk / (FREQ_HZ * 2);

	I2C(bus, I2C_CON) = CON_MASTER | CON_SPEED_100K | CON_RESTART | CON_SLAVE_OFF;
	I2C(bus, I2C_SDASU) = su > 255 ? 255 : (su > 1 ? su - 1 : su);
	I2C(bus, I2C_SDAHD) = hd > 0xffff ? 0xffff : hd;
	I2C(bus, I2C_SLCNT) = half < 8 ? 8 : half;
	I2C(bus, I2C_SHCNT) = half < 6 ? 6 : half;
	return pclk;
}

/* Address the device: TAR can only change while the controller is off */
static int start(int bus, uint8_t addr)
{
	I2C(bus, I2C_ENABLE) = 0;
	if (!wait_until(bus, I2C_ENBST, 1, 0)) {
		return I2C_TIMEOUT;
	}
	I2C(bus, I2C_TAR) = addr;
	(void)I2C(bus, I2C_CTXABT);
	I2C(bus, I2C_ENABLE) = 1;
	return wait_until(bus, I2C_ENBST, 1, 1) ? 0 : I2C_TIMEOUT;
}

/* Wait for the transfer to end: the FIFO drained and the master idle */
static int finish(int bus)
{
	int64_t end = k_uptime_get() + 20;

	for (;;) {
		if (I2C(bus, I2C_RINTST) & RINTST_TXABT) {
			(void)I2C(bus, I2C_CTXABT);
			return I2C_NO_ACK;
		}
		if ((I2C(bus, I2C_STATUS) & (STATUS_TFE | STATUS_MSTACT)) == STATUS_TFE) {
			return 0;
		}
		if (k_uptime_get() > end) {
			return I2C_TIMEOUT;
		}
	}
}

int x1000_i2c_write(int bus, uint8_t addr, uint8_t reg, uint8_t value)
{
	int err = start(bus, addr);

	if (err) {
		return err;
	}
	I2C(bus, I2C_DC) = reg;
	I2C(bus, I2C_DC) = value | DC_STOP;
	return finish(bus);
}

int x1000_i2c_read(int bus, uint8_t addr, uint8_t reg, uint8_t *data, int count)
{
	int err = start(bus, addr);

	if (err) {
		return err;
	}
	/* The register number, then one read request per byte; the controller
	 * puts a repeated start between them by itself. Stop after the last. */
	I2C(bus, I2C_DC) = reg;
	for (int i = 0; i < count; i++) {
		I2C(bus, I2C_DC) = DC_READ | (i == count - 1 ? DC_STOP : 0);
	}
	int64_t end = k_uptime_get() + 20;
	int got = 0;

	while (got < count) {
		if (I2C(bus, I2C_RINTST) & RINTST_TXABT) {
			(void)I2C(bus, I2C_CTXABT);
			return I2C_NO_ACK;
		}
		if (I2C(bus, I2C_RXFLR)) {
			data[got++] = (uint8_t)I2C(bus, I2C_DC);
		} else if (k_uptime_get() > end) {
			return I2C_TIMEOUT;
		}
	}
	return finish(bus);
}
