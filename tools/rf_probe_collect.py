#!/usr/bin/env python3
"""Collect RF_PROBE samples from the AP for one tag.

    tools/rf_probe_collect.py MAC [minutes] [out.csv]

Polls get_db, records one row per new check-in (lastseen changes): the
configuration the tag used (LQI - 0x50) and the RSSI it measured of the AP.
Prints a per-configuration summary at the end (and on Ctrl-C).
"""
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ap  # noqa: E402

NAMES = {0: "A as-shipped", 1: "B +CPE", 2: "C CPE+stock ovr", 3: "D CPE+RFE+stock ovr", 4: "E stock ovr only"}


def main():
    mac = sys.argv[1].upper()
    minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 40
    out = sys.argv[3] if len(sys.argv) > 3 else f"rf_probe_{mac}.csv"
    rows = []
    last = None
    t_end = time.time() + minutes * 60
    with open(out, "a") as f:
        try:
            while time.time() < t_end:
                try:
                    t = ap.tag(mac)
                except Exception:
                    time.sleep(10)
                    continue
                if t and t["lastseen"] != last and t["LQI"] >= 0x50 and t["LQI"] < 0x50 + 8:
                    last = t["lastseen"]
                    cfg = t["LQI"] - 0x50
                    rows.append((cfg, t["RSSI"], t["wakeupReason"]))
                    line = f"{time.strftime('%H:%M:%S', time.localtime(last))},{cfg},{t['RSSI']},{t['wakeupReason']:#x},{t['batteryMv']}"
                    print(line, flush=True)
                    f.write(line + "\n"); f.flush()
                time.sleep(10)
        except KeyboardInterrupt:
            pass
    print("\nconfig                     n   mean   min  max   faults")
    for cfg in sorted({r[0] for r in rows}):
        rs = [r[1] for r in rows if r[0] == cfg]
        faults = sum(1 for r in rows if r[0] == cfg and r[2] == 0xFE)
        print(f"{NAMES.get(cfg, cfg):24s} {len(rs):3d}  {statistics.mean(rs):6.1f}  {min(rs):4d} {max(rs):4d}   {faults}")


if __name__ == "__main__":
    main()
