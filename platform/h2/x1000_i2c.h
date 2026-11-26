/*
 * The X1000's I2C controllers, simple and polled: enough to talk to the
 * power chip. A Zephyr I2C driver (interrupts, the i2c API) comes later.
 * Register facts: docs/h2-hardware.md, "I2C".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef X1000_I2C_H
#define X1000_I2C_H

#include <stdint.h>

/* Errors (negative): no answer, or the bus stayed busy */
#define I2C_NO_ACK   (-1)
#define I2C_TIMEOUT  (-2)

/* Set up controller `bus` (0-2) at 100 kHz. Returns the peripheral clock
 * in Hz it worked from (0 if unknown, then it can't run). */
uint32_t x1000_i2c_init(int bus);

/* Read `count` registers from `reg` on. 0 or an error. */
int x1000_i2c_read(int bus, uint8_t addr, uint8_t reg, uint8_t *data, int count);

/* Write one register. 0 or an error. */
int x1000_i2c_write(int bus, uint8_t addr, uint8_t reg, uint8_t value);

#endif
