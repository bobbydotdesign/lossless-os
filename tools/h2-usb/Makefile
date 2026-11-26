# h2-usb: the H2's USB boot mode (see h2-usb.c). libusb-1.0 is LGPL-2.1,
# so it's linked dynamically.
# SPDX-License-Identifier: Apache-2.0

PKG_CONFIG ?= pkg-config
CFLAGS += -std=c11 -O2 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs libusb-1.0)

h2-usb: h2-usb.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f h2-usb

.PHONY: clean
