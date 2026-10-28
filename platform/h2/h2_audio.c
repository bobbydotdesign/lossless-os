/*
 * Sound on the H2: the X1000's audio controller (AIC) as I2S master into
 * the ES9018K2M DAC, fed by DMA. Bit-perfect: samples go out exactly as
 * decoded, 24 bits in each 32-bit I2S slot, at the file's own rate (the
 * I2S clock is set per file). Volume is the DAC's, never the samples'.
 *
 * The DMA loops endlessly over a ring of descriptors; the audio thread
 * writes ahead of where it reads and zeroes what it has played, so if the
 * thread ever falls behind the DAC gets silence, not old sound. No
 * interrupts needed.
 *
 * Facts and the power-up order (which avoids pops) are in
 * docs/h2-hardware.md, "Sound".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <string.h>
#include "los_sys.h"
#include "x1000_i2c.h"
#include "h2_audio.h"

#define REG(a)        (*(volatile uint32_t *)(a))
#define PHYS(p)       ((uint32_t)(uintptr_t)(p) & 0x1fffffffu)
#define UNCACHED(p)   (0xa0000000u | PHYS(p))

/* Clocks */
#define CPM_CCR       0xb0000000u
#define CPM_APCR      0xb0000010u
#define CPM_CLKGR     0xb0000020u
#define CPM_I2SCDR    0xb0000060u
#define CPM_I2SCDR1   0xb0000070u
#define CLKGR_AIC     BIT(11)
#define CLKGR_PDMA    BIT(21)
#define I2SCDR_CS     BIT(30)       /* from the divider below, not the crystal */
#define I2SCDR_CE     BIT(29)
#define I2SCDR_M(m)   ((uint32_t)(m) << 13)   /* 9 bits */
#define I2SCDR_N(n)   ((uint32_t)(n))         /* 13 bits */

/* Audio controller */
#define AIC_CFG       0xb0020000u
#define AIC_CCR       0xb0020004u
#define AIC_I2SCR     0xb0020010u
#define AIC_SR        0xb0020014u
#define AIC_I2SDIV    0xb0020030u
#define AIC_DR        0xb0020034u
#define AIC_SPENA     0xb0020080u
#define CFG_ENABLE    BIT(0)
#define CFG_SYNCD     BIT(1)        /* frame clock out: master */
#define CFG_BCKD      BIT(2)        /* bit clock out: master */
#define CFG_RST       BIT(3)
#define CFG_AUSEL     BIT(4)        /* I2S */
#define CFG_LSMP      BIT(6)        /* on underflow repeat the last sample */
#define CFG_TFTH(n)   ((uint32_t)(n) << 16)
#define CFG_RFTH(n)   ((uint32_t)(n) << 24)
#define CCR_ERPL      BIT(1)        /* playback on */
#define CCR_TDMS      BIT(14)       /* playback by DMA */
#define CCR_OSS_24    (4u << 19)
#define CCR_ISS_24    (4u << 16)
#define CCR_STEREO    (1u << 24)
#define I2SCR_ESCLK   BIT(4)        /* master clock out */
#define I2SCR_STPBK   BIT(12)       /* bit clock stopped */
#define SR_TFL(sr)    (((sr) >> 8) & 0x3f)

/* DMA, channel 0 */
#define DMA_CHN(c)    (0xb3420000u + (c) * 0x20)
#define DMA_SA        0x00
#define DMA_CS        0x10
#define DMA_DA        0x18
#define DMA_CTRL      0xb3421000u
#define DMA_DB_SET    0xb342100cu
#define CS_CTE        BIT(0)
#define CS_DES8       BIT(30)
#define CM_LINK       BIT(0)
#define CM_TSZ_AUTO   (7u << 8)
#define CM_RDIL(n)    ((uint32_t)(n) << 16)
#define CM_SAI        BIT(23)
#define RT_I2S_TX     6u
#define CTRL_ENABLE   BIT(0)
#define CTRL_FAST     (BIT(27) | BIT(28) | BIT(29) | BIT(30) | BIT(31))
#define CH            0

/* GPIO */
#define GPIO(port, off) REG(0xb0010000u + (port) * 0x100u + (off))
enum { PA, PB, PC, PD };
#define HPAMP_POWER   6             /* port B pins */
#define HPAMP_SHDN    8             /* 0: amp off */
#define DAC_ANALOG    9
#define DAC_POWER     12
#define SW_MUTE       15            /* 1: output switch muted */
#define SW_SELECT     5             /* 0: headphones, 1: line out */
#define SPDIF_POWER   21

/* DAC: ES9018K2M on I2C bus 1 */
#define DAC_BUS       1
#define DAC_ADDR      0x48
#define DAC_REG10     10            /* master mode / lock: 0 = wait 16384 frames */
#define DAC_REG12     12            /* DPLL bandwidth */
#define DAC_VOL_L     15            /* attenuation in 0.5 dB, 255 = mute */
#define DAC_VOL_R     16

/* The ring: 128 descriptors (all in one 4 KB page, as the DMA links them
 * by offset), 4 KB each = 512 KB: 1.5 s at 44.1 kHz, 0.34 s at 192 kHz */
#define DESCS         128
#define DESC_BYTES    4096u
#define RING_BYTES    (DESCS * DESC_BYTES)
#define FRAME_BYTES   8u            /* two 32-bit slots */

struct dma_desc {
	uint32_t cm, sa, ta, tc, sd, rt, pad0, pad1;
};

static uint32_t ring_mem[RING_BYTES / 4] __aligned(4096);
static struct dma_desc desc_mem[DESCS] __aligned(4096);
static volatile uint32_t *ring;           /* uncached views */
static volatile struct dma_desc *desc;

static K_MUTEX_DEFINE(lock);
static bool powered, running, paused;
static unsigned bits, channels, rate;
static uint64_t wpos, rpos;               /* bytes written, played (absolute) */
static uint32_t last_off;                 /* where the DMA was, in the ring */
uint32_t h2_audio_underruns;
static int volume_att = 80;               /* -40 dB to start: safe */
static int trim_half;                     /* make-up gain on top, 0.5 dB steps (the EQ's) */

/* What the DAC is set to: the volume, raised by the make-up gain, never
 * past the loudest setting */
static int dac_att(void)
{
	int att = volume_att - trim_half;

	return att < H2_AUDIO_ATT_MAX ? H2_AUDIO_ATT_MAX : att;
}
static uint32_t dbg_m, dbg_n, dbg_sclk;

/* ---- small helpers ------------------------------------------------------------------ */

static void gpio_out(int port, int pin, int level)
{
	uint32_t bit = BIT(pin);

	GPIO(port, level ? 0x44 : 0x48) = bit;     /* PAT0 set / clear: the level */
	GPIO(port, 0x18) = bit;                    /* INT clear */
	GPIO(port, 0x24) = bit;                    /* MSK set */
	GPIO(port, 0x38) = bit;                    /* PAT1 clear */
}

static void gpio_function(int port, int pin, int fn)
{
	uint32_t bit = BIT(pin);

	GPIO(port, 0x18) = bit;
	GPIO(port, 0x28) = bit;
	GPIO(port, (fn & 2) ? 0x34 : 0x38) = bit;
	GPIO(port, (fn & 1) ? 0x44 : 0x48) = bit;
}

static void flush_dcache(const void *p, size_t n)
{
	for (uintptr_t a = (uintptr_t)p & ~31u; a < (uintptr_t)p + n; a += 32) {
		__asm__ volatile("cache 0x15, 0(%0)" : : "r"(a) : "memory");
	}
	__asm__ volatile("sync" : : : "memory");
}

/* While Bluetooth has PC26 for its sleep clock, the DAC's I2C data line
 * is gone: no DAC writes then (h2_audio_lend_bus) */
static bool bus_lent;

static int dac_write(uint8_t reg, uint8_t val)
{
	if (bus_lent) {
		return -1;
	}
	return x1000_i2c_write(DAC_BUS, DAC_ADDR, reg, val);
}

static void dac_volume(int att)
{
	dac_write(DAC_VOL_L, (uint8_t)att);
	dac_write(DAC_VOL_R, (uint8_t)att);
}


/* The DAC's bus (its pins), settings and our volume */
static void dac_setup(void)
{
	x1000_i2c_init(DAC_BUS);
	dac_write(DAC_REG10, 0x00);
	dac_write(DAC_REG12, 0xda);
	dac_volume(dac_att());
}

/* SCLK_A, which feeds the I2S divider (APLL on this player) */
static uint32_t sclk_a_hz(void)
{
	uint32_t ccr = REG(CPM_CCR), v = REG(CPM_APCR);

	if ((ccr >> 30) == 1) {
		return 24000000u;
	}
	if ((ccr >> 30) != 2 || !(v & BIT(10))) {
		return 0;
	}
	uint64_t hz = 24000000ull * ((v >> 24 & 0x7f) + 1) / ((v >> 18 & 0x1f) + 1);
	return (uint32_t)(hz >> (v >> 16 & 3));
}

/* Master clock (PB0, I2S_MCLK: Ingenic X1000 datasheet) = rate x 256 from
 * SCLK_A x m / n (m < 512, n < 8192, n >= 2m), and the bit clock is
 * master / 4 = 64 x rate (two 32-bit slots). Above 192 kHz, rate x 128
 * and master / 2: ESS's ES9018K2M datasheet allows a synchronous master
 * clock of 128 x the rate up to 384 kHz (otherwise more than 192 x, and
 * over 50 MHz only with an external core supply), and the AIC's divider
 * has 128 x (X1000 programming manual, table 6-5). 384 kHz then runs on
 * the 49.152 MHz master clock 192 kHz already uses. */
static bool set_rate(unsigned r)
{
	unsigned per = r > 192000 ? 128 : 256;
	uint64_t src = sclk_a_hz(), target = (uint64_t)r * per;
	uint32_t best_m = 0, best_n = 0;
	uint64_t best_err = UINT64_MAX;

	if (!src || r == 0 || r > 384000) {
		return false;
	}
	for (uint32_t n = 2; n < 8192 && best_err; n++) {
		uint64_t m = (target * n + src / 2) / src;
		if (m == 0 || m >= 512 || n < 2 * m) {
			continue;
		}
		uint64_t got = src * m / n;
		uint64_t err = got > target ? got - target : target - got;
		if (err < best_err) {
			best_err = err;
			best_m = (uint32_t)m;
			best_n = n;
		}
	}
	if (!best_m) {
		return false;
	}
	dbg_m = best_m;
	dbg_n = best_n;
	dbg_sclk = (uint32_t)src;
	REG(AIC_I2SCR) |= I2SCR_STPBK;
	REG(CPM_I2SCDR) = I2SCDR_CS | I2SCDR_CE | I2SCDR_M(best_m) | I2SCDR_N(best_n);
	REG(CPM_I2SCDR1) = REG(CPM_I2SCDR1);
	REG(AIC_I2SDIV) = per / 64 - 1;
	REG(AIC_I2SCR) &= ~I2SCR_STPBK;
	rate = r;
	return true;
}

/* ---- power-up, in the order this player needs (docs/h2-hardware.md) ------------------ */

static void power_up(void)
{
	/* everything muted and off */
	gpio_out(PB, HPAMP_SHDN, 0);
	gpio_out(PB, SW_MUTE, 1);
	gpio_out(PB, DAC_POWER, 0);
	gpio_out(PB, SPDIF_POWER, 0);
	gpio_out(PB, SW_SELECT, 0);
	for (int pin = 0; pin <= 4; pin++) {
		gpio_function(PB, pin, 1);         /* PB0-PB4: I2S */
	}

	/* the audio controller: I2S master to an external codec, 24-bit stereo */
	REG(CPM_CLKGR) &= ~(CLKGR_AIC | CLKGR_PDMA);
	REG(AIC_CFG) |= CFG_RST;
	REG(AIC_I2SCR) = I2SCR_STPBK;
	REG(AIC_CFG) = CFG_LSMP | CFG_AUSEL | CFG_BCKD | CFG_SYNCD | CFG_TFTH(16) | CFG_RFTH(15);
	REG(AIC_CCR) = CCR_STEREO | CCR_OSS_24 | CCR_ISS_24;
	REG(AIC_SPENA) = 0;
	set_rate(48000);
	REG(AIC_I2SCR) |= I2SCR_ESCLK;
	REG(AIC_I2SCR) &= ~I2SCR_STPBK;
	REG(AIC_CFG) |= CFG_ENABLE;
	k_msleep(10);

	gpio_out(PB, DAC_ANALOG, 1);
	gpio_out(PB, HPAMP_POWER, 1);

	/* a few samples through, so the DAC doesn't auto-mute (no first click) */
	REG(AIC_CCR) |= CCR_ERPL;
	for (int i = 0; i < 32; i++) {
		REG(AIC_DR) = 0xffffff;
	}
	int64_t end = k_uptime_get() + 50;
	while (SR_TFL(REG(AIC_SR)) && k_uptime_get() < end) {
	}
	k_msleep(20);
	REG(AIC_CCR) &= ~CCR_ERPL;

	/* unmute, in this order */
	gpio_out(PB, SW_SELECT, 0);
	gpio_out(PB, HPAMP_SHDN, 1);
	k_msleep(10);
	gpio_out(PB, DAC_POWER, 1);
	k_msleep(10);
	gpio_out(PB, SW_MUTE, 0);

	/* the DAC: lock settings, then our volume (later, if Bluetooth has
	 * its data line just now: h2_audio_lend_bus) */
	if (!bus_lent) {
		dac_setup();
	}

	/* the DMA controller, and the ring seen uncached */
	REG(DMA_CTRL) = CTRL_ENABLE | CTRL_FAST;
	flush_dcache(ring_mem, sizeof(ring_mem));
	flush_dcache(desc_mem, sizeof(desc_mem));
	ring = (volatile uint32_t *)UNCACHED(ring_mem);
	desc = (volatile struct dma_desc *)UNCACHED(desc_mem);
	uint32_t ring_phys = PHYS(ring_mem), desc_phys = PHYS(desc_mem);
	for (int i = 0; i < DESCS; i++) {
		uint32_t next = desc_phys + (uint32_t)((i + 1) % DESCS) * sizeof(struct dma_desc);
		desc[i].cm = CM_SAI | CM_RDIL(9) | CM_TSZ_AUTO | CM_LINK;
		desc[i].sa = ring_phys + (uint32_t)i * DESC_BYTES;
		desc[i].ta = PHYS(AIC_DR);
		desc[i].tc = ((next >> 4) & 0xff) << 24 | DESC_BYTES;
		desc[i].sd = 0;
		desc[i].rt = RT_I2S_TX;
		desc[i].pad0 = desc[i].pad1 = 0;
	}
	powered = true;
}

/* ---- the DMA ring ------------------------------------------------------------------- */

static void dma_start_at(unsigned d)
{
	REG(DMA_CHN(CH) + DMA_CS) = 0;
	REG(DMA_CHN(CH) + DMA_DA) = PHYS(desc_mem) + d * sizeof(struct dma_desc);
	REG(DMA_CHN(CH) + DMA_CS) = CS_DES8;
	REG(DMA_DB_SET) = BIT(CH);
	REG(DMA_CHN(CH) + DMA_CS) = CS_DES8 | CS_CTE;
	REG(AIC_CCR) |= CCR_TDMS | CCR_ERPL;
	running = true;
}

static void dma_stop(void)
{
	REG(AIC_CCR) &= ~CCR_TDMS;
	REG(DMA_CHN(CH) + DMA_CS) = 0;
	int64_t end = k_uptime_get() + 20;
	while (SR_TFL(REG(AIC_SR)) && k_uptime_get() < end) {
	}
	REG(AIC_CCR) &= ~CCR_ERPL;
	running = false;
}

/* Follow the DMA: count what it played and zero it behind it */
static void follow(void)
{
	if (!running) {
		return;
	}
	uint32_t off = REG(DMA_CHN(CH) + DMA_SA) - PHYS(ring_mem);

	if (off >= RING_BYTES) {
		return;
	}
	off &= ~(FRAME_BYTES - 1);
	uint32_t delta = (off - last_off + RING_BYTES) % RING_BYTES;

	for (uint32_t i = 0; i < delta; i += 4) {
		ring[((last_off + i) % RING_BYTES) / 4] = 0;
	}
	rpos += delta;
	last_off = off;
}

/* ---- los_sys.h ------------------------------------------------------------------------ */

bool los_audio_start(unsigned r, unsigned b, unsigned ch)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (!powered) {
		power_up();
	}
	if (running) {
		dma_stop();
	}
	for (uint32_t i = 0; i < RING_BYTES / 4; i++) {
		ring[i] = 0;
	}
	wpos = rpos = 0;
	last_off = 0;
	paused = false;
	bits = b;
	channels = ch;
	bool ok = b >= 8 && b <= 32 && ch >= 1 && ch <= 2 && (r == rate || set_rate(r));
	dac_volume(dac_att());
	k_mutex_unlock(&lock);
	return ok;
}

size_t los_audio_write(const int32_t *frames, size_t count)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (paused || !powered) {
		k_mutex_unlock(&lock);
		return 0;
	}
	follow();
	if (rpos > wpos) {
		/* ran dry (music that comes as it plays, from a computer, can):
		 * carry on a little ahead of where the DMA is; what it plays
		 * meanwhile is silence, zeroed behind it on the last lap */
		wpos = rpos + 64 * FRAME_BYTES;
		h2_audio_underruns++;
	}
	uint64_t used = wpos - rpos;
	uint64_t room = used + DESC_BYTES < RING_BYTES ? (RING_BYTES - DESC_BYTES - used) / FRAME_BYTES : 0;
	size_t n = count < room ? count : (size_t)room;
	int up = bits <= 24 ? 24 - (int)bits : 0, down = bits > 24 ? (int)bits - 24 : 0;

	for (size_t i = 0; i < n; i++) {
		int32_t l = frames[i * channels];
		int32_t r = channels == 2 ? frames[i * channels + 1] : l;
		uint32_t w = (uint32_t)((wpos % RING_BYTES) / 4);

		/* 24 bits right-aligned, sign-extended: 16-bit samples padded
		 * exactly; only 32-bit files lose their lowest 8 bits */
		ring[w] = (uint32_t)(down ? l >> down : (int32_t)((uint32_t)l << up));
		ring[w + 1] = (uint32_t)(down ? r >> down : (int32_t)((uint32_t)r << up));
		wpos += FRAME_BYTES;
	}
	if (n && !running) {
		dma_start_at(0);
	}
	k_mutex_unlock(&lock);
	return n;
}

uint64_t los_audio_played(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	follow();
	uint64_t p = (rpos < wpos ? rpos : wpos) / FRAME_BYTES;
	k_mutex_unlock(&lock);
	return p;
}

void los_audio_pause(bool p)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (p != paused && powered) {
		if (p) {
			dac_volume(255);
			follow();
			if (running) {
				dma_stop();
			}
		} else if (wpos > rpos) {
			/* back from the start of the descriptor it was in (what it
			 * already played there was zeroed: a moment of silence) */
			dma_start_at(last_off / DESC_BYTES);
			last_off -= last_off % DESC_BYTES;
			dac_volume(dac_att());
		} else {
			dac_volume(dac_att());
		}
		paused = p;
	}
	k_mutex_unlock(&lock);
}

void los_audio_stop(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (running) {
		dma_stop();
	}
	wpos = rpos = 0;
	last_off = 0;
	paused = false;
	k_mutex_unlock(&lock);
}

/* ---- volume (Vol+ / Vol-) --------------------------------------------------------------- */

void h2_audio_volume_step(int db)
{
	k_mutex_lock(&lock, K_FOREVER);
	volume_att -= db * 2;
	if (volume_att < H2_AUDIO_ATT_MAX) {
		volume_att = H2_AUDIO_ATT_MAX;
	}
	if (volume_att > H2_AUDIO_ATT_MIN) {
		volume_att = H2_AUDIO_ATT_MIN;
	}
	if (powered && !paused) {
		dac_volume(dac_att());
	}
	k_mutex_unlock(&lock);
}

int h2_audio_volume_db(void)
{
	return -volume_att / 2;
}

void los_volume_set(int db)
{
	k_mutex_lock(&lock, K_FOREVER);
	int att = -db * 2;
	volume_att = att < H2_AUDIO_ATT_MAX ? H2_AUDIO_ATT_MAX :
		     att > H2_AUDIO_ATT_MIN ? H2_AUDIO_ATT_MIN : att;
	if (powered && !paused) {
		dac_volume(dac_att());
	}
	k_mutex_unlock(&lock);
}

void h2_audio_lend_bus(bool lent)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (lent != bus_lent) {
		bus_lent = lent;
		if (!lent && powered) {
			dac_setup();                /* its pins back, then settings */
			if (paused) {
				dac_volume(255);
			}
		}
	}
	k_mutex_unlock(&lock);
}

void los_volume_trim(int half_db)
{
	k_mutex_lock(&lock, K_FOREVER);
	trim_half = half_db > 0 ? half_db : 0;
	if (powered && !paused) {
		dac_volume(dac_att());
	}
	k_mutex_unlock(&lock);
}

bool los_volume(int *db, int *min_db, int *max_db)
{
	*db = -volume_att / 2;
	*min_db = -H2_AUDIO_ATT_MIN / 2;
	*max_db = -H2_AUDIO_ATT_MAX / 2;
	return true;
}

/* ---- for the check screen ------------------------------------------------------------- */

static int dac_read(uint8_t reg)
{
	uint8_t v;

	if (bus_lent) {
		return -1;
	}
	return x1000_i2c_read(DAC_BUS, DAC_ADDR, reg, &v, 1) ? -1 : v;
}

void h2_audio_state_get(struct h2_audio_state *s)
{
	k_mutex_lock(&lock, K_FOREVER);
	s->powered = powered;
	s->running = running;
	s->dac_reg0 = powered ? dac_read(0) : -1;
	s->dac_vol = powered ? dac_read(DAC_VOL_L) : -1;
	s->dac_reg10 = powered ? dac_read(DAC_REG10) : -1;
	s->aic_cfg = REG(AIC_CFG);
	s->aic_ccr = REG(AIC_CCR);
	s->aic_i2scr = REG(AIC_I2SCR);
	s->aic_sr = REG(AIC_SR);
	s->i2scdr = REG(CPM_I2SCDR);
	s->i2sdiv = REG(AIC_I2SDIV);
	s->dma_ctrl = REG(DMA_CTRL);
	s->dma_cs = REG(DMA_CHN(CH) + DMA_CS);
	s->dma_sa = REG(DMA_CHN(CH) + DMA_SA);
	s->dma_da = REG(DMA_CHN(CH) + DMA_DA);
	s->m = dbg_m;
	s->n = dbg_n;
	s->sclk = dbg_sclk;
	s->pins_b = GPIO(PB, 0x00);
	k_mutex_unlock(&lock);
}
