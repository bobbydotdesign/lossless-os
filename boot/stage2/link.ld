/* The second stage in main memory: code and data from 0x81e00000, stack
 * below 0x81f00000. Clear of where it loads things: Lossless OS's image
 * at 0x81000000, the original firmware's kernel unpacked at 0x80010000,
 * packed at 0x81000000.
 * SPDX-License-Identifier: Apache-2.0 */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0x81e00000, LENGTH = 0xf0000 }
SECTIONS
{
	.text : { *(.text.start) *(.text*) } > ram
	.rodata : { *(.rodata*) } > ram
	.data : { *(.data.params) *(.data*) *(.sdata*) } > ram
	.bss (NOLOAD) : {
		. = ALIGN(4);
		__bss_start = .;
		*(.bss*) *(.sbss*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > ram
	__stack_top = 0x81f00000;
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
