# TG-GR6000N Firmware Development Work Log

This log tracks what we've tried and the results, so context can be rebuilt
quickly if a session is interrupted.

---

## Hardware Setup

- **Target**: Solum TG-GR6000N e-paper price tag
  - CC2630F128 PG2.3 (7x7mm QFN48)
  - 128KB Flash, 20KB SRAM
  - 600×448 monochrome e-paper display (UC8159 controller)
  - Primary IEEE Address: `00:12:4B:00:18:18:80:B0`
- **Serial adapter**: FTDI FT232R USB UART → `/dev/ttyUSB0`
  - FTDI TX → CC2630 DIO2 (RX)
  - FTDI RX → CC2630 DIO3 (TX)
  - FTDI DTR → CC2630 RST (reset)
  - FTDI 3.3V → Tag power
- **RPi GPIO 17** → CC2630 DIO11 (D/L pin for bootloader entry)
- **SEGGER J-Link EDU Mini V1** (S/N: 801046674) connected via USB for cJTAG
  - J-Link pin 1 (VTref) → 3.3V
  - J-Link pin 2 (TMS) → CC2630 **pin 24** (JTAG_TMSC)
  - J-Link pin 3 (GND) → GND
  - J-Link pin 4 (TCK) → CC2630 **pin 25** (JTAG_TCKC)
  - **WARNING**: CC2630 pins 24/25 are dedicated JTAG, NOT DIO16/DIO17!
    DIO16 (pin 27) and DIO17 (pin 26) are regular GPIOs with no JTAG capability.
- **Host**: Raspberry Pi 5, 4GB RAM, Linux 6.12.62+rpt-rpi-2712

## Flash Programming

- **Tool**: cc2538-bsl at 500kbaud with mass erase (`-e -w -v`)
- **Script**: `tools/flash.sh` automates D/L pin (GPIO 17 LOW), flashing, release
- **Power cycle**: `echo 0|sudo tee /sys/bus/usb/devices/3-1/authorized` then `echo 1`
- Flash programming and readback verification always succeed (CRC match)
- Sometimes needs a USB power cycle before bootloader will sync

## Previous Tag (Bricked)

A first TG-GR6000N tag was bricked by modifying CCFG byte 51 from 0xC5 to 0x00,
which permanently disabled the UART bootloader. Without JTAG recovery, the device
is unrecoverable. It had previously checked in to the OEPL AP as:
- MAC: `00124B00174A79D6`, M3 6.0", 600×448, RSSI -43
- This tag was disassembled; its stock firmware was dumped first and confirmed
  identical to the current tag's firmware.

---

## Core Problem (PARTIALLY SOLVED)

**User firmware DOES execute** — confirmed via JTAG. The previous failures were
caused by two issues:
1. **DIO11 (D/L pin) floating LOW** → ROM always entered bootloader instead of
   jumping to user code. The flash.sh script correctly releases GPIO 17 after
   flashing, but the pin floats to an indeterminate state. Without an explicit
   HIGH drive, DIO11 may read LOW, trapping the ROM in bootloader mode forever.
2. **Wrong JTAG pins** — We initially wired J-Link to DIO16/DIO17 (package pins
   26/27) instead of JTAG_TMSC/JTAG_TCKC (package pins 24/25). DIO16/17 are
   regular GPIOs with no JTAG capability.

## What We Know Works

1. **ROM bootloader works** - Reliably enters bootloader when DIO11 (D/L) is LOW
2. **Flash programming works** - Mass erase, write, verify all succeed (CRC match)
3. **Flash readback is correct** - Vector table verified: SP=0x20005000, Reset=0x000000BD
4. **CCFG is correct** - IMAGE_VALID=0x00000000, BOOTLOADER_ENABLE=0xC5
5. **ROM jumps to user code** - When D/L is HIGH after reset, bootloader does NOT
   respond (i.e., ROM checked IMAGE_VALID, found it valid, and jumped to the
   Reset vector instead of entering bootloader mode)
6. **Correct driverlib** - Using cc26x0 driverlib (not cc13x1_cc26x1 which halts
   on CC2630 due to chip family check)

## What We've Confirmed

- **PRCM register offsets** match cc26x0 hw_prcm.h:
  - PDCTL0PERIPH=0x138, PDSTAT0PERIPH=0x14C, GPIOCLKGR=0x48, CLKLOADCTL=0x28
- **GPIO register offsets** match cc26x0 hw_gpio.h:
  - DOUT31_0=0x80, DOUTSET=0x90, DOUTCLR=0xA0, DOUTTGL=0xB0, DOE=0xD0
- **IOC base** = 0x40081000, GPIO_BASE=0x40022000, PRCM_BASE=0x40082000
- **PRCM_NONBUF_BASE** = 0x60082000 (required for CLKLOADCTL writes)
- **System clock** after ROM boot: 48MHz RCOSC_HF (not 24MHz)
- **ROM bootloader UART pins**: DIO2=RX, DIO3=TX (hardcoded for QFN48 7x7mm)
- **JTAG pins are dedicated** - JTAG_TMSC (pin 24) and JTAG_TCKC (pin 25) are NOT DIOs.
  DIO16 (pin 27) and DIO17 (pin 26) are regular GPIOs — NOT JTAG!
- **CCFG stock settings**: XOSC_FREQ=24MHz, SCLK_LF=XOSC_LF, DCDC enabled,
  BOD=1.8V, DIS_GPRAM=1 (8KB as cache)
- **Linker symbols correct**: _data=_edata=_bss=_ebss=0x20000000 (both zero-size)
- **Disassembly verified**: Reset_Handler skips .data/.bss, calls main() directly;
  main() code is correct per instruction-level trace

## CCFG (Stock, 88 bytes at 0x1FFA8)

```
00 00 00 00  10 00 82 FF  FD FF 54 00  3A FF BF F3
FF FF FF FF  FF FF FF FF  FF FF FF FF  FF FF FF FF
FF FF FF FF  FF FF FF FF  FF FF FF FF  FF FF FF FF
C5 0B FE C5  FF FF FF FF  C5 FF FF FF  C5 C5 C5 FF
C5 C5 C5 FF  00 00 00 00  FF FF FF FF  FF FF FF FF
FF FF FF FF  FF FF FF FF
```

Key fields: IMAGE_VALID=0x00000000, BL_ENABLE=0xC5, BL_PIN=DIO11,
BL_LEVEL=active LOW, XOSC_FREQ=24MHz, SCLK_LF=XOSC_LF

---

## Firmware Attempts (All Failed to Produce Output)

### Attempt 1: UART peripheral + wrong driverlib
- Used cc13x1_cc26x1 driverlib (wrong chip family)
- `SetupTrimDevice()` contains `ThisLibraryIsFor_CC13x1_CC26x1...HaltIfViolated()`
  which enters `while(1)` on CC2630
- **Result**: No output (firmware hangs in SetupTrimDevice)

### Attempt 2: UART peripheral + cc26x0 driverlib
- Switched to cc26x0 driverlib at `~/Code/ti/cc26x0/`
- Called `NOROM_SetupTrimDevice()` from cc26x0 setup.c
- cc26x0 has `ThisLibraryIsFor_CC26x0_HwRev22AndLater_HaltIfViolated()` which
  should PASS on our PG2.3
- Fixed UARTCLKGR offset from 0x54 (wrong, was GPTCLKGR) to 0x6C
- **Result**: No output

### Attempt 3: Bare metal (no SetupTrimDevice)
- Commented out SetupTrimDevice to eliminate it as failure point
- Configured UART0 peripheral directly via registers
- TX on DIO3, 115200 baud assuming 48MHz RCOSC_HF
- **Result**: No output at any baud rate

### Attempt 4: Bit-bang UART on DIO3 at 9600 baud
- Bypassed UART peripheral entirely
- Used delay loop + GPIO SET/CLR for UART timing
- 9600 baud at 48MHz = 5000 cycles/bit ≈ 1667 NOP iterations
- **Result**: No output

### Attempt 5: Bit-bang UART on ALL DIOs (0-14, skip 11)
- Sent "D3 HI", "D2 HI", "D0 HI", "D1 HI" on respective pins
- Sent "OK" on DIO4-DIO14
- Tested at 9600 baud
- **Result**: No output on any DIO at any baud rate

### Attempt 6: IOC fix + DIO11 toggle diagnostic
- Discovered IOC register value 0x20000000 has PULL_CTL=00 (undefined!)
- Fixed to 0x00006000 (PULL_CTL_DIS | PORT_ID_GPIO)
- Added DIO11 toggle at ~1Hz (DIO11 → RPi GPIO 17, directly readable)
- Also sends bit-bang UART on DIO3 and DIO2
- Monitored RPi GPIO 17 with `gpioget -c gpiochip0 17`
- GPIO 17 reads "active" (HIGH) consistently - no toggling detected
- **Note**: gpioget reliability on Pi 5 is uncertain - when we drove GPIO 17 LOW
  with gpioset and immediately read it, it still showed "active". May be a
  libgpiod v2 issue or a timing/bus contention issue.
- **Result**: No GPIO toggle observed (but measurement method may be unreliable)

### Attempt 7: Pre-built OEPL alpha firmware
- Flashed `reference/CC2630_5.8_OEPL_alpha.bin` (54KB) combined with stock CCFG
  to create full 128KB image
- This is a known-working OEPL firmware (worked on the bricked tag)
- Vector table: SP=0x200040D0, Reset=0x0000A95D
- **Result**: No UART output at any baud rate (9600-460800), no OEPL AP checkin

### Attempt 8: Stock firmware reflash
- Was about to try reflashing stock.bin but user clarified stock FW is Solum
  proprietary (not OEPL), so it wouldn't check in with AP
- Not attempted

### Attempt 9: JTAG debugging via J-Link cJTAG (BREAKTHROUGH)
- Fixed JTAG wiring: pin 24 (JTAG_TMSC) and pin 25 (JTAG_TCKC) — NOT DIO16/DIO17
- **cJTAG connection succeeded!** JLinkExe with `-if cJTAG` found:
  - ICE-Pick ID: `0x9B99A02F`
  - CPU TAP: `0x4BA00477`
  - Cortex-M3 r2p1 identified
- **With DIO11 floating (no GPIO 17 drive)**: CPU halts at **PC=0x10002EA8** (ROM!)
  - SP=0x20000FDC (ROM stack), LR=0x10002939 (ROM)
  - R5=0x40081008 (IOC register area)
  - CFSR=0x00000000, HFSR=0x00000000 (no faults)
  - **CPU is in ROM bootloader UART polling loop!**
  - This means the ROM entered bootloader mode because DIO11 was LOW/floating
- **With DIO11 driven HIGH (gpioset 17=1)**: After DTR hardware reset:
  - CPU **cannot be halted** — "CPU is not halted !"
  - This means firmware IS executing and entered standby/deep sleep
  - The OEPL firmware goes to sleep quickly, gating the CPU clock, which prevents
    JTAG from halting the core
- **No faults detected** — CFSR and HFSR both zero in every observation
- **Conclusion**: The ROM DOES jump to user code when DIO11 is HIGH.
  All previous "no output" results were likely because DIO11 was floating LOW,
  so the ROM never left bootloader mode.

**Key command**: `JLinkExe -device CC2630F128 -if cJTAG -speed 1000 -AutoConnect 1`

---

## Hypotheses (Updated)

### H1: ~~Firmware crashes (HardFault)~~ — RULED OUT
- JTAG shows CFSR=0x00000000, HFSR=0x00000000 — no faults
- CPU was in ROM bootloader, not crashed in user code

### H2: Power supply insufficient — NOT YET RULED OUT
- Still possible for peripherals like radio, e-paper, DCDC
- But JTAG confirms user code DOES start executing

### H3: Hardware defect — PARTIALLY RULED OUT
- JTAG works, CPU executes, ROM functions correctly
- Crystal/radio hardware not yet tested

### H4: ~~ROM prevents execution~~ — RULED OUT
- JTAG proves ROM jumps to user code when DIO11 is HIGH

### H5: ~~Flash write diagnostic needed~~ — SUPERSEDED
- JTAG provides direct CPU inspection, no longer need indirect proof

### H6: DIO11 floating LOW — CONFIRMED as root cause of "no output"
- When GPIO 17 is not actively driven HIGH, DIO11 floats LOW
- ROM interprets this as bootloader entry request
- **All previous firmware test results are invalid** — the firmware never executed!
- The flash.sh script releases GPIO 17 after flashing, but the pin floats
  to LOW, so the DTR reset immediately re-enters bootloader mode

### H7: OEPL firmware goes to standby too quickly for JTAG
- After DTR reset with DIO11 HIGH, CPU can't be halted (clock gated in standby)
- Need to test with non-sleeping firmware (DIO11 toggle diagnostic) to confirm
  the CPU is reachable in flash code
- Or use JTAG to single-step from ROM into user code

---

## Files

### Current firmware (on the tag right now)
Pre-built OEPL alpha: `reference/CC2630_5.8_OEPL_alpha_combined.bin` (128KB)

### Source files
- `firmware/main.c` - DIO11 toggle + bit-bang UART diagnostic (Attempt 6)
- `firmware/startup_cc2630.c` - Bare metal startup, SetupTrimDevice commented out
- `firmware/ccfg.c` - Stock CCFG (88 bytes, DO NOT MODIFY byte 51!)
- `firmware/cc2630f128.lds` - Linker script
- `Makefile` - Build system using cc26x0 driverlib

### Reference binaries
- `reference/CC2630_5.8_OEPL_alpha.bin` - Pre-built OEPL FW (54KB, no CCFG)
- `reference/CC2630_5.8_OEPL_alpha_combined.bin` - Above + stock CCFG (128KB)
- `reference/stock.bin` - TG-GR6000N stock firmware dump (128KB)
- `reference/stock_ccfg.bin` - Extracted CCFG (88 bytes)

### Tools
- `tools/flash.sh` - Flash script with D/L pin automation
- `tools/monitor_dio11.sh` - GPIO 17 monitor for DIO11 toggle test

### cc26x0 Driverlib
- `~/Code/ti/cc26x0/inc/` - Hardware register headers
- `~/Code/ti/cc26x0/driverlib/` - Driver source + prebuilt `bin/gcc/driverlib.lib`

---

## Next Steps

1. **Flash DIO11 toggle firmware + hold DIO11 HIGH** — Re-flash the diagnostic
   firmware (Attempt 6), then drive GPIO 17 HIGH before DTR reset. Use JTAG to
   halt and confirm PC is in flash (0x00000000-0x0001FFFF range). This firmware
   never sleeps, so JTAG halt should work.
2. **Fix flash.sh to hold DIO11 HIGH after reset** — After releasing the D/L pin,
   explicitly drive GPIO 17 HIGH before the final DTR reset. This ensures user
   code runs instead of bootloader.
3. **Verify UART output works** — With firmware confirmed running via JTAG, check
   if bit-bang UART on DIO3 produces output on the FTDI.
4. **Test OEPL AP checkin** — Flash OEPL alpha with proper DIO11 handling and
   check if tag appears on AP.
5. **Consider adding external pull-up on DIO11** — A 10K-100K resistor from
   DIO11 to 3.3V would prevent floating LOW issue permanently.
