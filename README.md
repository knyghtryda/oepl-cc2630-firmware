# OpenEPaperLink Firmware for CC2630 (TG-GR6000N)

Custom open-source OEPL firmware for the Solum TG-GR6000N 6.0" BWR e-paper tag.

## Hardware

- **Chip**: Texas Instruments CC2630F128 (ARM Cortex-M3, 48MHz, 128KB Flash, 20KB RAM)
- **Radio**: 2.4 GHz IEEE 802.15.4 (OEPL protocol)
- **Display**: 6.0" UC8159 e-paper controller, 600x448 BWR (black/white/red)
- **Model**: Solum TG-GR6000N (hwType 0x35)
- **MAC**: 00:12:4B:00:18:18:80:B0

## Current Status

**Fully working end-to-end.** The tag receives images from the OEPL AP and displays them.

- [x] RF core boot and IEEE 802.15.4 radio
- [x] AP channel scanning (6 channels: 11, 15, 20, 25, 26, 27)
- [x] OEPL checkin protocol (AvailDataReq/AvailDataInfo)
- [x] Block transfer with cumulative part tracking (42 parts/block), 8-entry RX ring, burst idle-timeout
- [x] OTA firmware update via the AP (`tools/ap.py ota`)
- [x] Crash/hang recovery: HardFault, RF-core hang and watchdog reset the tag and report `wakeupReason=0xFE` (fault PC on the splash and in the AP DB)
- [x] Panel powered off and deep-slept after every refresh
- [x] UC8159 display driver with OTP waveform loading
- [x] BWR (black/white/red) image display - 1bpp per layer, 17 blocks
- [x] Standby between check-ins (TI Power_sleep sequence), ~61 µA measured
- [x] CCFG backdoor enabled (DIO11 LOW enters bootloader)
- [x] SEGGER RTT debug output (512-byte buffer)
- [x] UART TX debug output on DIO3 at 115200 baud

**Firmware**: v0.22 — ~18KB flash, 13KB static RAM (see `PLAN.md` for history, `DEVELOPMENT.md` for the test loop)

## Project Structure

```
oepl-cc2630-firmware/
├── Makefile              Build system
├── firmware/             Source code
│   ├── main.c            Entry point, checkin loop, download+display
│   ├── startup_cc2630.c  Reset handler, vector table, HardFault handler
│   ├── ccfg.c            CC2630 customer configuration
│   ├── cc2630f128.lds    Linker script
│   ├── rtt.c/h           SEGGER RTT + UART TX debug output
│   ├── oepl_radio_cc2630.c/h    OEPL radio protocol (scan, checkin, blocks)
│   ├── oepl_rf_cc2630.c/h       Low-level RF core driver
│   ├── oepl_hw_abstraction_cc2630.c/h  GPIO, SPI, delays
│   └── drivers/
│       └── oepl_display_driver_uc8159_600x448.c/h  UC8159 display driver
├── binaries/             Pre-built firmware binary
├── docs/                 Analysis and development documentation
├── reference/            Stock firmware binaries and OEPL reference binary
└── tools/                Utility scripts
    ├── ap.py             AP helper: OTA push, image push, poll tag state, decode fault/diag reports
    ├── ap_log.py         Stream the AP's live log (block requests, xfer complete, timeouts)
    ├── weather_display.py  Home Assistant weather layout: preview, push, install automation
    ├── flash.sh          UART bootloader flash script
    ├── dl_pin.sh         D/L pin (GPIO17) control
    └── start_fw.jlink    JLink firmware launch script
```

## Build Requirements

### ARM Toolchain

```bash
sudo apt install gcc-arm-none-eabi gdb-multiarch libnewlib-arm-none-eabi
```

### TI Driverlib

The CC26x0 driverlib must be built at `~/Code/ti/cc26x0/driverlib/`.
See `docs/BUILD_STATUS.md` for details.

## Building

```bash
make                    # Build firmware
make size               # Show memory usage
make clean              # Clean build artifacts
```

## Programming

### Via J-Link (cJTAG) - Recommended

Requires SEGGER J-Link connected via cJTAG (2-wire).

```bash
# Start GDB server
JLinkGDBServer -device CC2630F128 -if cJTAG -speed 1000 \
  -port 2331 -RTTTelnetPort 19021 -notimeout &

# Flash and start firmware
gdb-multiarch -batch -nx \
  -ex "file build/Tag_FW_CC2630_TG-GR6000N.elf" \
  -ex "target remote :2331" \
  -ex "monitor halt" \
  -ex "monitor flash erase" \
  -ex "load" \
  -ex "set \$pc = 0x000000bc" \
  -ex "set \$sp = 0x20005000" \
  -ex "monitor go" \
  -ex "disconnect"
```

**Important**: Always `monitor flash erase` before `load` to avoid stale flash corruption.

**Note**: `monitor reset` does NOT work reliably on CC2630. Must set PC/SP manually.

### Via UART Bootloader (cc2538-bsl)

Requires CCFG backdoor enabled (byte 48 = 0xC5) in the currently-flashed firmware.
The D/L pin (DIO11) is controlled by Raspberry Pi GPIO17.

```bash
./tools/flash.sh binaries/Tag_FW_CC2630_TG-GR6000N.bin /dev/ttyUSB0
```

## Debugging

### RTT (with J-Link)

```bash
# Read RTT output (requires JLinkGDBServer running)
timeout 30 bash -c 'exec 3<>/dev/tcp/localhost/19021; cat <&3'

# Or use JLinkRTTClient
JLinkRTTClient
```

### UART

Debug output also goes to UART0 TX (DIO3) at 115200 baud, compatible with
the FTDI adapter used for cc2538-bsl flashing.

## Pin Assignments

| DIO | Function | Notes |
|-----|----------|-------|
| 2   | UART RX  | cc2538-bsl / debug |
| 3   | UART TX  | cc2538-bsl / debug |
| 5   | EPD Power | Enable (tentative) |
| 8   | SPI MISO | |
| 9   | SPI MOSI | |
| 10  | SPI CLK  | |
| 11  | Flash CS / D/L | Dual-use: SPI CS for flash, bootloader entry |
| 12  | DIR      | LOW = write |
| 13  | BUSY     | Input, HIGH = not busy |
| 14  | RST      | Active LOW |
| 15  | DC       | Data/Command |
| 18  | BS1      | LOW = 4-wire SPI |
| 20  | EPD CS   | GPIO output (not SSI0 FSS) |

## Known Issues

- **Weak-signal cliff at about −77 dBm.** The tag stops hearing the AP well
  above where a CC2630 should (≈−95 dBm for 802.15.4). This firmware applies
  no RF patches, while the stock and OEPL-alpha firmwares both apply
  `rf_patch_cpe_ieee` and stock adds override entries (LNA bias trim, RSSI
  offset). Untested at the edge — see PLAN.md.
- **A compressed image larger than 20 KB will not display.** Images are staged
  in flash sectors 16–20 before decoding; anything bigger is refused and
  retried. The weather layout is ~6 KB and a test card ~2.2 KB, so there is
  room, but a heavily dithered photograph could exceed it.
- **OEPL channel 27 is unusable on this chip** — `CMD_IEEE_RX` takes 11–26
  only, and channel 27 wedged the radio core until the tag reset (fixed in
  v0.22 by skipping it; the AP must not be set to channel 27).
- **The reported firmware version selects the AP's image format.** At 39 or
  above the AP sends zlib-compressed images (which this firmware decodes);
  below 39 it sends raw. See DEVELOPMENT.md before changing the version.
- **NFC is supported** (v0.30): the board's second antenna is a coil for a passive NTAG
  I2C chip. The tag writes its identity there at boot and accepts URLs pushed from the AP's
  "Set NFC URL" content mode. A tap works even on a flat or bricked tag, since the chip is
  powered by the reader.
- **UART debug mirror** off by default (`-DRTT_UART`); it never produced
  output on this board and kept the serial domain powered. Use RTT.

Fixed since the first release, and no longer issues: sleep current (≈2 mA →
59 µA → **9.1 µA**), image transfer time (~100 s → ~45 s, and 1–2 blocks
instead of 17 now that images are compressed), the radio core hanging on
channel 27, and DIO13/BUSY — the panel does drive BUSY and refreshes now end
on it (~7.7 s) rather than running to a timeout.

## Based On

- [OpenEPaperLink](https://github.com/OpenEPaperLink/OpenEPaperLink) project
- CC2630 OEPL alpha firmware (reference binary in `reference/`)
- TG-GR6000N stock firmware (display init sequence extracted via Ghidra)
