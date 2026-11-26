/*
 * Ingenic X1000 MSC (SD/MMC controller) as a Zephyr SD host controller.
 *
 * Zephyr's SD stack does the card protocol (CMD0, ACMD41, ...); this driver
 * sends one command at a time, collects its response and moves the data.
 * For now:
 *  - polled: no interrupts, the CPU waits on the controller's flags;
 *  - data through the controller's FIFO (no DMA), so any buffer works;
 *  - reads and writes (test 4c); erase commands are refused;
 *  - default speed (up to 25 MHz) or high speed (up to 50 MHz), 1 or 4
 *    data lines. Zephyr only moves a card to 4 lines while switching it to
 *    high speed, so high speed is what gets us 4 lines.
 *
 * Register facts: docs/h2-hardware.md, "SD card controller".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT ingenic_x1000_msc

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "x1000_msc.h"

LOG_MODULE_REGISTER(x1000_msc, CONFIG_SDHC_LOG_LEVEL);

#define REG(a)           (*(volatile uint32_t *)(a))

/* Controller registers */
#define MSC_CTRL         0x00
#define MSC_STAT         0x04
#define MSC_CLKRT        0x08
#define MSC_CMDAT        0x0c
#define MSC_BLKLEN       0x18
#define MSC_NOB          0x1c
#define MSC_IMASK        0x24
#define MSC_IFLAG        0x28
#define MSC_CMD          0x2c
#define MSC_ARG          0x30
#define MSC_RES          0x34
#define MSC_RXFIFO       0x38
#define MSC_TXFIFO       0x3c
#define MSC_LPM          0x40
#define MSC_DMAC         0x44
#define MSC_CTRL2        0x58

#define CTRL_START_OP    BIT(2)
#define CTRL_RESET       BIT(3)

#define STAT_FIFO_EMPTY  BIT(6)
#define STAT_FIFO_FULL   BIT(7)
#define STAT_CLOCK_EN    BIT(8)
#define STAT_RESETTING   BIT(15)

#define CMDAT_RESP_NONE  0
#define CMDAT_RESP_SHORT 1
#define CMDAT_RESP_LONG  2
#define CMDAT_RESP_NOCRC 3
#define CMDAT_DATA_EN    BIT(3)
#define CMDAT_WRITE      BIT(4)
#define CMDAT_BUSY       BIT(6)
#define CMDAT_INIT       BIT(7)      /* 80 clocks first: after power-up */
#define CMDAT_4BIT       (2u << 9)

/* IFLAG and IMASK share these bits */
#define F_DATA_DONE      BIT(0)
#define F_PROG_DONE      BIT(1)      /* card no longer busy */
#define F_END_CMD_RES    BIT(2)
#define F_TIMEOUT_READ   BIT(8)
#define F_TIMEOUT_RES    BIT(9)
#define F_CRC_WRITE      BIT(10)     /* the card reported a bad block */
#define F_CRC_READ       BIT(11)
#define F_CRC_RES        BIT(12)
#define F_WR_ALL_DONE    BIT(23)     /* written, and the card done programming */

#define WRITE_TIMEOUT_MS 2000        /* programming can take a while on a busy card */

#define LPM_ENABLE       BIT(0)      /* clock runs only while needed */
#define LPM_TIMING_MASK  (0xfu << 28)
#define LPM_TIMING_HS    (2u << 30 | 1u << 28)   /* drive and sample a little after the rising edge */
#define CTRL2_SPEED_MASK 7u
#define CTRL2_SPEED_HS   1u
#define DEFAULT_SPEED_HZ 25000000u

/* Clock: MSC0CDR divides SCLK_A (or MPLL) / 2, then CLKRT by 2^n */
#define CPM_CCR          0xb0000000u
#define CPM_APCR         0xb0000010u
#define CPM_MPCR         0xb0000014u
#define CPM_CLKGR        0xb0000020u
#define CPM_MSC0CDR      0xb0000068u
#define CLKGR_MSC0       BIT(4)
#define CDR_CLKSRC_MPLL  BIT(31)
#define CDR_CE           BIT(29)
#define CDR_BUSY         BIT(28)
#define CDR_DIV_MASK     0xffu

/* Card detect: PB11, low when a card is in */
#define GPIO_B_PIN       0xb0010100u
#define CARD_DETECT      BIT(11)

#define DEFAULT_TIMEOUT_MS 1000

struct msc_config {
	uintptr_t base;
};

struct msc_data {
	struct k_mutex lock;
	struct x1000_msc_stats stats;
	uint32_t bus;          /* CMDAT bus width bits */
	bool send_init;
	uint32_t clock_hz;
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct msc_config *cfg = dev->config;

	return REG(cfg->base + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t v)
{
	const struct msc_config *cfg = dev->config;

	REG(cfg->base + off) = v;
}

static uint32_t pll_hz(uint32_t reg, uint32_t on_bit)
{
	uint32_t v = REG(reg);

	if (!(v & on_bit)) {
		return 0;
	}
	uint64_t hz = 24000000ull * ((v >> 24 & 0x7f) + 1) / ((v >> 18 & 0x1f) + 1);
	return (uint32_t)(hz >> (v >> 16 & 3));
}

/* What the MSC clock divider is fed from */
static uint32_t source_hz(void)
{
	if (REG(CPM_MSC0CDR) & CDR_CLKSRC_MPLL) {
		return pll_hz(CPM_MPCR, BIT(0));
	}
	switch (REG(CPM_CCR) >> 30) {   /* SCLK_A */
	case 1: return 24000000u;
	case 2: return pll_hz(CPM_APCR, BIT(10));
	default: return 0;
	}
}

static int wait_stat_clear(const struct device *dev, uint32_t bits, int ms)
{
	int64_t end = k_uptime_get() + ms;

	while (rd(dev, MSC_STAT) & bits) {
		if (k_uptime_get() > end) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int set_clock(const struct device *dev, uint32_t hz)
{
	struct msc_data *data = dev->data;
	uint32_t src = source_hz();

	if (!hz || hz == data->clock_hz) {
		return 0;
	}
	if (!src) {
		return -EIO;
	}
	/* Let any running clock stop first */
	(void)wait_stat_clear(dev, STAT_CLOCK_EN, 100);

	/* Divider to about 50 MHz (or the target, if higher), then the
	 * controller halves that until it is at or below the target. */
	uint32_t want = MAX(hz, 50000000u);
	uint32_t div = DIV_ROUND_UP(src / 2, want);

	div = CLAMP(div, 1, 256);
	REG(CPM_MSC0CDR) = (REG(CPM_MSC0CDR) & ~CDR_DIV_MASK) | CDR_CE | (div - 1);
	for (int i = 0; i < 100000 && (REG(CPM_MSC0CDR) & CDR_BUSY); i++) {
	}
	REG(CPM_MSC0CDR) &= ~CDR_CE;

	uint32_t base = src / (2 * div), shift = 0;

	while (shift < 7 && (base >> shift) > hz) {
		shift++;
	}
	wr(dev, MSC_CLKRT, shift);

	/* Default speed: drive on the falling edge, sample on the rising one.
	 * Above 25 MHz (high speed): both shortly after the rising edge. */
	bool hs = (base >> shift) > DEFAULT_SPEED_HZ;

	wr(dev, MSC_LPM, (rd(dev, MSC_LPM) & ~LPM_TIMING_MASK) | LPM_ENABLE |
			 (hs ? LPM_TIMING_HS : 0));
	wr(dev, MSC_CTRL2, (rd(dev, MSC_CTRL2) & ~CTRL2_SPEED_MASK) | (hs ? CTRL2_SPEED_HS : 0));

	data->clock_hz = hz;
	LOG_INF("clock %u Hz (source %u, div %u, shift %u)", base >> shift, src, div, shift);
	return 0;
}

static int msc_reset(const struct device *dev)
{
	struct msc_data *data = dev->data;

	wr(dev, MSC_CTRL, CTRL_RESET);
	k_busy_wait(100);
	wr(dev, MSC_CTRL, 0);
	if (wait_stat_clear(dev, STAT_RESETTING, 100)) {
		return -EIO;
	}
	wr(dev, MSC_IMASK, 0xffffffffu);
	wr(dev, MSC_IFLAG, 0xffffffffu);
	wr(dev, MSC_DMAC, 0);           /* data through the FIFO */
	wr(dev, MSC_LPM, rd(dev, MSC_LPM) | LPM_ENABLE);
	data->clock_hz = 0;
	data->send_init = true;
	return 0;
}

/* Wait until one of `want` (or an error) is flagged; returns the flags */
static int wait_flags(const struct device *dev, uint32_t want, uint32_t errors,
		      int timeout_ms, uint32_t *flags)
{
	int64_t end = k_uptime_get() + (timeout_ms > 0 ? timeout_ms : DEFAULT_TIMEOUT_MS);

	for (;;) {
		uint32_t f = rd(dev, MSC_IFLAG);

		if (f & (want | errors)) {
			*flags = f;
			return (f & errors) ? -EIO : 0;
		}
		if (k_uptime_get() > end) {
			*flags = f;
			return -ETIMEDOUT;
		}
	}
}

/* The response comes out of RES 16 bits at a time, most significant first */
static void read_response(const struct device *dev, struct sdhc_command *cmd, int fmt)
{
	uint32_t r = rd(dev, MSC_RES) & 0xffff;

	if (fmt == CMDAT_RESP_LONG) {
		/* 128 bits, CID or CSD: Zephyr wants the top word in [3] */
		for (int i = 3; i >= 0; i--) {
			uint32_t w = r << 24;

			r = rd(dev, MSC_RES) & 0xffff;
			w |= r << 8;
			r = rd(dev, MSC_RES) & 0xffff;
			w |= r >> 8;
			cmd->response[i] = w;
		}
	} else {
		uint32_t w = r << 24;

		r = rd(dev, MSC_RES) & 0xffff;
		w |= r << 8;
		r = rd(dev, MSC_RES) & 0xffff;
		w |= r & 0xff;
		cmd->response[0] = w;
	}
}

/* Data to the card: into the FIFO a word at a time while it has room */
static int write_fifo(const struct device *dev, struct sdhc_data *sd)
{
	const uint8_t *src = sd->data;
	size_t left = (size_t)sd->blocks * sd->block_size;
	int64_t end = k_uptime_get() + WRITE_TIMEOUT_MS;

	while (left) {
		if (rd(dev, MSC_STAT) & STAT_FIFO_FULL) {
			if (rd(dev, MSC_IFLAG) & F_CRC_WRITE) {
				return -EIO;
			}
			if (k_uptime_get() > end) {
				return -ETIMEDOUT;
			}
			continue;
		}
		uint32_t w = 0;

		memcpy(&w, src, left >= 4 ? 4 : left);   /* lowest byte goes first */
		wr(dev, MSC_TXFIFO, w);
		src += left >= 4 ? 4 : left;
		left = left >= 4 ? left - 4 : 0;
	}
	sd->bytes_xfered = sd->blocks * sd->block_size;
	return 0;
}

static int read_fifo(const struct device *dev, struct sdhc_data *sd)
{
	uint8_t *dst = sd->data;
	size_t left = (size_t)sd->blocks * sd->block_size;
	int64_t end = k_uptime_get() + (sd->timeout_ms > 0 ? sd->timeout_ms : DEFAULT_TIMEOUT_MS);

	while (left) {
		/* Errors only matter while waiting: an error stops the data, so
		 * the FIFO runs empty and we look then. */
		if (rd(dev, MSC_STAT) & STAT_FIFO_EMPTY) {
			uint32_t f = rd(dev, MSC_IFLAG);

			if (f & (F_TIMEOUT_READ | F_CRC_READ)) {
				return (f & F_TIMEOUT_READ) ? -ETIMEDOUT : -EIO;
			}
			if (k_uptime_get() > end) {
				return -ETIMEDOUT;
			}
			continue;
		}
		uint32_t w = rd(dev, MSC_RXFIFO);

		if (left >= 4) {
			memcpy(dst, &w, 4);   /* first byte off the card is the lowest */
			dst += 4;
			left -= 4;
		} else {
			memcpy(dst, &w, left);
			left = 0;
		}
	}
	sd->bytes_xfered = sd->blocks * sd->block_size;
	return 0;
}

/* One command on the bus, with its data if any. Called with the lock held. */
static int run(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *sd)
{
	struct msc_data *data = dev->data;
	uint32_t type = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;
	uint32_t cmdat = data->bus, want, flags = 0;
	int fmt, err;

	switch (type) {
	case SD_RSP_TYPE_NONE: fmt = CMDAT_RESP_NONE; break;
	case SD_RSP_TYPE_R2:   fmt = CMDAT_RESP_LONG; break;
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:   fmt = CMDAT_RESP_NOCRC; break;
	case SD_RSP_TYPE_R1b:
	case SD_RSP_TYPE_R5b:  fmt = CMDAT_RESP_SHORT; cmdat |= CMDAT_BUSY; break;
	default:               fmt = CMDAT_RESP_SHORT; break;
	}
	cmdat |= fmt;

	if (data->send_init) {
		cmdat |= CMDAT_INIT;
		data->send_init = false;
	}
	bool writing = cmd->opcode == SD_WRITE_SINGLE_BLOCK ||
		       cmd->opcode == SD_WRITE_MULTIPLE_BLOCK;

	if (sd) {
		cmdat |= CMDAT_DATA_EN | (writing ? CMDAT_WRITE : 0);
		wr(dev, MSC_NOB, sd->blocks);
		wr(dev, MSC_BLKLEN, sd->block_size);
	} else {
		wr(dev, MSC_NOB, 0);
		wr(dev, MSC_BLKLEN, 0);
	}

	want = F_DATA_DONE | F_PROG_DONE | F_END_CMD_RES | F_TIMEOUT_READ |
	       F_TIMEOUT_RES | F_CRC_READ | F_CRC_RES | F_CRC_WRITE | F_WR_ALL_DONE;
	wr(dev, MSC_IFLAG, 0xffffffffu);
	wr(dev, MSC_IMASK, ~want);   /* flags latch (the line itself stays off) */
	wr(dev, MSC_CMD, cmd->opcode);
	wr(dev, MSC_ARG, cmd->arg);
	wr(dev, MSC_CMDAT, cmdat);
	wr(dev, MSC_CTRL, CTRL_START_OP);

	err = wait_flags(dev, F_END_CMD_RES, F_TIMEOUT_RES | F_CRC_RES, cmd->timeout_ms, &flags);
	if (err) {
		err = (flags & F_CRC_RES) ? -EIO : -ETIMEDOUT;
		goto out;
	}
	if (fmt != CMDAT_RESP_NONE) {
		read_response(dev, cmd, fmt);
	}

	if (sd && writing) {
		err = write_fifo(dev, sd);
		if (!err) {
			/* sent ("all written" may only come with the controller's
			 * DMA, which we don't use); the SD stack then asks the card
			 * itself (CMD13) until it's done programming */
			err = wait_flags(dev, F_DATA_DONE | F_WR_ALL_DONE, F_CRC_WRITE,
					 WRITE_TIMEOUT_MS, &flags);
		}
	} else if (sd) {
		err = read_fifo(dev, sd);
		if (!err) {
			err = wait_flags(dev, F_DATA_DONE, F_TIMEOUT_READ | F_CRC_READ,
					 sd->timeout_ms, &flags);
		}
	} else if (cmdat & CMDAT_BUSY) {
		err = wait_flags(dev, F_PROG_DONE, 0, cmd->timeout_ms, &flags);
	}

out:
	if (err) {
		data->stats.errors++;
		data->stats.last_opcode = cmd->opcode;
		data->stats.last_err = err;
		data->stats.last_flags = flags;
		data->stats.last_stat = rd(dev, MSC_STAT);
	}
	wr(dev, MSC_IMASK, 0xffffffffu);
	wr(dev, MSC_IFLAG, 0xffffffffu);
	return err;
}

/* After an error: a clean controller, same clock and bus */
static void recover(const struct device *dev)
{
	struct msc_data *data = dev->data;
	uint32_t hz = data->clock_hz, bus = data->bus;

	msc_reset(dev);
	data->send_init = false;
	data->bus = bus;
	set_clock(dev, hz);
}

/* End a multi-block transfer: CMD12. Zephyr's SD stack leaves this to the
 * host driver; without it the card goes on sending and answers nothing
 * else properly. */
static int stop_transmission(const struct device *dev)
{
	struct sdhc_command stop = {
		.opcode = SD_STOP_TRANSMISSION,
		.arg = 0,
		.response_type = SD_RSP_TYPE_R1b,
		.timeout_ms = DEFAULT_TIMEOUT_MS,
	};
	int err = run(dev, &stop, NULL);

	if (err) {
		recover(dev);
	}
	return err;
}

static int msc_request(const struct device *dev, struct sdhc_command *cmd,
		       struct sdhc_data *sd)
{
	struct msc_data *data = dev->data;
	int err;

	/* Writes go through (the file system only writes in /.lossless, see
	 * platform/zephyr/sys.c); erasing stays off: nothing needs it */
	switch (cmd->opcode) {
	case SD_ERASE_BLOCK_START:
	case SD_ERASE_BLOCK_END:
	case SD_ERASE_BLOCK_OPERATION:
		return -EROFS;
	default:
		break;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->stats.commands++;
	if (sd) {
		data->stats.blocks += sd->blocks;
	}
	err = run(dev, cmd, sd);
	if (err) {
		recover(dev);
	}
	if (cmd->opcode == SD_READ_MULTIPLE_BLOCK || cmd->opcode == SD_WRITE_MULTIPLE_BLOCK) {
		int stop = stop_transmission(dev);

		if (!err) {
			err = stop;
		}
	}
	k_mutex_unlock(&data->lock);
	return err;
}

void x1000_msc_get_stats(const struct device *dev, struct x1000_msc_stats *out)
{
	struct msc_data *data = dev->data;

	*out = data->stats;
}

static int msc_set_io(const struct device *dev, struct sdhc_io *ios)
{
	struct msc_data *data = dev->data;

	switch (ios->bus_width) {
	case SDHC_BUS_WIDTH1BIT: data->bus = 0; break;
	case SDHC_BUS_WIDTH4BIT: data->bus = CMDAT_4BIT; break;
	default: return -ENOTSUP;
	}
	if (ios->timing != SDHC_TIMING_LEGACY && ios->timing != SDHC_TIMING_HS) {
		return -ENOTSUP;
	}
	if (ios->signal_voltage != SD_VOL_3_3_V) {
		return -ENOTSUP;
	}
	/* The card is powered by the board and stays on; power-on just means
	 * the next command starts with the 80 initialisation clocks. */
	if (ios->power_mode == SDHC_POWER_ON && data->clock_hz == 0) {
		data->send_init = true;
	}
	return set_clock(dev, ios->clock);
}

static int msc_card_present(const struct device *dev)
{
	ARG_UNUSED(dev);
	return !(REG(GPIO_B_PIN) & CARD_DETECT);
}

static int msc_card_busy(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int msc_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	ARG_UNUSED(dev);
	memset(props, 0, sizeof(*props));
	props->f_min = SDMMC_CLOCK_400KHZ;
	props->f_max = SD_CLOCK_50MHZ;
	props->host_caps.high_spd_support = 1;
	props->power_delay = 10;
	props->host_caps.vol_330_support = 1;
	props->max_current_330 = 200;
	props->bus_4_bit_support = true;
	return 0;
}

static int msc_init(const struct device *dev)
{
	struct msc_data *data = dev->data;

	k_mutex_init(&data->lock);
	REG(CPM_CLKGR) &= ~CLKGR_MSC0;
	return msc_reset(dev);
}

static DEVICE_API(sdhc, msc_api) = {
	.reset = msc_reset,
	.request = msc_request,
	.set_io = msc_set_io,
	.get_card_present = msc_card_present,
	.card_busy = msc_card_busy,
	.get_host_props = msc_host_props,
};

#define MSC_INIT(n)                                                                  \
	static const struct msc_config msc_config_##n = {                            \
		.base = 0xa0000000u | DT_INST_REG_ADDR(n),                           \
	};                                                                           \
	static struct msc_data msc_data_##n;                                         \
	DEVICE_DT_INST_DEFINE(n, msc_init, NULL, &msc_data_##n, &msc_config_##n,     \
			      POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY, &msc_api);

DT_INST_FOREACH_STATUS_OKAY(MSC_INIT)
