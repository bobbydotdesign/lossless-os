/*
 * The screen the bootloader leaves running, as a Zephyr display.
 *
 * On the X1000 the LCD controller sends a frame from memory to the panel's
 * own memory through two DMA descriptors (a "write memory" command, then
 * the frame). Each start sends the frame once. We follow the descriptors
 * from LCD_DA to the frame, draw into it, and after the last piece of each
 * redraw wait for any transfer still running and start a new one, so every
 * change reaches the panel. Our own LCD set-up comes with our own
 * bootloader. Facts: docs/h2-hardware.md, "Screen".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT lossless_handover_framebuffer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <string.h>

#define REG(a)          (*(volatile uint32_t *)(a))
#define UNCACHED(pa)    (0xa0000000u | (uint32_t)(pa))
#define RAM_BYTES       (32u * 1024 * 1024)

#define LCD_DA          0x40
#define LCD_MCTRL       0xa4
#define LCD_MSTATE      0xa8
#define MCTRL_DMA_START BIT(1)
#define MCTRL_DMA_MODE  BIT(2)
#define MSTATE_BUSY     BIT(0)

struct hfb_config {
	uintptr_t lcd;
	uint16_t width, height;
};

struct hfb_data {
	volatile uint16_t *frame;   /* uncached */
};

static bool in_ram(uint32_t pa, uint32_t bytes)
{
	return pa < RAM_BYTES && bytes <= RAM_BYTES - pa;
}

static int hfb_write(const struct device *dev, uint16_t x, uint16_t y,
		     const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct hfb_config *cfg = dev->config;
	struct hfb_data *data = dev->data;
	const uint16_t *src = buf;

	if (!data->frame) {
		return -ENODEV;
	}
	if (x + desc->width > cfg->width || y + desc->height > cfg->height) {
		return -EINVAL;
	}
	for (uint16_t row = 0; row < desc->height; row++) {
		volatile uint16_t *dst = data->frame + (y + row) * cfg->width + x;
		const uint16_t *s = src + row * desc->pitch;

		for (uint16_t col = 0; col < desc->width; col++) {
			dst[col] = s[col];
		}
	}
	if (desc->frame_incomplete) {
		return 0;    /* more pieces of this redraw to come */
	}
	/* A transfer that started before our writes may have sent the old
	 * pixels already: let it finish, then send the frame again. */
	int64_t end = k_uptime_get() + 100;

	while ((REG(cfg->lcd + LCD_MSTATE) & MSTATE_BUSY) && k_uptime_get() < end) {
	}
	REG(cfg->lcd + LCD_MCTRL) |= MCTRL_DMA_START | MCTRL_DMA_MODE;
	return 0;
}

static int hfb_blanking(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static void hfb_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	const struct hfb_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = cfg->width;
	caps->y_resolution = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int hfb_set_pixel_format(const struct device *dev, enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return pf == PIXEL_FORMAT_RGB_565 ? 0 : -ENOTSUP;
}

static int hfb_init(const struct device *dev)
{
	const struct hfb_config *cfg = dev->config;
	struct hfb_data *data = dev->data;
	uint32_t frame_size = (cfg->width - 1) | ((cfg->height - 1) << 12);
	uint32_t pa = REG(cfg->lcd + LCD_DA);

	for (int i = 0; i < 4; i++) {
		if ((pa & 3) || !in_ram(pa, 32)) {
			break;
		}
		volatile uint32_t *desc = (volatile uint32_t *)UNCACHED(pa);
		uint32_t source = desc[1];

		if (desc[7] == frame_size && !(source & 3) &&
		    in_ram(source, cfg->width * cfg->height * 2)) {
			data->frame = (volatile uint16_t *)UNCACHED(source);
			return 0;
		}
		pa = desc[0];
	}
	return -ENODEV;
}

static DEVICE_API(display, hfb_api) = {
	.blanking_on = hfb_blanking,
	.blanking_off = hfb_blanking,
	.write = hfb_write,
	.get_capabilities = hfb_capabilities,
	.set_pixel_format = hfb_set_pixel_format,
};

#define HFB_INIT(n)                                                                  \
	static const struct hfb_config hfb_config_##n = {                            \
		.lcd = 0xa0000000u | DT_INST_REG_ADDR(n),                            \
		.width = DT_INST_PROP(n, width),                                     \
		.height = DT_INST_PROP(n, height),                                   \
	};                                                                           \
	static struct hfb_data hfb_data_##n;                                         \
	DEVICE_DT_INST_DEFINE(n, hfb_init, NULL, &hfb_data_##n, &hfb_config_##n,     \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &hfb_api);

DT_INST_FOREACH_STATUS_OKAY(HFB_INIT)
