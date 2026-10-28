/*
 * Zephyr's system timer on the X1000's OS timer (OST).
 *
 * Two parts of the OST, facts in docs/h2-hardware.md:
 *  - timer 1 counts up to its compare value, restarts from zero and raises an
 *    interrupt on CPU line 3 (not through the interrupt controller): our tick,
 *    CONFIG_SYS_CLOCK_TICKS_PER_SEC times a second;
 *  - counter 2 is a free-running 64-bit count of the 24 MHz crystal / 4:
 *    our cycle counter, so k_busy_wait() works even without interrupts.
 *
 * A periodic tick for now (no tickless mode). Time comes from counter 2,
 * not from counting interrupts: an interrupt that comes late, or a timer
 * period a count longer than asked, would make the clock run slow (the
 * Bluetooth audio, paced by it, then slowly starved the headphones).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/sys_clock.h>
#include <stdint.h>
#include <soc.h>

#define REG(a)            (*(volatile uint32_t *)(a))

#define CPM_CLKGR         0xb0000020u
#define CLKGR_OST         (1u << 20)        /* 1 = clock stopped */

#define OST_CTRL          0xb2000000u
#define OST_ENABLE        0xb2000004u
#define OST_CLEAR         0xb2000008u
#define OST_1FLG          0xb200000cu
#define OST_1MSK          0xb2000010u
#define OST_1DFR          0xb2000014u
#define OST_1CNT          0xb2000018u
#define OST_2CNTL         0xb2000020u
#define OST_ENABLE_SET    0xb2000034u
#define OST_ENABLE_CLR    0xb2000038u

#define OST1              (1u << 0)
#define OST2              (1u << 1)
#define CTRL_PRESCALE1    (3u << 0)
#define CTRL_PRESCALE2    (3u << 2)
#define PRESCALE1_BY_16   (2u << 0)
#define PRESCALE2_BY_4    (1u << 2)

#define CRYSTAL_HZ        24000000u
#define TIMER1_HZ         (CRYSTAL_HZ / 16)

BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == CRYSTAL_HZ / 4,
	     "cycles come from OST counter 2 (crystal / 4)");
BUILD_ASSERT(TIMER1_HZ % CONFIG_SYS_CLOCK_TICKS_PER_SEC == 0,
	     "the tick must be a whole number of timer 1 counts");

#define CYCLES_PER_TICK (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

static uint32_t announced;   /* counter 2 at the last tick announced */

static void ost_tick(const void *arg)
{
	ARG_UNUSED(arg);
	REG(OST_1FLG) = 0;
	uint32_t ticks = (REG(OST_2CNTL) - announced) / CYCLES_PER_TICK;

	if (ticks) {
		announced += ticks * CYCLES_PER_TICK;
		sys_clock_announce((int32_t)ticks);
	}
}

uint32_t sys_clock_cycle_get_32(void)
{
	return REG(OST_2CNTL);
}

uint32_t sys_clock_elapsed(void)
{
	return (REG(OST_2CNTL) - announced) / CYCLES_PER_TICK;
}

static int ost_init(void)
{
	REG(CPM_CLKGR) &= ~CLKGR_OST;

	/* Counter 2 at crystal / 4. The H2's bootloader already runs it
	 * like that; only restart it if it doesn't. */
	if (!(REG(OST_ENABLE) & OST2) ||
	    (REG(OST_CTRL) & CTRL_PRESCALE2) != PRESCALE2_BY_4) {
		REG(OST_ENABLE_CLR) = OST2;
		REG(OST_CTRL) = (REG(OST_CTRL) & ~CTRL_PRESCALE2) | PRESCALE2_BY_4;
		REG(OST_CLEAR) = OST2;
		REG(OST_ENABLE_SET) = OST2;
	}

	/* Timer 1: the tick */
	REG(OST_ENABLE_CLR) = OST1;
	REG(OST_CTRL) = (REG(OST_CTRL) & ~CTRL_PRESCALE1) | PRESCALE1_BY_16;
	REG(OST_1DFR) = TIMER1_HZ / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	REG(OST_1CNT) = 0;
	REG(OST_1FLG) = 0;
	REG(OST_1MSK) = 0;

	announced = REG(OST_2CNTL);
	IRQ_CONNECT(X1000_IRQ_OST, 0, ost_tick, NULL, 0);
	irq_enable(X1000_IRQ_OST);
	REG(OST_ENABLE_SET) = OST1;
	return 0;
}

SYS_INIT(ost_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
