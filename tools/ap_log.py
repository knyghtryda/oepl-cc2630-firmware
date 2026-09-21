#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Stream the OEPL AP's live log (its websocket console) to stdout.

    tools/ap_log.py [MAC-substring] [seconds]

The AP pushes JSON frames: {"logMsg": "..."} for console lines and
{"tags":[...]} / {"sys":...} for state. Log lines are printed with a
timestamp; tag-state frames for the filtered MAC are summarised. With a MAC
filter, only log lines mentioning that MAC (or with no MAC at all) are shown.

Environment: OEPL_AP (default http://192.168.5.4)
"""
import asyncio
import json
import os
import sys
import time

import websockets

AP = os.environ.get("OEPL_AP", "http://192.168.5.4").rstrip("/")
WS = AP.replace("http://", "ws://").replace("https://", "wss://") + "/ws"


def ts():
    return time.strftime("%H:%M:%S")


async def main(filt, seconds):
    t_end = time.time() + seconds
    async with websockets.connect(WS, max_size=None) as ws:
        print(f"[{ts()}] connected {WS}", flush=True)
        while time.time() < t_end:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=max(1, t_end - time.time()))
            except asyncio.TimeoutError:
                break
            try:
                msg = json.loads(raw)
            except Exception:
                print(f"[{ts()}] {raw!r}", flush=True)
                continue
            if "logMsg" in msg:
                line = msg["logMsg"].rstrip()
                if filt and filt.lower() not in line.lower() and any(
                        c in line for c in ("00007F", "0000300B")):
                    continue  # another tag's traffic
                print(f"[{ts()}] {line}", flush=True)
            elif "tags" in msg:
                for t in msg["tags"]:
                    if filt and filt.lower() not in t.get("mac", "").lower():
                        continue
                    print(f"[{ts()}] TAG {t.get('mac')} pending={t.get('pending')} "
                          f"hash={t.get('hash', '')[:8]} ver={t.get('ver')} "
                          f"wake=0x{t.get('wakeupReason', 0):02x}", flush=True)
            elif "sys" in msg:
                pass
            else:
                print(f"[{ts()}] {raw[:200]}", flush=True)


if __name__ == "__main__":
    filt = sys.argv[1] if len(sys.argv) > 1 else ""
    seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    try:
        asyncio.run(main(filt, seconds))
    except KeyboardInterrupt:
        pass
