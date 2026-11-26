# Tests of our own boot code, run from memory over USB boot (no flash).
#   make -C boot/test            build blink.bin
#   make -C boot/test run-blink  run it on the player (hold Menu, plug in)
# SPDX-License-Identifier: Apache-2.0

SDK ?= $(HOME)/zephyr-sdk-1.0.1
CROSS ?= $(SDK)/gnu/mips-zephyr-elf/bin/mips-zephyr-elf-
H2USB ?= ../../tools/h2-usb/h2-usb

# On-chip memory: the ROM runs a first stage from 0xf4001800 (manual 27.3).
TCSM_ENTRY = 0xf4001800

AFLAGS = -EL -march=mips32 -mno-abicalls -fno-pic -nostdlib

blink.bin: blink.S
	$(CROSS)gcc $(AFLAGS) -T tcsm.ld -o blink.elf $<
	$(CROSS)objcopy -O binary -j .text blink.elf $@
	$(CROSS)objdump -d blink.elf | tail -n +7

run-blink: blink.bin $(H2USB)
	$(H2USB) run1 blink.bin $(TCSM_ENTRY)

$(H2USB):
	$(MAKE) -C ../../tools/h2-usb

clean:
	rm -f *.elf *.bin

.PHONY: run-blink clean
