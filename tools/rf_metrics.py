#!/usr/bin/env python3
"""Link-quality metrics from an RTT log of block downloads and check-ins.

    tools/rf_metrics.py rtt.txt [more.txt ...]

burst yield: for block requests that got any parts, parts received / 42
             (the AP always sends a 42-frame burst)
noACK:       requests that got no BlockRequestAck
checkins:    AvailDataInfo received / attempted
download:    seconds from the first BlockRequest of block 0 to DATA OK
"""
import re
import sys

def ts(line):
    m = re.search(r"\+\s*([0-9.]+)\]", line)
    return float(m.group(1)) if m else None

for path in sys.argv[1:]:
    lines = open(path, errors="replace").read().splitlines()
    bp = [l for l in lines if "BP:" in l]
    got = []
    noack = 0
    nok = 0
    for l in bp:
        m = re.search(r"new=([0-9A-F]{2})", l)
        new = int(m.group(1), 16) if m else 0
        if "noACK" in l:
            noack += 1
        if new:
            got.append(min(new, 42))
        m = re.search(r"nok=([0-9A-F]{2})", l)
        nok += int(m.group(1), 16) if m else 0
    ok = sum("Got AvailDataInfo" in l for l in lines)
    fail = sum("No AvailDataInfo" in l for l in lines)
    t0 = next((ts(l) for l in lines if "B00BRQ" in l), None)
    t1 = next((ts(l) for l in lines if "DATA OK" in l or "DATA PARTIAL" in l), None)
    dl = f"{t1 - t0:.0f} s" if t0 is not None and t1 is not None else "incomplete"
    result = "DATA OK" if any("DATA OK" in l for l in lines) else ("partial" if any("DATA PARTIAL" in l for l in lines) else "-")
    yld = sum(got) / (42 * len(got)) if got else 0
    print(f"{path.split('/')[-1]:18s} requests {len(bp):3d}  noACK {noack:3d}  burst yield {yld*100:5.1f}% "
          f"(n={len(got)})  CRC err {nok:4d}  checkins {ok}/{ok+fail}  download {dl} {result}")
