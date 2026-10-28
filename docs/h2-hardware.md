# HiFi Walker H2: hardware facts

Facts our drivers rely on, each with where it came from. Code in this repo is
written from these notes and the X1000 programming manual, never copied from
GPL sources (see CONTRIBUTING.md, "Clean room").

Sources: **FW** = existing open-source firmware for this player (read for
facts only). **PM** = Ingenic X1000 programming manual. **H2** = seen on
our own player.

## Chip and memory

| Fact | Value | Source |
|---|---|---|
| SoC | Ingenic X1000, XBurst CPU, MIPS32 release 2, little-endian | FW |
| RAM | 32 MB DDR at physical 0, cached at `0x80000000`, uncached at `0xa0000000` | FW |
| Peripheral registers | Physical `0x10000000`+, used uncached at `0xb0000000`+ | FW, PM |
| Main crystal | 24 MHz | FW |
| CPU identity (PRId) | `0x2ed1024f` | H2 (test 2) |
| Caches | 16 KB instruction + 16 KB data (Config1) | H2 (test 2) |
| Clocks under the bootloader | CPU 1008 MHz, memory 200 MHz (from the CPM registers) | H2 (test 2) |
| Free memory above our image | 15.8 MB, write/read test passed | H2 (test 2) |

## Starting from the bootloader

| Fact | Value | Source |
|---|---|---|
| Recovery menu | Hold Vol+ at power-on; wheel moves, Play selects, Power quits | FW |
| "Linux" item | Loads `/uImage` from the card, needs `/linux_cmdline.txt` to exist | FW |
| Image format | Legacy U-Boot uImage, uncompressed, header and data CRC32 checked | FW |
| Jump | Copies the data to the header's load address, flushes caches, calls the entry point with interrupts off | FW |
| Bootloader's own memory | First 16 KB of RAM (exception vectors and its copy routine), then its code, data and heap from `0x80004000` | FW |
| Screen at jump time | Left running: the LCD controller keeps sending the bootloader's framebuffer to the panel | FW |

Confirmed on our H2 by test 1 (2026-10-27): the "Linux" item accepts our
uImage, Zephyr starts at `0x81000000`, the backlight pin responds, and the
bootloader's frame is found by following `LCD_DA` and repainted after
setting `DMA_START`.

### The boot file

At power-on (Vol+ not held) the bootloader starts a boot file from the
card: the `.erosq` file in its hidden folder. Lossless OS becomes that
file (`tools/mkbootfile.py`), without touching the flash; the installer
keeps the file that was there beside it, as `<name>.original`, and
`tools/h2-restore.sh` puts it back.

| Fact | Value | Source |
|---|---|---|
| Layout | 4-byte checksum, big-endian: 116 (the player's model number) plus the sum of every byte of the image; 4 bytes `eros`; then the image | FW; checked against the file on the card |
| Header | In the image's first 128 bytes: `BEGINHDR`, then 4-byte labels each followed by a 4-byte little-endian value, then `ENDH` | FW |
| Load | The image is copied to the address in its `LOAD` entry and run from there | FW |

Ours puts a jump to Zephyr's entry and that header in 256 bytes in front of
Zephyr, loaded 256 bytes below it, so Zephyr lands where it was linked.

Lossless OS loads at `0x81000000` for now, above everything the bootloader
uses, so its screen buffer stays intact. Zephyr does replace the general
exception entry at `0x80000180` with its own (it switches threads through an
exception); `soc/ingenic/x1000/soc.c` flushes that copy out of the caches.
Test 1 reached `main()`, which needs that exception, so the CPU's exception
base is `0x80000000` with BEV off when we start.

The bootloader's start code also turns on the CPU's separate interrupt vector
(`Cause.IV`, interrupts at `0x80000200`, where its own handler stays in
memory). Zephyr's start-up writes 0 to Cause, which turns that off, so all
interrupts come through Zephyr's entry at `0x180`. `start.S` saves Status
and Cause as handed over, before that.

Seen on our H2 (test 2): Status `0x10000c00` (coprocessor 0 usable, lines 2
and 3 unmasked, interrupts off, BEV off) and Cause `0x80800000` (IV on,
confirming the separate interrupt vector).

## Caches

| Fact | Value | Source |
|---|---|---|
| Line size | 32 bytes | FW |
| Flush one line | `cache 0x15` (write back and invalidate data), `cache 0x10` (invalidate instruction) | MIPS32 architecture manual |

## CPU interrupt lines

| Line | Source | Source of the fact |
|---|---|---|
| 2 | Interrupt controller (every peripheral) | FW; H2 (the bootloader hands over with lines 2 and 3 unmasked) |
| 3 | OST timer 1, wired straight to the CPU | FW |
| 7 | CP0 count/compare: **not usable**, the CP0 Count register doesn't run | H2 (test 2: "Count stopped") |

## Interrupt controller (INTC)

| Register | Address (uncached) |
|---|---|
| Mask set, bank 0 / 1 | `0xb0001008` / `0xb0001028` |
| Mask clear, bank 0 / 1 | `0xb000100c` / `0xb000102c` |
| Pending, bank 0 / 1 | `0xb0001010` / `0xb0001030` |

Source: FW. Writing all ones to mask set blocks every source.

## GPIO

Port n registers at `0xb0010000 + n * 0x100` (A=0, B=1, C=2, D=3). Each of
INT (`+0x10`), MSK (`+0x20`), PAT1 (`+0x30`), PAT0 (`+0x40`) has a set register
at `+0x4` and a clear register at `+0x8`. PIN (`+0x00`) reads the levels.

A pin is a plain output with INT=0, MSK=1, PAT1=0; PAT0 is then its level.

Source: FW.

## Backlight

| Fact | Value | Source |
|---|---|---|
| Control | PWM channel 0 on pin PC25 | FW |
| Level | High = lit (full duty), low = dark | FW |

Driven as a plain GPIO output it is either full brightness or off. As PWM
(TCU timer 0, see "PWM" under Bluetooth), the existing firmware uses a 30 us period and
sets brightness by the share of it the pin is high (its default is 70 of
255). Lossless OS keeps whatever level the bootloader left as "normal" and
dims from there (`platform/h2/h2_backlight.c`).

## Screen

| Fact | Value | Source |
|---|---|---|
| Panel | 320 x 240, RGB565, "smart" panel with its own frame memory | FW |
| Controller | X1000 LCD controller at `0xb3050000` | FW |
| `LCD_CTRL` | `+0x30`, bit 3 ENABLE | FW |
| `LCD_STATE` | `+0x34`, bit 5 EOF, bit 7 QD | FW |
| `LCD_DA` | `+0x40`, physical address of the next DMA descriptor | FW |
| `LCD_MCTRL` | `+0xa4`, bit 0 DMA_TX_EN, bit 1 DMA_START, bit 2 DMA_MODE | FW |
| `LCD_MSTATE` | `+0xa8`, bit 0 BUSY | FW |
| DMA descriptor | 8 words: next, source, frame id, command, offset size, page width, count, frame size | FW |
| Frame descriptor | Frame size word = `(width - 1) | (height - 1) << 12` | FW |

The bootloader chains two descriptors in a loop: a "write memory" command,
then the frame. Following `LCD_DA` finds the frame and its source buffer.
Each `DMA_START` sends the frame once: the existing firmware's screen update waits for
the frame in progress to end (`LCD_STATE` EOF, then `LCD_MSTATE` not busy),
copies the new picture into the buffer and starts a transfer again (FW,
`lcd_update`, `lcd_wait_frame`). On our H2, starting only when idle left
pieces of a redraw unsent (stale bands of the previous screen, test 4 run
3); waiting for the running transfer and then starting one after every
redraw fixes that.

## Timer (OST)

| Register | Address | Use |
|---|---|---|
| `OST_CTRL` | `0xb2000000` | bits 1:0 timer 1 prescale, bits 3:2 counter 2 prescale (0 = /1, 1 = /4, 2 = /16) |
| `OST_ENABLE` | `0xb2000004` | bit 0 timer 1, bit 1 counter 2; set at `+0x30` (`0xb2000034`), clear at `+0x34` |
| `OST_CLEAR` | `0xb2000008` | bit 1 zeroes counter 2 |
| `OST_1FLG` | `0xb200000c` | timer 1 reached its compare value; write 0 to acknowledge |
| `OST_1MSK` | `0xb2000010` | 1 = timer 1 interrupt masked |
| `OST_1DFR` | `0xb2000014` | timer 1 compare value; the count restarts from 0 when it reaches it |
| `OST_1CNT` | `0xb2000018` | timer 1 count |
| `OST_2CNTH` / `OST_2CNTL` | `0xb200001c` / `0xb2000020` | counter 2, 64 bits |
| `OST_2CNTHB` | `0xb2000024` | high half, latched when the low half is read |

Both run from the 24 MHz crystal. The bootloader starts counter 2 at
/4 (6 MHz) and leaves it running. The existing firmware's tick is timer 1 at /16 (1.5 MHz)
with the compare value set for the tick length, acknowledged by writing 0 to
`OST_1FLG`. Our system timer (`soc/ingenic/x1000/ost_timer.c`) does the same
at 1000 ticks a second and counts cycles on counter 2.

Confirmed on our H2 (test 2): counter 2 runs, ticks arrive at 1000 a second
(30 030 in 30 s), and a 200 ms sleep with the CPU idling took 200 ms.

Those 30 030 were a warning: about 0.1% fast against counter 2 (likely the
timer restarting one count early). Counting interrupts as ticks drifted, and
Bluetooth audio, paced by that clock, overfilled the headphones' buffer over
a few minutes (static that built up, test 7). Now the tick handler counts
ticks from counter 2 instead, and Bluetooth paces itself from counter 2
directly.

The OST's clock gate is bit 20 of `CPM_CLKGR` (`0xb0000020`, 1 = stopped).

Source: FW.

## Clocks (CPM)

| Register | Address | Fields |
|---|---|---|
| `CPM_CCR` | `0xb0000000` | bits 31:30 SCLK_A source (1 crystal, 2 APLL); bits 29:28 CPU source (1 SCLK_A, 2 MPLL); bits 3:0 CPU divider - 1 |
| `CPM_LCR` | `0xb0000004` | bits 1:0 what `wait` does: 0 IDLE (CPU only, any interrupt wakes it), 1 SLEEP |
| `CPM_APCR` | `0xb0000010` | APLL: M bits 30:24, N bits 22:18, OD bits 17:16, on bit 10 |
| `CPM_MPCR` | `0xb0000014` | MPLL: same fields, on bit 0 |
| `CPM_CLKGR` | `0xb0000020` | clock gates, 1 = stopped (OST bit 20, TCU bit 18) |
| `CPM_DDRCDR` | `0xb000002c` | bits 31:30 memory clock source (1 SCLK_A, 2 MPLL); bits 3:0 divider - 1 |

PLL output = 24 MHz x (M + 1) / (N + 1) / 2^OD. Zephyr idles the CPU with
`wait`, so `soc.c` sets `CPM_LCR` to IDLE.

Source: FW.

## Buttons and wheel

Every button pulls its pin low while pressed. Pin levels are read from
`PIN` (`+0x00`) of the port's GPIO registers. The bootloader leaves them
set up as inputs.

| Button | Pin |
|---|---|
| Play | PA16 |
| Vol + | PA17 |
| Vol - | PA19 |
| Back | PA18 (hardware revision 4; PD5 before) |
| Power | PB31 (hardware revision 4; PB7 before) |
| Menu | PB28 |
| Prev | PD4 |
| Next | PC24 |

The scroll wheel is a quadrature encoder on PB24 (A) and PB23 (B): turning
it steps the pair through 00, 01, 11, 10 one way and the reverse the other.
One step per click on our H2 (test 3; the existing firmware's note said about 2).

Source: FW. The H2 is
hardware revision 4. Confirmed on our H2 (test 3): all eight buttons read
correctly on these pins, and the wheel steps count. Turning clockwise walks
A,B through 00, 10, 11, 01.

## I2C

Three controllers at `0xb0050000 + n * 0x1000`:

| Register | Offset | Use |
|---|---|---|
| `CON` | `0x00` | bit 0 master, bits 2:1 speed (1 = 100 kHz, 2 = 400 kHz), bit 5 repeated start, bit 6 slave off |
| `TAR` | `0x04` | device address (change only while disabled) |
| `DC` | `0x10` | write: a byte, or bit 8 = read request; bit 9 stop after it, bit 10 restart before it. Read: received byte |
| `SHCNT` / `SLCNT` | `0x14` / `0x18` | 100 kHz clock high / low counts, in peripheral clocks |
| `FHCNT` / `FLCNT` | `0x1c` / `0x20` | the same for 400 kHz |
| `RINTST` | `0x34` | raw interrupt status, bit 6 transmit abort (no acknowledge) |
| `CTXABT` | `0x54` | read to clear the abort |
| `ENABLE` | `0x6c` | bit 0 on |
| `STATUS` | `0x70` | bit 2 transmit FIFO empty, bit 5 master busy |
| `RXFLR` | `0x78` | bytes in the receive FIFO |
| `SDAHD` / `SDASU` | `0x7c` / `0x94` | data hold / setup times |
| `ENBST` | `0x9c` | bit 0 really on |

The controller adds a repeated start by itself when a write turns into a
read. Clock gates: `CPM_CLKGR` bits 7, 8, 9 for I2C 0, 1, 2. Timing comes
from the peripheral clock: `CPM_CCR` bits 25:24 select its source (1 SCLK_A,
2 MPLL), bits 19:16 the divider - 1.

On the H2, bus 2 (pins PD0 and PD1, function 1) has the power chip and bus 1
the DAC (ES9018K2M, address `0x48`).

Source: FW.

## SD card controller (MSC0)

Registers at `0xb3450000` (MSC0; MSC1 at `+0x10000`):

| Register | Offset | Use |
|---|---|---|
| `CTRL` | `0x00` | bit 2 start the command, bit 3 reset the controller |
| `STAT` | `0x04` | bit 6 receive FIFO empty, bit 8 clock running, bit 15 resetting |
| `CLKRT` | `0x08` | the bus clock is the MSC clock / 2^CLKRT |
| `CMDAT` | `0x0c` | bits 2:0 response (0 none, 1 48-bit, 2 136-bit, 3 48-bit without CRC); bit 3 data; bit 4 write; bit 6 the card signals busy; bit 7 send 80 clocks first (after power-up); bits 10:9 bus (0 1-bit, 2 4-bit) |
| `BLKLEN` / `NOB` | `0x18` / `0x1c` | block length and number of blocks |
| `IMASK` / `IFLAG` | `0x24` / `0x28` | bit 0 data done, 1 card no longer busy, 2 response in, 8 read timeout, 9 response timeout, 10/11/12 CRC error on write/read/response; IFLAG write 1 to clear |
| `CMD` / `ARG` | `0x2c` / `0x30` | command index and argument |
| `RES` | `0x34` | the response, 16 bits per read, most significant first: a 48-bit response's 32 status bits are `h0 << 24 \| h1 << 8 \| (h2 & 0xff)`; a 136-bit one is 9 reads, each 32-bit word `h << 24 \| h' << 8 \| h'' >> 8`, sliding |
| `RXFIFO` | `0x38` | received data, 32 bits at a time |
| `TXFIFO` | `0x3c` | data to send, 32 bits at a time (lowest byte first); `STAT` bit 7 says it's full |
| writes | | `CMDAT` bit 4 with bit 3; `IFLAG` bit 23 "all written": the data is sent and the card has finished programming it; bit 10 the card reported a bad CRC; CMD12 after a multi-block write |
| `LPM` | `0x40` | bit 0 clock only while needed; bits 31:28 drive and sample edges (0 for default speed) |
| `DMAC` | `0x44` | bit 0 the controller's own DMA (we read the FIFO instead, for now) |
| `CTRL2` | `0x58` | bits 2:0 speed mode (0 default, 1 high speed) |

Clock: `CPM_MSC0CDR` (`0xb0000068`): bit 31 source (0 SCLK_A, 1 MPLL;
the existing firmware uses SCLK_A on this player), bit 29 change enable, bit 28 busy,
bits 7:0 divider - 1; the MSC clock is source / 2 / (divider). Clock gate:
`CPM_CLKGR` bit 4. Pins PA20-PA25 (function 1), card power PB10, card
detect PB11 (low when a card is in).

The bootloader reads `/uImage` from this card, so the controller and card
are known to work when we start.

Source: FW.

## Sound

The audio path: the X1000's audio controller (AIC) as I2S master, into an
ES9018K2M DAC, then the headphone amp (or line out, through a switch).

| Part | Facts |
|---|---|
| AIC registers | `0xb0020000`: `CFG` +0x00 (bit 0 on, 1/2 frame and bit clock out (master), 3 reset, 4 I2S, 6 repeat the last sample on underflow, TFTH 20:16); `CCR` +0x04 (bit 1 playback, 14 by DMA, OSS 21:19 (4 = 24-bit), ISS 18:16, channels 26:24 (1 = stereo), PACK16 bit 28); `I2SCR` +0x10 (bit 4 master clock out, bit 12 bit clock stopped); `SR` +0x14 (bits 13:8 samples in the transmit FIFO); `I2SDIV` +0x30; `DR` +0x34 |
| 24-bit samples | One 32-bit word per sample, the value right-aligned (LSB-justified) and sign-extended (X1000 programming manual, AICCR.OSS) |
| I2S clock | `CPM_I2SCDR` (`0xb0000060`): bit 31 source (0 SCLK_A), bit 30 from the divider, bit 29 change enable, M bits 21:13, N bits 12:0; master clock = SCLK_A x M / N (M < 512, N < 8192, N >= 2M), set to 256 x rate. `AIC_I2SDIV` = 3: bit clock = master / 4 = 64 x rate, two 32-bit slots. `CPM_I2SCDR1` (`0xb0000070`) written back to itself after. Clock gate `CPM_CLKGR` bit 11 |
| DMA | PDMA, channel registers at `0xb3420000 + n x 0x20` (SA +0, TA +4, TC +8, RT +0xc, CS +0x10, CM +0x14, DA +0x18), `DMA_CTRL` `0xb3421000`, doorbell set `0xb342100c`. 8-word descriptors; with LINK the next one is at the same 4 KB page, offset = TC bits 31:24 x 16. Request type 6 = I2S out. Clock gate bit 21 |
| Power and switches (port B) | HP amp power PB6, HP amp on PB8, DAC analogue PB9, DAC power PB12, output switch mute PB15 (1 = muted), select PB5 (0 = headphones), S/PDIF power PB21; I2S pins PB0-PB4 (function 1) |
| Pop-free start | Mute all; clocks on; analogue and amp power; a few samples through the FIFO so the DAC doesn't auto-mute; amp on, DAC power, unmute, with 10 ms between |
| DAC | ES9018K2M at `0x48` on I2C bus 1 (PC26, PC27, function 0). Reg 10 = 0 (lock over 16384 frames, for high rates), reg 12 = 0xda (DPLL bandwidth; lower drops out). Volume regs 15/16: attenuation in 0.5 dB, 255 mute; the existing firmware's range here -74 to -2 dB |
| Rates | The H2: PCM up to 384 kHz / 32-bit (HiFi Walker's H2 specifications). The ES9018K2M: I2S up to 384 kHz, 16 to 32-bit; its master clock (XI) more than 192 x the rate, or exactly 128 x the rate when synchronous; at most 50 MHz on its internal core supply (100 MHz with an external 1.3 V one), and the internal supply only up to 192 kHz (ESS ES9018K2M datasheet v3.7). The X1000's I2S: 8 to 24-bit samples (no 32-bit), no stated top rate for an external DAC (its 192 kHz is the internal codec's and S/PDIF's); PB0 function 1 is I2S_MCLK out; `I2SDIV.DV` 1 = master 128 x the rate, 3 = 256 x (Ingenic X1000 datasheet and programming manual, table 6-5). So: up to 192 kHz at 256 x (49.152 MHz at 192), above that at 128 x (49.152 MHz at 384); 32-bit sources reach the DAC as their top 24 bits |
| Headphone / line-out detect | PB14 / PB22, low when plugged (hardware revision 4) |

Source: FW. The existing firmware sends this DAC 16-bit samples when it uses
the DAC's volume; we send 24 bits (bit-perfect).

## Power

| Fact | Value | Source |
|---|---|---|
| Power chip | AXP2101 on hardware revision 4 (the H2), AXP192 before | FW |
| Address | `0x34` on I2C bus 2 | FW |
| Telling them apart | Register `0x03` is the chip ID: `0x4a` AXP2101, `0x03` AXP192 | X-Powers datasheets; H2 (test 3: `0x4a`, read over our I2C driver) |
| Switch off, AXP2101 | Set bit 0 of register `0x10` | FW; H2 (test 3: switched off) |
| Switch off, AXP192 | Set bit 7 of register `0x32` | FW |
| Battery, AXP2101 | Registers `0x34`-`0x35`: 14 bits, 1 mV each (ADC channel on in `0x30` bit 0, which the bootloader sets); percentage in `0xa4` | FW; H2 (test 3: 4.29 V, 100%) |
| Charging, AXP2101 | Register `0x01` bits 6:5: 0 full, 1 charging, 2 discharging; register `0x00` bit 5: USB power present | FW |
| Power key, AXP2101 | Register `0x27` bits 1:0, how long Power must be held to switch on (the bootloader sets 1 s) | FW |
| Forced off | Holding Power switches off in hardware; exact time to confirm | H2 (test 1 worked; time not measured) |

## Bluetooth

The chip is a CYW43438 (Bluetooth core BCM4343A1) in an AzureWave AW-NM372
module, on UART0, speaking standard HCI over H4.

| Fact | Value | Source |
|---|---|---|
| Serial pins | PC10 RX, PC11 TX, PC12 CTS, PC13 RTS, function 0 | FW |
| Power | PC22 `BT_PWR`, high = on | FW; H2 (a chip check, run 6: answered with it high) |
| Start | PC18 `BT_REG_ON` on hardware revision 4 (PC19 before): low, then high to start the core; give it 200 ms | FW; H2 (runs 6-16) |
| Wake pins | PC19 `HOST_WAKE_BT` output, held low (no sleep modes); PC20 `BT_WAKE_HOST` input, unused | FW; H2 (run 6) |
| Sleep clock | 32.768 kHz from PWM channel 1 on PC26 (function 1). The chip didn't answer without it in run 6 (before patching). **PC26 is also the DAC's I2C data line** (bus 1): while Bluetooth has it, the DAC can't be spoken to | FW; H2 (run 6) |
| Patch | `BCM4343A1.hcd` from the stock firmware, copied by the installer into `/.lossless/bluetooth/` from wherever the card has it. Until patched the chip answers only HCI Reset and Broadcom's download commands | H2 (runs 1-6); stock firmware (`brcm_patchram_plus` in `/usr/bin/bt_init`) |
| Patch steps | At 115200 baud: Reset (`0x0c03`), Download Minidriver (`0xfc2e`), wait 50 ms, then each record of the file as a command (2-byte opcode, 1-byte length, data) waiting for its Command Complete, then wait 250 ms (the chip restarts) and Reset again | H2 (runs 7-16) |
| Speed | Broadcom Update UART Baud Rate (`0xfc18`, 6 bytes: two zero bytes, then the rate, least significant first) to 3,000,000; after its Command Complete, wait 20 ms and switch the UART | H2 (runs 7-16); stock firmware (`--baudrate 3000000`) |
| Flow control | RTS/CTS in hardware, needed at 3 Mbaud | stock firmware; FW |

### UART

UARTn at `0xb0030000 + n * 0x1000`, 16550-like with Ingenic additions:

| Register | Offset | Use |
|---|---|---|
| `RBR` / `THR` / `DLL` | `0x00` | receive / send a byte; divider low with `LCR.DLAB` |
| `IER` / `DLH` | `0x04` | interrupts (bit 0 data, bit 4 receive timeout); divider high with DLAB |
| `FCR` | `0x08` | bit 0 FIFOs on, 1 / 2 reset receive / send FIFO, 3 DMA, **4 UART on**, bits 7:6 receive trigger |
| `LCR` | `0x0c` | bits 1:0 = 3 for 8 bits, bit 7 DLAB |
| `MCR` | `0x10` | bit 1 RTS, bit 6 hardware flow control, bit 7 modem control on (RTS must be set too) |
| `LSR` | `0x14` | bit 0 data ready, 1 overrun, 5 send FIFO wants data, 6 sender empty |
| `UMR` | `0x24` | clocks per bit (16 normally) |
| `UACR` | `0x28` | extra half-clocks per bit pattern (0 here) |
| `RCR` / `TCR` | `0x40` / `0x44` | bytes in the receive / send FIFO |

Baud = 24 MHz / (divider x UMR): 115200 is divider 13, UMR 16; 3,000,000 is
divider 1, UMR 8. Clock gate: `CPM_CLKGR` bit 14 (UART0). Interrupt: INTC
source 51 (bank 1 bit 19).

Source: FW.

### PWM (the sleep clock)

PWM channel n is TCU timer n, at `0xb0002000`: enable set / clear at
`+0x14` / `+0x18`, stop set / clear at `+0x2c` / `+0x3c`, full compare at
`+0x40 + n * 0x10`, half compare at `+0x44 + n * 0x10`, counter at `+0x48 + n
* 0x10`, control at `+0x4c + n * 0x10` (bits 2:0 clock source, 1 = peripheral
clock; bits 5:3 prescale; bit 7 PWM output on). The counter counts up to the
full compare; the pin changes at the half compare. Clock gate: `CPM_CLKGR`
bit 18 (TCU). 32.768 kHz from a 100.8 MHz peripheral clock is 3076 counts
(+55 ppm; Bluetooth allows 250).

Source: FW.

## Clock (RTC)

At `0xb0003000`. Keeps running while the player is off, as long as the
battery isn't flat. It keeps local time (no time zone), as the existing firmware does.

| Register | Offset | Use |
|---|---|---|
| `CR` | `0x00` | bit 0 on, bit 7 ready for a write |
| `SR` | `0x04` | seconds since 1970-01-01 |
| `GR` | `0x0c` | bits 15:0 the 1 Hz divider - 1 (32767 when set up) |
| `HSPR` | `0x34` | free register: `0x52544356` ("RTCV") is written once the clock is set up (the existing firmware's convention, kept); anything else means it lost power |
| `WENR` | `0x3c` | write `0xa55a` to unlock a write; bit 31 shows it's unlocked |

Each write: wait for `CR` bit 7, write `0xa55a` to `WENR`, wait for its bit
31, wait for `CR` bit 7, write the register, wait for `CR` bit 7 again.
Clock gate: `CPM_CLKGR` bit 27 (left running).

Source: FW.

## Screen, started by us

On a normal power-on the bootloader never starts the screen (only its menu
does), so Lossless OS sets up the LCD controller and the panel itself
(`platform/h2/h2_lcd.c`) when it finds no running frame.

| Step | Facts |
|---|---|
| Pins | PA0-PA15 function 1 (data; the bus is 8 bits); PB17 (WR), PB19 (probably TE), PB20 (DC) function 1; PB16 RD output high; PB18 chip select output, active low; PB13 reset output, active low; PB14 panel power on hardware revisions 1-3 only (an input on revision 4) |
| Clock | `CPM_CLKGR` bit 23 is the LCD's gate. Pixel clock `CPM_LPCDR` (`0xb0000064`): bit 31 source (0 SCLK_A), bit 28 change enable, bit 27 busy, bits 7:0 divider - 1; 20 MHz wanted, so SCLK_A 1008 MHz / 51 |
| Controller | `LCD_MCFG` (`+0xa0`) = `0x100` (8-bit commands); `LCD_MCFG_NEW` (`+0xb8`) = 0 while sending commands, then bit 0 (format conversion) and bits 9:8 = 1 (two bus cycles a pixel) for frames; `LCD_MCTRL` (`+0xa4`) bit 8 no TE, bit 4 fast mode, bit 2 DMA mode, bit 1 DMA start, bit 0 DMA transmit; `LCD_WTIME` (`+0xb0`), `LCD_TASH` (`+0xb4`), `LCD_SMWT` (`+0xbc`) = 0; `LCD_CTRL` bits 30:28 = 4 (64-word bursts), bit 3 enable; `LCD_DAH` (`+0x10`) = 320, `LCD_DAV` (`+0x14`) = 240 |
| Commands to the panel | `LCD_MDATA` (`+0xac`): bits 31:30 = 1 for a command, 0 for a parameter, the byte in bits 7:0; wait for `LCD_MSTATE` bit 0 (busy) to clear before each |
| Panel start | PB13 high after 20 ms, wait 12 ms, pixel clock, RD high, CS high, 5 ms, CS low; then the table below |
| Panel table (revisions 3 and 4) | FE; EF; 36 90; 3A 55; 84 04; 86 FB; 87 79; 89 0B; 8A 20; 8B 80; 8D 3B; 8E CF; EC 33 02 4C; 98 3E; 9C 4B; 99 3E; 9D 4B; 9B 55; E8 11 00; FF 62; C3 20; C4 03; C9 2A; F0 4A 10 0A 0A 26 39; F2 4A 10 0A 0A 26 39; F1 50 8F AF 3B 3F 7F; F3 50 8F AF 3B 3F 7F; BA 0A; 21; FE; EE; 11, 120 ms; 29, 20 ms. A GalaxyCore GC9xxx-like panel (not confirmed). Revisions 1-2 have an ILI9342C and a different table |
| Frames | Two DMA descriptors (8 words, 32-byte aligned) in a loop: a command one (source: 4 bytes `2C 2C 2C 2C`, 64-byte aligned; frame id `0xC0`; command word `0x20000001`; count 4) and the frame (frame id `0xF0`; command word `0x44009600`: end-of-frame flag, frame enable, 38400 words; count `0x20000000`, 16 bits a pixel; size `0x000EF13F`). Start: `LCD_STATE` = 0, `LCD_DA` = the command descriptor, `LCD_CTRL` enable; then each `DMA_START` sends a frame |

Source: FW, gathered as facts only.

## USB

The controller is a Synopsys DesignWare OTG core ("DWC2"), high speed, with
a 16-bit UTMI+ PHY inside the X1000.

| Fact | Value | Source |
|---|---|---|
| Controller registers | `0xb3500000` (physical `0x13500000`) | FW |
| Endpoints | 9 (0 to 8), dedicated TX FIFOs, internal DMA, 3576 words of FIFO memory | FW |
| Interrupt | INTC bank 0 bit 21; nothing to acknowledge at the INTC (clearing the controller's status clears it) | FW |
| Cable | PD3, input, high while a cable is in (computer or charger); polled, 200 ms settle | FW |
| Also on the cable | AXP2101 register `0x00` bit 5: USB power present | FW |
| USB-C chip pins | PB30 low; PD5 low on hardware revision 4: "upstream facing port" (PD5 is Back on revisions 1 to 3) | FW |
| Host-mode VBUS | PB25, output low (we never use host mode) | FW |
| ID pin | not used | FW |
| Controller speed setting | GUSBCFG: 16-bit PHY (bit 3), turnaround 5 (bits 13:10); DCFG high speed | FW |

### Clock and PHY (CPM)

| Register | Address | Fields we use |
|---|---|---|
| `CPM_CLKGR` | `0xb0000020` | bit 3: OTG clock stopped |
| `CPM_OPCR` | `0xb0000024` | bit 23: PHY clock stopped; bit 7: PHY 0 awake |
| `CPM_USBPCR` | `0xb000003c` | bit 22 PHY reset (POR), bit 21 SIDDQ (PHY off), bit 20 OTG off; VBUS forced valid (bits 24, 23); tuning |
| `CPM_USBRDT` | `0xb0000040` | bit 25 VBUS filter load; bits 22:0 reset timer |
| `CPM_USBVBFIL` | `0xb0000044` | VBUS and ID filters |
| `CPM_USBPCR1` | `0xb0000048` | bit 31 session valid; bits 27:26 reference clock source (2 = USB clock); bits 25:24 reference rate (1 = 24 MHz); bit 19 16-bit interface |
| `CPM_USBCDR` | `0xb0000050` | bits 31:30 source (0 = crystal); bit 29 change enable; bit 28 busy; bit 27 stop; bit 26 PHY gate; bits 7:0 divider - 1 |
| `CPM_SRBC` | `0xb00000c4` | bit 12: OTG reset |

Switching on, in order: OTG clock on; USB clock from the 24 MHz crystal, not
divided (change enable, wait for busy to clear); OTG reset for 10 us; PHY
clock on and PHY awake, 45 us; `USBVBFIL` = 0, `USBRDT` = `0x02000096`,
`USBPCR` = `0x03903857`, `USBPCR1` = `0x89080000`; PHY reset (POR) for 1 ms,
then 1 ms more. Switching off is the reverse: soft disconnect, stop the
core's clock (`PCGCCTL` = 1), PHY asleep, SIDDQ and OTG off, PHY clock
stopped, USB clock stopped, OTG clock stopped.

Source: FW, which follows Ingenic's own
Linux kernel. `platform/h2/h2_usb.c` notes the controller's identity and
configuration registers (`GSNPSID`, `GHWCFG1` to `4`) in
`/.lossless/logs/usb.txt` on the first start, for the USB driver.

### Mass storage

The computer only counts as there once it resets the bus (a charger never
does). Everything using the card stops, the file system is unmounted, the
card is given to the computer, and a full-screen "USB" page stays up until
the cable comes out; then the card is mounted again. Source: FW
(`firmware/usb.c`, `usb_screen.c`).

### As a sound card (USB DAC)

Lossless OS's own: a USB Audio Class 2 device, asynchronous (the player's
clock decides; a feedback endpoint tells the computer how many frames to
send each microframe, as 16.16 frames per microframe). The facts it rests
on are the USB Audio 2.0 specification's and the controller's:

| Fact | Value | Source |
|---|---|---|
| Interrupt line | the CPU's interrupt 2 is the INTC; INTC bank 0 pending at `0xb0001010`, mask set `0xb0001008`, mask clear `0xb000100c`; OTG is bit 21 | FW |
| Isochronous endpoints | DIEPCTL / DOEPCTL type 1; each packet is for an even or odd microframe (set even / set odd, bits 28 / 29), which must match the frame number in DSTS (bits 8 to 21) when it goes | FW, USB 2.0 §5.6 |
| Missed packets | GINTSTS bit 20 (an IN packet not sent in its microframe) and bit 21 (an OUT one not received): the endpoint is set up again for the next | FW |
| Formats | 16-bit in 2 bytes, 24-bit in 4 (left justified), 32-bit in 4; 44.1 to 384 kHz | USB Audio 2.0 Formats §2.3.1 |
| IDs | vendor `0x1209` (pid.codes, for open-source hardware), product `0x0002` (pid.codes' test ID until we have our own) | pid.codes |
| Feedback endpoint | asked for every 8 microframes (bInterval 4); a NAK event (DIEPINT bit 13) says the computer asked with nothing ready | USB Audio 2.0 §3.16.2.2; the standard DWC2 register layout |
| Stopping an OUT endpoint | only under a global OUT NAK (DCTL bit 9, wait for GINTSTS bit 7), then EPDIS and wait for EPDISD, then DCTL bit 10 | the standard DWC2 register layout; seen on the player |

Learned on the player, with macOS (48 kHz, 16-bit, through album changes):

- A packet's "done" can come after the next microframe has begun, so the
  next packet is set up for the parity after the one the last used, not
  from the frame counter (which then points one microframe too far).
- Long control replies go 64 bytes at a time; a new setup drops whatever
  of the last reply is left, and the IN "done" is handled before a setup.
- Zephyr's MIPS port keeps the CPU's interrupt mask (CP0 Status) in each
  thread's saved state: an interrupt line opened from one thread stays
  shut in the others. The INTC's line is opened at start, before any
  thread (`soc/ingenic/x1000/soc.c`), and each source is switched at the
  INTC.
- Music arriving as it plays can run the DMA past what's written; the
  audio driver then carries on just ahead of the DMA.

Each time the sound card stops, `/.lossless/logs/usb-dac.txt` gets its
counters and its last endpoint-0 events: `S` a request (its 8 bytes), `P`
the data a request carried (length, our answer: 0 taken, -1 refused, the
first 4 bytes), `N` the stream stopping (1: the OUT NAK took effect).
