#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
# Power the bench tag from the PPK2 and log RTT for SECS seconds.
#   tools/rtt_session.sh SECS out.txt
# (Debugger attached: the tag won't reach standby -- use for radio/protocol
# work, not for current measurements.)
set -e
cd "$(dirname "$0")/.."
SECS="$1"; OUT="$2"
tools/ppk.py hold $((SECS + 20)) 2>/dev/null &
HOLD=$!
sleep 3
pkill -f '^JLinkGDBServer' 2>/dev/null || true
JLinkGDBServer -device CC2630F128 -if cJTAG -speed 1000 -port 2331 -RTTTelnetPort 19021 \
    -notimeout -nogui -silent >/dev/null 2>&1 &
sleep 4
# a gdb attach makes the server connect to the target (RTT needs that)
timeout 30 gdb-multiarch -batch -nx -ex "file build/Tag_FW_CC2630_TG-GR6000N.elf" \
    -ex "target remote :2331" -ex "monitor go" -ex "disconnect" >/dev/null 2>&1 || true
python3 -u tools/rtt_log.py "$SECS" "$OUT" >/dev/null 2>&1 || true
pkill -f '^JLinkGDBServer' 2>/dev/null || true
kill $HOLD 2>/dev/null || true
wait $HOLD 2>/dev/null || true
