/*
 * Ingenic X1000. Register facts: docs/h2-hardware.md
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOSSLESS_SOC_X1000_H
#define LOSSLESS_SOC_X1000_H

/* CPU interrupt lines (Zephyr IRQ numbers on MIPS) */
#define X1000_IRQ_INTC         2   /* the interrupt controller, every peripheral */
#define X1000_IRQ_OST          3   /* OS timer 1, wired straight to the CPU */
#define MIPS_MACHINE_TIMER_IRQ 7   /* CP0 count/compare */

#ifndef _ASMLANGUAGE
#include <stdint.h>

/* CPU state as the bootloader handed it over, saved by start.S before
 * Zephyr resets it: [0] Status, [1] Cause. */
extern uint32_t x1000_boot_cp0[2];
#endif

#endif
