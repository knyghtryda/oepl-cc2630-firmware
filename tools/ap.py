#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""OEPL access-point helper for bench-testing CC2630 tags.

Talks to the AP's web endpoints (the same ones the AP's web UI uses) so a
build → OTA → image → verify loop can run unattended from the dev host.

    tools/ap.py status [MAC]             show CC2630 tags (hwType 0x35) or one tag
    tools/ap.py ota MAC [OTA_BIN]        stage firmware on AP, queue OTA for tag
    tools/ap.py image MAC [FILE]         push an image (default: generated test card)
    tools/ap.py wait-ver MAC VER         block until AP reports tag firmware VER
    tools/ap.py wait-image MAC           block until pending==0 and hash != 0
    tools/ap.py cmd MAC {reset,scan,del,deepsleep}

Environment: OEPL_AP (default http://192.168.5.4)
"""
import io
import json
import os
import sys
import time
import urllib.request

AP = os.environ.get("OEPL_AP", "http://192.168.5.4").rstrip("/")
HWTYPE_CC2630 = 0x35
ZERO_HASH = "0" * 32
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OTA = os.path.join(REPO, "binaries", "Tag_FW_CC2630_TG-GR6000N_ota.bin")


def get(path):
    with urllib.request.urlopen(f"{AP}/{path}", timeout=10) as r:
        return r.read()


def post_multipart(path, fields, files=()):
    """fields: dict of str -> str; files: [(fieldname, filename, bytes)]"""
    boundary = "----oeplap%d" % int(time.time() * 1000)
    body = io.BytesIO()
    for k, v in fields.items():
        body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode())
    for k, fn, data in files:
        body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"; filename=\"{fn}\"\r\n"
                   f"Content-Type: application/octet-stream\r\n\r\n".encode())
        body.write(data)
        body.write(b"\r\n")
    body.write(f"--{boundary}--\r\n".encode())
    req = urllib.request.Request(f"{AP}/{path}", data=body.getvalue(), method="POST")
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.status, r.read().decode(errors="replace")


def tags():
    # get_db is paginated: keep asking with pos=<count so far> until a page is empty
    out = []
    while True:
        page = json.loads(get(f"get_db?pos={len(out)}"))["tags"]
        if not page:
            return out
        out.extend(page)


def tag(mac):
    mac = mac.upper()
    for t in json.loads(get(f"get_db?mac={mac}"))["tags"]:
        if t["mac"].upper() == mac:
            return t
    return None


def fmt(t):
    now = time.time()
    seen = t["lastseen"]
    seen_s = f"{int(now - seen)}s ago" if seen else "never"
    nxt = t["nextcheckin"]
    nxt_s = f"in {int(nxt - now)}s" if nxt and nxt < 3216153600 else "?"
    hsh = "ZERO" if t["hash"] == ZERO_HASH else t["hash"][:8]
    fault = ""
    temp8 = t["temperature"] & 0xFF
    detail, why = t["batteryMv"] & 0xFFFF, t["LQI"]
    fault = ""
    if 0x80 <= temp8 <= 0x87:
        pc = (0x20000000 if temp8 & 4 else 0) | ((temp8 & 3) << 16) | detail
        fault = f" FAULT hardfault pc=0x{pc:08x} ufsr={why >> 4:x} bfsr={why & 0xF:x}"
    elif temp8 == 0x90:
        name = (f"direct 0x{detail & 0x7FFF:04x}" if detail & 0x8000 else f"struct @0x2000{detail:04x}")
        fault = f" FAULT rf-doorbell cmd={name} phase={why >> 6} cmdsta=0x{why & 0x3F:02x}"
    elif temp8 == 0x98:
        fault = " FAULT watchdog"
    elif temp8 == 0xA0:
        # not a fault: the first checkin after an OTA apply
        fault = (f" OTA-APPLIED sectors={why} retries={detail >> 8} "
                 f"unverified={detail & 0xFF}")
    elif temp8 == 0xAD and detail == 0x0DB0:
        fault = " FAULT rf-doorbell (pre-v0.21 report)"
    if os.environ.get("OEPL_DIAG") and not fault:
        # DIAG=1 firmware: LQI/temperature/battery carry download diagnostics
        lqi, temp, bat = t["LQI"], t["temperature"] & 0xFF, t["batteryMv"]
        xfer = {0: "not-sent", 0xE: "tx-fail", 0xF: "no-ack"}.get(lqi & 0xF, f"acked@{lqi & 0xF}")
        fault += (f" DIAG failed_blocks={lqi >> 4} xfer={xfer} requests={temp} "
                  f"rf_crc_err={bat >> 8} rf_buf_full={bat & 0xFF} | ")
    return (fault + f"{t['mac']} v{t['ver']} hash={hsh} pending={t['pending']} mode={t['contentMode']} "
            f"seen={seen_s} next={nxt_s} ch={t['ch']} rssi={t['RSSI']} lqi={t['LQI']} "
            f"bat={t['batteryMv']}mV temp={t['temperature']}C wake=0x{t['wakeupReason']:02x} "
            f"alias={t['alias']!r}")


def cmd_status(args):
    if args:
        t = tag(args[0])
        if not t:
            print("tag not found on AP:", args[0]); return 1
        print(fmt(t)); return 0
    found = [t for t in tags() if t["hwType"] == HWTYPE_CC2630 or t["mac"].upper().startswith("00124B")]
    if not found:
        print("no CC2630 tags on AP"); return 1
    for t in found:
        print(fmt(t))
    return 0


def cmd_ota(args):
    if not args:
        print("usage: ota MAC [OTA_BIN]"); return 2
    mac = args[0].upper()
    path = args[1] if len(args) > 1 else DEFAULT_OTA
    data = open(path, "rb").read()
    name = os.path.basename(path)
    t = tag(mac)
    if not t:
        print("tag not found on AP:", mac); return 1
    print(f"uploading {name} ({len(data)} bytes) to AP littlefs /{name}")
    st, body = post_multipart("littlefs_put", {"path": "/" + name}, [("file", name, data)])
    print(" ", st, body.strip())
    info = json.loads(get(f"check_file?path=/{name}"))
    if info.get("filesize") != len(data):
        print("size mismatch on AP:", info); return 1
    print(f"queueing OTA (contentmode 5) for {mac}, current ver={t['ver']}")
    st, body = post_multipart("save_cfg", {
        "mac": mac, "alias": t["alias"], "contentmode": "5",
        "modecfgjson": json.dumps({"filename": "/" + name}),
    })
    print(" ", st, body.strip())
    return 0


def test_card(text):
    """600x448 test image: black text, red bar, checker corners. Needs Pillow."""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new("RGB", (600, 448), "white")
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default(size=40)
    d.rectangle([0, 0, 599, 40], fill="red")
    d.rectangle([0, 407, 599, 447], fill="black")
    for y in range(60, 400, 40):
        d.line([20, y, 580, y], fill="black", width=3)
    d.rectangle([20, 160, 580, 290], fill="white")
    d.text((30, 170), text, fill="black", font=font)
    d.text((30, 230), text, fill="red", font=font)
    for i in range(0, 600, 20):
        d.rectangle([i, 45, i + 9, 55], fill="black" if (i // 20) % 2 else "red")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=95)
    return buf.getvalue()


def cmd_image(args):
    if not args:
        print("usage: image MAC [FILE]"); return 2
    mac = args[0].upper()
    if len(args) > 1:
        data = open(args[1], "rb").read()
        name = os.path.basename(args[1])
    else:
        data = test_card(time.strftime("v0.15 test %Y-%m-%d %H:%M:%S"))
        name = "testcard.jpg"
    print(f"uploading image {name} ({len(data)} bytes) for {mac}")
    st, body = post_multipart("imgupload", {"mac": mac, "dither": "0"}, [("file", name, data)])
    print(" ", st, body.strip())
    return 0


def poll(mac, cond, timeout, what):
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        try:
            t = tag(mac)
        except Exception as e:          # the AP stalls / reboots now and then
            print(f"[{int(time.time() - t0):4d}s] AP not answering ({e.__class__.__name__}), retrying")
            time.sleep(10)
            continue
        if t:
            line = fmt(t)
            if line != last:
                print(f"[{int(time.time() - t0):4d}s] {line}")
                last = line
            if cond(t):
                print(f"OK: {what}")
                return 0
        time.sleep(5)
    print(f"TIMEOUT after {timeout}s waiting for {what}")
    return 1


def cmd_wait_ver(args):
    mac, ver = args[0].upper(), int(args[1], 0)
    timeout = int(args[2]) if len(args) > 2 else 1200
    return poll(mac, lambda t: t["ver"] == ver, timeout, f"ver == {ver}")


def cmd_wait_image(args):
    """Wait for a *new* image to be confirmed: the AP takes a few seconds to
    register an upload as pending, so first wait for pending=1, then for it to
    clear with a hash different from the one the tag had before."""
    mac = args[0].upper()
    timeout = int(args[1]) if len(args) > 1 else 1200
    before = tag(mac)["hash"]
    rc = poll(mac, lambda t: t["pending"] == 1 or (t["hash"] != before and t["hash"] != ZERO_HASH),
              60, "AP registered pending image")
    if rc:
        return rc
    return poll(mac, lambda t: t["pending"] == 0 and t["hash"] != ZERO_HASH and t["hash"] != before,
                timeout, "image confirmed (pending==0, new hash)")


def cmd_cmd(args):
    st, body = post_multipart("tag_cmd", {"mac": args[0].upper(), "cmd": args[1]})
    print(st, body.strip()); return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    fn = {"status": cmd_status, "ota": cmd_ota, "image": cmd_image,
          "wait-ver": cmd_wait_ver, "wait-image": cmd_wait_image, "cmd": cmd_cmd}.get(sys.argv[1])
    if not fn:
        print(__doc__); return 2
    return fn(sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
