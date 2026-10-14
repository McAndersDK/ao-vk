#!/bin/sh
# Profile the running game for SECONDS (default 90) with wall-clock sample times, then show where its threads spent
# the slow frames randy-vk.log reported in that time (tools/profile-hitches.py). Do the thing that hitches (open the
# map, the options window...) a few times while it records. (tools/profile-game.sh: a whole stretch instead.)
# Usage: tools/profile-hitches.sh [SECONDS] [out-dir]       (default out-dir: build/profile-hitches)
# RANDY_MAP: the .map of the randy31.dll the game runs (default: the staged one ao-wine.sh names).
cd "$(dirname "$0")/.." || exit 1
seconds=${1:-90}
out=${2:-build/profile-hitches}
mkdir -p "$out"
base=$HOME/.wine-prk/drive_c/linux
pid=
for p in $(pgrep -f '\.exe'); do
    grep -q randy31_orig "/proc/$p/maps" 2>/dev/null && pid=$p
done
[ -z "$pid" ] && { echo "no game process with randy31_orig.dll mapped (is the client running?)"; exit 1; }
map=${RANDY_MAP:-$base/randy-vk/$(sed -n 's/^AO_RANDYVK_DLL=\([^ #]*\).*/\1/p' "$base/ao-wine.sh" | head -1 | sed 's/\.dll$/.map/')}
since=$(date +%H:%M:%S)
cat "/proc/$pid/maps" > "$out/maps.txt"
echo "recording the game (pid $pid) for $seconds s from $since"
perf record -q -e cycles:u -F 1999 -k CLOCK_REALTIME -p "$pid" -o "$out/perf.data" -- sleep "$seconds" 2>/dev/null
perf script -i "$out/perf.data" -F tid,time,ip 2>/dev/null > "$out/samples.txt"
python3 -I tools/profile-hitches.py "$out/samples.txt" "$out/maps.txt" "$base/logs/randy-vk.log" "$map" --since "$since" \
    ${PROFILE_TOP:+--top $PROFILE_TOP}
