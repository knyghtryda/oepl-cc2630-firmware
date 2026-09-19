#!/bin/bash
# Host tests for the tag's zlib image decoder.
#
# Vectors are produced by host_test/apzlib, a replica of the AP's compressor
# (ESP32_AP-Flasher/src/makeimage.cpp + lib/miniz-oepl, 4 KB dictionary), so
# what the decoder is asked to handle is bit-for-bit what the AP sends.
#   host_test/run_tests.sh [--fetch]     (--fetch: re-pull raw images from the AP)
set -e
cd "$(dirname "$0")/.."
MINIZ=~/Code/OpenEPaperLink/ESP32_AP-Flasher/lib/miniz-oepl
AP=${OEPL_AP:-http://192.168.5.4}

g++ -O2 -I host_test/shim -I $MINIZ -o host_test/apzlib host_test/apzlib.cpp $MINIZ/miniz-oepl.cpp
gcc -O2 -Wall -Wextra -I firmware -o host_test/test_inflate host_test/test_inflate.c firmware/inflate.c
gcc -O2 -Wall -Wextra -fsanitize=address,undefined -I firmware \
    -o host_test/fuzz_inflate host_test/fuzz_inflate.c firmware/inflate.c

if [ "$1" = "--fetch" ]; then
    curl -s -m 20 "$AP/current/00124B00181880B0.raw" -o host_test/bench_raw.bin
    curl -s -m 20 "$AP/current/00124B0018177B31.raw" -o host_test/weather_raw.bin
fi
mkdir -p host_test/vec
python3 - <<'PY'
import random
random.seed(7)
open('host_test/vec/zeros.bin','wb').write(b'\x00'*67200)
open('host_test/vec/ones.bin','wb').write(b'\xff'*67200)
open('host_test/vec/random.bin','wb').write(bytes(random.getrandbits(8) for _ in range(67200)))
raw = open('host_test/bench_raw.bin','rb').read()
open('host_test/vec/oneplane.bin','wb').write(raw[:33600])
PY

fail=0
for f in host_test/bench_raw.bin host_test/weather_raw.bin host_test/vec/zeros.bin \
         host_test/vec/ones.bin host_test/vec/random.bin host_test/vec/oneplane.bin; do
    [ -s "$f" ] || { echo "missing $f"; continue; }
    size=$(stat -c%s "$f")
    planes=2; [ "$size" = 33600 ] && planes=1
    ./host_test/apzlib "$f" $planes 600 448 host_test/vec/tmp.zlib 2>/dev/null
    ./host_test/test_inflate host_test/vec/tmp.zlib "$f" || fail=1
done

echo "--- robustness (truncated and bit-flipped streams, asan+ubsan)"
./host_test/apzlib host_test/bench_raw.bin 2 600 448 host_test/vec/tmp.zlib 2>/dev/null
./host_test/fuzz_inflate host_test/vec/tmp.zlib || fail=1
exit $fail
