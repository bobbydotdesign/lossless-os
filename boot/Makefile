# Our bootloader, as the update file the original firmware's recovery
# installs (copy it to the card as update.upt, switch on holding Vol+):
#
#   make -C boot              out/uboot.bin (the flash image) and
#                             out/update.upt
#   make -C boot rescue       out/rescue: the rescue kit's programs
#
# Needs the Zephyr SDK's MIPS toolchain and the FatFs module (as for
# Lossless OS itself, docs/building.md), and xorriso (tools/mkupdate.py).
# SPDX-License-Identifier: Apache-2.0

SDK ?= $(HOME)/zephyr-sdk-1.0.1
OUT = ../out

all: $(OUT)/update.upt

$(OUT)/update.upt: $(OUT)/uboot.bin ../tools/mkupdate.py
	python3 ../tools/mkupdate.py $(OUT)/uboot.bin $@

$(OUT)/uboot.bin: FORCE ../tools/mkbootimage.py
	$(MAKE) -C stage1 SDK=$(SDK) stage1-flash.bin
	$(MAKE) -C stage2 SDK=$(SDK) stage2.bin
	@mkdir -p $(OUT)
	python3 ../tools/mkbootimage.py stage1/stage1-flash.bin stage2/stage2.bin $@

# The rescue kit's two programs, which run in the player's memory over USB
# boot and never stay on it (tools/h2-rescue.sh): the first stage (main
# memory) and the writer
rescue: FORCE
	$(MAKE) -C stage1 SDK=$(SDK) stage1.bin
	$(MAKE) -C stage2 SDK=$(SDK) flash.bin
	@mkdir -p $(OUT)/rescue
	cp stage1/stage1.bin $(OUT)/rescue/stage1.bin
	cp stage2/flash.bin $(OUT)/rescue/writer.bin

FORCE:
.PHONY: all rescue FORCE
