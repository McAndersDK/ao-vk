#!/bin/sh
# The render-batching test set (docs/render-batching-plan.md): the city bench's picture, frame dump and timing at the
# game's size (twice), and the older scenes' pictures, into one directory - then, with a baseline, the comparison
# (tools/city-compare.py). Headless under Wine, no validation layer (it would skew the times).
# Usage: tools/city-bench.sh OUT_DIR [BASELINE_DIR]     (the baseline of 2026-10-14: build/rb-base)
cd "$(dirname "$0")/.." || exit 1
[ $# -ge 1 ] || { echo "usage: $0 OUT_DIR [BASELINE_DIR]"; exit 1; }
out=$1
base=${2:-}
mkdir -p "$out"
exe=$PWD/build/linux-release/rvk_demo.exe
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all
run() {
    name=$1; shift
    (cd "$out" && timeout 300 wine "$exe" "$@" > "$name.txt" 2>&1)
    echo "$name: exit $?"
}
run city --city-bench --pixel-lighting --frames 120 --city-warm 20 --shot city.bmp --dump city-dump.txt
run city-time1 --city-bench --pixel-lighting --size 2560 1440 --frames 700 --shot city-time.bmp
run city-time2 --city-bench --pixel-lighting --size 2560 1440 --frames 700 --shot city-time2.bmp
run shadow-hdr --shadow-test --hdr --frames 30 --shot shadow-hdr.bmp
run cache --cache-test 100 --frames 300 --shot cache.bmp
run statel --statel-test 'C:\linux\client\randy-vk\statels\790.bin' 790 1380 0 2950 --frames 60 --shot statel.bmp
run walker --shadow-test --walker-light --grass-walk -3 --pixel-lighting --light-override --hdr --frames 60 --fixed-clock 16 --shot walker.bmp
run bench --bench 300 --threaded
[ -n "$base" ] && python3 -I tools/city-compare.py "$base" "$out"
exit 0
