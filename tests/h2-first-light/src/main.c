/*
 * Test 1 on the H2: our code runs.
 *
 * What you should see after choosing "Linux" in the bootloader menu:
 *   1. the backlight goes dark and comes back three times (slowly),
 *   2. the whole screen turns lossless orange.
 * If the screen buffer can't be found, the backlight flickers fast ten
 * times instead of step 2 and the bootloader menu stays on screen.
 *
 * No drivers yet: the bootloader left the screen controller running,
 * endlessly sending its framebuffer to the panel, so we fill that buffer.
 * Register facts: docs/h2-hardware.md
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>

#define REG(a)          (*(volatile uint32_t *)(a))
#define UNCACHED(pa)    (0xa0000000u | (uint32_t)(pa))
#define RAM_BYTES       (32u * 1024 * 1024)

/* GPIO port C */
#define GPIO_C          0xb0010200u
#define GPIO_INT_CLR    (GPIO_C + 0x18)
#define GPIO_MSK_SET    (GPIO_C + 0x24)
#define GPIO_PAT1_CLR   (GPIO_C + 0x38)
#define GPIO_PAT0_SET   (GPIO_C + 0x44)
#define GPIO_PAT0_CLR   (GPIO_C + 0x48)
#define BACKLIGHT_PIN   (1u << 25)            /* PC25, high = lit */

/* LCD controller */
#define LCD_DA          0xb3050040u
#define LCD_MCTRL       0xb30500a4u
#define MCTRL_DMA_START (1u << 1)
#define MCTRL_DMA_MODE  (1u << 2)

#define WIDTH           320
#define HEIGHT          240
#define FRAME_SIZE      ((WIDTH - 1) | ((HEIGHT - 1) << 12))

/* lossless accent #FF5A1F as RGB565 */
#define ORANGE          ((0xff >> 3) << 11 | (0x5a >> 2) << 5 | (0x1f >> 3))

/* No timer yet: a counted loop. At about 1 GHz with caches on this is
 * roughly 0.4 s; much slower blinks would mean the caches are off. */
static void pause(uint32_t loops)
{
	for (volatile uint32_t i = 0; i < loops; i++) {
	}
}

#define SLOW 60000000u
#define FAST 8000000u

static void backlight(bool on)
{
	REG(on ? GPIO_PAT0_SET : GPIO_PAT0_CLR) = BACKLIGHT_PIN;
}

/* Take the backlight pin from the PWM and drive it ourselves: lit. */
static void backlight_take(void)
{
	REG(GPIO_PAT0_SET) = BACKLIGHT_PIN;
	REG(GPIO_INT_CLR) = BACKLIGHT_PIN;
	REG(GPIO_MSK_SET) = BACKLIGHT_PIN;
	REG(GPIO_PAT1_CLR) = BACKLIGHT_PIN;
}

static void blink(int times, uint32_t loops)
{
	for (int i = 0; i < times; i++) {
		backlight(false);
		pause(loops);
		backlight(true);
		pause(loops);
	}
}

static bool in_ram(uint32_t pa, uint32_t bytes)
{
	return pa < RAM_BYTES && bytes <= RAM_BYTES - pa;
}

/* Follow the controller's descriptor chain to the frame it is sending. */
static volatile uint16_t *find_framebuffer(void)
{
	uint32_t pa = REG(LCD_DA);

	for (int i = 0; i < 4; i++) {
		if ((pa & 3) || !in_ram(pa, 32)) {
			return 0;
		}
		volatile uint32_t *desc = (volatile uint32_t *)UNCACHED(pa);
		uint32_t source = desc[1];
		uint32_t frame_size = desc[7];

		if (frame_size == FRAME_SIZE && !(source & 3) &&
		    in_ram(source, WIDTH * HEIGHT * 2)) {
			return (volatile uint16_t *)UNCACHED(source);
		}
		pa = desc[0];
	}
	return 0;
}

int main(void)
{
	backlight_take();
	pause(SLOW);
	blink(3, SLOW);

	volatile uint16_t *fb = find_framebuffer();
	if (!fb) {
		blink(10, FAST);
	} else {
		for (int i = 0; i < WIDTH * HEIGHT; i++) {
			fb[i] = ORANGE;
		}
		REG(LCD_MCTRL) |= MCTRL_DMA_START | MCTRL_DMA_MODE;
	}

	for (;;) {
		/* hold Power to switch off */
	}
	return 0;
}
