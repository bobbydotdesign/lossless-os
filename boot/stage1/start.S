/*
 * Our first stage's entry: the X1000 boot ROM jumps here, to 0xf4001800 in
 * on-chip memory (TCSM), from flash or over USB boot (X1000 Programming
 * Manual 27.3, 27.5).
 *
 * Over USB boot a first stage may hand back to the ROM with `jr ra`, so
 * this keeps the ROM's registers: it saves sp, ra and the callee-saved
 * registers, runs on its own stack in the first stage's part of TCSM,
 * zeroes .bss, calls stage1_main() and restores them before returning.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
	.set	noreorder
	.set	noat
	.section .text.start, "ax"
	.globl	_start
_start:
	la	$t0, rom_regs
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

	la	$sp, __stack_top

	la	$t0, __bss_start	/* zero .bss */
	la	$t1, __bss_end
1:	beq	$t0, $t1, 2f
	nop
	sw	$zero, 0($t0)
	b	1b
	addiu	$t0, $t0, 4
2:
	jal	stage1_main
	nop

	/* Invalidate the whole instruction cache (16 KB, index by index)
	 * before handing back: the ROM doesn't, so the next first stage
	 * loaded at this address would otherwise run some of our stale
	 * instructions. Index ops don't touch memory, so kseg0 indices do. */
	.set	push
	.set	mips32
	lui	$t0, 0x8000
	ori	$t1, $zero, 0x4000
	addu	$t1, $t0, $t1
3:	cache	0x00, 0($t0)		/* Index_Invalidate_I */
	addiu	$t0, $t0, 32
	bne	$t0, $t1, 3b
	nop
	.set	pop

	la	$t0, rom_regs
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
	jr	$ra
	nop

	/* In .data, not .bss: .bss is zeroed after these are saved */
	.section .data
	.align	2
rom_regs:
	.space	48
