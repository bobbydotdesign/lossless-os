/*
 * USB on the H2: the X1000's USB clock and PHY, and the controller (a
 * Synopsys DesignWare OTG core, "DWC2") as a USB device: the player as a
 * drive on a computer (usb_msc.c is the drive).
 *
 * The controller is driven from its own thread, polled: no interrupts. It
 * starts when a cable comes in (PD3) and stops when it goes. A charger
 * never sets the device up, so only a computer gets as far as the app
 * (los_usb_state: LOS_USB_COMPUTER), which then hands over the card
 * (los_usb_share).
 *
 * Slave mode (the CPU moves data through the FIFOs): no DMA, so no cache
 * maintenance. Endpoint 0 for control, endpoint 1 IN and OUT for bulk.
 *
 * At start, once, it also notes the controller's registers in
 * /.lossless/logs/usb.txt (h2_usb_probe), for comparing with the facts.
 *
 * Facts: docs/h2-hardware.md, "USB". The DWC2 register layout is the
 * standard one (the same in every chip that has this core).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include "x1000_soc.h"
#include "h2_usb.h"
#include "usb_msc.h"
#include "usb_dac.h"
#include "h2_audio.h"
#include <zephyr/irq.h>
#include <soc.h>
#include "los_sys.h"

/* ---- clock and PHY (CPM) ---- */

#define CPM_CLKGR    0xb0000020u   /* bit 3: OTG clock stopped */
#define CPM_OPCR     0xb0000024u   /* bit 23: PHY clock stopped; bit 7: PHY awake */
#define CPM_USBPCR   0xb000003cu
#define CPM_USBRDT   0xb0000040u
#define CPM_USBVBFIL 0xb0000044u
#define CPM_USBPCR1  0xb0000048u
#define CPM_USBCDR   0xb0000050u
#define CPM_SRBC     0xb00000c4u   /* bit 12: OTG reset */

#define CLKGR_OTG        (1u << 3)
#define OPCR_PHY_STOPPED (1u << 23)
#define OPCR_PHY_AWAKE   (1u << 7)
#define USBCDR_CE        (1u << 29)
#define USBCDR_BUSY      (1u << 28)
#define USBCDR_STOP      (1u << 27)
#define USBCDR_PHY_GATE  (1u << 26)
#define USBPCR_POR       (1u << 22)
#define USBPCR_SIDDQ     (1u << 21)
#define USBPCR_OTG_OFF   (1u << 20)
#define SRBC_OTG         (1u << 12)

/* USB device mode, the PHY always seeing a valid VBUS (the cable is
 * detected on a pin instead), and the PHY's tuning */
#define USBPCR_DEVICE 0x03903857u
/* Reference clock from the USB clock divider, 24 MHz, 16-bit interface,
 * session valid */
#define USBPCR1_DEVICE 0x89080000u
/* VBUS filter load on, reset timer 0x96 */
#define USBRDT_DEVICE 0x02000096u

/* ---- the controller (DWC2) ---- */

#define OTG 0xb3500000u
#define R(off) X1000_REG(OTG + (off))

#define GAHBCFG   0x008
#define GUSBCFG   0x00c
#define GRSTCTL   0x010
#define GINTSTS   0x014
#define GINTMSK   0x018
#define GRXSTSP   0x020
#define GRXFSIZ   0x024
#define DIEPTXF0  0x028
#define GSNPSID   0x040
#define GHWCFG1   0x044
#define GHWCFG2   0x048
#define GHWCFG3   0x04c
#define GHWCFG4   0x050
#define GDFIFOCFG 0x05c
#define DIEPTXF1  0x104
#define DCFG      0x800
#define DCTL      0x804
#define DSTS      0x808
#define DIEPMSK   0x810
#define DOEPMSK   0x814
#define DAINTMSK  0x81c
#define DIEPCTL(n)  (0x900 + 0x20 * (n))
#define DIEPINT(n)  (0x908 + 0x20 * (n))
#define DIEPTSIZ(n) (0x910 + 0x20 * (n))
#define DTXFSTS(n)  (0x918 + 0x20 * (n))
#define DOEPCTL(n)  (0xb00 + 0x20 * (n))
#define DOEPINT(n)  (0xb08 + 0x20 * (n))
#define DOEPTSIZ(n) (0xb10 + 0x20 * (n))
#define PCGCCTL   0xe00
#define FIFO(n)   (0x1000 * ((n) + 1))

#define GUSBCFG_PHYIF16 (1u << 3)
#define GUSBCFG_TRDT(n) ((uint32_t)(n) << 10)
#define GRSTCTL_CSRST   (1u << 0)
#define GRSTCTL_RXFFLSH (1u << 4)
#define GRSTCTL_TXFFLSH (1u << 5)
#define GRSTCTL_TXFNUM(n) ((uint32_t)(n) << 6)
#define GRSTCTL_AHBIDL  (1u << 31)
#define GINT_RXFLVL     (1u << 4)
#define GINT_USBSUSP    (1u << 11)
#define GINT_USBRST     (1u << 12)
#define GINT_ENUMDNE    (1u << 13)
#define DCFG_NZLSOHSK   (1u << 2)
#define DCFG_DAD(a)     ((uint32_t)(a) << 4)
#define DCFG_DAD_MASK   (0x7fu << 4)
#define DCTL_SDIS       (1u << 1)
#define DCTL_CGINAK     (1u << 8)
#define DCTL_SGOUTNAK   (1u << 9)
#define DCTL_CGOUTNAK   (1u << 10)
#define GINT_GOUTNAKEFF (1u << 7)
#define EPCTL_USBAEP    (1u << 15)
#define EPCTL_ISO       (1u << 18)
#define EPCTL_BULK      (2u << 18)
#define EPCTL_STALL     (1u << 21)
#define EPCTL_TXFNUM(n) ((uint32_t)(n) << 22)
#define EPCTL_CNAK      (1u << 26)
#define EPCTL_SNAK      (1u << 27)
#define EPCTL_SD0PID    (1u << 28)   /* also "even frame" for isochronous */
#define EPCTL_SODDFRM   (1u << 29)
#define EPCTL_EPDIS     (1u << 30)
#define EPCTL_EPENA     (1u << 31)
#define EPINT_XFRC      (1u << 0)
#define EPINT_EPDISD    (1u << 1)
#define EPINT_NAK       (1u << 13)   /* isochronous IN: asked for, nothing ready (a zero-length went) */
#define GINT_IEPINT     (1u << 18)
#define GINT_OEPINT     (1u << 19)
#define GINT_IISOIXFR   (1u << 20)   /* an isochronous IN that didn't go */
#define GINT_INCOMPISO  (1u << 21)   /* an isochronous OUT in the wrong microframe */
#define GAHBCFG_GINT    (1u << 0)

/* The interrupt controller: USB is source 21 of bank 0 */
#define INTC_PND0      0xb0001010u
#define INTC_MSK_SET0  0xb0001008u
#define INTC_MSK_CLR0  0xb000100cu
#define INTC_OTG       (1u << 21)
#define EPINT_STUP      (1u << 3)
#define TSIZ_PKTCNT(n)  ((uint32_t)(n) << 19)
#define TSIZ_STUPCNT(n) ((uint32_t)(n) << 29)

/* FIFO memory, in words: receive, endpoint 0's transmit, endpoint 1's */
#define RX_WORDS  816
#define TX0_WORDS 32
#define TX1_WORDS 384

#define CABLE_PIN 3                /* PD3: high while a USB cable is in */
#define GPIO_PIN_LEVEL 0x00

/* Our own USB IDs come later (pid.codes); until then pid.codes' test ID,
 * which is for testing only. docs/release-checklist.md has it. */
#define USB_VID 0x1209
#define USB_PID 0x0001

static void usb_clock_wait(void)
{
	for (int i = 0; i < 10000 && (X1000_REG(CPM_USBCDR) & USBCDR_BUSY); i++)
		k_busy_wait(1);
}

void h2_usb_power(bool on)
{
	if (on) {
		X1000_REG(CPM_CLKGR) &= ~CLKGR_OTG;
		/* the USB clock: the 24 MHz crystal, not divided */
		uint32_t cdr = X1000_REG(CPM_USBCDR);
		cdr &= ~((3u << 30) | 0xffu | USBCDR_PHY_GATE | USBCDR_STOP);
		X1000_REG(CPM_USBCDR) = cdr | USBCDR_CE;
		usb_clock_wait();
		X1000_REG(CPM_USBCDR) &= ~USBCDR_CE;

		X1000_REG(CPM_SRBC) |= SRBC_OTG;
		k_busy_wait(10);
		X1000_REG(CPM_SRBC) &= ~SRBC_OTG;

		X1000_REG(CPM_OPCR) &= ~OPCR_PHY_STOPPED;
		X1000_REG(CPM_OPCR) |= OPCR_PHY_AWAKE;
		k_busy_wait(45);

		X1000_REG(CPM_USBVBFIL) = 0;
		X1000_REG(CPM_USBRDT) = USBRDT_DEVICE;
		X1000_REG(CPM_USBPCR) = USBPCR_DEVICE;
		X1000_REG(CPM_USBPCR1) = USBPCR1_DEVICE;
		/* reset the PHY */
		X1000_REG(CPM_USBPCR) = USBPCR_DEVICE | USBPCR_POR;
		k_busy_wait(1000);
		X1000_REG(CPM_USBPCR) = USBPCR_DEVICE;
		k_busy_wait(1000);
		R(PCGCCTL) = 0;                  /* the core's own clock on */
	} else {
		/* off the bus first, the core still clocked */
		R(DCTL) |= DCTL_SDIS;
		k_busy_wait(10);
		R(PCGCCTL) = 1;

		X1000_REG(CPM_OPCR) &= ~OPCR_PHY_AWAKE;
		k_busy_wait(5);
		X1000_REG(CPM_USBPCR) |= USBPCR_OTG_OFF | USBPCR_SIDDQ;
		X1000_REG(CPM_OPCR) |= OPCR_PHY_STOPPED;
		X1000_REG(CPM_USBCDR) |= USBCDR_CE | USBCDR_STOP | USBCDR_PHY_GATE;
		usb_clock_wait();
		X1000_REG(CPM_USBCDR) &= ~USBCDR_CE;
		X1000_REG(CPM_CLKGR) |= CLKGR_OTG;
	}
}

bool h2_usb_cable(void)
{
	return (X1000_GPIO(X1000_PD, GPIO_PIN_LEVEL) >> CABLE_PIN) & 1;
}

/* ---- descriptors ---- */

static const uint8_t device_desc[18] = {
	18, 1, 0x00, 0x02,              /* USB 2.0 */
	0, 0, 0, 64,                    /* class in the interface; endpoint 0: 64 bytes */
	USB_VID & 0xff, USB_VID >> 8, USB_PID & 0xff, USB_PID >> 8,
	0x00, 0x01,                     /* device release 1.00 */
	1, 2, 3,                        /* strings: maker, product, serial */
	1,                              /* one configuration */
};

static const uint8_t qualifier_desc[10] = {
	10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0,
};

/* Configuration, the mass storage interface (SCSI, bulk only) and its two
 * endpoints; the packet size is filled in for the speed */
static uint8_t config_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 250,          /* bus powered, 500 mA */
	9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,       /* mass storage, SCSI, bulk only */
	7, 5, 0x81, 2, 0, 2, 0,                   /* bulk IN 1 */
	7, 5, 0x01, 2, 0, 2, 0,                   /* bulk OUT 1 */
};

static const char *const strings[] = { NULL, "Lossless", "Lossless OS player", "4C4F53534C455353" };

/* ---- the device ---- */

static volatile enum los_usb state;
static unsigned mps = 512;             /* bulk packet size: 512 high speed, 64 full */
static volatile uint32_t generation;   /* goes up at each bus reset, cable or role change */

/* The role: a drive (polled, usb_msc.c) or a sound card (from the
 * interrupt, usb_dac.c). The app asks; the USB thread switches. */
static volatile bool dac_role, dac_wanted;

/* Endpoint 0: replies longer than a packet go a packet at a time; a
 * request with data from the computer waits for it */
static const uint8_t *ep0_tx;
static uint32_t ep0_tx_left;
static bool ep0_zlp;                   /* a reply a whole number of packets, shorter than asked */
static uint8_t ep0_buf[128];
static bool ep0_pending_out;
static bool ep0_data_ready;            /* data came in after the last request */
static uint8_t ep0_pending[8];
static uint32_t ep0_out_len;
static bool configured;
static bool in_done, out_done, in_halted;
static uint8_t *out_buf;
static uint32_t out_got, out_max;
static uint8_t setup[8];
static uint8_t ep0_out[64];
static uint32_t cable_low_ms;
static bool cable_gone;

static void wait_bits_clear(uint32_t off, uint32_t bits)
{
	for (int i = 0; i < 100000 && (R(off) & bits); i++)
		k_busy_wait(1);
}

static void wait_bits_set(uint32_t off, uint32_t bits)
{
	for (int i = 0; i < 100000 && !(R(off) & bits); i++)
		k_busy_wait(1);
}

static void flush_tx(unsigned n)
{
	R(GRSTCTL) = GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM(n);
	wait_bits_clear(GRSTCTL, GRSTCTL_TXFFLSH);
}

static void flush_rx(void)
{
	R(GRSTCTL) = GRSTCTL_RXFFLSH;
	wait_bits_clear(GRSTCTL, GRSTCTL_RXFFLSH);
}

/* Words into a transmit FIFO, from bytes at any alignment */
static void fifo_write(unsigned ep, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (unsigned b = 0; b < 4 && i + b < n; b++)
			w |= (uint32_t)p[i + b] << (8 * b);
		R(FIFO(ep)) = w;
	}
}

/* n bytes out of the receive FIFO; past 'max' they're dropped */
static void fifo_read(uint8_t *p, uint32_t n, uint32_t max)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = R(FIFO(0));
		for (unsigned b = 0; b < 4 && i + b < n; b++)
			if (p && i + b < max)
				p[i + b] = (uint8_t)(w >> (8 * b));
	}
}

/* ---- a log of endpoint 0, for the sound card (written to the card when
 * it stops: /.lossless/logs/usb-dac.txt) ---- */

#define LOG_N 256
struct dlog_entry { uint32_t t, a, b, c; char k; };
static struct dlog_entry dlog[LOG_N];
static volatile uint32_t dlog_n;

static void note(char k, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t i = dlog_n++ % LOG_N;

	dlog[i].t = k_cycle_get_32();
	dlog[i].k = k;
	dlog[i].a = a;
	dlog[i].b = b;
	dlog[i].c = c;
}

static int dac_counts(char *buf, unsigned n);

static void dlog_save(void)
{
	static char text[LOG_N * 48 + 600];
	static const char path[] = "/SD:/.lossless/logs/usb-dac.txt";
	uint32_t n = dlog_n, first = n > LOG_N ? n - LOG_N : 0;
	int len = dac_counts(text, 500);

	len = len > 0 && len < 500 ? len : 0;
	len += snprintf(text + len, sizeof(text) - len, "\nendpoint 0: %u events\n", n);

	for (uint32_t i = first; i < n && len < (int)sizeof(text) - 48; i++) {
		const struct dlog_entry *e = &dlog[i % LOG_N];
		len += snprintf(text + len, sizeof(text) - len, "%08x %c %08x %08x %08x\n",
				e->t, e->k, e->a, e->b, e->c);
	}
	los_mkdir("/SD:/.lossless");
	los_mkdir("/SD:/.lossless/logs");
	los_file_save(path, text, (size_t)len);
}

static void ep0_arm(void)
{
	R(DOEPTSIZ(0)) = TSIZ_STUPCNT(3) | TSIZ_PKTCNT(1) | 64;
	R(DOEPCTL(0)) |= EPCTL_EPENA | EPCTL_CNAK;
}

static void ep0_chunk(void)
{
	uint32_t n = ep0_tx_left < 64 ? ep0_tx_left : 64;

	if (!n)
		ep0_zlp = false;
	R(DIEPTSIZ(0)) = TSIZ_PKTCNT(1) | n;
	R(DIEPCTL(0)) |= EPCTL_EPENA | EPCTL_CNAK;
	fifo_write(0, ep0_tx, n);
	ep0_tx += n;
	ep0_tx_left -= n;
}

/* A reply of 'len' bytes to a request for 'want' */
static void ep0_reply(const void *data, uint32_t len, uint32_t want)
{
	if (len > want)
		len = want;
	ep0_tx = data;
	ep0_tx_left = len;
	ep0_zlp = len < want && len % 64 == 0 && len;
	ep0_chunk();
}

static void ep0_send(const void *data, uint32_t len)
{
	ep0_reply(data, len, len);
}

static void ep0_stall(void)
{
	R(DIEPCTL(0)) |= EPCTL_STALL;
	R(DOEPCTL(0)) |= EPCTL_STALL;
}

static void bulk_off(void)
{
	if (R(DIEPCTL(1)) & EPCTL_EPENA)
		R(DIEPCTL(1)) |= EPCTL_SNAK | EPCTL_EPDIS;
	if (R(DOEPCTL(1)) & EPCTL_EPENA)
		R(DOEPCTL(1)) |= EPCTL_SNAK | EPCTL_EPDIS;
	k_busy_wait(10);
	R(DIEPCTL(1)) = 0;
	R(DOEPCTL(1)) = 0;
	flush_tx(1);
	configured = false;
}

static void bulk_on(void)
{
	R(DIEPCTL(1)) = EPCTL_USBAEP | EPCTL_BULK | EPCTL_TXFNUM(1) | EPCTL_SD0PID | mps;
	R(DOEPCTL(1)) = EPCTL_USBAEP | EPCTL_BULK | EPCTL_SD0PID | mps;
	flush_tx(1);
	in_halted = false;
	configured = true;
}

static void on_bus_reset(void)
{
	generation++;
	bulk_off();
	R(DCFG) &= ~DCFG_DAD_MASK;
	R(DIEPINT(0)) = 0xffffffffu;
	R(DOEPINT(0)) = 0xffffffffu;
	R(DIEPINT(1)) = 0xffffffffu;
	R(DOEPINT(1)) = 0xffffffffu;
	flush_tx(0);
	flush_rx();
	ep0_arm();
	if (state == LOS_USB_COMPUTER)
		state = LOS_USB_CABLE;            /* set up again before the app hears of it */
	if (dac_role)
		dac_reset();
	else
		msc_reset();
}

static void on_enum_done(void)
{
	unsigned speed = (R(DSTS) >> 1) & 3;   /* 0 high speed */

	mps = speed == 0 ? 512 : 64;
	R(DIEPCTL(0)) &= ~3u;                   /* endpoint 0: 64 bytes */
	R(DCTL) |= DCTL_CGINAK;
	ep0_arm();
}

static void send_string(unsigned i, uint16_t want)
{
	uint8_t d[64];
	unsigned n;

	if (i == 0) {
		d[2] = 0x09;                    /* English (US) */
		d[3] = 0x04;
		n = 4;
	} else {
		const char *s = strings[i];

		n = 2;
		for (; *s && n + 2 <= sizeof(d); s++) {
			d[n++] = (uint8_t)*s;
			d[n++] = 0;
		}
	}
	d[0] = (uint8_t)n;
	d[1] = 3;
	ep0_send(d, n < want ? n : want);
}

static void on_setup(void)
{
	uint8_t type = setup[0], req = setup[1];
	uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
	uint16_t index = (uint16_t)(setup[4] | setup[5] << 8);
	uint16_t len = (uint16_t)(setup[6] | setup[7] << 8);
	static uint8_t reply[32];
	bool status_out = false;            /* a data stage IN, then the host's status */

	if (dac_role)
		note('S', setup[0] | setup[1] << 8 | setup[2] << 16 | (uint32_t)setup[3] << 24,
		     setup[4] | setup[5] << 8 | setup[6] << 16 | (uint32_t)setup[7] << 24, R(DIEPCTL(0)));
	/* a new request ends the last one: whatever of its reply hasn't gone
	 * is dropped, and its "done" mustn't send more of it */
	ep0_tx_left = 0;
	ep0_zlp = false;
	if (R(DIEPCTL(0)) & EPCTL_EPENA) {
		R(DIEPCTL(0)) |= EPCTL_SNAK | EPCTL_EPDIS;
		for (int i = 0; i < 100 && !(R(DIEPINT(0)) & EPINT_EPDISD); i++)
			k_busy_wait(1);
	}
	flush_tx(0);
	R(DIEPINT(0)) = 0xffffffffu;

	if ((type & 0x60) == 0x20 && dac_role) {
		/* the sound card's: data from the computer waits for it */
		if (!(type & 0x80) && len) {
			memcpy(ep0_pending, setup, 8);
			ep0_pending_out = true;
		} else if (type & 0x80) {
			int n = dac_request(setup, ep0_buf, len);

			if (n < 0)
				ep0_stall();
			else
				ep0_reply(ep0_buf, (uint32_t)n, len);
		} else {
			if (dac_request(setup, NULL, 0) < 0)
				ep0_stall();
			else
				ep0_send(NULL, 0);
		}
	} else if ((type & 0x60) == 0x20) {
		/* class requests to the interface */
		if (req == 0xff && len == 0) {          /* bulk-only mass storage reset */
			msc_reset();
			ep0_send(NULL, 0);
		} else if (req == 0xfe && len >= 1) {   /* get max LUN: one drive */
			reply[0] = 0;
			ep0_send(reply, 1);
			status_out = true;
		} else {
			ep0_stall();
		}
	} else if ((type & 0x60) != 0) {
		ep0_stall();
	} else {
		switch (req) {
		case 0x00:                              /* get status */
			reply[0] = (type & 0x1f) == 2 && (index & 0x7f) == 1 && (index & 0x80) &&
				   in_halted;
			reply[1] = 0;
			ep0_send(reply, len < 2 ? len : 2);
			status_out = true;
			break;
		case 0x01:                              /* clear feature */
			if ((type & 0x1f) == 2 && value == 0 && (index & 0x7f) == 1) {
				/* endpoint halt: carry on, from DATA0 */
				if (index & 0x80) {
					R(DIEPCTL(1)) = (R(DIEPCTL(1)) & ~EPCTL_STALL) | EPCTL_SD0PID;
					in_halted = false;
				} else {
					R(DOEPCTL(1)) = (R(DOEPCTL(1)) & ~EPCTL_STALL) | EPCTL_SD0PID;
				}
			}
			ep0_send(NULL, 0);
			break;
		case 0x03:                              /* set feature: nothing we do */
			ep0_send(NULL, 0);
			break;
		case 0x05:                              /* set address */
			R(DCFG) = (R(DCFG) & ~DCFG_DAD_MASK) | DCFG_DAD(value & 0x7f);
			ep0_send(NULL, 0);
			break;
		case 0x06: {                            /* get descriptor */
			uint8_t kind = value >> 8;
			unsigned dlen;
			const uint8_t *d = dac_role ? dac_descriptor(kind, &dlen) : NULL;

			if (d) {
				ep0_reply(d, dlen, len);
			} else if (kind == 1) {
				ep0_send(device_desc, len < 18 ? len : 18);
			} else if (kind == 2) {
				config_desc[22] = config_desc[29] = (uint8_t)(mps & 0xff);
				config_desc[23] = config_desc[30] = (uint8_t)(mps >> 8);
				ep0_send(config_desc, len < 32 ? len : 32);
			} else if (kind == 3 && (value & 0xff) < ARRAY_SIZE(strings)) {
				/* the same names in both roles */
				send_string(value & 0xff, len);
			} else if (kind == 6) {
				ep0_send(qualifier_desc, len < 10 ? len : 10);
			} else {
				ep0_stall();
				break;
			}
			status_out = true;
			break;
		}
		case 0x08:                              /* get configuration */
			reply[0] = configured;
			ep0_send(reply, 1);
			status_out = true;
			break;
		case 0x09:                              /* set configuration */
			if (dac_role) {
				configured = value == 1;
				if (!configured)
					dac_set_interface(0);
			} else if (value == 1) {
				bulk_on();
				state = LOS_USB_COMPUTER;
			} else {
				bulk_off();
			}
			ep0_send(NULL, 0);
			break;
		case 0x0a:                              /* get interface */
			reply[0] = dac_role && (index & 0xff) == 1 ? (uint8_t)dac_get_interface() : 0;
			ep0_send(reply, 1);
			status_out = true;
			break;
		case 0x0b:                              /* set interface */
			if (dac_role && (index & 0xff) == 1)
				dac_set_interface(value);
			ep0_send(NULL, 0);
			break;
		default:
			ep0_stall();
			break;
		}
	}
	(void)status_out;
	ep0_arm();                                  /* the status stage, and the next setup */
}

/* What arrived: one entry of the receive FIFO */
static void rx_pop(void)
{
	uint32_t s = R(GRXSTSP);
	unsigned ep = s & 15, count = (s >> 4) & 0x7ff, kind = (s >> 17) & 15;

	if (kind == 6) {                           /* setup data */
		fifo_read(setup, count, sizeof(setup));
		ep0_data_ready = false;
	} else if (kind == 2) {                    /* OUT data */
		if (ep == 1 && dac_role) {
			dac_rx(count);
		} else if (ep == 0) {
			ep0_out_len = count < sizeof(ep0_out) ? count : sizeof(ep0_out);
			fifo_read(ep0_out, count, sizeof(ep0_out));
			if (count)
				ep0_data_ready = true;  /* a status stage's is empty */
		} else if (ep == 1 && out_buf) {
			uint32_t room = out_got < out_max ? out_max - out_got : 0;

			fifo_read(out_buf + out_got, count, room);
			out_got += count < room ? count : room;
		} else {
			fifo_read(ep == 0 ? ep0_out : NULL, count, ep == 0 ? sizeof(ep0_out) : 0);
		}
	}
}

/* Watch the cable: gone once it has been out for 200 ms */
static void cable_watch(void)
{
	static int64_t last;
	int64_t now = k_uptime_get();

	if (h2_usb_cable()) {
		cable_low_ms = 0;
	} else if (now != last) {
		cable_low_ms += (uint32_t)(now - last);
		if (cable_low_ms >= 200 && !cable_gone) {
			cable_gone = true;
			generation++;
		}
	}
	last = now;
}

static volatile uint32_t n_incomp_out, n_incomp_in, n_irq;

void usb_iso_out_arm(void);
static void out_arm(unsigned parity);
static unsigned out_parity, fb_parity;   /* the microframe each is set for: even 0, odd 1 */
static void fb_asked(void);
static void fb_sent(void);
static void fb_missed(void);

/* An isochronous endpoint missed the microframe it was set for (the
 * computer sent nothing then, or asked for nothing): it stays enabled with
 * its packet, so it only needs the other microframe, the next one. Its
 * size and count mustn't be touched while it's enabled. Only one set for
 * the microframe now ending is meant; one already set for the next is left
 * alone (and if we're late, it simply waits a microframe more). */
static void iso_missed(uint32_t ctl_reg, bool out)
{
	uint32_t c = R(ctl_reg);

	if (!(c & EPCTL_EPENA)) {
		if (out)
			out_arm(((R(DSTS) >> 8) + 1) & 1);
		return;
	}
	unsigned set_for = (c >> 16) & 1, now = (R(DSTS) >> 8) & 1;

	if (set_for == now) {
		R(ctl_reg) = (c & ~(EPCTL_EPDIS | EPCTL_SNAK | EPCTL_CNAK)) |
			     (set_for ? EPCTL_SD0PID : EPCTL_SODDFRM);
		*(out ? &out_parity : &fb_parity) = !set_for;
	}
}

void usb_service(void)
{
	if (!dac_role)
		cable_watch();                  /* as the sound card, the thread watches it */
	uint32_t st = R(GINTSTS);

	if (st & GINT_USBRST) {
		R(GINTSTS) = GINT_USBRST;
		on_bus_reset();
	}
	if (st & GINT_ENUMDNE) {
		R(GINTSTS) = GINT_ENUMDNE;
		on_enum_done();
	}
	if (st & GINT_USBSUSP)
		R(GINTSTS) = GINT_USBSUSP;
	for (int i = 0; i < 64 && (R(GINTSTS) & GINT_RXFLVL); i++)
		rx_pop();

	/* endpoint 0 IN first: a reply's "done" belongs with that reply, not
	 * with a request that arrived after it */
	uint32_t i0 = R(DIEPINT(0));

	if (i0) {
		R(DIEPINT(0)) = i0;
		if ((i0 & EPINT_XFRC) && (ep0_tx_left || ep0_zlp))
			ep0_chunk();                /* the rest of a long reply */
	}
	uint32_t o0 = R(DOEPINT(0));

	if (o0) {
		R(DOEPINT(0)) = o0;
		/* a request, and the data it carried, can both be here by now:
		 * the request first, then its data (only the request was taken
		 * before, so the computer had to send it again, twice) */
		if (o0 & EPINT_STUP) {
			ep0_pending_out = false;
			on_setup();
		}
		if (o0 & EPINT_XFRC) {
			/* only once its data is here: a "done" can be the last
			 * request's status stage, and acting on that (no data)
			 * refused the request: the Mac's change to 192 kHz */
			if (ep0_pending_out && ep0_data_ready) {
				/* the data a request carried: act on it, then the status */
				int r = dac_request(ep0_pending, ep0_out, ep0_out_len);

				ep0_pending_out = ep0_data_ready = false;
				if (dac_role)
					note('P', ep0_out_len, (uint32_t)r,
					     ep0_out[0] | ep0_out[1] << 8 | ep0_out[2] << 16 | (uint32_t)ep0_out[3] << 24);
				if (r < 0)
					ep0_stall();
				else
					ep0_send(NULL, 0);
			}
			if (!(o0 & EPINT_STUP))
				ep0_arm();              /* on_setup arms it itself */
		}
	}
	uint32_t i1 = R(DIEPINT(1));

	if (i1) {
		R(DIEPINT(1)) = i1;
		if (dac_role) {
			if (i1 & EPINT_EPDISD)
				flush_tx(1);            /* a feedback that was given up on */
			if (i1 & EPINT_XFRC)
				fb_sent();
			if (i1 & EPINT_NAK)
				fb_asked();
		} else if (i1 & EPINT_XFRC) {
			in_done = true;
		}
	}
	uint32_t o1 = R(DOEPINT(1));

	if (o1) {
		R(DOEPINT(1)) = o1;
		if (o1 & EPINT_XFRC) {
			out_done = true;
			if (dac_role)
				dac_out_done();
		}
	}
	if (st & GINT_INCOMPISO) {
		R(GINTSTS) = GINT_INCOMPISO;
		n_incomp_out++;
		if (dac_role)
			iso_missed(DOEPCTL(1), true);
	}
	if (st & GINT_IISOIXFR) {
		R(GINTSTS) = GINT_IISOIXFR;
		n_incomp_in++;
		if (dac_role)
			fb_missed();
	}
}

/* ---- isochronous endpoints, for the sound card ---- */

static unsigned iso_mps;

void usb_fifo_read(uint32_t *dst, unsigned words)
{
	for (unsigned i = 0; i < words; i++) {
		uint32_t w = R(FIFO(0));

		if (dst)
			dst[i] = w;
	}
}

/* Even or odd: the microframe after this one */
static uint32_t next_parity(void)
{
	return (((R(DSTS) >> 8) & 0x3fff) + 1) & 1;
}

/* Both stream endpoints really stopped. An enabled OUT endpoint only
 * stops under a global OUT NAK: set it, wait for it to take, disable the
 * endpoint, wait for that, clear it. (Writing its control register to 0
 * doesn't stop it: it stayed enabled, and the next stream found it so and
 * never set it up: no audio after the computer stopped and restarted.) */
static void iso_stop(void)
{
	iso_mps = 0;
	if (R(DOEPCTL(1)) & EPCTL_EPENA) {
		R(DCTL) |= DCTL_SGOUTNAK;
		/* "took effect" comes through the receive FIFO, behind whatever
		 * is in it: keep reading it while waiting */
		for (int i = 0; i < 1000 && !(R(GINTSTS) & GINT_GOUTNAKEFF); i++) {
			for (int k = 0; k < 64 && (R(GINTSTS) & GINT_RXFLVL); k++)
				rx_pop();
			k_busy_wait(1);
		}
		note('N', R(GINTSTS) & GINT_GOUTNAKEFF ? 1 : 0, 0, 0);
		R(DOEPCTL(1)) |= EPCTL_EPDIS | EPCTL_SNAK;
		for (int i = 0; i < 1000 && !(R(DOEPINT(1)) & EPINT_EPDISD); i++)
			k_busy_wait(1);
		R(DOEPINT(1)) = EPINT_EPDISD;
		R(DCTL) |= DCTL_CGOUTNAK;
	}
	if (R(DIEPCTL(1)) & EPCTL_EPENA) {
		R(DIEPCTL(1)) |= EPCTL_EPDIS | EPCTL_SNAK;
		for (int i = 0; i < 1000 && !(R(DIEPINT(1)) & EPINT_EPDISD); i++)
			k_busy_wait(1);
		R(DIEPINT(1)) = EPINT_EPDISD;
	}
	flush_tx(1);
	R(DOEPCTL(1)) = 0;
	R(DIEPCTL(1)) = 0;
}

void usb_iso_open(unsigned out_mps)
{
	iso_stop();                         /* from a stream before, if any */
	iso_mps = out_mps;
	R(DOEPCTL(1)) = EPCTL_USBAEP | EPCTL_ISO | out_mps;
	R(DIEPCTL(1)) = EPCTL_USBAEP | EPCTL_ISO | EPCTL_TXFNUM(1) | 4;
	flush_tx(1);
	out_arm(next_parity());
}

void usb_iso_close(void)
{
	iso_stop();
	bulk_off();
	configured = dac_role;              /* the configuration stands; the stream stops */
}

static void out_arm(unsigned parity)
{
	if (!iso_mps || (R(DOEPCTL(1)) & EPCTL_EPENA))
		return;                         /* off, or already waiting for a packet */
	out_parity = parity & 1;
	R(DOEPTSIZ(1)) = TSIZ_PKTCNT(1) | iso_mps;
	R(DOEPCTL(1)) |= (out_parity ? EPCTL_SODDFRM : EPCTL_SD0PID) | EPCTL_EPENA | EPCTL_CNAK;
}

/* A packet came in: set up for the next microframe, the other parity from
 * the one it came in. Not from the frame counter: "done" can arrive after
 * the next microframe has begun, and the counter would then point one too
 * far (every other packet was lost that way). */
static uint32_t n_late;

void usb_iso_out_arm(void)
{
	if ((((R(DSTS) >> 8) & 1)) != out_parity)
		n_late++;                       /* "done" seen after the next microframe began */
	out_arm(!out_parity);
}

/* The feedback endpoint. An isochronous IN only goes in a microframe of
 * the parity it was set for. Each is set for the microframe after the one
 * the last went in (worked out from that one's parity, not the frame
 * counter, which can already be a microframe on); one that wasn't asked
 * for in its microframe is moved on to the next, its value kept. When the
 * computer asks with nothing ready (it gets a zero-length packet: the NAK
 * event), one is set for the next microframe. */
static volatile uint32_t fb_value;
static volatile uint32_t n_fb_asked, n_fb_sent, n_fb_dropped;

static void fb_arm(unsigned parity)
{
	if (R(DIEPCTL(1)) & EPCTL_EPENA)
		return;
	fb_parity = parity & 1;
	R(DIEPTSIZ(1)) = (1u << 29) | TSIZ_PKTCNT(1) | 4;    /* one packet in its microframe */
	R(DIEPCTL(1)) |= (fb_parity ? EPCTL_SODDFRM : EPCTL_SD0PID) | EPCTL_EPENA | EPCTL_CNAK;
	R(FIFO(1)) = fb_value;
}

static void fb_asked(void)
{
	n_fb_asked++;
	fb_arm(next_parity());
}

static void fb_sent(void)
{
	n_fb_sent++;
	dac_fb_done();                      /* the latest value, through usb_iso_fb_send */
	fb_arm(!fb_parity);
}

static void fb_missed(void)
{
	if (R(DIEPCTL(1)) & EPCTL_EPENA) {
		n_fb_dropped++;                 /* counted; moved on, not dropped */
		iso_missed(DIEPCTL(1), false);
	}
}

void usb_iso_fb_send(uint32_t value)
{
	fb_value = value;                   /* goes with the next one set */
}

void usb_iso_fb_counts(uint32_t *asked, uint32_t *sent, uint32_t *dropped)
{
	*asked = n_fb_asked;
	*sent = n_fb_sent;
	*dropped = n_fb_dropped;
}

/* The controller's interrupt, through the interrupt controller: only the
 * sound card uses it */
/* how long the interrupt takes, and the longest it went without one
 * while streaming (in cycles) */
static uint32_t isr_last, isr_max, gap_max;

static void otg_isr(const void *arg)
{
	ARG_UNUSED(arg);
	if (X1000_REG(INTC_PND0) & INTC_OTG) {
		uint32_t t0 = k_cycle_get_32();

		if (dac_get_interface() && isr_last && t0 - isr_last > gap_max)
			gap_max = t0 - isr_last;
		isr_last = t0;
		n_irq++;
		usb_service();
		uint32_t d = k_cycle_get_32() - t0;

		if (d > isr_max)
			isr_max = d;
	}
}

static void irq_line(bool on)
{
	/* the CPU's line from the interrupt controller is always open (soc.c:
	 * opening it from here would leave it shut in other threads); the
	 * controller's mask for USB is the switch */
	if (on) {
		R(GAHBCFG) |= GAHBCFG_GINT;
		X1000_REG(INTC_MSK_CLR0) = INTC_OTG;
	} else {
		X1000_REG(INTC_MSK_SET0) = INTC_OTG;
		R(GAHBCFG) &= ~GAHBCFG_GINT;
	}
}

unsigned usb_bulk_mps(void)
{
	return mps;
}

bool usb_bulk_in(const void *buf, uint32_t len)
{
	uint32_t gen = generation, packets = len ? (len + mps - 1) / mps : 1, sent = 0;
	const uint8_t *p = buf;

	in_done = false;
	R(DIEPTSIZ(1)) = TSIZ_PKTCNT(packets) | len;
	R(DIEPCTL(1)) |= EPCTL_EPENA | EPCTL_CNAK;
	while (sent < len) {
		if (gen != generation || !configured)
			return false;
		uint32_t n = len - sent < mps ? len - sent : mps;

		if ((R(DTXFSTS(1)) & 0xffff) >= (n + 3) / 4) {
			fifo_write(1, p + sent, n);
			sent += n;
		} else {
			usb_service();
		}
	}
	while (!in_done) {
		if (gen != generation || !configured)
			return false;
		usb_service();
	}
	return true;
}

int usb_bulk_out(void *buf, uint32_t len)
{
	uint32_t gen = generation, packets = (len + mps - 1) / mps;

	out_buf = buf;
	out_got = 0;
	out_max = len;
	out_done = false;
	R(DOEPTSIZ(1)) = TSIZ_PKTCNT(packets) | (packets * mps);
	R(DOEPCTL(1)) |= EPCTL_EPENA | EPCTL_CNAK;
	while (!out_done) {
		if (gen != generation || !configured) {
			out_buf = NULL;
			return -1;
		}
		usb_service();
		if (!out_done)
			k_yield();
	}
	out_buf = NULL;
	return (int)out_got;
}

bool usb_bulk_stall_in(void)
{
	uint32_t gen = generation;

	in_halted = true;
	R(DIEPCTL(1)) |= EPCTL_STALL;
	/* until the host clears it (clear feature, in on_setup) */
	while (in_halted) {
		if (gen != generation || !configured)
			return false;
		usb_service();
		k_yield();
	}
	return true;
}

static void device_start(void)
{
	h2_usb_power(true);
	R(GUSBCFG) = GUSBCFG_PHYIF16 | GUSBCFG_TRDT(5);
	k_busy_wait(100);
	wait_bits_set(GRSTCTL, GRSTCTL_AHBIDL);
	R(GRSTCTL) = GRSTCTL_CSRST;
	wait_bits_clear(GRSTCTL, GRSTCTL_CSRST);
	wait_bits_set(GRSTCTL, GRSTCTL_AHBIDL);
	R(GUSBCFG) = GUSBCFG_PHYIF16 | GUSBCFG_TRDT(5);

	R(GRXFSIZ) = RX_WORDS;
	R(DIEPTXF0) = (TX0_WORDS << 16) | RX_WORDS;
	R(DIEPTXF1) = (TX1_WORDS << 16) | (RX_WORDS + TX0_WORDS);
	flush_tx(0x10);
	flush_rx();
	k_busy_wait(1);

	R(GAHBCFG) = 0;                     /* slave mode, no interrupt line: polled */
	R(DCFG) = DCFG_NZLSOHSK;            /* high speed, address 0 */
	R(DIEPMSK) = EPINT_XFRC | (dac_role ? EPINT_NAK | EPINT_EPDISD : 0);
	R(DOEPMSK) = EPINT_XFRC | EPINT_STUP;
	R(DAINTMSK) = 0xffffffffu;
	/* no interrupt line (GAHBCFG), but the events still show in GINTSTS */
	R(GINTMSK) = GINT_RXFLVL | GINT_USBSUSP | GINT_USBRST | GINT_ENUMDNE | GINT_IEPINT |
		     GINT_OEPINT | (dac_role ? GINT_IISOIXFR | GINT_INCOMPISO : 0);
	R(GINTSTS) = 0xffffffffu;
	ep0_arm();

	R(DCTL) = DCTL_SDIS;
	k_busy_wait(3000);
	R(DCTL) &= ~DCTL_SDIS;              /* on the bus */
}

static void device_stop(void)
{
	bulk_off();
	h2_usb_power(false);
}

/* ---- the USB thread ---- */

#define USB_PRIORITY 6                  /* below the UI (5): it polls */
#define DAC_PRIORITY 2                  /* as the audio thread: the DAC mustn't run dry */

static void device_start(void);

/* Off the bus, and on again as the other device: the computer sees the
 * drive go and the sound card come (or back) */
static void switch_role(bool dac)
{
	if (dac_role)
		irq_line(false);
	R(DCTL) |= DCTL_SDIS;
	bulk_off();
	if (dac_role && !dac) {
		dac_end();
		dlog_save();
	}
	if (dac) {
		dlog_n = 0;
		isr_max = gap_max = 0;
	}
	k_msleep(50);
	dac_role = dac;
	configured = false;
	state = LOS_USB_CABLE;
	if (dac) {
		los_volume_trim(0);             /* the computer's audio has no EQ to make up for */
		k_thread_priority_set(k_current_get(), DAC_PRIORITY);
	} else {
		k_thread_priority_set(k_current_get(), USB_PRIORITY);
	}
	device_start();
	if (dac)
		irq_line(true);
}

K_THREAD_STACK_DEFINE(usb_stack, 4096);
static struct k_thread usb_thread_data;

static void usb_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	x1000_pin_in(X1000_PD, CABLE_PIN);
	for (;;) {
		/* a cable, steadily for 200 ms */
		int high = 0;

		while (high < 4) {
			high = h2_usb_cable() ? high + 1 : 0;
			k_msleep(50);
		}
		cable_gone = false;
		cable_low_ms = 0;
		state = LOS_USB_CABLE;
		dac_role = dac_wanted = false;
		device_start();
		while (!cable_gone) {
			if (dac_wanted != dac_role) {
				switch_role(dac_wanted);
				continue;
			}
			if (dac_role) {
				cable_watch();
				dac_pump();
				k_msleep(1);
				continue;
			}
			usb_service();
			if (configured)
				msc_run();              /* one command, or returns at once */
			else
				k_msleep(1);
		}
		if (dac_role) {
			irq_line(false);
			dac_end();
			dlog_save();
			k_thread_priority_set(k_current_get(), USB_PRIORITY);
		}
		dac_role = dac_wanted = false;
		device_stop();
		msc_share(false, false);
		state = LOS_USB_NONE;
	}
}

void h2_usb_start(void)
{
	IRQ_CONNECT(X1000_IRQ_INTC, 0, otg_isr, NULL, 0);
	k_thread_create(&usb_thread_data, usb_stack, K_THREAD_STACK_SIZEOF(usb_stack), usb_thread,
			NULL, NULL, NULL, USB_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&usb_thread_data, "usb");
}

/* ---- for the app (los_sys.h) ---- */

enum los_usb los_usb_state(void)
{
	enum los_usb s = state;

	if (dac_role || dac_wanted)
		return LOS_USB_DAC;
	if (s == LOS_USB_COMPUTER && msc_shared())
		s = msc_ejected() ? LOS_USB_EJECTED : LOS_USB_SHARED;
	return s;
}

bool los_usb_dac(bool on)
{
	if (on && state != LOS_USB_COMPUTER && !dac_role)
		return false;
	dac_wanted = on;
	generation++;                       /* a drive command waiting gives up */
	return true;
}

/* The sound card's counters for this time, for the log */
static int dac_counts(char *buf, unsigned n)
{
	int k = dac_debug(buf, n);
	uint32_t us = sys_clock_hw_cycles_per_sec() / 1000000;

	if (k > 0 && (unsigned)k < n)
		k += snprintf(buf + k, n - k,
			      "\nmissed microframes: out %u in %u; DAC ran dry %u\n"
			      "feedback: asked with nothing ready %u, sent %u; packets seen late %u\n"
			      "interrupt: longest %u us, longest gap %u us\n",
			      n_incomp_out, n_incomp_in, h2_audio_underruns, n_fb_asked, n_fb_sent,
			      n_late, isr_max / us, gap_max / us);
	return k;
}

bool los_usb_dac_info(unsigned *rate, unsigned *bits)
{
	return dac_role && dac_status(rate, bits);
}

bool los_usb_share(bool on)
{
	if (on) {
		/* written to only if the card passed the write check at start */
		bool can_write = los_writes_ok();

		if (state != LOS_USB_COMPUTER || !h2_card_mount(false))
			return false;
		msc_share(true, can_write);
		return true;
	}
	msc_share(false, false);
	return h2_card_mount(true);
}

/* ---- the probe ---- */

void h2_usb_probe(void)
{
	static char text[600];
	static const char path[] = "/SD:/.lossless/logs/usb.txt";

	void *f = los_file_open(path);
	if (f) {
		los_file_close(f);       /* done on an earlier start */
		return;
	}
	x1000_pin_in(X1000_PD, CABLE_PIN);
	uint32_t before[4] = { X1000_REG(CPM_CLKGR), X1000_REG(CPM_OPCR),
			       X1000_REG(CPM_USBCDR), X1000_REG(CPM_USBPCR) };
	h2_usb_power(true);
	uint32_t id = R(GSNPSID);
	uint32_t hw1 = R(GHWCFG1), hw2 = R(GHWCFG2), hw3 = R(GHWCFG3), hw4 = R(GHWCFG4);
	uint32_t usbcfg = R(GUSBCFG), rst = R(GRSTCTL), fifo = R(GDFIFOCFG);
	h2_usb_power(false);

	int n = snprintf(text, sizeof(text),
			 "USB probe\n"
			 "cable PD3: %d\n"
			 "before: CLKGR %08x OPCR %08x USBCDR %08x USBPCR %08x\n"
			 "GSNPSID %08x\n"
			 "GHWCFG1 %08x\nGHWCFG2 %08x\nGHWCFG3 %08x\nGHWCFG4 %08x\n"
			 "GDFIFOCFG %08x\nGUSBCFG %08x\nGRSTCTL %08x\n",
			 h2_usb_cable(), before[0], before[1], before[2], before[3],
			 id, hw1, hw2, hw3, hw4, fifo, usbcfg, rst);
	los_mkdir("/SD:/.lossless");
	los_mkdir("/SD:/.lossless/logs");
	los_file_save(path, text, (size_t)n);
}
