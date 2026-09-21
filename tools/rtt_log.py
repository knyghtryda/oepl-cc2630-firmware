#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Log SEGGER RTT output from a running JLinkGDBServer (-RTTTelnetPort 19021).

    tools/rtt_log.py [seconds] [out.txt]

Timestamps each line. The server only forwards RTT while it has a target
connection; run once `gdb-multiarch ... -ex "target remote :2331"` has been
attached at least once since the server started.
"""
import socket, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 600
out = open(sys.argv[2], "a") if len(sys.argv) > 2 else None
s = socket.create_connection(("localhost", 19021), timeout=2)
t0 = time.time(); buf = b""
while time.time() - t0 < secs:
    try:
        d = s.recv(4096)
        if not d: break
        buf += d
    except socket.timeout:
        continue
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        txt = f"[{time.strftime('%H:%M:%S')} +{time.time()-t0:7.1f}] {line.replace(b'\\r', b'').decode(errors='replace')}"
        print(txt, flush=True)
        if out: out.write(txt + "\n"); out.flush()
