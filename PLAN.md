# Plan

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done (validated)

## v0.8–v0.10 — image transfers (bug report "One Buffer, 714 Packets", 2026-09-13)

Symptom: tags check in fine but never display an image; AP hash stays
all-zero; every check-in triggers a full panel refresh of a partial image.

Findings on the bench (Weather6, `00124B0018177B31`, RSSI −67, AP 192.168.5.4):

- The report's cause (single-entry RX queue) is real and fixed, but on this AP
  it was not sufficient: v0.8 (ring only) still never completed an image.
- The dominant cost was the fixed 15 s RX window per block request: any lost
  part meant sitting out the full window, so ~18 block fetches took longer than
  the AP keeps data pending (~8.5 min observed), the AP dropped it mid-download,
  and the tag painted a partial image every cycle. Measured: the 5-block OTA
  took ~4.5 min on v0.8 and ~25 s on v0.9.
- v0.9 (300 ms burst-idle timeout) completed an image, but only after ~8 cycles.
- v0.10 (2 s idle / 40 s window) confirmed an image in 2 cycles (~6 min from
  push), first confirmation ever recorded for this tag — but only ~50% of
  cycles. The AP's websocket log (`tools/ap_log.py`) showed every download
  completing (2–3 requests per block, ~100 s per image) and then *no*
  "reports xfer complete": the single unacknowledged `XferComplete` was being
  transmitted the instant `DISPLAY_REFRESH` was issued (BUSY reads HIGH, so the
  wait exited immediately), i.e. during the panel's peak current draw.
- AP-side rules (from the C6 AP source): 20 min budget per offered image
  (`attemptsLeft=20`, one housekeeping tick/min) → "xfer timeout"; a
  `BLOCK_REQUEST` for an already-buffered block still re-fetches it from the
  ESP32 (`pleaseWaitMs=550`), `BLOCK_PARTIAL_REQUEST` serves from the buffer
  (`pleaseWaitMs=30`); every `XferComplete` is answered with `XferCompleteAck`.
- v0.11: refresh waited out (30 s fixed unless BUSY is seen LOW), panel powered
  off + deep-slept after every refresh (never was before), `XferComplete`
  retried until acked, partial requests for retries. First-cycle success.
- v0.11 also caught one HardFault via the new handler (`wake=0xFE`, recovered by
  reset). Root cause unknown — PC went only to RTT. v0.12 puts the record on
  the splash and in the first post-crash check-in's telemetry fields.

- [x] 8-entry RX ring + read cursor; release entry on malformed-frame path (v0.8)
- [x] Burst idle-timeout on block reception, timed by the RF core RAT (v0.9/v0.10)
- [x] HardFault: record to `.noinit`, reset, report `wakeupReason=0xFE` to AP (was: spin forever)
- [x] Bounded RF doorbell; hang → recorded pseudo-fault + reset (driverlib's loops never return)
- [x] RTT `BP:` line now reports parts-per-request, measured inter-part gap, RF core counters
- [x] `tools/ap.py` — OTA push / image push / poll AP from the dev host (paginated `get_db`)
- [x] `tools/ap_log.py` — stream the AP's websocket log (block requests, xfer complete, timeouts)
- [x] Refresh wait + panel power-off/deep-sleep after every update (`uc8159_refresh_and_sleep`)
- [x] `XferComplete` acked + retried; OTA uses it too
- [x] `BLOCK_PARTIAL_REQUEST` for retries
- [x] Fault record on splash + one-shot in check-in telemetry (`tools/ap.py status` decodes it)
- [x] OTA v0.7→…→v0.12 over the air, all applied cleanly
- [x] Image confirmed on Weather6 first cycle on v0.11 (265 s from push)
- [x] 3 timed trials on v0.12: 470 s (2 cycles), 192 s (1 cycle), never (5+ cycles). ~40% of cycles confirm.
- [x] v0.13: 30 s minimum refresh wait; BlockData checksum verified in the image path
      (note: cannot catch the AP's stale-buffer relabel — the header travels with the data)
- [x] v0.14: `make DIAG=1` build reports failed blocks / XferComplete outcome / request
      count / RF CRC+overflow counters in the next check-in (`OEPL_DIAG=1 tools/ap.py status`)
- [x] Block-idle timer only extended by our own ACK/parts (other tags' frames were starving it);
      `CANCEL_XFER` honoured with a 1.5 s back-off
- [x] Watchdog (90 s, kicked from delay/RX-poll primitives, stops in standby); WDT reset
      reported as fault PC `0xDEADD006`
- [x] Weather6 went silent 11:23 (2026-09-13) taking the v0.14 OTA. Battery pull next day →
      boot loop with `FAULT PC=DEAD0DB0` (RF doorbell hang) on the splash and at the AP: the
      crash-reporting path works. Splash read 2.83 V (3.16 V the morning before, ~50 refreshes).
      Marginal replacement batteries + moving it next to the AP: no more faults. **Root cause:
      battery.** Yesterday afternoon's ~50% cycle failures were the same battery plus the
      BUSY-glitch refresh exit.
- [x] v0.15 DIAG trials (2026-09-14): 3/3 first cycle (209/218/158 s). Diagnostics:
      `failed_blocks=0`, `xfer=acked@1`, `rf_buf_full=0`, `rf_crc_err≤1`, ~3 requests per block
      (55–57 for 18 fetches) — the AP delivers a full block on the first ask only sometimes;
      ring depth and RF are not the limit.
- [x] Production v0.15: 2/3 first cycle (167 s, 152 s); the miss coincided with the AP rebooting
      (it does that — the "DB snapshot reverted" events yesterday were the same).
- [x] DIAG builds report version with bit 15 set (32783) so a debug build can't be mistaken
      for production at the AP.
- [x] Commit, update README (pushed 2026-09-14)

## Weather display (2026-09-14)

- [x] Redesigned the HA `drawcustom` layout for Weather6 (`tools/weather_display.py`):
      header with current conditions + details, 8-column 16 h hourly strip, 5-day columns,
      in-bounds footer. Installed as `weather_forecast_oepl_display`, refresh every 2 h at :05.
- [x] v0.16: no panel power-on at boot (was left powered through every sleep); AP `maxsleep=30`,
      `stopsleep=0`; automation `ttl: 7200` → Weather6 checks in every 30 min instead of every minute.
- [ ] Battery life on a fresh set with 12 refreshes/day — watch `batteryMv` in the AP DB.
      Open by nature: the estimate is now ~6 years (0.83 mAh/day), so this is a long-run
      observation rather than a task. Weather6 has been the reference tag since 2026-09-13.
      First-principles estimate if standby is real: ~9 mAh/day → 5–8 months (see DEVELOPMENT.md).
- [x] Bench: PPK2 as supply/meter, J-Link flashing with readback verify, RTT, scripted
      flash → power-cycle → measure loop (2026-09-16).
- [x] Sleep current 1950 µA → ~61 µA (v0.19): TI standby sequence, AUX release + uLDO, panel
      supply off with lines released, flash CS high + deep power-down. Table in DEVELOPMENT.md.
- [x] Remaining ~59 µA: not the J-Link, not the battery monitor (measured). Board photo pending.
- [x] Image update measured: 0.331 mAh (v0.19) → 0.267 mAh (v0.20, CPU idles between RTC ticks).
- [x] Drain the AP burst before the next block request (download 119 → 75 s at −68 dBm).
- [x] RF_CFG A/B (CPE patch, stock overrides, RFE patch): no difference at −68 dBm.
- [x] Back-off on failed check-ins and updates (30 s doubling to 15 min), bench-tested with forced failures.
- [x] Soak v0.20, 14 h: all 28 image pushes landed, but the RF doorbell hung 6 times
      (~every 2.3 h) on a steady 3.0 V supply — each time recorded, reset and recovered.
      **Weather6 stays on v0.18 until this is understood.**
- [x] Crash capture (last ~1 KB of log + registers, frozen in .noinit) + tools/crash_dump.py;
      fault reports carry a class marker the AP can't erase (v0.21). Verified with forced faults.
- [x] HardFault record was garbage (review M8 — MSP read after the prologue). Naked entry;
      verified: PC/LR now point at the forced fault site.
- [x] **Found it** (2026-09-17, capture in scratchpad/repro/capture1): a scan that reaches OEPL
      channel 27 (only when the AP's reply is missed on the earlier channels) hands CMD_IEEE_RX
      an illegal channel -> IEEE_ERROR_PAR -> CPE INTERNAL_ERROR -> the doorbell stops answering,
      including the CMD_ABORT sent to clean up. Upstream OEPL lists 27 for Telink radios; the
      CC2630 accepts 11-26 only. Fixed in v0.22 (skip unsupported channels), plus: a hung
      doorbell no longer resets the tag -- the RF core is powered off before sleep and
      re-initialised on the next wake, and the event is reported to the AP.
      This also explains Weather6's boot loop on weak batteries: missed PONG -> channel 27.
- [x] Confirmed on v0.23, 16 h unattended (2026-09-17 12:58 → 09-18 04:58): **0 radio hangs**
      (v0.20 had 6 in 14 h), 32/32 pushed images displayed, typically 2.5–3 min each,
      1133 check-ins with no unexplained gaps.
- [~] Weather6 updated from v0.18 to v0.23 over the air (2026-09-18). Run 1 (2026-09-16
      17:00) caught every download failing on AP stalls → patience fix 6ce6be1; run 2 from 17:24.

## External review (2026-09-16)

PRs #5–#7 (spectrumjade, 2026-08-16) and PeitzGreene's fork with REVIEW-FINDINGS.md (65 findings).
Most findings were already fixed and measured in v0.19/v0.20; PRs are superseded.

- [x] Closed PRs #5–#7 with thank-you comments pointing at the superseding commits (2026-09-16)
- [x] M17: learn the AP address from AvailDataInfo. A/B on the bench with BENCH_FORCE_SCAN_FAIL:
      without it 12 requests / 12 unanswered / download fails; with it DATA OK, 89.7% burst yield.
- [x] H10: RF core flushes frames rejected by the address filter (bAutoFlushIgn=1); downloads
      unaffected (89.7% yield in the same run).
- [x] H1/M2: Makefile header deps, flags stamp, link deps — verified (no-op, header touch, flag change)
- [x] OTA robustness (H8, M14, M15, L19–L22) — v0.24, bench-verified 2026-09-18 (see Next)
- [x] M21: OEPL channel 27 isn't valid for CMD_IEEE_RX — fixed in v0.22, documented in
      DEVELOPMENT.md ("the channel-27 trap") and README
- [x] Radio configs at the real edge — done 2026-09-19 with the tag shielded to ≈−74 dBm.
      No config differs significantly; the failures were uplink, not sensitivity. See
      DEVELOPMENT.md, "Radio: what limits a marginal link"
- [x] Identify the remaining ~59 µA — done 2026-09-18: the panel's DC and DIR lines were
      floating. Sleep floor is now 9.1 µA

## Next

- [x] **OTA robustness (H8, M14, M15, L19–L22)** — done 2026-09-18, v0.24. The apply now checks
      every erase/program status and reads each sector back with the VIMS cache invalidated,
      retrying a bad sector up to 5 times; the "applied" dataVer record is written from the RAM
      function *after* the copy verifies (an interrupted apply retries instead of being skipped
      forever); the RF core is shut down and the supply checked (≥ 2400 mV, also before the
      download starts) before the first erase; the staged reset vector must have the Thumb bit
      set and point inside the image. Sector 30's record also carries what the copy did, and the
      installed image reports it once — RTT `OTA apply: sectors=06 retries=00 bad=00` and
      `OTA-APPLIED` in `tools/ap.py status`.
      Bench (tag 00124B00181880B0, PPK2 at 3.0 V): 6 OTA cycles, all applied and rebooted, one in
      **92 s** from queue to the new version checking in. A bad-vector image (Thumb bit cleared —
      accepted by the old bound) is rejected before any erase and the tag keeps running and backs
      off. With `BENCH_OTA_FLAKY_APPLY` sabotaging sector 0's first program, the tag still came up
      on the new image: without the readback+retry that sector would have stayed erased, i.e. a
      brick. Bench gotcha found and documented: a running `JLinkGDBServer` halts the tag on any
      reset it performs itself, so reset paths must be tested with the server killed.
- [x] **A failed transfer no longer silences the tag** (2026-09-18, v0.25). Found while
      bench-testing rejected OTA images: the update back-off was applied to the sleep
      interval, so a tag offered a broken image stopped checking in for up to 15 minutes and
      looked dead. The back-off now gates the transfer only. A/B on the bench with a
      bad-vector image queued: before, silence for 15+ min; after, 14 consecutive check-ins
      over 10 min with no re-download (`tools/ap_log.py` shows no block requests).
- [x] **The ~59 µA sleep floor** (2026-09-18): a third of it was the panel's control lines
      floating. Pulled up during sleep the floor is **38.8 µA**; the shared SPI bus must stay
      high-impedance. A tag whose panel has never been powered reaches 29.9 µA with every panel
      line pulled up, so ~10 µA more is available once the bus side is understood. Every other
      pin group made things worse. The remaining ~39 µA is board-side (the MCU's own standby is
      ~1 µA) and has no firmware control found so far.
- [x] **Battery budget re-measured** (2026-09-18, v0.26): see DEVELOPMENT.md. 4.17 mAh/day for
      the weather tag's cadence → ≈450 days on 4x CR2450, up from ≈410.
- [x] **Decode zlib images (dataType 0x30)** — done 2026-09-18, v0.27. `firmware/inflate.c`
      is a ~350-line DEFLATE/zlib decoder sized for this tag: input addressed in flash (so no
      resumable state machine), a caller-supplied 4 KB window (`red_buf`, matching the AP's
      4 KB dictionary), output through a sink, ~800 bytes of state. The image path stages the
      compressed picture in flash like firmware, then decodes it into the panel in one pass —
      plane 1 parked in flash, plane 2 interleaved against it row by row — and checks the
      stream's Adler-32 before refreshing.
      Measured on the bench: the same picture is **2.2 KB on the air instead of 67.2 KB**, one
      block instead of 17, confirmed in 30 s instead of 2-3 min, **0.046 mAh per update against
      0.267**, decode ~1 s, 1.8 KB of stack still untouched. Daily budget 4.17 -> 1.51 mAh,
      so ~450 days -> **~3.4 years** on 4x CR2450.
      Tested byte-exact on the host against the AP's own compressor (six images including
      incompressible data) and fuzzed with 20,000 corrupt streams under ASan/UBSan.

- [x] **Sleep floor 39 µA → 9.1 µA** (2026-09-18, v0.29). The panel's SDA-direction line
      (DIO12) was the missing piece: pulled up alongside DC (DIO15) the floor drops from
      39.3 µA to 8.6, and with the other three control lines 9.1 µA. Per-pin measurements and
      the things that did *not* help (BATMON gating, configuring the ~20 idle DIOs the way the
      stock firmware does — which also sagged the tag's supply reading to 2203 mV) are in
      DEVELOPMENT.md. The stock firmware was disassembled for comparison: identical CCFG, DC/DC
      and standby sequence, so pin configuration was the whole difference.
      Daily budget 1.54 → **0.83 mAh**, i.e. ~6 years on 4x CR2450, where cell self-discharge
      starts to matter as much as the tag. Updates are now 71% of the budget.

- [x] **NFC** (2026-09-19, v0.30). The second antenna turned out to be an NFC coil with a
      passive NTAG I2C chip on DIO24/25 — no second transmitter. The tag now writes its own
      identity there at cold boot and accepts AP-pushed content, so OEPL's "Set NFC URL"
      content mode works on these tags for the first time. Verified by phone: identity record,
      then a pushed URL that opened.

## Follow-ups (not blocking)

- **NFC wake** (`CAPABILITY_NFC_WAKE`, `WAKEUP_REASON_NFC`): DIO21 is the chip's field-detect
  pin, open-drain, confirmed pulsing low while a phone reads the coil (2026-09-19). Waking the
  tag on a tap would need it configured as an AON edge wake source. Unknown: whether the pulse
  is long enough to catch reliably, and what it costs in standby to keep the input armed.

- [x] **Radio link margin** — answered 2026-09-19, and it was not sensitivity. With the tag
      shielded to ~−74 dBm, 28% of check-ins failed because the AP never heard the request
      against 4% where the tag missed the reply. The RF_CFG sweep (patches/overrides, 4 configs
      x 2 rounds) found no significant difference, as expected once the failures are known to be
      uplink. Fixed instead by retrying the AvailDataReq up to 3x within one wake: 68% -> 88%
      of wakes complete, and the AP now hears every wake. See DEVELOPMENT.md.
- [x] Fault report is no longer consumed on TX: it stays pending until the AP answers
  (2026-09-19). The check-in carrying a crash report is the one most likely to fail.
- AP `maxsleep` must stay < 20 min (radio drops pending data after 20 housekeeping minutes).

- The one v0.11 HardFault (2026-09-13 10:16) was never identified; it happened at 2.9 V and
  has not recurred. Any recurrence now shows `FAULT pc=` in `tools/ap.py status`.
- ~3 block requests per block (~100 s per image) is AP-side (C6 serves after `pleaseWaitMs`
  whether or not the ESP32 has delivered). Options: AP firmware update (check whether newer
  releases wait for the block), or the "high-speed serial" AP option (`pleaseWaitMs` 140).
- Battery: ~50 BWR refreshes took a coin-cell set from 3.16 V to 2.83 V in a day. The tag now
  powers the panel off after every refresh (it never did before v0.11); measure the sleep
  current with a meter to see what's left.
- RTT `BP:` lines (parts per request, gap, RF counters) are there for the next J-Link session.

Acceptance (met): a v0.7 tag updated over the air displays a pushed image and
the AP records a matching hash. Stretch (open): first-cycle success.

## Backlog (from README known issues — not scheduled)

- ~~DIO13 (BUSY) always reads HIGH~~ — resolved: refreshes end on BUSY at ~7.7 s (2026-09-19)
- ~~AON_RTC CH0 compare event unreliable~~ — resolved by the standby rework (v0.19)
- UART TX debug output unverified (and off by default; it kept the serial domain powered)
- Channel-11 congestion causes occasional part loss — much less exposure now that a
  compressed image is 1-2 blocks instead of 17
