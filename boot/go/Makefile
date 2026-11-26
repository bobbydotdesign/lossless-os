# The last step before Lossless OS over USB boot: caches on, then jump.
#   make -C boot/go          build go.bin (runs at 0xa0f00000, uncached)
# SPDX-License-Identifier: Apache-2.0

SDK ?= $(HOME)/zephyr-sdk-1.0.1
CROSS ?= $(SDK)/gnu/mips-zephyr-elf/bin/mips-zephyr-elf-
GO_ENTRY ?= 0x81001000

go.bin: go.S
	$(CROSS)gcc -EL -march=mips32 -mno-abicalls -fno-pic -nostdlib -DGO_ENTRY=$(GO_ENTRY) \
		-Wl,-Ttext=0xa0f00000 -Wl,--no-warn-rwx-segments -T go.ld -o go.elf $<
	$(CROSS)objcopy -O binary -j .text go.elf $@
	$(CROSS)objdump -d go.elf | tail -n +7

clean:
	rm -f go.elf go.bin
