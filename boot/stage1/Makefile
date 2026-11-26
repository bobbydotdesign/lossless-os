# Our first stage: clocks and memory, from the X1000's on-chip memory.
#   make -C boot/stage1           build stage1.bin
#   make -C boot/stage1 run       run it over USB boot (hold Menu, plug in)
#                                 and show the status block and clocks
# SPDX-License-Identifier: Apache-2.0

SDK ?= $(HOME)/zephyr-sdk-1.0.1
CROSS ?= $(SDK)/gnu/mips-zephyr-elf/bin/mips-zephyr-elf-
H2USB ?= ../../tools/h2-usb/h2-usb
ENTRY = 0xf4001800

CFLAGS = -EL -march=mips32 -Os -std=c11 -Wall -Wextra -ffreestanding \
	 -fno-pic -mno-abicalls -G0 -fno-builtin -nostdlib
SRCS = start.S stage1.c clocks.c memory.c
POWER = power.c ../../platform/h2/x1000_i2c.c
PFLAGS = -DPOWER_ON -Ishim -I../../platform/h2

# Padded to the status block (0xf4002e00), so the stack is inside what the
# ROM loaded.
PADTO = dd if=/dev/null of=$@ bs=1 seek=5632 2>/dev/null

stage1.bin: $(SRCS) $(POWER) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) $(PFLAGS) -T tcsm.ld -o stage1.elf $(SRCS) $(POWER) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data stage1.elf $@
	@$(PADTO)
	@$(CROSS)size stage1.elf
	@ls -l $@

stop%.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DSTOP_AT=$* -T tcsm.ld -o stop$*.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data stop$*.elf $@
	@$(PADTO)

set%.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DSTOP_AT=4 -DMEMORY_SET=$* -T tcsm.ld -o set$*.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data set$*.elf $@
	@$(PADTO)

pmu.bin: $(SRCS) stage1.h tcsm.ld ../../platform/h2/x1000_i2c.c
	$(CROSS)gcc $(CFLAGS) -DPMU_READ -Ishim -I../../platform/h2 -T tcsm.ld -o pmu.elf \
		start.S stage1.c clocks.c ../../platform/h2/x1000_i2c.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data pmu.elf $@
	@$(PADTO)
	@$(CROSS)size pmu.elf

# The power supplies on, then hand back
pwr.bin: $(SRCS) power.c stage1.h tcsm.ld ../../platform/h2/x1000_i2c.c
	$(CROSS)gcc $(CFLAGS) -DPOWER_ON -DPOWER_ONLY -Ishim -I../../platform/h2 -T tcsm.ld \
		-o pwr.elf $(SRCS) power.c ../../platform/h2/x1000_i2c.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data pwr.elf $@
	@$(PADTO)

# Power-chip writes from the computer (tools: patch "PWRS" in the image),
# then hand back
pset.bin: $(SRCS) power.c stage1.h tcsm.ld ../../platform/h2/x1000_i2c.c
	$(CROSS)gcc $(CFLAGS) -DPOWER_ON -DPOWER_ONLY -DPOWER_PARAMS -Ishim -I../../platform/h2 -T tcsm.ld \
		-o pset.elf $(SRCS) power.c ../../platform/h2/x1000_i2c.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data pset.elf $@
	@$(PADTO)

# Switch the player off through the power chip (reg 0x10 bit 0, as
# Lossless OS does), so the next test starts from a true power-up
off.bin: pset.bin pset.py
	python3 pset.py $@ 10=01:01

# Diagnostics with the power supplies on: power-diag7 = ours, -diag1 = measured
power-diag%.bin: $(SRCS) power.c stage1.h tcsm.ld ../../platform/h2/x1000_i2c.c
	$(CROSS)gcc $(CFLAGS) -DDIAG -DPOWER_ON -DMEMORY_SET=$* -Ishim -I../../platform/h2 -T tcsm.ld \
		-o power-diag$*.elf $(SRCS) power.c ../../platform/h2/x1000_i2c.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data power-diag$*.elf $@
	@$(PADTO)
	@$(CROSS)size power-diag$*.elf

# The first stage without its power step (supplies brought up beforehand)
stage1np.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -T tcsm.ld -o stage1np.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data stage1np.elf $@
	@$(PADTO)

nandh.bin: $(SRCS) stage1.h tcsm.ld ../common/nand.c ../common/nand.h
	$(CROSS)gcc $(CFLAGS) -DNAND_TEST -DNAND_HEADERS -I../common -T tcsm.ld -o nandh.elf $(SRCS) ../common/nand.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data nandh.elf $@
	@$(PADTO)

nandt.bin: $(SRCS) stage1.h tcsm.ld ../common/nand.c ../common/nand.h
	$(CROSS)gcc $(CFLAGS) -DNAND_TEST -I../common -T tcsm.ld -o nandt.elf $(SRCS) ../common/nand.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data nandt.elf $@
	@$(PADTO)
	@$(CROSS)size nandt.elf

delay.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DDELAYTEST -T tcsm.ld -o delay.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data delay.elf $@
	@$(PADTO)

# The first stage for flash: everything, then the second stage from flash.
# Linked for the ROM's flash boot (tcsm-flash.ld); tools/mkbootimage.py
# puts it after the boot ROM header.
stage1-flash.bin: $(SRCS) $(POWER) stage1.h tcsm-flash.ld ../common/nand.c ../common/nand.h
	$(CROSS)gcc $(CFLAGS) $(PFLAGS) -DFLASH_BOOT -I../common -T tcsm-flash.ld -o stage1-flash.elf \
		$(SRCS) $(POWER) ../common/nand.c -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data stage1-flash.elf $@
	@$(CROSS)size stage1-flash.elf
	@ls -l $@

regs.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DREGS -T tcsm.ld -o regs.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data regs.elf $@
	@$(PADTO)

regs2.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DREGS2 -T tcsm.ld -o regs2.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data regs2.elf $@
	@$(PADTO)

touch.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DTOUCH -T tcsm.ld -o touch.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data touch.elf $@
	@$(PADTO)

tune.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DTUNE -DMEMORY_SET=7 -T tcsm.ld -o tune.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data tune.elf $@
	@$(PADTO)

diag%.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DDIAG -DMEMORY_SET=$* -T tcsm.ld -o diag$*.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data diag$*.elf $@
	@$(PADTO)

fill%.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DFILL_MB=$* -T tcsm.ld -o fill$*.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data fill$*.elf $@
	@$(PADTO)

frame.bin: $(SRCS) stage1.h tcsm.ld
	$(CROSS)gcc $(CFLAGS) -DFRAME_ONLY -T tcsm.ld -o frame.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data frame.elf $@
	@$(PADTO)

run: stage1.bin $(H2USB)
	$(H2USB) run1 stage1.bin $(ENTRY)
	$(H2USB) read 0xf4002e00 256 status.bin
	@xxd -e status.bin | head -8
	$(H2USB) read 0xb0000000 0x100 cpm.bin
	@xxd -e cpm.bin | head -3

$(H2USB):
	$(MAKE) -C ../../tools/h2-usb

clean:
	rm -f *.elf *.bin

.PHONY: run clean run-frame

run-frame: frame.bin $(H2USB)
	$(H2USB) run1 frame.bin $(ENTRY)
	$(H2USB) read 0xf4002e00 64 status.bin
	@xxd -e status.bin | head -4
