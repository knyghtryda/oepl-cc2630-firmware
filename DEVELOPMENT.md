# Development

How to build, flash, and verify this firmware. See `ARCHITECTURE.md` for how
the pieces fit and `PLAN.md` for what's in flight.

## Build

Requires `gcc-arm-none-eabi` and the TI cc26x0 driverlib at
`~/Code/ti/cc26x0/` (`CC26X0_DIR` overrides; see `docs/BUILD_STATUS.md`).

```bash
make          # build/Tag_FW_CC2630_TG-GR6000N.elf + binaries/*.bin (full 128KB image with CCFG)
make ota      # binaries/Tag_FW_CC2630_TG-GR6000N_ota.bin (no CCFG — what the AP sends over the air)
make size
make clean
```

RAM budget: 20KB total, 18KB for `.data/.bss/.noinit/.heap`, 2KB stack. `make`
prints `text/data/bss`; `.bss` is dominated by the two 4100-byte block buffers
and the 8×256-byte RX ring.

`make DIAG=1` builds a diagnostics variant: the check-in after each image
download reports download statistics in the LQI/temperature/battery fields
(`OEPL_DIAG=1 tools/ap.py status` decodes them), and the version is reported
with bit 15 set (32783 for v0.15). Never ship it — it replaces real telemetry.

## Version bumps

**The reported version selects the AP's image format.** The AP decides whether
to compress from the tag's *reported version*, not from the `capabilities`
field: `http://192.168.5.4/tagtypes/35.json` carries
`"zlib_compression": "27"`, read as hex (39), and `contentmanager.cpp` does
`if (hwdata.zlib != 0 && taginfo->tagSoftwareVersion >= hwdata.zlib)`. A tag
reporting 39 or more is served `DATATYPE_IMG_ZLIB` (0x30) instead of raw.

Since v0.27 that is what we want — `firmware/inflate.c` decodes it and the
same picture costs 2.2 KB instead of 67 KB. Reporting **below 39 asks the AP
for raw images again**, which is the escape hatch if the decoder ever needs to
be bypassed. (Before v0.27 it was a trap: crossing 39 during OTA testing broke
images on both tags with no error anywhere except the tag's RTT log.)

## Compressed images

```
tools/ap.py image MAC            # AP compresses for a tag reporting >= 39
host_test/run_tests.sh           # decoder tests, no hardware needed
host_test/run_tests.sh --fetch   # re-pull raw images from the AP first
```

The payload is a 4-byte uncompressed length, then a zlib stream whose content
is a 6-byte header (`6, width, height, planes`) followed by plane 1 and, for a
red image, plane 2. The AP's miniz is built with a 4 KB dictionary and stamps
`CINFO=4`, so a 4 KB window is enough — `red_buf` is reused for it.

The tag stages the compressed image in flash (sectors 16–20) exactly as it
stages firmware, then decodes straight out of it. For a two-plane image the
first plane goes back to flash (sectors 21–29) because the panel wants the
planes interleaved as 4bpp pixels while the stream delivers them one after the
other; the second plane is interleaved against it row by row, so the panel is
fed in a single pass. The stream's Adler-32 is checked before the refresh, so
a corrupt image leaves the previous picture on the panel instead of painting
it. Decode takes ~1 s for a 600x448 BWR image.

`host_test/` holds a replica of the AP's compressor built from the AP's own
`miniz-oepl`, so the tests run against bit-identical input: six images
(including incompressible random data) decode byte-exact, and 20,000
truncated or bit-flipped streams are rejected without a crash, an overrun or a
hang under ASan/UBSan.

**Stack:** each check-in logs `stack free=` — the bytes of stack never touched
since cold boot. The decoder brought this down to ~1.8 KB spare; if a change
takes it near zero, .noinit (crash records) is what gets corrupted first.


Two places, keep them in sync:

- `firmware/oepl_radio_cc2630.h` — `TAG_FW_VERSION` (what the AP shows as `ver`)
- `firmware/splash.c` — `"FW vX.Y"` on the boot splash

## Delivering firmware to a tag

### Over the air (preferred — no jig, works on a sealed tag)

The tag's OTA path (`firmware/oepl_ota_cc2630.c`) is the normal way to update
a tag already running this firmware. `tools/ap.py` drives the AP's web
endpoints from the dev host:

```bash
make ota
tools/ap.py status                          # list CC2630 tags the AP knows (hwType 0x35)
tools/ap.py ota 00124B00181880B0            # stage binaries/*_ota.bin on AP, queue for tag
tools/ap.py wait-ver 00124B00181880B0 8     # poll until AP reports the new version
```

The AP offers `dataType=0x03` at the tag's next check-in; the tag stages the
image to flash sectors 16–29, verifies checksums and the vector table, copies
it to the active area from a RAM-resident function, and resets. OTA block
download is strict (all 42 parts, 20 retries, no early bail-out) so it works
even from firmware whose image path is broken.

What the apply guarantees (v0.24 on; the whole path is one-way once the first
sector is erased, so every check happens before that point):

- **Won't start weak.** Below `OTA_APPLY_MIN_MV` (2400 mV) the tag skips the
  download entirely, and re-checks just before the apply in case the supply
  sagged during it. The AP re-offers while the versions differ, so waiting
  costs nothing.
- **Won't jump into a bad image.** The staged reset vector must have the Thumb
  bit set and point inside the staged image; the initial SP must be in SRAM.
  A vector with bit 0 clear used to pass and would HardFault before the first
  instruction of the new image.
- **Reads its own writes.** Flash readbacks (staging and active) run with the
  VIMS cache invalidated, otherwise they compare against cached bytes of the
  image being replaced.
- **Retries a bad sector.** Each active sector is erased, programmed and read
  back, up to 5 times; the RAM buffer still holds the data, so a retry is free.
- **Marks "applied" last.** The dataVer record (sector 30) is written from the
  RAM function after the copy verifies, not before the copy starts — an
  interrupted apply is retried instead of being skipped forever.
- **Radio off first.** `oepl_rf_shutdown()` runs before the erase, so the flash
  charge pump doesn't share the supply with the RF core.

The first boot of the new image prints what the copy did:

```
OTA apply: sectors=06 retries=00 bad=00
```

(`retries` = erase/program attempts beyond the first, `bad` = sectors that
never read back correctly.) The same three numbers ride to the AP in that first
check-in — `tools/ap.py status` shows `OTA-APPLIED sectors=6 retries=0
unverified=0` — which is the only way to see inside an apply on a sealed tag.
They live in the dataVer record in sector 30, not in RAM: `.noinit` addresses
move between builds, and this record has to be read by the *next* image.
`x/4xw 0x1E000` over J-Link shows it long after the fact (`4F544156` magic,
dataVer, then sectors/retries/bad/reported).

Bench switch `EXTRA_DEFINES=-DBENCH_OTA_FLAKY_APPLY` makes the first attempt at
sector 0 skip its program step, so the retry path runs for real.

Only the AP at `http://192.168.5.4` is assumed; set `OEPL_AP` to override.

### J-Link (cJTAG, pins 24/25) — bring-up or recovery

```bash
JLinkGDBServer -device CC2630F128 -if cJTAG -speed 1000 -port 2331 -RTTTelnetPort 19021 -notimeout &
gdb-multiarch -batch -nx -ex "file build/Tag_FW_CC2630_TG-GR6000N.elf" -ex "target remote :2331" \
  -ex "monitor halt" -ex "monitor flash erase" -ex "load" \
  -ex 'set $pc = 0x000000bc' -ex 'set $sp = 0x20005000' -ex "monitor go" -ex "disconnect"
```

Always erase before load. `monitor reset` is unreliable on CC2630 — set PC/SP
by hand. `tools/flash_jlink.jlink` is the JLinkExe equivalent.

### UART ROM bootloader (cc2538-bsl)

`tools/flash.sh binaries/Tag_FW_CC2630_TG-GR6000N.bin /dev/ttyUSB0` — needs
the CCFG backdoor (DIO11 low at reset, driven by Pi GPIO17). See
`docs/WORKLOG.md` for the DIO11-floats-low trap.

## Verifying a change

The AP is the oracle. `get_db` reports, per tag, the firmware `ver`, the
`hash` of the last image the tag confirmed with XferComplete (all-zero = never),
and `pending` (AP still holds data for the tag).

End-to-end image test, fully scriptable:

```bash
tools/ap.py image 00124B00181880B0          # push a generated 600x448 BWR test card
tools/ap.py wait-image 00124B00181880B0     # OK when pending==0 and hash != 0
```

A pass means every one of the 17 blocks (714 BlockParts) landed and the tag
sent XferComplete. `wait-image` waits for the AP to register the upload as
pending and then for a *new* hash, so it can be run back-to-back. Expect 1–2
check-in cycles (a few minutes at a 60 s interval) on the current firmware.
The test card fills the full panel height so a partial
download shows as a white band at the bottom.

Check-in alone proves nothing about the receive path — it's a two-packet
exchange. Always run the image test.

### AP log (no hardware needed)

```bash
tools/ap_log.py 7B31 600      # AP console lines mentioning that MAC, for 600 s
```

Shows each block request the AP receives (one line per request — a healthy
block takes 1–3), `"<mac> reports xfer complete"`, and `"xfer timeout"` when
the AP's 20-minute per-image budget runs out. Together with `get_db` this
localises most failures without touching the tag.

### Reading a crash

A HardFault (or a hung RF doorbell) resets the tag. The next boot:

- prints `LAST FAULT: PC=... CFSR=...` on RTT,
- draws `FAULT PC=xxxxxxxx CFSR=xxxxxxxx` in red on the splash,
- reports `wakeupReason=0xFE` at its first check-in with the PC packed into
  `batteryMv` (bits 15:0) and `temperature` (bits 23:16), and CFSR nibbles in
  `LQI`. `tools/ap.py status` decodes this as `FAULT pc=0x...`. PC `0xDEAD0DB0`
  means the RF doorbell hung. Map a real PC with
  `arm-none-eabi-addr2line -e build/Tag_FW_CC2630_TG-GR6000N.elf 0x<pc>`.

The next check-in reports normal telemetry again, so poll at least every 30 s
to catch it. Pseudo-PCs: `0xDEAD0DB0` RF doorbell hang (radio core stopped
answering — seen on a tag at 2.83 V), `0xDEADD006` watchdog (something spun
for >90 s without kicking; deep sleep doesn't count, the WDT stops in standby).

### The channel-27 trap (fixed in v0.22)

A scan that reached OEPL channel 27 handed `CMD_IEEE_RX` an illegal channel
(11–26 only on this chip): the CPE raised INTERNAL_ERROR and stopped
acknowledging the doorbell, including the `CMD_ABORT` sent to clean up, and
the tag reset itself. It only happened when the AP's reply was missed on the
earlier channels, so it looked random — ~1 per 2.3 h in a 14 h soak, and it
was behind Weather6's boot loop on weak batteries. A 16 h run on v0.23 saw
zero. Don't set an AP to channel 27 for these tags.

### Low battery is not the only cause of a radio hang

On 2026-09-13 a coin-cell set sagging from 3.16 V to 2.83 V coincided with a
lost `XferComplete` and then a radio-core hang on every download, and the
battery got the blame. **That was wrong**: the 14 h soak on 2026-09-16 hit the
same doorbell hang 6 times on a steady 3.0 V bench supply, roughly every
2.3 h. Low voltage may make it more likely; it is not the cause. Still check
the voltage first (the splash and every check-in report it), but don't stop
there.

The AP also reboots now and then — `tools/ap_log.py` disconnecting, or a tag's
DB record jumping back to an older state, is that.

### Reading a crash capture

A fault (HardFault, doorbell hang, watchdog) freezes the last ~1 KB of debug
output plus the relevant registers into `.noinit` and resets the tag. The
capture survives the reset but not a power cycle:

```bash
tools/crash_dump.py [out_prefix]     # J-Link; tag powered, e.g. tools/ppk.py hold 120
```

The next check-in also reports it: `tools/ap.py status` decodes
`FAULT hardfault pc=… ufsr=… bfsr=…`, `FAULT rf-doorbell cmd=… phase=… cmdsta=…`
or `FAULT watchdog`. The fault class rides in the temperature field as an
impossible value (−128…−104 °C), because **the AP clears `wakeupReason` within
seconds of the tag reporting it** — don't key tooling on `wake=0xfe`.

Bench switches to exercise the paths (never ship): `BENCH_TEST_HARDFAULT`
(bus fault at a known PC), `BENCH_TEST_DOORBELL_HANG` (gates the RF core clock,
then aborts).

### RTT (needs J-Link attached)

```bash
timeout 60 bash -c 'exec 3<>/dev/tcp/localhost/19021; cat <&3'
```

Block download prints `Bnn` then `+` (42/42), `~` (41/42 accepted late), `R`
(retry), `X` (AP silent), `!` (gave up). After each request:
`BP:<total>/2A new=<this request> gap=<avg ms between parts> rf[d= nok= ign= full=]`
where the `rf[]` counters come from the RF core (`nok` CRC errors, `ign`
filtered, `full` dropped for lack of a free RX entry). A boot after a crash
prints `LAST FAULT: PC=...` (PC `DEAD0DB0` = RF doorbell hang) and the first
check-in carries the fault class/detail (see "Reading a crash capture").

## NFC

The board carries a passive NFC Type-2 tag chip (NXP NTAG I2C family) with its
own coil, separate from the 2.4 GHz antenna. It has no transmitter: a reader
powers it from its own field, so **it answers a phone even when the tag's
battery is flat or its firmware is broken** — confirmed on a bricked tag.

```
DIO24 = SDA, DIO25 = SCL, address 0x55, 16-byte blocks
DIO21 = FD (field detect), open-drain, pulses LOW while a reader's field is on
DIO5  = shared peripheral power (panel *and* NFC); only needed for I2C access
```

`firmware/oepl_nfc_cc2630.c` handles it. Block 0 (UID, capability container,
the chip's own I2C address) is never written; user memory starts at block 1.

- **On every cold boot** the tag writes its own identity — `OEPL <mac> v<ver>
  <volts>` as an NDEF text record — so tapping a tag that has gone quiet still
  says what it is.
- **The AP can push content.** OEPL's web UI offers content mode 14, "Set NFC
  URL", to any tag reporting `CAPABILITY_HAS_NFC` (0x40) — which is why it
  never appeared before, since this firmware used to report no capabilities at
  all. The AP sends `DATATYPE_NFC_RAW_CONTENT` (0xA0), whose payload is a
  finished NDEF TLV that the tag copies straight in; `DATATYPE_NFC_URL_DIRECT`
  (0xA1) sends a bare URL and the tag builds the record.
- Boards without the chip populated (it is a per-variant option) simply report
  no capability and skip all of it — `oepl_nfc_init()` returns false.

The identity write does not clobber AP-pushed content: if block 1 already
holds a URI record (what "Set NFC URL" writes), it is left alone. So a tag can
be given a URL once and then put back into display mode -- the chip is
non-volatile and the URL survives reboots and battery changes.

Verified 2026-09-19: identity record read by a phone, then a URL pushed from
the AP (`contentmode 14`) and opened by the phone. Field detect was confirmed
by watching DIO21 while a phone was held to the coil — it pulsed low on each
read, which is what `CAPABILITY_NFC_WAKE` would build on.

Sleep current is unaffected: the shared rail is raised only for the
milliseconds of I2C traffic and dropped again.

## Board pin map (recovered from the stock firmware)

Read out of `reference/stock.bin` (TI-RTOS PIN/SPI/I2C/UART driver tables at
0xEAA8/0xEB50/0xEB9C/0xEB10) on 2026-09-19, and spot-checked against the
binary by hand. Our firmware uses only the panel and flash pins; the rest is
recorded here because it answers several long-standing unknowns.

| DIO | stock function |
|---|---|
| 2, 3 | debug UART RX / TX |
| 4 | pull-up input, never read |
| 5 | **peripheral power enable (shared: panel *and* NFC)**, open-drain |
| 6, 7 | 8 mA outputs driven low at boot, never touched again |
| 8, 9, 10 | SSI0 MISO / MOSI / CLK |
| 11 | external SPI flash CS |
| 12–15, 18, 19, 20 | e-paper panel (DIR, BUSY, RST, D/C, BS, data readback, CS) |
| 16, 17 | outputs gated by per-variant capability bits (LED/buzzer candidates) |
| 21 | pull-up input, sampled at wake and inside DIO22's interrupt |
| 22 | positive-edge interrupt input |
| **24, 25** | **I²C SDA / SCL → NFC tag chip at address 0x55** |
| 26, 27 | two debounced buttons (short/long press) |
| 0, 1, 23, 28–30 | unused |

**The second antenna is an NFC coil, not a second radio.** Beside it is a
passive NFC Type-2 tag IC (NXP NTAG I²C family) that harvests power from a
phone's field — it has no transmitter of its own. The CC2630 writes an NDEF
payload into it over I²C (the payload comes from the external SPI flash at
0xC0000), auto-detecting the 1k and 2k variants from the capability container
(`E1 10 6D 00` / `E1 10 EA 00`) and verifying its own writes. The whole path
is behind a capability bit, so the chip is a populate option per variant.

Two consequences for us:

- **DIO5 is probably not "panel power" but a shared peripheral rail** — the
  stock NFC init power-cycles DIO5 before opening I²C. Worth remembering if
  anything ever needs the NFC chip or behaves oddly around DIO5.
- To find out whether our boards actually have the NFC chip fitted: drive DIO5
  high, then read block 0 from I²C address 0x55 on DIO24/25. An ACK proves it
  is populated and returns its UID.

Stock never uses BLE or any proprietary/sub-GHz mode — a structural scan of
the image finds `CMD_IEEE_RX`/`CMD_IEEE_ED_SCAN` command structures and no
`CMD_BLE_*` or `CMD_PROP_*` at all. (The CC2630 is a 2.4 GHz part; the
"900 MHz sub-GHz" claims in `docs/` predate working hardware and are wrong.)
Stock's CPE patch body is byte-identical to our `rf_patches/rf_patch_cpe_ieee.h`,
so `RF_CFG=1` applies exactly the patch stock uses — it simply does not help,
because receive sensitivity was never the limit.

## Radio: what limits a marginal link

Measured 2026-09-19 with the bench tag shielded so both ends sat at about
−74 dBm and check-ins failed roughly a third of the time. Counting the tag's
RTT log against the AP's websocket log over the same window splits the
failures:

```
50 attempts:  28% the AP never heard the request   (uplink frame lost)
               4% the tag missed the AP's reply    (downlink)
              68% succeeded
```

So the limit at a weak spot is **the uplink frame going missing, not receive
sensitivity** — which is why the radio-configuration sweep below found
nothing. The tag now sends the AvailDataReq up to `CHECKIN_TX_TRIES` (3) times
within one wake, with a fresh sequence number and a short varying gap, instead
of giving up and backing off for 30 s. Same spot, same shielding:

| | single frame | 3 tries per wake |
|---|---|---|
| check-ins completed per wake | 68% | **88%** |
| wakes the AP heard at all | 72% | **100%** |

It costs nothing when the link is good (the loop exits on the first reply) and
less than a wasted wake when it is not: early tries listen 800 ms, only the
last one waits the full 5 s.

### Radio configurations (RF_CFG) — no measurable difference

`make RF_CFG=n` selects: 0 = no patches, Contiki-NG overrides (shipped);
1 = + TI's CPE IEEE patch (stock and the OEPL alpha both apply it); 2 = 1 +
the stock firmware's extra overrides (RSSI offset −2 dB, LNA bias trim 15);
3 = 2 + the RFE patch. Four configs x two rounds x 8 min at the attenuated
spot, counting completed check-ins:

```
          round 1   round 2   pooled
cfg 0       60%       71%       66%
cfg 1       63%       71%       67%
cfg 2       57%       60%       59%
cfg 3       77%       70%       74%
```

Mean RSSI stayed within 0.5 dB across every run, so the link did not drift.
Nothing here is significant: the difference between two rounds of the *same*
config (11 points for cfg 0) is as large as the difference between configs,
and cfg 3's 8-point edge is about 1.3 standard errors. Worth noting that cfg 2
reported the same RSSI as the others despite carrying stock's "RSSI −2 dB"
override, which suggests those overrides may not be taking effect at all.

The honest conclusion is that RX sensitivity was never the bottleneck, so
these patches had nothing to fix. Re-open only with a way to vary attenuation
mechanically, which would measure the cliff position directly instead of
inferring it from a success rate.

## Weather display (Home Assistant)

**Two traps in the HA integration, both found 2026-09-19/20.**

*The AP dropdowns in HA can lie, and writing to them is real.*
`select.openepaperlink_ap_maximum_sleep` displays **"shortest (40 sec)"**
while the AP's own config says `maxsleep = 15` (minutes) -- 15 is not in the
integration's option list (`shortest (40 sec)`, `5 min`, `10 min`, `30 min`),
so it falls back to showing the first entry. Reading it is harmless;
*selecting* a value writes it to the AP for real. Picking the displayed value
would set maxsleep to 40 seconds and wreck battery life on every tag. Check
`http://192.168.5.4/get_ap_config` for the truth.

*The integration can accept a push and drop it.* On 2026-09-19 the automation
fired and `open_epaper_link.drawcustom` returned HTTP 200, but nothing ever
reached the AP: no upload in the AP's websocket log, the tag stayed at
`pending=0`. It recovered on its own -- every AP entity in HA re-initialised at
11:00 UTC on 2026-09-20, i.e. the integration reconnected -- and the path has
worked since, with the tag still reporting the same capabilities. So a stale
integration-to-AP connection is the likely cause, not anything on the tag.
`tools/ha_watch.py` now watches for a recurrence: it notices when the
automation's `last_triggered` advances and logs whether an image actually
reached the tag within five minutes. `tools/weather_display.py push MAC` is
the fallback that bypasses HA entirely.


Weather6 is driven by the HA automation `weather_forecast_oepl_display`,
whose `drawcustom` payload is generated by `tools/weather_display.py`:

```bash
tools/weather_display.py preview out.png   # render locally against live HA data
tools/weather_display.py push <MAC>        # render locally, upload straight to the tag
tools/weather_display.py yaml              # the automation config
tools/weather_display.py install           # write it into HA (asks first)
```

The local renderer emulates the integration (PIL, 1-bit font mode, ppb/rbm
fonts, MDI icons — fetched into `tools/weather_assets/` on first use), so the
preview is pixel-identical to what HA sends. Edit `build_payload()`, preview,
push to the tag to judge it on the panel, then `install`. Needs
`~/secrets.toml` with `[homeassistant] bearer_token_local`.

## Power (measured on the bench, 2026-09-16)

### Battery budget (v0.29, measured 2026-09-18 at 3.0 V)

| item | measured | note |
|---|---|---|
| sleep floor | **9.1 µA** | median of quiet samples, debugger detached (p10 8.3, p90 9.5) |
| check-in, nothing pending | ~0.2 µAh | 0.2–0.3 s of radio |
| image update, compressed | 0.046–0.049 mAh | 45–48 s; 0.049 for a weather-layout image |

Weather tag as configured (update every 2 h, check-in every 15 min):

```
sleep      0.0091 mA x 24 h          = 0.22 mAh/day
check-ins  96 x 0.2 uAh              = 0.02 mAh/day
updates    12 x 0.049 mAh            = 0.59 mAh/day
                                       ---------------
                                       0.83 mAh/day
```

4x CR2450 in parallel is 2480 mAh nominal; at ~75% usable against these pulse
loads and a 2.5 V cutoff, ≈1900 mAh → **≈6 years**, at which point the cells'
own self-discharge (~1%/year) matters as much as the tag does. The updates are
now 71% of the budget and sleep 27%.

Where it came from, in order: 59.8 µA originally, 39.3 µA once the panel's
control lines were pulled up (2026-09-18), 9.1 µA once DIO12 (SDA direction)
joined them.

### Sleep floor: what actually mattered

Everything below was measured on the bench with `tools/bench_sleep.sh`, which
builds, flashes, power-cycles and reports the median of the quiet seconds.

| configuration | floor |
|---|---|
| all nine panel lines floating | 59.8 µA |
| pull-up on DIO15 (DC) only | 36.5 µA |
| pull-up on DIO12 (DIR) only | 25.1 µA |
| pull-ups on DIO12 + DIO15 | 8.6 µA |
| **pull-ups on DIO12,13,14,15,18,20** | **9.1 µA** (shipped) |
| …plus the SPI bus (DIO8,9,10) | >2000 µA, never settles |

The panel's inputs were floating, so their input stages drew crowbar current;
DC and DIR are what matter and the rest are worth a few tenths of a µA. The
SPI bus is shared with the external flash and has to stay high-impedance.

Things that were tried and made no measurable difference at this floor, so are
not in the firmware: disabling AON_BATMON between readings (9.1 vs 9.2 µA, and
it broke telemetry — see oepl_hw_get_voltage), and copying the stock
firmware's configuration for the ~20 DIOs nothing else touches (unchanged
standby, and the tag's own supply reading fell to 2203 mV, so those pins draw
current while awake). Powering the external flash down does matter: ~4 µA,
and it is now done on every boot rather than only a cold one.

The stock firmware was disassembled to compare (`reference/stock.bin`, TI-RTOS
+ PowerCC26XX): its CCFG is byte-identical to ours, it uses the same DC/DC
settings and the same standby sequence, and it has nothing in that sequence we
lack — the difference was entirely in pin configuration.



Bench tag `00124B00181880B0` "OEPL-DEBUG" powered by a Nordic PPK2 at 3.0 V,
debugger detached, power-cycled after flashing (a JTAG connection keeps the
CC26xx debug domain on until power is cut, which blocks standby — any reading
taken after a debugger has been attached is not a sleep measurement).

| Build | Sleep floor |
|---|---|
| v0.18 (PERIPH off only, DIO5 high, UART mirror on) | 1950 µA |
| + DIO5 (panel supply) low while asleep | 1790 µA |
| + Contiki-style domain shutdown | 1055 µA |
| sleep-only diagnostic, pins floating | 1205 µA |
| + TI Power_sleep order: AUX released, uLDO requested | 380 µA |
| + SPI flash CS held high (was floating) | 100 µA |
| + SPI flash deep power-down | 93 µA |
| **v0.19 production** (radio + display, pins in defined states) | **~61 µA** |

Dead ends measured along the way: driving the panel lines low while it is
unpowered (+570 µA: pull-ups on the board); DIO5 high (+160 µA: that's the
panel supply, high = on); bypassing the LF clock qualifiers alone (no change,
needed but not sufficient).

Still unexplained: the last ~59 µA (CC2630 standby is ~1–2 µA). Ruled out by
measurement: the J-Link (59.6 µA unplugged vs 59.2 µA attached) and the
battery monitor (59.5 µA with it off during sleep). Probably on the board;
needs a look at the PCB.

### One image update (v0.20, same tag, debugger detached)

| | Receive current | Charge per update |
|---|---|---|
| v0.19 (CPU spinning in wait loops) | 9.99 mA | 0.331 mAh |
| v0.20 (CPU sleeps between 2 ms RTC ticks) | 8.38 mA | 0.267 mAh |

~115 s awake per update: ~75–100 s download + ~30 s refresh. The panel
refresh itself isn't a big spike (~10 mA including radio-off CPU wait).

A failed update can cost far more: before back-off, 1.2 mAh was measured over
9 minutes of fruitless attempts at the bench tag's weak spot. v0.20 backs
off (30 s doubling to 15 min) on failed check-ins and failed updates.

Budget at v0.20 for 4×CR2450 (~2000 mAh usable), 15-min check-ins, 12 image
updates/day: sleep 59 µA → 1.4 mAh/day; check-ins ~0.15 mAh/day; updates
0.27 mAh × 12 → 3.2 mAh/day. ≈ 4.8 mAh/day → on the order of a year before
coin-cell derating, if updates succeed first time.

### Radio link (bench tag at −68 dBm)

`tools/rtt_session.sh 300 log.txt` + `tools/rf_metrics.py log.txt` after an
image push. Burst yield = parts received per 42-frame AP burst.

- Draining the AP's burst before the next request: requests 68 → 43, no-ACK
  32 → 8, download 119 s → 75 s.
- RF_CFG 0–3 (CPE patch, stock overrides, RFE patch): all ~90% yield, 73–88 s.
  No config difference measurable at this signal level.
- Yield varies run to run with the room (76–91% for the same firmware on the
  same day); compare builds back to back, not across hours.

### Bench tools

```bash
tools/ppk.py measure 300 trace.csv        # power the tag at 3.0 V and log current (1 ms averages)
tools/ppk.py hold 60                      # just power it (the PPK2 only sources while a session is open)
tools/ppk_analyze.py trace.csv --from 90  # average, sleep floor (median of quiet seconds), per-10 s table
tools/jflash.sh [bin]                     # JLinkExe: reset, erase, program, verify by readback, run
tools/rtt_log.py 600 log.txt              # timestamped RTT (needs JLinkGDBServer running)
tools/bench_sleep.sh <label> [secs] [settle] [make args]   # build, flash, power-cycle, measure, summarise
tools/update_charge.py trace.csv --from 75  # charge of the update window in a trace
tools/rtt_session.sh 300 log.txt          # power + RTT log (debugger attached: no standby)
tools/rf_metrics.py log.txt               # burst yield, no-ACK, CRC errors, download time
```

Test switches (bench only): `EXTRA_DEFINES="-DBENCH_FORCE_CHECKIN_FAIL -DRETRY_MIN_S=5 -DRETRY_MAX_S=80"`
(or `-DBENCH_FORCE_XFER_FAIL`) exercise the back-off in minutes.

`make DIAG_SLEEP_ONLY=1` builds a firmware that does nothing but standby in a
loop (no radio, no display) and snapshots the power registers into `.noinit`
before each sleep — for separating MCU problems from board problems.

**A running `JLinkGDBServer` stops the tag on any reset it performs itself.**
An OTA apply, a watchdog bite or a fault reset leaves the core halted in ROM
(PC ≈ 0x10003982, ~2.9 mA, silent on the AP) until the server is restarted or
the tag is power-cycled. Test anything that resets — OTA applies above all —
with the server killed, and read `.noinit` (crash ring, `g_ota_apply`)
afterwards instead of watching RTT live.

**A short power gap doesn't reset the tag.** The board's capacitors carry it
through a few seconds in standby (~60 µA), so `off; sleep 3; on` resumes the
old sleep instead of booting. Leave it off for 15 s or more. Note too that
`tools/ppk.py hold` is what holds VOUT up: killing the shell that launched it
leaves the python child (and the power) running — kill the child by PID.

Don't trust gdb's `load` + `compare-sections` on this chip: when the halt
lands in standby, programming silently fails and the comparison is answered
from J-Link's flash cache. `tools/jflash.sh` resets first and reads back.

**A failed transfer backs off the transfer, not the check-in** (v0.25 on). A
tag that keeps failing the same download used to skip check-ins with it, for up
to `RETRY_MAX_S` (15 min), which is indistinguishable from a dead tag at the
AP. Now the tag keeps the AP's cadence and refuses only to *start* a transfer
until the hold expires (`Transfer held Ns more after xN failures` over RTT).
A different `dataVer` — the AP offering a replacement — cuts the hold to one
check-in rather than clearing it, because an AP with several transfers queued
offers a different dataVer every time and clearing outright turns that into a
continuous download loop (measured on the bench).

When nothing is pending the cadence is the AP's `maxsleep` (15 min here), so a
quarter-hour of silence from a healthy tag is normal; a tag with data pending
should be seen every 30–60 s.

**Queueing an OTA makes the tag sleep for an hour.** `contentmode 5` sets
`taginfo->nextupdate = 3216153600` on the AP (contentmanager.cpp), so the
`nextCheckIn` it hands out is enormous and the tag sleeps its 3600 s cap. If
the tag misses the offer — because it was mid-cycle when the OTA was queued —
it goes quiet for a full hour before trying again, which looks exactly like a
dead tag. Queue OTAs when the tag is awake and checking in every 30–60 s, and
don't panic before the hour is up (seen on Weather6, 2026-09-18: silent
13:33→14:35, then fine).

Check-in cadence is the AP's: `min(minutes until the content's TTL, maxsleep)`,
sent only if > 1 min and only when `stopsleep=0` or no web UI is connected.
HA `drawcustom` defaults `ttl` to 60 s → 1-minute check-ins. The AP is set to
`maxsleep=15`, `stopsleep=0`; the weather automation sends `ttl: 7200`.
**`maxsleep` must stay below 20**: the AP's radio drops pending data after
20 housekeeping minutes (`MAX_XFER_ATTEMPTS`), so a tag sleeping longer than
that can miss an image pushed just after its check-in (seen with 30).

### Long-running

`tools/monitor_48h.sh` polls the AP for a soak test.
