#!/bin/bash
# Monitor DIO11 via RPi GPIO 17
# If firmware is toggling DIO11, we'll see the value change
# between reads.
#
# Usage: bash tools/monitor_dio11.sh [count]

CHIP="gpiochip0"
LINE=17
COUNT=${1:-20}

echo "=== Monitoring CC2630 DIO11 via RPi GPIO $LINE ==="
echo "Reading $COUNT samples at ~200ms intervals..."
echo "If firmware is running, values should alternate 0/1"
echo ""

for i in $(seq 1 $COUNT); do
    VAL=$(gpioget -c "$CHIP" "$LINE" 2>/dev/null || gpioget "$CHIP" "$LINE" 2>/dev/null)
    printf "[%02d] GPIO %d = %s\n" "$i" "$LINE" "$VAL"
    sleep 0.2
done

echo ""
echo "Done. If all values are the same, firmware may not be toggling DIO11."
