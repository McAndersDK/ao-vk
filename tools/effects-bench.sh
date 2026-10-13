#!/bin/zsh
# GPU benchmark of the newer effects (smoke, rain, water sim, waterfalls, particles) in rvk_demo, no validation.
# Each scene runs 1210 frames; the second "gpu ms" profile line (frames 600-1200) is kept.
# Usage: tools/effects-bench.sh <out-dir> [scene-filter-regex] [repeats]
# Prints one line per scene: name, total and the effect passes.
root=${0:A:h:h}
out=${1:?out dir}; filter=${2:-.}; reps=${3:-1}
mkdir -p $out
exe=${RVK_DEMO:-$root/build/linux-release/rvk_demo.exe}
typeset -A scenes
scenes=(
  smoke1      "--water-lake --smoke --water-z -20 --water-pitch 0.05"
  smoke2      "--water-lake --smoke --smoke-two --water-z -20 --water-pitch 0.05"
  smokegame   "--water-lake --smoke --smoke-game --water-z -20 --water-pitch 0.05"
  smokeclose  "--water-lake --smoke --smoke-two --water-z -13 --water-pitch 0.05"
  rain        "--water-lake --rain 1"
  watersim    "--water-lake --water-walker --water-z 24 --water-height 4 --water-pitch 0.22"
  waterfall   "--water-lake --water-wall"
  particles   "--particle-test"
)
for name in ${(ko)scenes}; do
  [[ $name =~ $filter ]] || continue
  for r in $(seq $reps); do
    log=$out/$name.$r.log
    ( cd $out && WINEPREFIX=$HOME/.wine-prk WINEDEBUG=-all timeout 300 wine $exe --frames 1210 --fixed-clock 7 --shot $name.bmp ${=scenes[$name]} ${=RVK_BENCH_EXTRA} > $log 2>&1 )
    line=$(grep 'gpu ms:' $log | sed -n 2p)
    total=$(echo $line | grep -oE 'total [0-9.]+' )
    fx=$(echo $line | tr '|' '\n' | grep -E 'smoke|rain|water|particle|scene |depth' | grep -vE ' 0\.00' | tr -s ' ' | tr '\n' '|')
    echo "$name#$r: $total |$fx"
  done
done
