/*
 * First instructions of Lossless OS on the X1000.
 *
 * The H2's bootloader copies our image to its load address and jumps
 * here with interrupts off. Its exception handlers are still at the bottom
 * of RAM, so until we have our own, nothing may interrupt us: mask every
 * source in the interrupt controller and close the CPU's interrupt lines.
 * The CPU's Status and Cause go to x1000_boot_cp0 first, for diagnostics
 * (Zephyr's start-up clears Cause, which also turns off the bootloader's
 * separate interrupt vector, and BEV).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/toolchain.h>
#include <mips/regdef.h>
#include <mips/mipsregs.h>

#define INTC_MASK_SET0 0xb0001008
#define INTC_MASK_SET1 0xb0001028
#define STATUS_IM      0x0000ff00   /* CPU interrupt lines 0-7 */

GTEXT(__initialize)
GTEXT(__start)

SECTION_FUNC(vectors, __start)
	.set noreorder
	li	t0, 0xffffffff
	li	t1, INTC_MASK_SET0
	sw	t0, 0(t1)
	li	t1, INTC_MASK_SET1
	sw	t0, 0(t1)

	la	t2, x1000_boot_cp0
	mfc0	t0, CP0_STATUS
	sw	t0, 0(t2)
	mfc0	t1, CP0_CAUSE
	sw	t1, 4(t2)

	li	t1, ~(STATUS_IM | ST0_IE)
	and	t0, t0, t1
	mtc0	t0, CP0_STATUS
	ehb

	la	v0, __initialize
	jr	v0
	nop
