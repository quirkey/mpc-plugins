#!/usr/bin/env bash
# ATTACK / GATE / RELEASE checks:  Breaks/tools/shape_check.sh
#   1. the envelope over each 16th of the Grid break, against plain playback (check_shape.py): attack alone, a hard
#      gate, gate + release (the fade from the gate on), all three, release with no gate (nothing); and over each
#      32nd hit of a held roll
#   2. no clicks: a short gate and a fast attack on the tone break and a real one (check_clicks.py)
# Off (the defaults) renders identically to before: regress_check.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B/shape"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
g++ -O2 -std=gnu++14 -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread -o "$B/render_shape" \
  2>/dev/null
fails=0
run() { "$@" || fails=$((fails + 1)); }
render() {   # render <out> <notes> <data> [params...]
  local out="$1" notes="$2" data="$3"
  shift 3
  MPC_RENDER_NOTES="$notes" MPC_RENDER_DATA_DIR="$data" MPC_RENDER_BPM=120 "$B/render_shape" "$out" "$@" >/dev/null 2>&1
}

echo "== 1. envelope (Grid break, 120 BPM)"
render "$B/shape/plain.wav" none "$B/test_breaks" break=2
for spec in "attack=40|0.1:0.25,0.2:0.5,0.3:0.75,0.45:1" \
            "gate=40|0.1:1,0.3:1,0.45:0,0.55:0" \
            "gate=20 release=40|0.1:1,0.3:0.75,0.4:0.5,0.5:0.25,0.55:0.125" \
            "attack=20 gate=30 release=20|0.05:0.25,0.2:1,0.25:1,0.35:0.75,0.45:0.25,0.55:0" \
            "gate=100 release=50|0.25:1,0.5:1,0.55:1"; do
  IFS='|' read -r params points <<< "$spec"
  # shellcheck disable=SC2086
  render "$B/shape/s.wav" none "$B/test_breaks" break=2 $params
  printf "%-28s " "$params"
  run python3 "$HERE/tools/check_shape.py" "$B/shape/plain.wav" "$B/shape/s.wav" 0.25 "$points"
done
echo "-- a held roll (pad 5): the envelope on each 32nd hit"
render "$B/shape/plain.wav" "1.0:40:4.0" "$B/test_breaks" break=2 pad_len=0
render "$B/shape/s.wav" "1.0:40:4.0" "$B/test_breaks" break=2 pad_len=0 attack=40
printf "%-28s " "roll attack=40"
run python3 "$HERE/tools/check_shape.py" "$B/shape/plain.wav" "$B/shape/s.wav" 0.125 "0.1:0.25,0.2:0.5,0.3:0.75,0.5:1" \
  120 1.0 5.0

echo "== 2. clicks"
DATA="$B/classics"
[ -d "$DATA" ] && ls "$DATA"/*.wav >/dev/null 2>&1 || DATA="$B/test_breaks"
for spec in "$B/test_breaks:2" "$DATA:0"; do
  IFS=: read -r data brk <<< "$spec"
  for grain in 0 5; do
    render "$B/shape/plain.wav" none "$data" break=$brk grain=$grain
    for params in "gate=30" "attack=5" "attack=5 gate=50 release=10" "gate=30 release=40"; do
      # shellcheck disable=SC2086
      render "$B/shape/s.wav" none "$data" break=$brk grain=$grain $params
      printf "%-44s " "$(basename "$data") grain $grain $params"
      run python3 "$HERE/tools/check_clicks.py" "$B/shape/plain.wav" "$B/shape/s.wav"
    done
  done
done
[ "$fails" = 0 ] && echo "PASSED" || { echo "FAILED ($fails)"; exit 1; }
