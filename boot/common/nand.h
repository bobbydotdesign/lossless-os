/*
 * The player's SPI NAND flash, for reading (nand.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_NAND_H
#define BOOT_NAND_H

#include <stdint.h>

#define NAND_PAGE        2048u
#define NAND_BLOCK_PAGES 64u

/* Pins, clock, reset; *id = the JEDEC ID (0xefaa21 Winbond, 0x0b11xx XTX: NAND_KNOWN). Main memory
 * must be running: data comes by DMA through a buffer at physical 14 MB.
 * 0 or < 0. */
int nand_init(uint32_t *id);

/* len bytes from column col of a page; *ecc = the flash's ECC result
 * (0xff: unknown, as status reads don't work yet). 0 or < 0. */
/* Every block writable: protection register (0xa0) set to 0. The chip
 * powers up with all of its blocks write-protected; a bootloader clears
 * that before Linux, whose flash driver doesn't, and which writes the
 * flash (its file systems, the recovery's updater). 0 or < 0. */
int nand_unlock(void);
/* Writing (only the bootloader writer uses these: boot/stage2, MODE_FLASH):
 * erase the 128 KB block holding page; program one whole page (2048
 * bytes) that has been erased. Each sends write enable first and then
 * waits the datasheet's longest time (no status reads: see nand.c);
 * the caller checks by reading back. 0 or < 0. */
int nand_erase_block(uint32_t page);
int nand_program_page(uint32_t page, const uint8_t *data);
int nand_block_bad(uint32_t block);
int nand_read_page(uint32_t page, uint32_t col, uint8_t *buf, uint32_t len, uint32_t *ecc);

/* One transfer (command, address of width bytes, dummy clocks, len bytes
 * of data). 0 or < 0. */
int nand_xfer(uint32_t cmd, uint32_t addr, uint32_t width, uint32_t dummy,
	      uint8_t *buf, uint32_t len);

/* len bytes of a partition (flash bytes start..end, block aligned) into
 * buf, skipping bad blocks as they come. 0 or < 0. */
int nand_read(uint32_t start, uint32_t end, uint8_t *buf, uint32_t len);

/* For tests: what the last transfer saw (nand.c) */
extern uint32_t nand_debug[8];

/* A flash chip this driver knows (see nand.c) */
#define NAND_KNOWN(id) ((id) == 0xefaa21u || ((id) >> 8) == 0x0b11u)

#endif
