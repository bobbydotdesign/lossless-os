/*
 * X1000 start-up in C.
 *
 * Zephyr copies its exception entry to 0x80000180 with ordinary (cached)
 * stores and switches to the first thread through an exception. On real
 * hardware the new code can still be sitting in the data cache while the
 * instruction cache holds the bootloader's old entry, so write it out and
 * drop the stale copy before the first switch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/init.h>
#include <zephyr/toolchain.h>
#include <zephyr/linker/section_tags.h>
#include <stdint.h>
#include <soc.h>
#include <zephyr/irq.h>

#define REG(a)      (*(volatile uint32_t *)(a))

#define CACHE_LINE  32
#define VECTOR      0x80000180u
#define VECTOR_SIZE 0x80u

/* Low-power mode for the CPU's "wait" (Zephyr's idle): IDLE stops only the
 * CPU until the next interrupt; SLEEP would stop the clocks too. */
#define CPM_LCR     0xb0000004u

/* The interrupt controller's mask-set registers: nothing of ours goes
 * through it yet (the tick is on its own CPU line), and whoever started us
 * (the bootloader's uImage path, or its card path, which leaves the LCD's
 * interrupts on) may have left sources open */
#define INTC_MASK_SET0 0xb0001008u
#define INTC_MASK_SET1 0xb0001028u
#define LCR_LPM     (3u << 0)
#define LPM_IDLE    0u

__noinit uint32_t x1000_boot_cp0[2];

static int x1000_init(void)
{
	for (uint32_t a = VECTOR; a < VECTOR + VECTOR_SIZE; a += CACHE_LINE) {
		/* Hit_Writeback_Invalidate_D, then Hit_Invalidate_I */
		__asm__ volatile(".set push\n.set noreorder\n"
				 "cache 0x15, 0(%0)\n"
				 "sync\n"
				 "cache 0x10, 0(%0)\n"
				 ".set pop" : : "r"(a) : "memory");
	}
	__asm__ volatile("sync\nehb" : : : "memory");

	REG(INTC_MASK_SET0) = 0xffffffffu;
	REG(INTC_MASK_SET1) = 0xffffffffu;
	/* The interrupt controller's CPU line stays open from here on, with
	 * each source let through by its mask above. Opened now, before any
	 * thread exists: Zephyr keeps the CPU's interrupt mask in each
	 * thread's saved state, so a line opened later from one thread stays
	 * shut whenever another runs (USB interrupts were taken up to 2 ms
	 * late that way, at the next timer tick). */
	irq_enable(X1000_IRQ_INTC);
	REG(CPM_LCR) = (REG(CPM_LCR) & ~LCR_LPM) | LPM_IDLE;
	return 0;
}

SYS_INIT(x1000_init, PRE_KERNEL_1, 0);
