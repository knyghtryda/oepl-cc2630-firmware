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

### Long-running

`tools/monitor_48h.sh` polls the AP for a soak test.
