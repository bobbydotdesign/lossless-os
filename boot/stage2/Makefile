# Our second stage: the card, /.lossless/boot/lossless.bin, Lossless OS.
#   make -C boot/stage2      build stage2.bin (at 0x81e00000; start it at
#                            0xa1e00000)
# FatFs (BSD-style licence, ChaN) is built from the Zephyr workspace.
# SPDX-License-Identifier: Apache-2.0

SDK ?= $(HOME)/zephyr-sdk-1.0.1
CROSS ?= $(SDK)/gnu/mips-zephyr-elf/bin/mips-zephyr-elf-
FATFS ?= ../../../zephyr-ws/modules/fs/fatfs

CFLAGS = -EL -march=mips32 -Os -std=gnu11 -Wall -ffreestanding -fno-pic -mno-abicalls -G0 \
	 -fno-builtin -nostdlib -I. -I../common -I../stage1/shim -I../../platform/h2 -I../../app -I$(FATFS)/include -DZEPHYR_CONFIG_OVERRIDE=fatfs_conf.h
SRCS = start.S stage2.c sd.c diskio.c ../common/nand.c ../common/inflate.c ../../platform/h2/x1000_i2c.c \
	../../platform/h2/h2_lcd.c ../../platform/h2/draw.c $(FATFS)/ff.c $(FATFS)/option/ffunicode.c

stage2.bin: $(SRCS) stage2.h fatfs_conf.h link.ld ../common/nand.h ../common/inflate.h
	$(CROSS)gcc $(CFLAGS) -T link.ld -o stage2.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data stage2.elf $@
	@$(CROSS)size stage2.elf
	@ls -l $@

clean:
	rm -f stage2.elf stage2.bin

# Copies with a mode patched in, for tests over USB boot: 1 the flash test,
# 2 the original firmware, 3 its recovery
MODE = python3 -c "import sys;d=bytearray(open('stage2.bin','rb').read());i=d.find(b'MODE');d[i+4:i+8]=int(sys.argv[1]).to_bytes(4,'little');open(sys.argv[2],'wb').write(d)"
nandtest.bin: stage2.bin
	$(MODE) 1 $@
player.bin: stage2.bin
	$(MODE) 2 $@
recovery.bin: stage2.bin
	$(MODE) 3 $@
# The original firmware with " panic=3" on its command line, in a separate build
panic.bin: $(SRCS) stage2.h fatfs_conf.h link.ld ../common/nand.h ../common/inflate.h
	$(CROSS)gcc $(CFLAGS) '-DPLAYER_ARGS_EXTRA=" panic=3"' -T link.ld -o panic.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data panic.elf panic-s2.bin
	python3 -c "import sys;d=bytearray(open('panic-s2.bin','rb').read());i=d.find(b'MODE');d[i+4:i+8]=(2).to_bytes(4,'little');open('panic.bin','wb').write(d)"

# ... and with its console on the screen too, every message
screen.bin: $(SRCS) stage2.h fatfs_conf.h link.ld ../common/nand.h ../common/inflate.h
	$(CROSS)gcc $(CFLAGS) '-DPLAYER_ARGS_EXTRA=" console=tty0 loglevel=8 panic=5"' -T link.ld -o screen.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data screen.elf screen-s2.bin
	python3 -c "import sys;d=bytearray(open('screen-s2.bin','rb').read());i=d.find(b'MODE');d[i+4:i+8]=(2).to_bytes(4,'little');open('screen.bin','wb').write(d)"

# ... and catching an early exception after the jump (breadcrumb 0xee)
trap.bin: $(SRCS) stage2.h fatfs_conf.h link.ld ../common/nand.h ../common/inflate.h
	$(CROSS)gcc $(CFLAGS) -DTRAP_TEST -T link.ld -o trap.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data trap.elf trap-s2.bin
	python3 -c "import sys;d=bytearray(open('trap-s2.bin','rb').read());i=d.find(b'MODE');d[i+4:i+8]=(2).to_bytes(4,'little');open('trap.bin','wb').write(d)"

# The bootloader writer (MODE_FLASH): tools/h2-flash-boot.sh loads the
# image, then starts this
flash.bin: stage2.bin
	$(MODE) 7 $@

# ... and told to switch off as its first program (does the kernel reach
# its file system on flash?)
offtest.bin: $(SRCS) stage2.h fatfs_conf.h link.ld ../common/nand.h ../common/inflate.h
	$(CROSS)gcc $(CFLAGS) '-DPLAYER_ARGS_EXTRA=" panic=0 init=/sbin/poweroff -f"' -T link.ld -o offtest.elf $(SRCS) -lgcc
	$(CROSS)objcopy -O binary -j .text -j .rodata -j .data offtest.elf offtest-s2.bin
	python3 -c "import sys;d=bytearray(open('offtest-s2.bin','rb').read());i=d.find(b'MODE');d[i+4:i+8]=(2).to_bytes(4,'little');open('offtest.bin','wb').write(d)"

dry.bin: stage2.bin
	$(MODE) 4 $@
return.bin: stage2.bin
	$(MODE) 5 $@

.PHONY: clean
