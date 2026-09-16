#!/usr/bin/env python3
"""Charge used by an image update in a PPK2 trace (tools/ppk.py measure ... out.csv).

    tools/update_charge.py trace.csv --from SECS

Finds the longest stretch after --from where 1 s averages stay above 1 mA
(allowing gaps up to 5 s: retries), and reports its duration, charge, and the
typical current while receiving (median of 1 s averages in 6-15 mA) and while
refreshing (> 15 mA).
"""
import statistics
import sys

path = sys.argv[1]
t_from = float(sys.argv[sys.argv.index("--from") + 1]) if "--from" in sys.argv else 0
buckets = {}
for line in open(path):
    try:
        t, i = map(float, line.split(","))
    except ValueError:
        continue
    if t >= t_from:
        buckets.setdefault(int(t), []).append(i)
secs = sorted(buckets)
avg = {s: sum(buckets[s]) / len(buckets[s]) for s in secs}
runs, cur = [], []
for s in secs:
    if avg[s] > 1000:
        if cur and s - cur[-1] > 5:
            runs.append(cur); cur = []
        cur.append(s)
if cur:
    runs.append(cur)
if not runs:
    sys.exit("no activity found")
run = max(runs, key=lambda r: r[-1] - r[0])
t0, t1 = run[0], run[-1] + 1
charge = sum(avg.get(s, 0) for s in range(t0, t1)) / 3600 / 1000   # mAh (missing seconds count as 0)
rx = [avg[s] for s in range(t0, t1) if s in avg and 6000 <= avg[s] <= 15000]
ref = [avg[s] for s in range(t0, t1) if s in avg and avg[s] > 15000]
print(f"update window {t0}-{t1} s ({t1 - t0} s), charge {charge:.3f} mAh")
if rx:
    print(f"  receive-like seconds: {len(rx)}, median {statistics.median(rx) / 1000:.2f} mA")
if ref:
    print(f"  >15 mA seconds: {len(ref)}, median {statistics.median(ref) / 1000:.2f} mA")
