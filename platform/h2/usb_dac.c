/*
 * The player as a USB DAC (usb_dac.h): a USB Audio Class 2 sound card, the
 * standard one computers have drivers for (macOS, Linux, Windows 10 and
 * later). The computer sends its audio; it goes to the player's DAC as it
 * came, at the rate and depth the computer chose: bit-perfect. The DAC's
 * own volume (the player's wheel and buttons) sets the level; the sound
 * card has no volume control of its own, so the computer sends at full
 * level and leaves the samples alone.
 *
 * Asynchronous: the player's clock decides. The computer sends a packet
 * every 125 us (a USB microframe), and the player tells it how many frames
 * to put in each (the feedback endpoint), more or less as the music
 * waiting here fills or empties.
 *
 * Interrupt time (h2_usb.c's interrupt calls dac_rx and friends): the
 * packets go into a ring. Thread time (dac_pump, every millisecond): the
 * ring goes to the DAC, and the feedback is worked out.
 *
 * Source: USB Device Class Definition for Audio Devices 2.0, and its
 * Audio Data Formats 2.0 and Terminal Types 2.0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "h2_usb.h"
#include "usb_dac.h"
#include "los_sys.h"

#define USB_VID 0x1209
#define USB_PID 0x0002                   /* pid.codes' test IDs, as the drive's */

/* Entities */
#define ID_CLOCK  0x10
#define ID_INPUT  0x01
#define ID_OUTPUT 0x03

/* Class requests */
#define REQ_CUR   0x01
#define REQ_RANGE 0x02
#define CS_SAM_FREQ    0x01
#define CS_CLOCK_VALID 0x02
#define AS_ACT_ALT     0x01
#define AS_VAL_ALT     0x02

static const uint32_t rates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
#define NRATES (sizeof(rates) / sizeof(rates[0]))

/* The streaming interface's settings: 1 = 16-bit, 2 = 24-bit (in 32), 3 = 32-bit.
 * 32-bit is offered as the H2 is specified for it, but reaches the DAC as
 * its top 24 bits: the X1000's I2S carries 8 to 24 (docs/h2-hardware.md,
 * "Rates") */
#define NALTS 3
static const struct { uint8_t subslot, bits; } formats[NALTS + 1] = { { 0, 0 }, { 2, 16 }, { 4, 24 }, { 4, 32 } };
/* the most a packet holds: 384 kHz is 48 frames a microframe, one more
 * when the computer catches up */
#define MAX_FRAMES 49

static const uint8_t device_desc[18] = {
	18, 1, 0x00, 0x02,
	0xef, 0x02, 0x01, 64,            /* miscellaneous, interface association */
	USB_VID & 0xff, USB_VID >> 8, USB_PID & 0xff, USB_PID >> 8,
	0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t qualifier_desc[10] = {
	10, 6, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 1, 0,
};

#define AC_LEN (9 + 8 + 17 + 12)         /* header, clock, input and output terminals */
#define ALT_LEN (9 + 16 + 6 + 7 + 8 + 7)
#define CONFIG_LEN (9 + 8 + 9 + AC_LEN + 9 + NALTS * ALT_LEN)

static uint8_t config_desc[CONFIG_LEN];

static uint8_t *put(uint8_t *p, const uint8_t *d, unsigned n)
{
	memcpy(p, d, n);
	return p + n;
}

static void build_config(void)
{
	uint8_t *p = config_desc;
	const uint8_t config[9] = { 9, 2, CONFIG_LEN & 0xff, CONFIG_LEN >> 8, 2, 1, 0, 0x80, 250 };
	/* the two interfaces are one audio function */
	const uint8_t iad[8] = { 8, 0x0b, 0, 2, 0x01, 0x00, 0x20, 0 };
	const uint8_t ac_if[9] = { 9, 4, 0, 0, 0, 0x01, 0x01, 0x20, 0 };
	const uint8_t ac_header[9] = { 9, 0x24, 0x01, 0x00, 0x02, 0x01,      /* UAC 2.0, desktop speaker */
				       AC_LEN & 0xff, AC_LEN >> 8, 0 };
	/* the clock: internal, its rate set by the computer, always valid */
	const uint8_t clock[8] = { 8, 0x24, 0x0a, ID_CLOCK, 0x03, 0x07, 0, 0 };
	/* from USB, two channels (front left and right) */
	const uint8_t input[17] = { 17, 0x24, 0x02, ID_INPUT, 0x01, 0x01, 0, ID_CLOCK, 2,
				    0x03, 0, 0, 0, 0, 0, 0, 0 };
	/* to headphones */
	const uint8_t output[12] = { 12, 0x24, 0x03, ID_OUTPUT, 0x02, 0x03, 0, ID_INPUT, ID_CLOCK,
				     0, 0, 0 };
	const uint8_t as_idle[9] = { 9, 4, 1, 0, 0, 0x01, 0x02, 0x20, 0 };

	p = put(p, config, 9);
	p = put(p, iad, 8);
	p = put(p, ac_if, 9);
	p = put(p, ac_header, 9);
	p = put(p, clock, 8);
	p = put(p, input, 17);
	p = put(p, output, 12);
	p = put(p, as_idle, 9);
	for (int alt = 1; alt <= NALTS; alt++) {
		unsigned mps = MAX_FRAMES * 2 * formats[alt].subslot;
		const uint8_t as_if[9] = { 9, 4, 1, (uint8_t)alt, 2, 0x01, 0x02, 0x20, 0 };
		const uint8_t general[16] = { 16, 0x24, 0x01, ID_INPUT, 0, 0x01, 0x01, 0, 0, 0,
					      2, 0x03, 0, 0, 0, 0 };
		const uint8_t format[6] = { 6, 0x24, 0x02, 0x01, formats[alt].subslot, formats[alt].bits };
		/* isochronous, asynchronous, every microframe */
		const uint8_t data_ep[7] = { 7, 5, 0x01, 0x05, (uint8_t)(mps & 0xff), (uint8_t)(mps >> 8), 1 };
		const uint8_t data_cs[8] = { 8, 0x25, 0x01, 0, 0, 0, 0, 0 };
		/* how many frames to send: isochronous, feedback, every 8 microframes */
		const uint8_t fb_ep[7] = { 7, 5, 0x81, 0x11, 4, 0, 4 };   /* every 1 ms */

		p = put(p, as_if, 9);
		p = put(p, general, 16);
		p = put(p, format, 6);
		p = put(p, data_ep, 7);
		p = put(p, data_cs, 8);
		p = put(p, fb_ep, 7);
	}
}

const uint8_t *dac_descriptor(unsigned kind, unsigned *len)
{
	if (kind == 1) {
		*len = sizeof(device_desc);
		return device_desc;
	}
	if (kind == 6) {
		*len = sizeof(qualifier_desc);
		return qualifier_desc;
	}
	if (kind == 2) {
		if (!config_desc[0])
			build_config();
		*len = sizeof(config_desc);
		return config_desc;
	}
	return NULL;
}

/* ---- the stream ---- */

#define RING_BYTES (128 * 1024)          /* 85 ms at 384 kHz, 32-bit; a power of 2 */
static uint32_t ring[RING_BYTES / 4];
static volatile uint32_t ring_w, ring_r; /* bytes, counting up; interrupt / thread */

static volatile uint32_t rate = 48000;   /* as the computer set it */
static volatile int alt;                 /* the streaming interface's setting */
static volatile bool restart;            /* the format changed: start the DAC again */
static volatile uint32_t feedback;       /* frames a microframe, 16.16 */
static volatile uint32_t overruns;
/* for the DAC screen's status line while it's being brought up */
static volatile uint32_t n_rx, n_rx_bytes, n_fb;
static uint32_t n_written, n_refused;

static uint32_t nominal(void)
{
	/* rate / 8000 microframes a second, in 16.16 */
	return (uint32_t)(((uint64_t)rate << 16) / 8000);
}

void dac_reset(void)
{
	alt = 0;
	restart = true;
}

int dac_get_interface(void)
{
	return alt;
}

void dac_set_interface(int a)
{
	if (a < 0 || a > NALTS)
		a = 0;
	if (a != alt) {
		alt = a;
		restart = true;
	}
	if (a) {
		feedback = nominal();
		usb_iso_open(MAX_FRAMES * 2 * formats[a].subslot);
		usb_iso_fb_send(feedback);
	} else {
		usb_iso_close();
	}
}

/* Packet data waiting in the receive FIFO (interrupt time) */
void dac_rx(unsigned bytes)
{
	unsigned words = (bytes + 3) / 4;
	n_rx++;
	n_rx_bytes += bytes;
	uint32_t free_bytes = RING_BYTES - (ring_w - ring_r);
	if (!alt || words * 4 > free_bytes) {
		usb_fifo_read(NULL, words);      /* nowhere to put it */
		overruns++;
		return;
	}
	uint32_t at = (ring_w % RING_BYTES) / 4;
	unsigned first = RING_BYTES / 4 - at;
	if (first > words)
		first = words;
	usb_fifo_read(ring + at, first);
	usb_fifo_read(ring, words - first);
	ring_w += bytes - bytes % 4;             /* whole frames are a multiple of 4 */
}

void dac_out_done(void)
{
	if (alt)
		usb_iso_out_arm();
}

void dac_fb_done(void)
{
	n_fb++;
	if (alt)
		usb_iso_fb_send(feedback);
}

/* Class requests: the clock's rate, the streaming interface's settings.
 * IN: fill buf, return its length, or -1 to stall. OUT (data in buf): 0
 * or -1. */
int dac_request(const uint8_t *setup, uint8_t *buf, unsigned len)
{
	bool in = setup[0] & 0x80;
	unsigned req = setup[1], cs = setup[3], entity = setup[5], iface = setup[4];

	if (iface == 0 && entity == ID_CLOCK && cs == CS_SAM_FREQ) {
		if (in && req == REQ_CUR) {
			uint32_t r = rate;
			memcpy(buf, &r, 4);
			return 4;
		}
		if (in && req == REQ_RANGE) {
			/* each rate on its own: min = max, no steps */
			uint16_t n = NRATES;
			memcpy(buf, &n, 2);
			for (unsigned i = 0; i < NRATES; i++) {
				uint32_t r[3] = { rates[i], rates[i], 0 };
				memcpy(buf + 2 + 12 * i, r, 12);
			}
			return 2 + 12 * NRATES;
		}
		if (!in && req == REQ_CUR && len >= 4) {
			uint32_t r;
			memcpy(&r, buf, 4);
			for (unsigned i = 0; i < NRATES; i++)
				if (rates[i] == r) {
					if (r != rate) {
						rate = r;
						restart = true;
					}
					feedback = nominal();
					return 0;
				}
			return -1;
		}
	}
	if (iface == 0 && entity == ID_CLOCK && cs == CS_CLOCK_VALID && in && req == REQ_CUR) {
		buf[0] = 1;
		return 1;
	}
	if (iface == 1 && entity == 0 && in && req == REQ_CUR) {
		if (cs == AS_ACT_ALT) {
			buf[0] = (uint8_t)alt;
			return 1;
		}
		if (cs == AS_VAL_ALT) {
			buf[0] = 1;
			buf[1] = (1u << (NALTS + 1)) - 1;   /* settings 0 to NALTS */
			return 2;
		}
	}
	return -1;
}

/* ---- thread time: to the DAC ---- */

static bool dac_on;
static bool dac_refused;                 /* the DAC said no to this format */
static unsigned dac_rate, dac_bits;
static uint64_t dac_written;
static int32_t frames_buf[1024 * 2];

static void dac_stop(void)
{
	if (dac_on)
		los_audio_stop();
	dac_on = false;
}

void dac_pump(void)
{
	int a = alt;
	if (restart) {
		restart = false;
		dac_refused = false;
		dac_stop();
		ring_r = ring_w;                 /* what came in the old format */
	}
	if (!a) {
		dac_stop();
		ring_r = ring_w;
		return;
	}
	unsigned slot = formats[a].subslot, frame = 2 * slot;
	if (dac_refused) {
		/* not again each millisecond (each try clears the DAC's ring
		 * and talks to it: the player stopped answering at 384 kHz) */
		ring_r = ring_w;
		return;
	}
	if (!dac_on) {
		/* a little music first (10 ms), so the DAC doesn't start short */
		if ((ring_w - ring_r) / frame < rate / 100)
			return;
		dac_rate = rate;
		dac_bits = formats[a].bits;
		if (!los_audio_start(dac_rate, dac_bits, 2)) {
			n_refused++;
			dac_refused = true;
			return;
		}
		dac_on = true;
		dac_written = 0;
	}
	/* as much as the DAC takes */
	for (;;) {
		uint32_t have = (ring_w - ring_r) / frame;
		if (!have)
			break;
		unsigned n = have < 1024 ? have : 1024;
		const uint8_t *src = (const uint8_t *)ring;
		for (unsigned i = 0; i < n * 2; i++) {
			uint32_t off = (ring_r + i * slot) % RING_BYTES;
			int32_t v;
			if (slot == 2) {
				int16_t s;
				memcpy(&s, src + off, 2);
				v = s;
			} else {
				memcpy(&v, src + off, 4);
				if (dac_bits == 24)
					v >>= 8;             /* 24 bits at the top of 32 */
			}
			frames_buf[i] = v;
		}
		size_t took = los_audio_write(frames_buf, n);
		ring_r += (uint32_t)took * frame;
		dac_written += took;
		n_written += (uint32_t)took;
		if (took < n)
			break;
	}
	/* how fast to send: the rate, nudged to keep about 20 ms waiting
	 * here and in the DAC (up to a tenth of a percent either way) */
	int64_t waiting = (int64_t)((ring_w - ring_r) / frame) + (int64_t)(dac_written - los_audio_played());
	int64_t target = dac_rate / 50;
	int64_t err = target - waiting;
	if (err > target)
		err = target;
	if (err < -target)
		err = -target;
	uint32_t nom = nominal();
	feedback = (uint32_t)((int64_t)nom + (int64_t)nom * err / target / 1000);
}

int dac_debug(char *buf, unsigned n)
{
	return snprintf(buf, n, "alt %d  in %u (%u kB)  fb %u  over %u\ndac %s%s  out %u  ring %u",
			alt, n_rx, n_rx_bytes / 1024, n_fb, overruns, dac_on ? "on" : "off",
			n_refused ? " (refused)" : "", n_written, ring_w - ring_r);
}

bool dac_status(unsigned *r, unsigned *bits)
{
	int a = alt;
	*r = rate;
	*bits = a ? formats[a].bits : 0;
	return a != 0;
}

void dac_end(void)
{
	alt = 0;
	dac_stop();
}
