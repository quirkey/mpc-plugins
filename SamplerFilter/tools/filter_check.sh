#!/usr/bin/env bash
# SamplerFilter checks:  SamplerFilter/tools/filter_check.sh
#   filter_check.cpp: the 6-pole's response (cutoff, slope, no resonance), bypass, Breaks' BITS / RATE, TRACK, DRIVE
#   then a drum loop through a few settings (tools/render.cpp): no clicks or NaN, levels and CPU
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
g++ -O1 -std=gnu++14 -fsanitize=address,undefined -I"$B" -I"$MV/wrapper" "$HERE/SamplerFilter.cpp" \
  "$HERE/tools/filter_check.cpp" -o "$B/filter_check" 2>/dev/null
"$B/filter_check"
g++ -O2 -std=gnu++14 -I"$B" -I"$MV/wrapper" "$HERE/SamplerFilter.cpp" "$MPC/tools/render.cpp" -lpthread \
  -o "$B/render_sf" 2>/dev/null
echo "== drum loop"
for p in "filter=100" "filter=60" "filter=35 drive=40" "bits_step=4 rate_step=5" "bits_step=4 rate_step=5 track=1 filter=70" \
         "bits_step=2 rate_step=1 track=1" "bits_step=0 rate_step=0 filter=20 mix=50"; do
  # shellcheck disable=SC2086
  printf "%-40s " "$p"; "$B/render_sf" "$B/loop.wav" $p | sed 's/^[^:]*: //'
done
