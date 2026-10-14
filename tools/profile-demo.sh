#!/bin/sh
# Profile rvk_demo with perf: runs it with the given arguments (make it run well over 10 seconds, e.g. --city-bench
# --frames 3000), samples it for 6 seconds once it is drawing, and prints where its busiest threads spend their time,
# by module and by function (tools/profile-demo.py; the demo's functions from build/linux-release/rvk_demo.map).
# Usage: tools/profile-demo.sh [demo args...]
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/profile-demo
out=$PWD/build/profile-demo
exe=$PWD/build/linux-release/rvk_demo.exe
(cd "$out" && WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all timeout 600 wine "$exe" "$@" > "$out/demo.txt" 2>&1) &
runner=$!
# The Wine process running the demo: the one with rvk_demo.exe mapped (not the wrappers around it).
pid=
for _ in $(seq 60); do
    sleep 1
    for p in $(pgrep -f 'rvk_demo'); do
        grep -q 'rvk_demo.exe' "/proc/$p/maps" 2>/dev/null && pid=$p
    done
    [ -n "$pid" ] && break
done
[ -z "$pid" ] && { echo "demo didn't start"; exit 1; }
sleep ${PROFILE_DELAY:-8}                                   # device creation, the scene's warm-up
cat "/proc/$pid/maps" > "$out/maps.txt"
perf record -q -e cycles:u -F 1999 -p "$pid" -o "$out/perf.data" -- sleep 6 2>/dev/null
wait "$runner"
perf script -i "$out/perf.data" -F tid,ip 2>/dev/null > "$out/samples.txt"
grep '^city bench: [0-9]* draws' "$out/demo.txt"
python3 -I tools/profile-demo.py "$out/samples.txt" "$out/maps.txt" ${PROFILE_TOP:+--top $PROFILE_TOP}
