/*
 * Stand-in for the one Zephyr call our I2C driver (platform/h2/x1000_i2c.c)
 * uses, so the first stage can build it unchanged: k_uptime_get() as a
 * count of calls. Its 20 ms timeouts become 20,000 polls, far longer than
 * a byte at 100 kHz needs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef STAGE1_SHIM_KERNEL_H
#define STAGE1_SHIM_KERNEL_H

#include <stdint.h>
#include <stddef.h>

#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

static inline int64_t k_uptime_get(void)
{
	static uint32_t calls;
	return calls++ / 1000;
}

#endif
