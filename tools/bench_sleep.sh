#!/bin/bash
# Bench loop for power work: flash over J-Link, then measure with the
# debugger disconnected and a clean power cycle.
#
#   tools/bench_sleep.sh <label> [measure_secs] [settle_secs] [make args...]
#
# 1. PPK2 powers the tag, J-Link flashes the build (make <args>).
# 2. Debugger server stopped, PPK2 session closed (VOUT off, 3 s): a JTAG
#    connection keeps the CC26xx debug power domain on until power is cut,
#    which blocks standby, so this power cycle is required.
# 3. PPK2 powers the tag again and records measure_secs of current.
# 4. Summary for the window after settle_secs (boot + splash excluded).
set -e
cd "$(dirname "$0")/.."
LABEL="$1"; shift
MEAS="${1:-300}"; shift || true
SETTLE="${1:-120}"; shift || true
OUT="${BENCH_DIR:-/tmp/bench}/$LABEL"
mkdir -p "$OUT"

make clean >/dev/null
make "$@" 2>&1 | grep -E "error|warning" && { echo "build failed"; exit 1; } || true
make "$@" >/dev/null

tools/ppk.py hold 90 2>/dev/null &
HOLD=$!
sleep 3
tools/jflash.sh > "$OUT/flash.txt" 2>&1
grep -q "flashed" "$OUT/flash.txt" || { cat "$OUT/flash.txt"; kill $HOLD; exit 1; }
pkill -f '^JLinkGDBServer' || true
kill $HOLD 2>/dev/null || true
# ppk.py holds VOUT from a child process; kill that too or power never drops
pgrep -f '^python3 tools/ppk[.]py hold' | xargs -r kill 2>/dev/null || true
wait $HOLD 2>/dev/null || true
# 15 s, not 3: the board's capacitors carry the tag through a few seconds in
# standby, and it then resumes the old sleep instead of booting
sleep 15

tools/ppk.py measure "$MEAS" "$OUT/trace.csv" --stats-every 5 > "$OUT/live.txt" 2>&1
tools/ppk_analyze.py "$OUT/trace.csv" --from "$SETTLE" | tee "$OUT/summary.txt" | head -5
