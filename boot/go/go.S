/*
 * The last step before Lossless OS: set up the caches and turn caching on
 * for kseg0, then jump to the image already in memory. Lossless OS takes
 * both for granted (it never initialises the caches); the boot ROM leaves
 * them as it likes, and without this Lossless OS runs far slower.
 *
 * MIPS32: index-store-tag with TagLo = 0 on every line of the 16 KB
 * instruction and data caches (32-byte lines; index ops don't touch
 * memory, so kseg0 indices do), then Config.K0 (bits 2:0) = 3, cacheable.
 * Runs from kseg1 (uncached), so changing K0 doesn't affect it.
 *
 * Entry point of the image to start: GO_ENTRY.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
	.set	noreorder
	.set	noat
	.set	mips32
	.text
	.globl	_start
_start:
	mfc0	$t0, $12		/* Status: interrupts off */
	li	$t1, ~1
	and	$t0, $t0, $t1
	mtc0	$t0, $12
	ehb

	mtc0	$zero, $28, 0		/* TagLo */
	mtc0	$zero, $29, 0		/* TagHi */
	lui	$t0, 0x8000
	ori	$t1, $zero, 0x4000
	addu	$t1, $t0, $t1
1:	cache	0x08, 0($t0)		/* Index_Store_Tag_I */
	cache	0x09, 0($t0)		/* Index_Store_Tag_D */
	addiu	$t0, $t0, 32
	bne	$t0, $t1, 1b
	nop

	mfc0	$t0, $16		/* Config.K0 = 3: kseg0 cacheable */
	li	$t1, ~7
	and	$t0, $t0, $t1
	ori	$t0, $t0, 3
	mtc0	$t0, $16
	ehb

	li	$t0, GO_ENTRY
	jr.hb	$t0
	nop
