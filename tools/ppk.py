#!/usr/bin/env python3
"""Nordic PPK2 control for the bench tag: it is the tag's power supply
(source-meter mode) and current meter.

    tools/ppk.py on [mV]            enable VOUT at mV (default 3000; refuses > 3300)
    tools/ppk.py off                disable VOUT
    tools/ppk.py hold SECS [mV]     keep VOUT on for SECS seconds (no measuring)
    tools/ppk.py cycle [mV] [secs]  off, wait, on  (power-cycle the tag)
    tools/ppk.py measure SECS [out.csv] [--stats-every N]
                                    stream current at 100 kS/s for SECS seconds; prints
                                    avg/min/max per second and the overall average

The PPK2 powers up with VOUT off and only turns it on when told; USB 5 V
never reaches VOUT. `on`/`cycle` never leave it above PPK_MAX_MV.

NOTE: VOUT stays on only while a session holds the serial port. `on` alone
therefore powers the tag just for the life of the command; use `measure`
(or keep a session open) to hold power. `hold SECS` does exactly that.
"""
import os
import sys
import time

from ppk2_api.ppk2_api import PPK2_API

PPK_MAX_MV = 3300
DEFAULT_MV = 3000


def open_ppk():
    # The PPK2 can drop off USB and re-enumerate when a session closes; give
    # it a few seconds to come back before giving up.
    devs = []
    for _ in range(20):
        devs = PPK2_API.list_devices()
        if devs:
            break
        time.sleep(0.5)
    if not devs:
        sys.exit("no PPK2 found")
    port = devs[0] if isinstance(devs[0], str) else devs[0][0]
    p = PPK2_API(port, timeout=1, write_timeout=1, exclusive=True)
    p.get_modifiers()
    p.use_source_meter()
    return p


def read_samples(p, secs):
    """Collect raw current samples (uA) for secs seconds; returns a list."""
    out = []
    t0 = time.time()
    while time.time() - t0 < secs:
        raw = p.get_data()
        if raw:
            s, _ = p.get_samples(raw)
            out.extend(s)
        else:
            time.sleep(0.005)
    return out


def power_on(p, mv):
    if mv > PPK_MAX_MV:
        sys.exit(f"refusing {mv} mV (> {PPK_MAX_MV} mV)")
    p.set_source_voltage(mv)
    p.toggle_DUT_power("ON")
    print(f"VOUT on, {mv} mV", file=sys.stderr)


def power_off(p):
    p.toggle_DUT_power("OFF")
    print("VOUT off", file=sys.stderr)


def measure(p, secs, out=None, stats_every=1.0, mv=DEFAULT_MV):
    # Each run is a fresh session: the PPK2 stops sourcing when the previous
    # session closed, so re-enable VOUT (measuring requires it anyway).
    power_on(p, mv)
    p.start_measuring()
    # The first ~0.5 s of samples after enabling are garbage (huge spikes)
    read_samples(p, 0.5)
    f = open(out, "w") if out else None
    t0 = time.time()
    t_last = t0
    win = []
    total = 0.0
    n_total = 0
    print(f"{'t':>6s} {'avg_uA':>10s} {'min_uA':>9s} {'max_uA':>10s}", flush=True)
    # The PPK2 streams ~100 kS/s; writing every sample to disk on the Pi falls
    # behind and the serial buffer overruns. Log 1 ms averages instead.
    DECIM = 100
    try:
        while time.time() - t0 < secs:
            raw = p.get_data()
            if raw:
                samples, _ = p.get_samples(raw)
                win.extend(samples)
                if f:
                    ts = time.time() - t0
                    for i in range(0, len(samples) - DECIM + 1, DECIM):
                        chunk = samples[i:i + DECIM]
                        f.write(f"{ts:.3f},{sum(chunk) / DECIM:.1f}\n")
            else:
                time.sleep(0.002)
            now = time.time()
            if now - t_last >= stats_every and win:
                avg = sum(win) / len(win)
                print(f"{now - t0:6.1f} {avg:10.1f} {min(win):9.1f} {max(win):10.1f}", flush=True)
                total += sum(win)
                n_total += len(win)
                win = []
                t_last = now
    except KeyboardInterrupt:
        pass
    finally:
        p.stop_measuring()
        if f:
            f.close()
    if n_total:
        print(f"overall average {total / n_total:.1f} uA over {n_total} samples "
              f"({n_total / (time.time() - t0):.0f} S/s)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    p = open_ppk()
    if cmd == "on":
        power_on(p, int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MV)
    elif cmd == "off":
        power_off(p)
    elif cmd == "cycle":
        mv = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MV
        wait = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
        power_off(p)
        time.sleep(wait)
        power_on(p, mv)
    elif cmd == "hold":
        mv = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_MV
        power_on(p, mv)
        # Confirm the output is really sourcing: the PPK2 re-enumerates after
        # every session close, and a half-open session leaves the tag dark.
        p.start_measuring()
        s0 = read_samples(p, 1.0)[-5000:]
        p.stop_measuring()
        if not s0 or max(s0) < 20:          # a powered tag always draws > 20 uA at boot
            sys.exit(f"hold: VOUT did not come up (max {max(s0) if s0 else 'no'} uA)")
        print(f"hold: tag drawing, {sum(s0) / len(s0):.0f} uA avg over 1 s", file=sys.stderr)
        secs = float(sys.argv[2])
        try:
            time.sleep(secs)
        except KeyboardInterrupt:
            pass
    elif cmd == "measure":
        secs = float(sys.argv[2])
        out = None
        stats_every = 1.0
        rest = sys.argv[3:]
        if "--stats-every" in rest:
            i = rest.index("--stats-every")
            stats_every = float(rest[i + 1])
            del rest[i:i + 2]
        if rest:
            out = rest[0]
        measure(p, secs, out, stats_every)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
