#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Claude quota display for a 6" BWR tag.

Builds an OpenEPaperLink `drawcustom` payload from a list of accounts and
either previews it locally or posts it to a Home Assistant webhook, which
relays it to the tag.

    tools/quota_display.py preview [out.png]      render sample data locally
    tools/quota_display.py preview --in data.json [out.png]
    tools/quota_display.py push --in data.json    send to the tag via HA
    tools/quota_display.py payload --in data.json print the drawcustom JSON
    tools/quota_display.py install WEBHOOK_ID DEVICE_ID
                                                  (re)create the HA relay automation

Input JSON:

    {"accounts": [
       {"name": "work",
        "resets": "2026-09-22T09:00:00-07:00",   # ISO 8601, any tz
        "week": 62, "five_hour": 88, "fable": 12},
       ...
    ]}

Percentages are **used**, so 100 means exhausted; a bar at or above
RED_AT is drawn red. Accounts are sorted by reset time, soonest first.
Any number of accounts is accepted -- the row height shrinks to fit, down
to MIN_ROW_H, past which the extras are dropped and a count is shown.

This module owns the layout so that the sending machine needs no fonts,
no PIL and no OEPL knowledge: it builds JSON and POSTs it. The local
renderer (borrowed from weather_display.py) emulates the integration's
drawing code, so `preview` is what the tag will show.

Sending needs only this file and one secret, the webhook URL: set
QUOTA_WEBHOOK_URL, or HA_URL + QUOTA_WEBHOOK_ID. `preview` and `install`
additionally need PIL and HA credentials, and only ever run on the dev host.
"""
import datetime as dt
import json
import os
import sys
import urllib.request


def _wd():
    """weather_display supplies the local renderer and the HA API helper. It
    needs PIL and reads ~/secrets.toml, so it is imported only by the commands
    that run on the dev host -- never on the machine that just sends data."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import weather_display
    return weather_display

# Sending needs exactly one secret: the webhook URL. Give it whole via
# QUOTA_WEBHOOK_URL, or as HA_URL + QUOTA_WEBHOOK_ID.
HA_URL = os.environ.get("HA_URL", "http://192.168.13.122:8123").rstrip("/")
WEBHOOK_ID = os.environ.get("QUOTA_WEBHOOK_ID", "")
WEBHOOK_URL = os.environ.get("QUOTA_WEBHOOK_URL") or f"{HA_URL}/api/webhook/{WEBHOOK_ID}"

W, H = 600, 448
RED_AT = 80          # a bar this full or fuller is drawn red
MIN_ROW_H = 44       # below this the rows stop being legible at arm's length

# --- geometry -------------------------------------------------------------
MARGIN = 8
TITLE_Y = 4
RULE_Y = 42
COLHDR_Y = 50
BODY_Y = 74

COL_NAME = 10
COL_RESET = 166
BARS_X = 256         # first bar column starts here
BAR_W = 78           # bar body
BAR_SLOT = 115       # bar + its number + gap
BAR_H = 16
NUM_DX = BAR_W + 6   # number sits just right of the bar

COLUMNS = ("5-HOUR", "WEEK", "FABLE")
KEYS = ("five_hour", "week", "fable")


def _fmt_reset(iso, now):
    """'2h 14m' / '3d 4h' / 'now' -- a countdown reads better than a date."""
    if not iso:
        return "-"
    try:
        t = dt.datetime.fromisoformat(iso)
    except ValueError:
        return str(iso)
    if t.tzinfo is None:
        t = t.replace(tzinfo=now.tzinfo)
    secs = int((t - now).total_seconds())
    if secs <= 0:
        return "now"
    d, rem = divmod(secs, 86400)
    h, rem = divmod(rem, 3600)
    m = rem // 60
    if d:
        return f"{d}d {h}h"
    if h:
        return f"{h}h {m:02d}m"
    return f"{m}m"


def _bar(x, y, pct, elements, bar_h=BAR_H, num_sz=15):
    """Outlined track with a filled portion; red once it passes RED_AT."""
    pct = max(0, min(100, int(round(pct))))
    colour = "red" if pct >= RED_AT else "black"
    elements.append({"type": "rectangle", "x_start": x, "y_start": y,
                     "x_end": x + BAR_W, "y_end": y + bar_h,
                     "outline": "black", "width": 1, "radius": 2})
    if pct >= 3:                      # below this a sliver just thickens the border
        fill_w = int(BAR_W * pct / 100)
        elements.append({"type": "rectangle", "x_start": x, "y_start": y,
                         "x_end": x + fill_w, "y_end": y + bar_h,
                         "fill": colour, "outline": colour, "width": 1, "radius": 2})
    elements.append({"type": "text", "x": x + NUM_DX, "y": y + bar_h // 2,
                     "value": str(pct), "size": num_sz, "font": "ppb.ttf",
                     "color": colour, "anchor": "lm"})


def build_payload(data, now=None):
    now = now or dt.datetime.now().astimezone()
    accounts = sorted(
        data.get("accounts", []),
        key=lambda a: a.get("resets") or "9999",
    )

    el = []
    el.append({"type": "text", "x": MARGIN, "y": TITLE_Y, "value": "CLAUDE QUOTA",
               "size": 30, "font": "ppb.ttf", "color": "black", "anchor": "la"})
    el.append({"type": "text", "x": W - MARGIN, "y": TITLE_Y + 8,
               "value": now.strftime("%a %H:%M"), "size": 17, "font": "ppb.ttf",
               "color": "black", "anchor": "ra"})
    el.append({"type": "line", "x_start": MARGIN, "y_start": RULE_Y,
               "x_end": W - MARGIN, "y_end": RULE_Y, "fill": "red", "width": 3})

    el.append({"type": "text", "x": COL_NAME, "y": COLHDR_Y, "value": "ACCOUNT",
               "size": 14, "font": "ppb.ttf", "color": "black", "anchor": "la"})
    el.append({"type": "text", "x": COL_RESET, "y": COLHDR_Y, "value": "RESETS",
               "size": 14, "font": "ppb.ttf", "color": "black", "anchor": "la"})
    for i, name in enumerate(COLUMNS):
        el.append({"type": "text", "x": BARS_X + i * BAR_SLOT, "y": COLHDR_Y,
                   "value": name, "size": 14, "font": "ppb.ttf",
                   "color": "black", "anchor": "la"})
    el.append({"type": "line", "x_start": MARGIN, "y_start": BODY_Y - 6,
               "x_end": W - MARGIN, "y_end": BODY_Y - 6, "fill": "black", "width": 1})

    avail = H - BODY_Y - MARGIN
    n = len(accounts)
    shown = accounts
    if n and avail // n < MIN_ROW_H:
        shown = accounts[: max(1, avail // MIN_ROW_H) - 1]
    # Rows fill the panel: few accounts means big readable rows, many means
    # compact ones. Everything on a row shares one baseline -- stacking the
    # name above the reset made the two look unrelated.
    row_h = avail // max(1, len(shown))
    scale = lambda lo, frac, hi: int(max(lo, min(hi, row_h * frac)))
    name_sz = scale(18, 0.40, 24)   # capped so ~10 chars clear the column
    reset_sz = scale(13, 0.27, 18)
    bar_h = scale(13, 0.30, 26)
    num_sz = scale(13, 0.26, 20)

    for i, acct in enumerate(shown):
        top = BODY_Y + i * row_h
        if i:
            el.append({"type": "line", "x_start": MARGIN, "y_start": top - 1,
                       "x_end": W - MARGIN, "y_end": top - 1, "fill": "black", "width": 1})
        mid = top + row_h // 2
        el.append({"type": "text", "x": COL_NAME, "y": mid, "anchor": "lm",
                   "value": str(acct.get("name", "?")), "size": name_sz,
                   "font": "ppb.ttf", "color": "black",
                   "max_width": COL_RESET - COL_NAME - 10})
        reset = _fmt_reset(acct.get("resets"), now)
        urgent = reset == "now" or (reset.endswith("m") and "h" not in reset)
        el.append({"type": "text", "x": COL_RESET, "y": mid, "anchor": "lm",
                   "value": reset, "size": reset_sz, "font": "ppb.ttf",
                   "color": "red" if urgent else "black"})
        for j, key in enumerate(KEYS):
            _bar(BARS_X + j * BAR_SLOT, mid - bar_h // 2, acct.get(key, 0) or 0,
                 el, bar_h, num_sz)

    if len(shown) != n:
        el.append({"type": "text", "x": W - MARGIN, "y": H - MARGIN,
                   "value": f"+{n - len(shown)} more", "size": 14, "font": "ppb.ttf",
                   "color": "red", "anchor": "rd"})
    return el


SAMPLE = {"accounts": [
    {"name": "work",      "resets": None, "week": 62, "five_hour": 88, "fable": 12},
    {"name": "personal",  "resets": None, "week": 31, "five_hour": 14, "fable": 0},
    {"name": "steppefolk", "resets": None, "week": 94, "five_hour": 41, "fable": 77},
    {"name": "research",  "resets": None, "week": 8,  "five_hour": 0,  "fable": 3},
    {"name": "spare",     "resets": None, "week": 55, "five_hour": 67, "fable": 22},
]}


def _sample(now):
    """Sample data with resets spread out, so ordering is visible."""
    d = json.loads(json.dumps(SAMPLE))
    for i, a in enumerate(d["accounts"]):
        a["resets"] = (now + dt.timedelta(minutes=37 + i * 431)).isoformat()
    return d


def _load(args):
    now = dt.datetime.now().astimezone()
    if "--in" in args:
        path = args[args.index("--in") + 1]
        return json.load(open(path)), [a for a in args if a != "--in" and a != path]
    return _sample(now), args


def cmd_preview(args):
    data, rest = _load(args)
    out = rest[0] if rest else "/tmp/quota.png"
    img = _wd().render(build_payload(data))
    img.save(out)
    print(f"wrote {out}")


def cmd_payload(args):
    data, _ = _load(args)
    print(json.dumps(build_payload(data), indent=1))


# The HA side is a thin relay: a webhook automation that passes the payload
# straight to the tag. HA holds the AP credentials and the tag identity; the
# sending machine never learns either, and the webhook is revocable on its own
# (an HA API token, by contrast, could call any service in the house).
AUTOMATION_ID = "claude_quota_display"


def build_automation(webhook_id, device_id, ttl=3900):
    return {
        "alias": "Claude quota display",
        "description": ("Relays a drawcustom payload from the Steppefolk VM to the 6\" tag. "
                        "The sender owns the layout; this holds only the credential and the "
                        "tag identity. ttl 3900s -> the tag checks in every 15 min (the AP's "
                        "maxsleep), so a push lands within 15 min."),
        "triggers": [{"trigger": "webhook", "webhook_id": webhook_id,
                      "allowed_methods": ["POST"], "local_only": True}],
        "conditions": [],
        "actions": [{"action": "open_epaper_link.drawcustom",
                     "target": {"device_id": device_id},
                     "data": {"background": "white", "rotate": 0, "dither": 0,
                              "ttl": ttl, "payload": "{{ trigger.json.payload }}"}}],
        "mode": "queued", "max": 5,
    }


def cmd_install(args):
    if len(args) < 2:
        print("usage: install WEBHOOK_ID DEVICE_ID")
        return 2
    print(_wd().ha(f"/api/config/automation/config/{AUTOMATION_ID}",
                build_automation(args[0], args[1])))


def cmd_push(args):
    data, _ = _load(args)
    body = json.dumps({"payload": build_payload(data)}).encode()
    if not WEBHOOK_ID and not os.environ.get("QUOTA_WEBHOOK_URL"):
        print("set QUOTA_WEBHOOK_URL (or QUOTA_WEBHOOK_ID)", file=sys.stderr)
        return 2
    req = urllib.request.Request(WEBHOOK_URL, data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        print(r.status, (r.read() or b"").decode(errors="replace")[:200])


if __name__ == "__main__":
    cmds = {"preview": cmd_preview, "push": cmd_push, "payload": cmd_payload,
            "install": cmd_install}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        sys.exit(2)
    sys.exit(cmds[sys.argv[1]](sys.argv[2:]) or 0)
