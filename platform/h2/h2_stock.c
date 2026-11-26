/*
 * The original firmware's system partition, read only (los_stock_*,
 * los_sys.h), for the Bluetooth patch it carries (app/stockfw.c); and the
 * bootloader partition, read only (los_boot_flash_read), to tell whether
 * our update file's bootloader is in (app/install.c).
 *
 * The flash: the bootloader's driver (boot/common/nand.c), reads only.
 * Nothing here writes, erases or unlocks the flash. The partition: the
 * original firmware's "rootfs", flash 0x1000000 to 0x5000000 (its
 * kernel's partition table; research/own-bootloader/b-binaries.md,
 * section 3), UBI with UBIFS inside.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <string.h>
#include "los_sys.h"
#include "nand.h"

#define STOCK_START  0x1000000u
#define STOCK_SIZE   0x4000000u
#define BLOCK_BYTES  (NAND_PAGE * NAND_BLOCK_PAGES)
#define STOCK_BLOCKS (STOCK_SIZE / BLOCK_BYTES)

/* The flash driver's DMA buffer (nand.c, NAND_BOUNCE_EXTERN): its own
 * cache lines, flushed once, then only ever used uncached */
uint32_t nand_bounce_phys;
static uint8_t bounce[4096] __aligned(32);

static enum { NOT_YET, READY, NO_FLASH } state;
static uint8_t block_state[STOCK_BLOCKS];        /* 0 unknown, 1 good, 2 bad */
static K_MUTEX_DEFINE(lock);

static bool ready(void)
{
    if (state == NOT_YET) {
        for (uintptr_t a = (uintptr_t)bounce; a < (uintptr_t)bounce + sizeof(bounce); a += 32)
            __asm__ volatile("cache 0x15, 0(%0)" : : "r"(a) : "memory");   /* write back, invalidate */
        __asm__ volatile("sync" : : : "memory");
        nand_bounce_phys = (uint32_t)(uintptr_t)bounce & 0x1fffffffu;
        uint32_t id = 0;
        state = !nand_init(&id) && NAND_KNOWN(id) ? READY : NO_FLASH;
    }
    return state == READY;
}

bool los_stock_info(uint32_t *block_size, uint32_t *blocks)
{
    k_mutex_lock(&lock, K_FOREVER);
    bool ok = ready();
    k_mutex_unlock(&lock);
    *block_size = BLOCK_BYTES;
    *blocks = STOCK_BLOCKS;
    return ok;
}

bool los_stock_read(uint32_t block, uint32_t offset, void *buf, uint32_t len)
{
    uint8_t *out = buf;
    bool ok = true;

    if (block >= STOCK_BLOCKS || offset > BLOCK_BYTES || len > BLOCK_BYTES - offset)
        return false;
    k_mutex_lock(&lock, K_FOREVER);
    uint32_t flash_block = STOCK_START / BLOCK_BYTES + block;
    if (!ready()) {
        ok = false;
    } else {
        if (!block_state[block])
            block_state[block] = nand_block_bad(flash_block) ? 2 : 1;
        ok = block_state[block] == 1;
    }
    while (ok && len) {
        uint32_t page = flash_block * NAND_BLOCK_PAGES + offset / NAND_PAGE;
        uint32_t col = offset % NAND_PAGE, n = NAND_PAGE - col;
        if (n > len)
            n = len;
        ok = !nand_read_page(page, col, out, n, NULL);
        out += n;
        offset += n;
        len -= n;
    }
    k_mutex_unlock(&lock);
    return ok;
}

#define BOOT_SIZE 0x80000u                       /* 4 blocks, as our bootloader writes them */

bool los_boot_flash_read(uint32_t offset, void *buf, uint32_t len)
{
    uint8_t *out = buf;

    if (offset > BOOT_SIZE || len > BOOT_SIZE - offset)
        return false;
    k_mutex_lock(&lock, K_FOREVER);
    bool ok = ready();
    while (ok && len) {
        uint32_t col = offset % NAND_PAGE, n = NAND_PAGE - col;
        if (n > len)
            n = len;
        ok = !nand_read_page(offset / NAND_PAGE, col, out, n, NULL);
        out += n;
        offset += n;
        len -= n;
    }
    k_mutex_unlock(&lock);
    return ok;
}
