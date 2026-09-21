#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Watch the Home Assistant -> AP image path and record when it breaks.

On 2026-09-19 the weather automation fired, `open_epaper_link.drawcustom`
returned 200, and nothing ever reached the AP; it started working again after
the integration reconnected to the AP (every AP entity in HA re-initialised at
11:00 UTC on 2026-09-20). It is not reproducible on demand, so this watches
for a recurrence instead of guessing.

    tools/ha_watch.py [logfile]        (default /tmp/oepl_ha_watch.log)

Each time the automation's last_triggered advances, the AP is watched for a
new image for that tag. A line is written either way, so a silent failure
leaves evidence with a timestamp.
"""
import json
import sys
import time
import urllib.request

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import ap  # noqa: E402
import weather_display as wd  # noqa: E402

AUTOMATION = "automation.weather_forecast_display_oepl_6"
MAC = "00124B0018177B31"
WAIT_S = 300          # how long the AP gets to show an image after a trigger


def ha_state(entity):
    hdr = {"Authorization": f"Bearer {wd.ha_token()}"}
    req = urllib.request.Request(f"{wd.HA_URL}/api/states/{entity}", headers=hdr)
    return json.load(urllib.request.urlopen(req, timeout=20))


def tag_hash():
    t = ap.tag(MAC)
    return t["hash"] if t else None


def main():
    log = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/oepl_ha_watch.log", "a")

    def say(msg):
        line = f"{time.strftime('%F %H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    say("watching " + AUTOMATION)
    last_trigger = None
    while True:
        try:
            trig = ha_state(AUTOMATION)["attributes"].get("last_triggered")
            if trig and trig != last_trigger:
                if last_trigger is not None:          # skip the first reading
                    before = tag_hash()
                    say(f"automation fired ({trig}); tag hash {before}")
                    deadline = time.time() + WAIT_S
                    while time.time() < deadline:
                        time.sleep(15)
                        now = tag_hash()
                        if now and now != before:
                            say(f"  OK: displayed, hash {now}")
                            break
                    else:
                        say("  FAIL: no new image reached the tag -- check whether "
                            "the integration is still connected to the AP")
                last_trigger = trig
        except Exception as exc:                       # keep watching regardless
            say(f"  (poll error: {exc})")
        time.sleep(60)


if __name__ == "__main__":
    main()
