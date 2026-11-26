/*
 * The first stage in the X1000's on-chip memory (TCSM, 16 KB at
 * 0xf4000000): the ROM runs it from 0xf4001800 and gives it up to
 * 0xf4004000 (X1000 Programming Manual 27.3). Over USB boot the ROM
 * itself uses the top of that (a first stage writing there never handed
 * back), so ours stays low: code and data from 0xf4001800, the stack up to
 * 0xf4002e00, and the status block (1 KB) the computer reads back above it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { tcsm (rwx) : ORIGIN = 0xf4001800, LENGTH = 0x1200 }
SECTIONS
{
	.text : { *(.text.start) *(.text*) } > tcsm
	.rodata : { *(.rodata*) } > tcsm
	.data : { *(.data.params) *(.data*) *(.sdata*) } > tcsm
	.bss (NOLOAD) : {
		. = ALIGN(4);
		__bss_start = .;
		*(.bss*) *(.sbss*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > tcsm
	__stack_top = 0xf4002e00;
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
ASSERT(__bss_end <= 0xf4002a00, "first stage too big: keep 1 KB of stack")
