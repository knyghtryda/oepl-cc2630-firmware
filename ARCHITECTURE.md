# Architecture

Bare-metal firmware for the Solum TG-GR6000N (CC2630F128, 20KB RAM) that
speaks the OpenEPaperLink tag protocol over IEEE 802.15.4 and drives a
600×448 BWR UC8159 panel. No RTOS, no interrupts in the main flow (RF and
display are polled), no heap.

## Layers

```
main.c                     boot, warm-boot detection, checkin loop, download+display, deep sleep
  oepl_ota_cc2630.c        OTA: stage to flash 0x10000+, verify, RAM-resident copy to 0x0, reset
  oepl_radio_cc2630.c      OEPL protocol: PING/PONG scan, AvailDataReq/Info, BlockRequest/Part, XferComplete
    oepl_rf_cc2630.c       RF core: CPE/RFE/MCE patches, CMD_RADIO_SETUP, CMD_FS, IEEE RX/TX, RX ring
  drivers/…uc8159…         display init (sequence recovered from stock FW), OTP waveform, DTM1 streaming
  splash.c                 cold-boot splash (MAC, battery, temp, channel) via font8x8.h
  oepl_hw_abstraction…     GPIO, SPI, delays, AON_BATMON battery/temperature
  rtt.c                    SEGGER RTT + UART0 TX debug
  startup_cc2630.c         vectors, VTOR reset, SetupTrimDevice, .data/.ramfunc/.bss, HardFault → RTT
  ccfg.c                   stock CCFG (backdoor on DIO11; byte 51 must stay 0xC5)
```

## Key decisions

- **Streaming display, no framebuffer.** A 600×448 BWR frame is 67,200 bytes;
  RAM is 20KB. Two 4100-byte block caches (B/W and red) are pulled on demand
  as rows are converted to 4bpp and pushed to the panel inside one DTM1 frame.
  Blocks are therefore downloaded interleaved with display writes.
- **RX ring of 8 entries** (`oepl_rf_cc2630.c`). Block transfers arrive as
  bursts of up to 42 frames, as little as ~4ms apart on a fast AP; the
  firmware polls, so the queue must absorb the gap between the radio finishing
  an entry and firmware releasing it. v0.7 and earlier had a single entry.
  Every `oepl_rf_rx_get()` that returns a frame is
  paired with exactly one `oepl_rf_rx_flush()`, which advances the read cursor;
  `oepl_rf_rx_start()` rewinds radio and cursor to entry 0.
- **Block reception is idle-timed, not window-timed.** The AP's part burst is
  followed until it goes quiet (`BLOCK_PART_TIMEOUT_MS`, measured off the RF
  core's 4 MHz RAT), then missing parts are re-requested. A fixed 15 s window
  per request made partial blocks cost 15 s each and pushed whole-image
  downloads past the AP's pending-data timeout. Timeouts are deliberately
  generous because AP pacing varies by AP hardware.
- **Faults and hangs reset and get reported.** `HardFault_Handler` writes a
  record to `.noinit` and resets; the RF doorbell is bounded and does the
  same on a hang. The next boot prints the record and reports
  `wakeupReason=0xFE` at its first check-in, visible in the AP DB.
- **Watchdog.** 45 s reload, reset on the second expiry (~90 s), kicked from
  `oepl_hw_delay_us`, `oepl_rf_rx_get`, the RF status waits and the main
  loop's delay — the primitives every long operation already spins on. It
  stops in standby. A WDT reset is reported like a crash (`0xDEADD006`).
- **RX is the background command; TX is foreground.** Every exchange is
  `rx_start → tx → poll → rx_stop`.
- **The panel is refreshed, waited out, then powered off — and only then is
  `XferComplete` sent, with ack and retries.** BUSY (DIO13) reads HIGH on this
  hardware, so the refresh wait is a fixed 30 s unless BUSY is ever seen LOW.
  Sending the (previously unacknowledged) `XferComplete` during the refresh's
  current spike lost it on about half of all cycles. The AP answers every
  `XferComplete` and only acts on the first per check-in, so retrying is safe.
- **Retries of a partly-received block use `BLOCK_PARTIAL_REQUEST`.** The AP
  serves those from its buffer (`pleaseWaitMs=30`); a plain `BLOCK_REQUEST`
  makes it re-fetch the block from the ESP32 first (`pleaseWaitMs=550`).
- **Partial images are tolerated, but not confirmed.** A block that fails
  after retries is drawn white; XferComplete is only sent when every block
  landed, so the AP re-offers the image next check-in.
- **Standby follows TI's `Power_sleep()` step for step** (`enter_sleep` in
  `main.c`): IOs frozen, crystal off, AUX released, RF/serial/peripheral/CPU
  domains off, uLDO requested, VIMS cache off, recharge configured; the wake
  path undoes it in TI's order — AUX must be powered before anything touches
  the oscillator/DDI registers (touching them early is a BusFault). Execution
  resumes after `PRCMDeepSleep()`; there is no reset. The LF clock qualifiers
  are bypassed once SCLK_LF is on the 32 kHz crystal. With a debugger attached
  the JTAG domain stays on and WFI just returns.
- **Board parts are parked before sleep.** Panel supply (DIO5) off and its
  lines high-impedance; external SPI flash (unused) in deep power-down with CS
  held high. Each of these was worth tens to hundreds of µA on the PPK2.
- **OTA copies from staging in a `.ramfunc`** because it erases the sectors
  it would otherwise be executing from. Last applied `dataVer` is recorded in
  sector 30 so the AP's re-offer doesn't loop.
- **CCFG is stock and untouched** except the backdoor stays enabled. Changing
  byte 51 bricked a tag.

## Flash layout (128KB)

| Range             | Use                                  |
|-------------------|--------------------------------------|
| 0x00000–0x0FFFF   | active firmware (≤64KB)              |
| 0x10000–0x1DFFF   | OTA staging                          |
| 0x1E000–0x1EFFF   | OTA dataVer record                   |
| 0x1FFA8–0x1FFFF   | CCFG                                 |

## Test surface

The AP (`http://192.168.5.4`) is the oracle: `ver`, `hash`, `pending` per tag.
`tools/ap.py` wraps its endpoints; see `DEVELOPMENT.md`.
