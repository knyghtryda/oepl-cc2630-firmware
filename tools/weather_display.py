#!/usr/bin/env python3
"""Weather display for the 6" BWR tag, driven by Home Assistant.

The layout is written once as an OpenEPaperLink `drawcustom` payload whose
values are Jinja templates. This tool can:

    tools/weather_display.py preview [out.png]   render locally against live HA data (needs fonts, see below)
    tools/weather_display.py push MAC            render locally and upload to the tag via the AP
    tools/weather_display.py yaml                print the full HA automation config
    tools/weather_display.py install             write the automation into HA (asks first)

Local rendering emulates the integration's drawing code (PIL, 1-bit font
mode, same fonts) so the preview is what HA will produce. Fonts are fetched
from the integration repo into tools/weather_assets/ on first use.

Environment: OEPL_AP (default http://192.168.5.4), HA_URL (default
http://ha-home.local:8123), HA token from ~/secrets.toml [homeassistant].
"""
import datetime as dt
import io
import json
import os
import sys
import tomllib
import urllib.request
import zoneinfo

HA_URL = os.environ.get("HA_URL", "http://ha-home.local:8123").rstrip("/")
WEATHER = "weather.forecast_home"
DEVICE_ID = "9c0e15aed2491c6c08f2d0069a11d038"     # Weather6 in HA
AUTOMATION_ID = "weather_forecast_oepl_display"
W, H = 600, 448
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "weather_assets")
ASSET_URL = ("https://raw.githubusercontent.com/OpenEPaperLink/Home_Assistant_Integration/"
             "main/custom_components/open_epaper_link/imagegen/assets/")

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

# HA weather condition -> MDI icon. Daily rows force night conditions to their
# day equivalent (met.no reports "clear-night" for whole days).
ICON_MAP = ("{'clear-night':'weather-night','cloudy':'weather-cloudy','fog':'weather-fog',"
            "'hail':'weather-hail','lightning':'weather-lightning','lightning-rainy':'weather-lightning-rainy',"
            "'partlycloudy':'weather-partly-cloudy','pouring':'weather-pouring','rainy':'weather-rainy',"
            "'snowy':'weather-snowy','snowy-rainy':'weather-snowy-rainy','sunny':'weather-sunny',"
            "'windy':'weather-windy','windy-variant':'weather-windy-variant','exceptional':'alert-circle-outline'}")
TEXT_MAP = ("{'clear-night':'Clear','partlycloudy':'Partly Cloudy','lightning-rainy':'Thunderstorms',"
            "'snowy-rainy':'Sleet','windy-variant':'Windy','pouring':'Heavy Rain'}")


def icon_tpl(cond_expr, day=False):
    fix = "{% if c == 'clear-night' %}{% set c = 'sunny' %}{% endif %}" if day else ""
    return f"{{% set c = {cond_expr} %}}{fix}{{{{ {ICON_MAP}.get(c, 'weather-' ~ c) }}}}"


def hourly(i, key):
    return f"hourly['{WEATHER}']['forecast'][{i}]['{key}']"


def daily(i, key):
    return f"daily['{WEATHER}']['forecast'][{i}]['{key}']"


def text(value, x, y, size, anchor="la", color="black", font="rbm.ttf", **kw):
    return {"type": "text", "value": value, "x": x, "y": y, "size": size, "color": color,
            "font": font, "anchor": anchor, **kw}


def icon(value, x, y, size, anchor="mm", color="black"):
    return {"type": "icon", "value": value, "x": x, "y": y, "size": size, "color": color, "anchor": anchor}


def line(x0, y0, x1, y1, width=1, fill="black"):
    return {"type": "line", "x_start": x0, "y_start": y0, "x_end": x1, "y_end": y1, "width": width, "fill": fill}


def build_payload():
    p = []
    L, R = 16, 584                      # content margins

    # --- header: current conditions ------------------------------------
    p.append(line(0, 3, W, 3, width=6, fill="red"))
    p.append(icon(icon_tpl(f"states('{WEATHER}')"), 82, 84, 120))
    p.append(text(f"{{{{ state_attr('{WEATHER}', 'temperature') | round | int }}}}°",
                  160, 118, 104, anchor="ls", font="ppb.ttf"))
    cond = (f"{{% set c = states('{WEATHER}') %}}{{{{ {TEXT_MAP}.get(c, c | title) }}}}")
    p.append(text(cond, 164, 126, 26, anchor="la", max_width=250))
    # today's high / low next to the temperature
    p.append(icon("arrow-up-bold", 366, 66, 22, color="red"))
    p.append(text(f"{{{{ {daily(0, 'temperature')} | round | int }}}}°", 386, 66, 30, anchor="lm", color="red", font="ppb.ttf"))
    p.append(icon("arrow-down-bold", 366, 102, 22))
    p.append(text(f"{{{{ {daily(0, 'templow')} | round | int }}}}°", 386, 102, 30, anchor="lm", font="ppb.ttf"))

    # right column: date right-aligned, details as an icon + left-aligned text list
    p.append(text("{{ now().strftime('%A, %b %-d') }}", R, 14, 22, anchor="ra", font="ppb.ttf"))
    cx_icon, x_txt = 452, 470
    rows = [
        ("water-percent", f"{{{{ state_attr('{WEATHER}', 'humidity') | int }}}}% humidity"),
        ("weather-windy", f"{{{{ state_attr('{WEATHER}', 'wind_speed') | round | int }}}} mph "
                          f"{{% set d = ['N','NE','E','SE','S','SW','W','NW'] %}}"
                          f"{{{{ d[(((state_attr('{WEATHER}', 'wind_bearing') | float) / 45) | round | int) % 8] }}}}"),
        ("white-balance-sunny", f"UV index {{{{ state_attr('{WEATHER}', 'uv_index') | round | int }}}}"),
    ]
    y = 58
    for ic, val in rows:
        p.append(icon(ic, cx_icon, y, 22))
        p.append(text(val, x_txt, y, 20, anchor="lm"))
        y += 28
    # sunrise / sunset on one row
    p.append(icon("weather-sunset-up", cx_icon, y, 22))
    p.append(text("{{ as_timestamp(state_attr('sun.sun', 'next_rising')) | timestamp_custom('%-I:%M') }}",
                  x_txt, y, 20, anchor="lm"))
    p.append(icon("weather-sunset-down", cx_icon + 68, y, 22))
    p.append(text("{{ as_timestamp(state_attr('sun.sun', 'next_setting')) | timestamp_custom('%-I:%M') }}",
                  x_txt + 68, y, 20, anchor="lm"))

    p.append(line(L, 162, R, 162))

    # --- hourly strip: every 2 hours, 8 columns ---------------------------
    cols = 8
    cw = (R - L) / cols
    for i in range(cols):
        cx = int(L + cw * (i + 0.5))
        h = 2 * i
        p.append(text(f"{{{{ as_timestamp({hourly(h, 'datetime')}) | timestamp_custom('%-I%p') | lower }}}}",
                      cx, 174, 17, anchor="mt"))
        p.append(icon(icon_tpl(hourly(h, 'condition')), cx, 218, 42))
        p.append(text(f"{{{{ {hourly(h, 'temperature')} | round | int }}}}°", cx, 244, 24, anchor="mt", font="ppb.ttf"))
        p.append(text(f"{{% if {hourly(h, 'precipitation')} > 0 %}}{{{{ {hourly(h, 'precipitation')} }}}}\"{{% endif %}}",
                      cx, 274, 14, anchor="mt", color="red"))

    p.append(line(L, 296, R, 296))

    # --- 5-day: columns, high in red over low in black ---------------------
    cols = 5
    cw = (R - L) / cols
    for i in range(cols):
        cx = int(L + cw * (i + 0.5))
        p.append(text("Today" if i == 0 else
                      f"{{{{ as_timestamp({daily(i, 'datetime')}) | timestamp_custom('%a') }}}}",
                      cx, 305, 22, anchor="mt", font="ppb.ttf"))
        p.append(icon(icon_tpl(daily(i, 'condition'), day=True), cx - 26, 358, 48))
        p.append(text(f"{{{{ {daily(i, 'temperature')} | round | int }}}}°", cx + 2, 344, 28, anchor="lm", color="red", font="ppb.ttf"))
        p.append(text(f"{{{{ {daily(i, 'templow')} | round | int }}}}°", cx + 2, 374, 22, anchor="lm"))
        p.append(text(f"{{% if {daily(i, 'precipitation')} > 0 %}}{{{{ {daily(i, 'precipitation')} }}}}\" rain{{% endif %}}",
                      cx, 398, 14, anchor="mt", color="red"))

    # --- footer -----------------------------------------------------------
    p.append(line(L, 418, R, 418))
    p.append(text("Updated {{ now().strftime('%-I:%M %p') }}", L, 440, 14, anchor="ls"))
    p.append(text("Forecast: met.no", R, 440, 14, anchor="rs"))
    return p


def build_automation():
    return {
        "id": AUTOMATION_ID,
        "alias": 'Weather Forecast Display (OEPL 6")',
        "description": "Weather forecast on the 6 inch BWR e-paper tag (Weather6). Layout generated by tools/weather_display.py in oepl-cc2630-firmware.",
        "triggers": [{"trigger": "time_pattern", "minutes": "5", "hours": "/2"},
                     {"trigger": "homeassistant", "event": "start"}],
        "actions": [
            {"action": "weather.get_forecasts", "target": {"entity_id": WEATHER},
             "data": {"type": "hourly"}, "response_variable": "hourly"},
            {"action": "weather.get_forecasts", "target": {"entity_id": WEATHER},
             "data": {"type": "daily"}, "response_variable": "daily"},
            {"action": "open_epaper_link.drawcustom", "target": {"device_id": DEVICE_ID},
             # ttl: how long the tag may sleep between check-ins after this image
             # (seconds; the AP caps it at its maxsleep setting, the tag at 1 h)
             "data": {"background": "white", "rotate": 0, "dither": 0, "ttl": 7200, "payload": build_payload()}},
        ],
        "mode": "single",
    }


# ---------------------------------------------------------------------------
# Home Assistant access + local Jinja evaluation
# ---------------------------------------------------------------------------

def ha_token():
    return tomllib.load(open(os.path.expanduser("~/secrets.toml"), "rb"))["homeassistant"]["bearer_token_local"]


def ha(path, data=None):
    hdr = {"Authorization": f"Bearer {ha_token()}", "Content-Type": "application/json"}
    req = urllib.request.Request(HA_URL + path, headers=hdr,
                                 data=json.dumps(data).encode() if data is not None else None,
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b"null")


def fetch_context():
    """Live HA data in the shape the automation's templates see."""
    states = {s["entity_id"]: s for s in ha("/api/states")}
    tz = zoneinfo.ZoneInfo(ha("/api/config")["time_zone"])
    hourly = ha("/api/services/weather/get_forecasts?return_response",
                {"entity_id": WEATHER, "type": "hourly"})["service_response"]
    daily = ha("/api/services/weather/get_forecasts?return_response",
               {"entity_id": WEATHER, "type": "daily"})["service_response"]
    return states, tz, hourly, daily


def jinja_env(states, tz):
    import jinja2

    def as_timestamp(v):
        if isinstance(v, (int, float)):
            return float(v)
        return dt.datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp()

    def timestamp_custom(ts, fmt, local=True):
        return dt.datetime.fromtimestamp(float(ts), tz if local else dt.timezone.utc).strftime(fmt)

    env = jinja2.Environment()
    env.globals.update(
        states=lambda e: states.get(e, {}).get("state", "unknown"),
        state_attr=lambda e, a: states.get(e, {}).get("attributes", {}).get(a),
        as_timestamp=as_timestamp,
        now=lambda: dt.datetime.now(tz),
    )
    env.filters.update(timestamp_custom=timestamp_custom, as_timestamp=as_timestamp)
    return env


def resolve(payload, states, tz, hourly, daily):
    env = jinja_env(states, tz)
    out = []
    for el in payload:
        el = dict(el)
        if isinstance(el.get("value"), str) and "{" in el["value"]:
            el["value"] = env.from_string(el["value"]).render(hourly=hourly, daily=daily)
        out.append(el)
    return out


# ---------------------------------------------------------------------------
# Local renderer emulating the integration's drawcustom
# ---------------------------------------------------------------------------

def ensure_assets():
    os.makedirs(ASSETS, exist_ok=True)
    for f in ("ppb.ttf", "rbm.ttf", "materialdesignicons-webfont.ttf", "materialdesignicons-webfont_meta.json"):
        path = os.path.join(ASSETS, f)
        if not os.path.exists(path):
            print("fetching", f, file=sys.stderr)
            urllib.request.urlretrieve(ASSET_URL + f, path)


def render(payload):
    from PIL import Image, ImageDraw, ImageFont
    ensure_assets()
    colors = {"black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0)}
    mdi = {i["name"]: chr(int(i["codepoint"], 16))
           for i in json.load(open(os.path.join(ASSETS, "materialdesignicons-webfont_meta.json")))}
    fonts = {}

    def font(name, size):
        key = (name, size)
        if key not in fonts:
            fonts[key] = ImageFont.truetype(os.path.join(ASSETS, name), size)
        return fonts[key]

    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    for el in payload:
        t = el["type"]
        if t == "text":
            s = str(el["value"])
            f = font(el.get("font", "ppb.ttf"), el["size"])
            if el.get("max_width") and d.textlength(s, font=f) > el["max_width"]:
                while s and d.textlength(s + "...", font=f) > el["max_width"]:
                    s = s[:-1]
                s += "..."
            d.text((el["x"], el["y"]), s, fill=colors[el.get("color", "black")], font=f, anchor=el.get("anchor", "la"))
        elif t == "icon":
            f = font("materialdesignicons-webfont.ttf", el["size"])
            d.text((el["x"], el["y"]), mdi[el["value"].removeprefix("mdi:")],
                   fill=colors[el.get("color", "black")], font=f, anchor=el.get("anchor", "la"))
        elif t == "line":
            d.line([(el["x_start"], el["y_start"]), (el["x_end"], el["y_end"])],
                   fill=colors[el.get("fill", "black")], width=el.get("width", 1))
        elif t == "rectangle":
            d.rounded_rectangle((el["x_start"], el["y_start"], el["x_end"], el["y_end"]),
                                fill=colors.get(el.get("fill")), outline=colors.get(el.get("outline", "black")),
                                width=el.get("width", 1), radius=el.get("radius", 0))
        else:
            raise ValueError(f"unsupported element type {t}")
    return img


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_preview(args):
    out = args[0] if args else "weather_preview.png"
    img = render(resolve(build_payload(), *fetch_context()))
    img.save(out)
    print("wrote", out)


def cmd_push(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ap
    mac = args[0].upper()
    img = render(resolve(build_payload(), *fetch_context()))
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=95)
    st, body = ap.post_multipart("imgupload", {"mac": mac, "dither": "0"}, [("file", "weather.jpg", buf.getvalue())])
    print(st, body.strip())


def cmd_yaml(args):
    import yaml
    print(yaml.safe_dump(build_automation(), sort_keys=False, width=110, allow_unicode=True))


def cmd_install(args):
    if input(f"Overwrite HA automation '{AUTOMATION_ID}'? [y/N] ").strip().lower() != "y":
        return 1
    print(ha(f"/api/config/automation/config/{AUTOMATION_ID}", build_automation()))
    print("installed; HA reloads automations automatically")


if __name__ == "__main__":
    cmds = {"preview": cmd_preview, "push": cmd_push, "yaml": cmd_yaml, "install": cmd_install}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        sys.exit(2)
    sys.exit(cmds[sys.argv[1]](sys.argv[2:]) or 0)
