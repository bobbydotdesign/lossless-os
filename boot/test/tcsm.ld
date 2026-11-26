/* The X1000's on-chip memory as the ROM runs a first stage from it:
 * code from 0xf4001800 to the end at 0xf4004000 (manual 27.3).
 * SPDX-License-Identifier: Apache-2.0 */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { tcsm (rwx) : ORIGIN = 0xf4001800, LENGTH = 10K }
SECTIONS
{
	.text : { *(.text*) *(.rodata*) *(.data*) } > tcsm
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) }
}
