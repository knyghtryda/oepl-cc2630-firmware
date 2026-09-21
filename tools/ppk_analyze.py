#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Summarise a PPK2 trace written by `tools/ppk.py measure SECS out.csv`.

    tools/ppk_analyze.py trace.csv [--from SECS] [--to SECS] [--quiet-uA N]

The CSV holds 1 ms average current (uA) with a host timestamp. Reports:
  - overall average current and charge over the window
  - the "quiet" level: median current of 1 s windows whose max stays below
    --quiet-uA (default 2000), i.e. what the tag draws while asleep
  - time spent above the quiet threshold (awake) and the charge it used
  - a per-10 s table so wakeups are visible
"""
import statistics
import sys


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    t_from = float(args[args.index("--from") + 1]) if "--from" in args else 0.0
    t_to = float(args[args.index("--to") + 1]) if "--to" in args else 1e12
    quiet = float(args[args.index("--quiet-uA") + 1]) if "--quiet-uA" in args else 2000.0

    ts, ua = [], []
    with open(path) as f:
        for line in f:
            try:
                t, i = line.split(",")
                t, i = float(t), float(i)
            except ValueError:
                continue
            if t_from <= t <= t_to:
                ts.append(t)
                ua.append(i)
    if not ua:
        print("no samples in window")
        return 1

    n = len(ua)
    dur = n / 1000.0          # one sample per ms
    avg = sum(ua) / n
    wall = ts[-1] - ts[0]     # the Pi drops sample blocks; charge uses wall time
    print(f"window {ts[0]:.0f}-{ts[-1]:.0f} s, {n} ms samples ({dur:.0f} s of data)")
    print(f"average {avg:.0f} uA, charge over {wall:.0f} s: {avg * wall / 3600 / 1000:.4f} mAh")

    # 1-second windows
    sec = [ua[i:i + 1000] for i in range(0, n - 999, 1000)]
    quiet_secs = [statistics.median(w) for w in sec if max(w) < quiet]
    awake = [w for w in sec if max(w) >= quiet]
    if quiet_secs:
        print(f"quiet level (median of {len(quiet_secs)} quiet seconds): "
              f"{statistics.median(quiet_secs):.1f} uA  "
              f"[p10 {sorted(quiet_secs)[len(quiet_secs) // 10]:.1f}, "
              f"p90 {sorted(quiet_secs)[len(quiet_secs) * 9 // 10]:.1f}]")
    else:
        print(f"no quiet seconds (every second peaks above {quiet:.0f} uA)")
    if awake:
        a = sum(sum(w) for w in awake) / (len(awake) * 1000)
        print(f"awake seconds: {len(awake)} of {len(sec)}, average {a:.0f} uA while awake")

    print("\n  t(s)   avg_uA   median_uA   max_uA")
    for i in range(0, len(sec), 10):
        block = [x for w in sec[i:i + 10] for x in w]
        print(f"{ts[i * 1000]:6.0f} {sum(block) / len(block):8.0f} "
              f"{statistics.median(block):10.1f} {max(block):8.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
