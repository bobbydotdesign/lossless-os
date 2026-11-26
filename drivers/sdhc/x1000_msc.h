/*
 * What the X1000 SD controller driver counts, for diagnostics screens.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef X1000_MSC_H
#define X1000_MSC_H

#include <stdint.h>
#include <zephyr/device.h>

struct x1000_msc_stats {
	uint32_t commands;      /* requests from the SD stack */
	uint32_t blocks;        /* data blocks asked for */
	uint32_t errors;
	uint32_t last_opcode;   /* the last command that failed, and how */
	int last_err;
	uint32_t last_flags;    /* IFLAG */
	uint32_t last_stat;     /* STAT */
};

void x1000_msc_get_stats(const struct device *dev, struct x1000_msc_stats *out);

#endif
