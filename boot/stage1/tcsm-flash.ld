/*
 * The first stage as the boot ROM loads it from flash: it copies the
 * image's first `length` bytes into on-chip memory at 0xf4001000 (the 2 KB
 * header) and runs the code at 0xf4001800, up to 0xf4004000 (X1000
 * Programming Manual 27.3). The ROM is done by then, so the 4 KB below
 * 0xf4001000 is ours, for the stack. The status block is the top 1 KB
 * (0xf4003c00), where the ROM's USB boot mode leaves it alone, so after a
 * start from flash that stuck it can be read over USB boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { tcsm (rwx) : ORIGIN = 0xf4001800, LENGTH = 0x2400 }
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
	__stack_top = 0xf4001000;
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
