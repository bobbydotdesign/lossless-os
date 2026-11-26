/*
 * First test of our own boot code: run from the X1000's on-chip memory
 * over USB boot (tools/h2-usb), blink the backlight three times, leave it
 * on and hand back to the boot ROM.
 *
 * Uses no stack and only the temporary registers, so the ROM's state is
 * untouched and `jr ra` returns to it (X1000 Programming Manual 27.5: a
 * first stage "can return back to the internal boot ROM by jumping to ra").
 *
 * Backlight: pin PC25, high = lit (docs/h2-hardware.md, "Backlight").
 * GPIO port C at 0xb0010200; each of INT (+0x10), MSK (+0x20), PAT1 (+0x30)
 * and PAT0 (+0x40) has set (+4) and clear (+8) registers. A plain output is
 * INT=0, MSK=1, PAT1=0, and PAT0 is its level ("GPIO").
 *
 * The CPU clock under the ROM isn't known yet, so the delay is a fixed
 * loop count; how long a blink lasts tells us roughly how fast it runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
	.set	noreorder
	.set	noat
	.text
	.globl	_start
_start:
	lui	$t0, 0xb001
	ori	$t0, $t0, 0x0200	/* GPIO port C */
	lui	$t1, 0x0200		/* bit 25 */

	sw	$t1, 0x18($t0)		/* INT clear */
	sw	$t1, 0x24($t0)		/* MSK set */
	sw	$t1, 0x38($t0)		/* PAT1 clear */

	li	$t2, 3			/* blinks */
1:
	sw	$t1, 0x48($t0)		/* PAT0 clear: dark */
	lui	$t3, 0x0080		/* about 8 million loop turns */
2:	addiu	$t3, $t3, -1
	bnez	$t3, 2b
	nop

	sw	$t1, 0x44($t0)		/* PAT0 set: lit */
	lui	$t3, 0x0080
3:	addiu	$t3, $t3, -1
	bnez	$t3, 3b
	nop

	addiu	$t2, $t2, -1
	bnez	$t2, 1b
	nop

	jr	$ra			/* back to the ROM, backlight left on */
	nop
