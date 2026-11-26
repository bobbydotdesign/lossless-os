/* go.bin in memory at 0xa0f00000 (kseg1, uncached). SPDX-License-Identifier: Apache-2.0 */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0xa0f00000, LENGTH = 4K }
SECTIONS
{
	.text : { *(.text*) } > ram
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) }
}
