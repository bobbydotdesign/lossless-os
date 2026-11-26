/*
 * h2-usb: talks to the H2's processor in its USB boot mode, to run code
 * from memory without touching the player's flash.
 *
 * The X1000's boot ROM enters USB boot mode when the Menu button is held
 * while the player is connected (Menu is the processor's boot select pin
 * PB28). The computer then sees USB device a108:1000, and the ROM answers
 * six vendor requests on endpoint 0 and moves data on bulk endpoint 1.
 * Source: Ingenic, X1000 Programming Manual (2016-01-13), 27.5 "USB Boot
 * Specification", Tables 27-3 to 27-9.
 *
 *   h2-usb info                    the processor's identity (8 bytes)
 *   h2-usb run1 FILE ADDR [ENTRY]  load FILE at ADDR in on-chip memory and
 *                                  start it (it may return to the ROM)
 *   h2-usb run2 FILE ADDR [ENTRY]  load FILE at ADDR in main memory and
 *                                  start it (use the uncached window,
 *                                  0xa0000000 up: the ROM's cache flush
 *                                  hangs on the H2)
 *   h2-usb load FILE ADDR          load FILE at ADDR without starting it
 *   h2-usb read ADDR LEN OUT       copy LEN bytes at ADDR to the file OUT
 *
 * Addresses take 0x... hex. ENTRY defaults to ADDR. The tool waits up to
 * 30 s for the player to appear.
 *
 * Build: make -C tools/h2-usb (needs libusb-1.0, linked dynamically).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VID 0xa108
#define PID 0x1000
#define EP_OUT 0x01
#define EP_IN 0x81

enum {
	VR_GET_CPU_INFO = 0x00,
	VR_SET_DATA_ADDRESS = 0x01,
	VR_SET_DATA_LENGTH = 0x02,
	VR_FLUSH_CACHES = 0x03,
	VR_PROGRAM_START1 = 0x04,
	VR_PROGRAM_START2 = 0x05,
};

/* Vendor request, device as recipient. The manual's tables give 0x40 for
 * every request; GET_CPU_INFO returns data, so it goes device-to-host. */
#define RT_OUT (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define RT_IN (LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)

#define TIMEOUT_MS 5000

static libusb_device_handle *dev;

static void die(const char *what, int err)
{
	fprintf(stderr, "h2-usb: %s: %s\n", what, err ? libusb_strerror(err) : "failed");
	exit(1);
}

static void open_player(void)
{
	int err = libusb_init(NULL);
	if (err)
		die("libusb", err);
	fprintf(stderr, "Waiting for the player in USB boot mode (hold Menu, then plug in)...\n");
	for (int i = 0; i < 300 && !dev; i++) {
		dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
		if (!dev)
			usleep(100 * 1000);
	}
	if (!dev) {
		fprintf(stderr, "h2-usb: no device %04x:%04x after 30 s\n", VID, PID);
		exit(1);
	}
	err = libusb_claim_interface(dev, 0);
	if (err)
		die("claim interface", err);
	fprintf(stderr, "Found %04x:%04x\n", VID, PID);
}

/* A request carrying a 32-bit value: wValue = bits 31:16, wIndex = 15:0. */
static void request(uint8_t req, uint32_t value)
{
	int n = libusb_control_transfer(dev, RT_OUT, req, value >> 16, value & 0xffff, NULL, 0,
					TIMEOUT_MS);
	if (n < 0) {
		char what[48];
		snprintf(what, sizeof(what), "request 0x%02x (0x%08x)", req, value);
		die(what, n);
	}
}

static void cpu_info(void)
{
	unsigned char buf[8] = {0};
	int n = libusb_control_transfer(dev, RT_IN, VR_GET_CPU_INFO, 0, 0, buf, sizeof(buf),
					TIMEOUT_MS);
	if (n < 0)
		die("cpu info", n);
	printf("CPU info: \"");
	for (int i = 0; i < n; i++)
		putchar(buf[i] >= 0x20 && buf[i] < 0x7f ? buf[i] : '.');
	printf("\" (");
	for (int i = 0; i < n; i++)
		printf("%s%02x", i ? " " : "", buf[i]);
	printf(")\n");
}

static void bulk(unsigned char ep, unsigned char *data, int len)
{
	int done = 0;
	while (done < len) {
		int n = 0;
		int err = libusb_bulk_transfer(dev, ep, data + done, len - done, &n, TIMEOUT_MS);
		if (err)
			die(ep == EP_OUT ? "send" : "receive", err);
		done += n;
	}
}

static unsigned char *load_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "h2-usb: %s: %s\n", path, strerror(errno));
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	rewind(f);
	unsigned char *buf = malloc(*len ? *len : 1);
	if (!buf || fread(buf, 1, *len, f) != (size_t)*len) {
		fprintf(stderr, "h2-usb: can't read %s\n", path);
		exit(1);
	}
	fclose(f);
	return buf;
}

static uint32_t number(const char *s)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);
	if (*s == 0 || *end != 0 || v > 0xffffffffUL) {
		fprintf(stderr, "h2-usb: not a number: %s\n", s);
		exit(1);
	}
	return (uint32_t)v;
}

static void send_to(uint32_t addr, unsigned char *data, uint32_t len)
{
	request(VR_SET_DATA_ADDRESS, addr);
	request(VR_SET_DATA_LENGTH, len);
	bulk(EP_OUT, data, len);
}

static void usage(void)
{
	fprintf(stderr, "usage: h2-usb info\n"
			"       h2-usb run1 FILE ADDR [ENTRY]\n"
			"       h2-usb run2 FILE ADDR [ENTRY]\n"
			"       h2-usb load FILE ADDR\n"
			"       h2-usb read ADDR LEN OUT\n");
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	const char *cmd = argv[1];

	if (!strcmp(cmd, "info") && argc == 2) {
		open_player();
		cpu_info();
	} else if ((!strcmp(cmd, "run1") || !strcmp(cmd, "run2")) && (argc == 4 || argc == 5)) {
		long len;
		unsigned char *data = load_file(argv[2], &len);
		uint32_t addr = number(argv[3]);
		uint32_t entry = argc == 5 ? number(argv[4]) : addr;
		bool first = cmd[3] == '1';
		if (first && len > 10 * 1024) {
			fprintf(stderr, "h2-usb: %s is %ld bytes; a first stage fits 10 KB\n",
				argv[2], len);
			return 1;
		}
		open_player();
		cpu_info();
		send_to(addr, data, (uint32_t)len);
		fprintf(stderr, "Sent %ld bytes to 0x%08x\n", len, addr);
		/* Over the uncached window (0xa0000000 up) the data is already in
		 * memory; the ROM's flush hung on the H2, so it's sent only for
		 * cached addresses */
		if (!first && addr < 0xa0000000u)
			request(VR_FLUSH_CACHES, 0);
		request(first ? VR_PROGRAM_START1 : VR_PROGRAM_START2, entry);
		fprintf(stderr, "Started at 0x%08x\n", entry);
		if (first) {
			/* A first stage that hands back leaves the ROM answering again. */
			fprintf(stderr, "Checking whether it handed back (up to 2 minutes)...\n");
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			unsigned char buf[8];
			int n = libusb_control_transfer(dev, RT_IN, VR_GET_CPU_INFO, 0, 0, buf,
							sizeof(buf), 120000);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
			if (n >= 0)
				fprintf(stderr, "Handed back after %.1f s: the ROM answers again\n", s);
			else
				fprintf(stderr, "No answer from the ROM after %.0f s (still running, or stopped)\n", s);
		}
		free(data);
	} else if (!strcmp(cmd, "load") && argc == 4) {
		long len;
		unsigned char *data = load_file(argv[2], &len);
		uint32_t addr = number(argv[3]);
		open_player();
		send_to(addr, data, (uint32_t)len);
		fprintf(stderr, "Sent %ld bytes to 0x%08x\n", len, addr);
		free(data);
	} else if (!strcmp(cmd, "read") && argc == 5) {
		uint32_t addr = number(argv[2]);
		uint32_t len = number(argv[3]);
		unsigned char *buf = malloc(len ? len : 1);
		open_player();
		request(VR_SET_DATA_ADDRESS, addr);
		request(VR_SET_DATA_LENGTH, len);
		bulk(EP_IN, buf, (int)len);
		FILE *f = fopen(argv[4], "wb");
		if (!f || fwrite(buf, 1, len, f) != len) {
			fprintf(stderr, "h2-usb: can't write %s\n", argv[4]);
			return 1;
		}
		fclose(f);
		fprintf(stderr, "Read %u bytes from 0x%08x into %s\n", len, addr, argv[4]);
		free(buf);
	} else {
		usage();
	}

	libusb_release_interface(dev, 0);
	libusb_close(dev);
	libusb_exit(NULL);
	return 0;
}
