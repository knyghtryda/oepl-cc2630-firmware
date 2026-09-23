#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Flash and verify the tag over a TI XDS110 (LaunchPad debugger) using UniFlash's DSLite.
#
#   tools/xflash.sh [image]      default binaries/Tag_FW_CC2630_TG-GR6000N.bin
#
# A .bin is loaded at 0x0 (full image incl. CCFG); a .elf carries its own addresses.
# Wiring, config and troubleshooting: docs/FLASHING_XDS110.md
#
# UniFlash is located via $UNIFLASH_DIR, else the newest ~/ti/uniflash_* or /opt/ti/uniflash_*
# (or /c/ti/uniflash_* under Git Bash on Windows).
#
# After a successful flash the tag must be fully power cycled — the XDS110's reset
# pulse does not start the new image.
set -e

IMAGE="${1:-binaries/Tag_FW_CC2630_TG-GR6000N.bin}"
[ -f "$IMAGE" ] || { echo "no such image: $IMAGE"; exit 1; }

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
CCXML="$TOOLS_DIR/cc2630_xds110.ccxml"
[ -f "$CCXML" ] || { echo "missing target config: $CCXML"; exit 1; }

if [ -z "$UNIFLASH_DIR" ]; then
    UNIFLASH_DIR=$(ls -d "$HOME"/ti/uniflash_* /opt/ti/uniflash_* /c/ti/uniflash_* 2>/dev/null | sort -V | tail -1)
fi
if [ -x "$UNIFLASH_DIR/dslite.sh" ]; then
    DSLITE="$UNIFLASH_DIR/dslite.sh"
elif [ -f "$UNIFLASH_DIR/dslite.bat" ]; then
    DSLITE="$UNIFLASH_DIR/dslite.bat"
else
    echo "UniFlash not found (set UNIFLASH_DIR to the uniflash_x.y.z directory)"; exit 1
fi

# .bin needs an explicit load address; .elf/.out/.hex do not
case "$IMAGE" in
    *.bin) TARGET="$IMAGE,0x0" ;;
    *)     TARGET="$IMAGE" ;;
esac

echo "=== TG-GR6000N XDS110 Flash & Verify ==="
echo "Image:    $IMAGE"
echo "UniFlash: $UNIFLASH_DIR"
echo ""

LOG=$(mktemp)
"$DSLITE" --config="$CCXML" -e -f -v "$TARGET" 2>&1 | tee "$LOG" | grep -E "Erasing|Loading Program|Verifying Program|verification|error|fatal|Failed|^Success" || true

if grep -q "Program verification successful" "$LOG" && grep -q "^Success" "$LOG"; then
    rm -f "$LOG"
    echo ""
    echo "=== PASS === flashed and verified $IMAGE"
    echo "Now power cycle the tag to boot the new image."
else
    echo ""
    echo "=== FAIL === see docs/FLASHING_XDS110.md, Troubleshooting. Full log: $LOG"
    exit 1
fi
