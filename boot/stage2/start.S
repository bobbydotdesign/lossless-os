/*
 * Our second stage's entry, in main memory. The first stage (or, for
 * tests, the computer over USB boot) puts it at 0x81e00000 and starts it
 * through the uncached window, at 0xa1e00000.
 *
 * Before anything touches kseg0 it sets up the caches (index-store-tag on
 * every line of the caches, the 128 KB L2 included) and makes kseg0 cacheable
 * (Config.K0 = 3), then continues in kseg0. It keeps the caller's
 * registers and Config at 0xa0fff800, so on failure it can hand back to the
 * USB boot ROM (stage2_main returns) with kseg0 as it was.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
	.set	noreorder
	.set	noat
	.set	mips32
	.section .text.start, "ax"
	.globl	_start
_start:
	lui	$t0, 0xa0ff
	ori	$t0, $t0, 0xf800		/* saved registers, uncached */
	sw	$sp, 0($t0)
	sw	$ra, 4($t0)
	sw	$s0, 8($t0)
	sw	$s1, 12($t0)
	sw	$s2, 16($t0)
	sw	$s3, 20($t0)
	sw	$s4, 24($t0)
	sw	$s5, 28($t0)
	sw	$s6, 32($t0)
	sw	$s7, 36($t0)
	sw	$gp, 40($t0)
	sw	$fp, 44($t0)
	mfc0	$t1, $16
	sw	$t1, 48($t0)			/* Config, for the way back */
	mfc0	$t1, $12
	sw	$t1, 52($t0)			/* Status */

	mfc0	$t1, $12			/* interrupts off */
	li	$t2, ~1
	and	$t1, $t1, $t2
	mtc0	$t1, $12
	ehb

	/* Every cache line invalid: 128 KB of index stores, the size of the
	 * unified L2 (X1000 Programming Manual 2.2: the L1 cache instructions
	 * manage the L2 too), which also covers the 16 KB L1s. Started from
	 * flash, the boot ROM leaves both as they powered up: started over
	 * USB, it has set them up already. */
	mtc0	$zero, $28, 0			/* TagLo */
	mtc0	$zero, $29, 0			/* TagHi */
	lui	$t1, 0x8000
	lui	$t2, 0x0002			/* 128 KB */
	addu	$t2, $t1, $t2
1:	cache	0x08, 0($t1)			/* Index_Store_Tag_I */
	cache	0x09, 0($t1)			/* Index_Store_Tag_D */
	addiu	$t1, $t1, 32
	bne	$t1, $t2, 1b
	nop

	mfc0	$t1, $16			/* Config.K0 = 3: cacheable */
	li	$t2, ~7
	and	$t1, $t1, $t2
	ori	$t1, $t1, 3
	mtc0	$t1, $16
	ehb

	la	$t1, 2f				/* on in kseg0 */
	jr.hb	$t1
	nop
2:
	la	$sp, __stack_top
	la	$t1, __bss_start
	la	$t2, __bss_end
3:	beq	$t1, $t2, 4f
	nop
	sw	$zero, 0($t1)
	b	3b
	addiu	$t1, $t1, 4
4:
	jal	stage2_main
	nop

	/* Failed: write back the data cache, kseg0 back as it was, then the
	 * caller's registers and back to it */
	lui	$t1, 0x8000
	ori	$t2, $zero, 0x4000
	addu	$t2, $t1, $t2
5:	cache	0x01, 0($t1)			/* Index_Writeback_Inv_D */
	addiu	$t1, $t1, 32
	bne	$t1, $t2, 5b
	nop
	sync
	lui	$t0, 0xa0ff
	ori	$t0, $t0, 0xf800
	la	$t1, 6f
	lui	$t2, 0x2000
	or	$t1, $t1, $t2			/* the rest uncached */
	jr.hb	$t1
	nop
6:	lw	$t1, 48($t0)
	mtc0	$t1, $16
	ehb
	lui	$t1, 0x8000
	ori	$t2, $zero, 0x4000
	addu	$t2, $t1, $t2
7:	cache	0x00, 0($t1)			/* Index_Invalidate_I */
	addiu	$t1, $t1, 32
	bne	$t1, $t2, 7b
	nop
	lw	$sp, 0($t0)
	lw	$ra, 4($t0)
	lw	$s0, 8($t0)
	lw	$s1, 12($t0)
	lw	$s2, 16($t0)
	lw	$s3, 20($t0)
	lw	$s4, 24($t0)
	lw	$s5, 28($t0)
	lw	$s6, 32($t0)
	lw	$s7, 36($t0)
	lw	$gp, 40($t0)
	lw	$fp, 44($t0)
	lw	$t1, 52($t0)
	mtc0	$t1, $12
	ehb
	jr.hb	$ra
	nop

/*
 * For tests: an exception taken after the jump to the original firmware's
 * kernel, before it installs its own handlers, lands here (stage2.c puts a
 * jump to this at the vectors). On a stack of its own, it reports Cause,
 * EPC and BadVAddr in the power chip's breadcrumbs, then waits.
 */
	.text
	.globl	trap_entry
trap_entry:
	lui	$sp, 0x81f8
	mfc0	$a0, $13			/* Cause */
	mfc0	$a1, $14			/* EPC */
	mfc0	$a2, $8				/* BadVAddr */
	jal	trap_report
	nop
1:	b	1b
	nop
