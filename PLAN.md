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
- [ ] Soak v0.20 on the bench tag (debugger detached), then OTA it to Weather6.
- [ ] Radio configs at the real edge (−77 dBm or worse) if a spot like that is available.
- [ ] Identify the remaining ~59 µA from the board photo.

## Follow-ups (not blocking)

- **Radio link margin** (2026-09-15): the tag fails below ≈−77 dBm while its RX sensitivity
  should be ≈−95. Facts: the firmware applies *no* RF patches; stock and alpha both apply
  `rf_patch_cpe_ieee` (found in both binaries) and neither applies MCE/RFE; the stock override
  table (stock.bin @0xF070) adds `0x000288A3` (RSSI −2 dB reporting), `0x000F8883` (LNA bias
  trim offset 15 vs TI generic 3) and `0x00018063`. `make RF_PROBE=1` rotates five configs
  per check-in and reports the config in LQI (`tools/rf_probe_collect.py`): at −46 dBm all
  five read identically (uninformative), and **the probe build broke image downloads**
  (block 1 rejected by the checksum on every config; OTA blocks fine) — cause unknown, so
  its results are void. Next: J-Link on the bench, one config per build, at the far spot
  (−77…−82): measure check-in success rate and `BP:` parts-per-request. Candidates in
  order: +CPE patch; +stock overrides; +CPE+RFE.
- Fault report is consumed on the first AvailDataReq TX even if the AP never hears it
  (`oepl_radio_cc2630.c`); keep it pending until a checkin succeeds.
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

- DIO13 (BUSY) always reads HIGH; refresh wait runs to full timeout
- AON_RTC CH0 compare event unreliable (RTC polling workaround in place)
- UART TX debug output unverified
- Channel-11 congestion causes occasional part loss (ring buffer should help; re-measure)
