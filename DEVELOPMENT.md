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

### When a tag misbehaves, check the battery first

The whole afternoon of 2026-09-13 was spent on failures that were a coin-cell
set sagging from 3.16 V to 2.83 V: lost `XferComplete`, then a radio-core hang
on every download. The splash shows the voltage; the AP shows it on every
check-in. The AP also reboots now and then — `tools/ap_log.py` disconnecting,
or a tag's DB record jumping back to an older state, is that.

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
check-in shows `wakeupReason=254` in the AP DB.

## Weather display (Home Assistant)

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

Don't trust gdb's `load` + `compare-sections` on this chip: when the halt
lands in standby, programming silently fails and the comparison is answered
from J-Link's flash cache. `tools/jflash.sh` resets first and reads back.

Check-in cadence is the AP's: `min(minutes until the content's TTL, maxsleep)`,
sent only if > 1 min and only when `stopsleep=0` or no web UI is connected.
HA `drawcustom` defaults `ttl` to 60 s → 1-minute check-ins. The AP is set to
`maxsleep=15`, `stopsleep=0`; the weather automation sends `ttl: 7200`.
**`maxsleep` must stay below 20**: the AP's radio drops pending data after
20 housekeeping minutes (`MAX_XFER_ATTEMPTS`), so a tag sleeping longer than
that can miss an image pushed just after its check-in (seen with 30).

### Long-running

`tools/monitor_48h.sh` polls the AP for a soak test.
