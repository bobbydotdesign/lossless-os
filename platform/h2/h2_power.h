/*
 * The H2's power chip (AXP2101 on hardware revision 4, AXP192 before) on
 * I2C bus 2. Facts: docs/h2-hardware.md, "Power".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_POWER_H
#define H2_POWER_H

#include <stdint.h>

enum h2_pmu { PMU_NONE, PMU_AXP192, PMU_AXP2101, PMU_UNKNOWN };

/* Find the chip. *chip_id gets what its ID register said. */
enum h2_pmu h2_power_init(uint8_t *chip_id);

/* Battery in millivolts and percent (AXP2101 only), or negative */
int h2_power_battery_mv(void);
int h2_power_battery_percent(void);

/* Ask the chip to switch everything off. Returns only if that failed
 * (negative), after about half a second. */
int h2_power_off(void);

#endif
