#!/bin/bash
# Flash the bench tag over J-Link (cJTAG) with a readback verify, then run it.
#
#   tools/jflash.sh [bin]        default binaries/Tag_FW_CC2630_TG-GR6000N.bin (full image incl. CCFG)
#
# The PPK2 must be supplying the tag (tools/ppk.py hold ...).
#
# Uses JLinkExe rather than gdb: gdb's `load` + `compare-sections` can report
# success from J-Link's flash cache while nothing was programmed (seen when
# the halt lands in deep sleep with flash powered down). Here the target is
# reset first and `verifybin` reads the flash back.
set -e
BIN="${1:-binaries/Tag_FW_CC2630_TG-GR6000N.bin}"
[ -f "$BIN" ] || { echo "no such image: $BIN"; exit 1; }
pkill -f '^JLinkGDBServer' 2>/dev/null || true
CMD=$(mktemp); LOG=$(mktemp)
# JLinkExe's interactive connect prompts: device, interface (T = cJTAG), speed, JTAG chain position
printf 'connect\nCC2630F128\nT\n1000\n\nr\nh\nerase\nloadfile %s 0x0\nverifybin %s 0x0\nr\ng\nexit\n' "$BIN" "$BIN" > "$CMD"
timeout 120 JLinkExe -nogui 1 < "$CMD" > "$LOG" 2>&1 || true
if grep -q "Verify successful" "$LOG" && ! grep -qi "Verify failed\|Could not connect" "$LOG"; then
    echo "flashed and verified $BIN"
    rm -f "$CMD" "$LOG"
else
    echo "FLASH FAILED for $BIN:"; grep -v "^$" "$LOG" | tail -25
    rm -f "$CMD" "$LOG"; exit 1
fi
