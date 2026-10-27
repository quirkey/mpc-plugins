#!/usr/bin/env bash
# The break follows the sequence:  Breaks/tools/seq_check.sh
#   1. a break picked mid-pass takes over at the next wrap, from its top (check_switch.py), with Half and Grain
#      too; and right away when nothing is playing yet
#   2. each pass of a loop plays the same, except under EVOLVE (check_passes.py)
# Play, wraps and locates under MPC's transport quirks: stretch_check.sh (check_sync.py).
set -euo pipefail
export MPC_RENDER_NOTES=none
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B/seq"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
g++ -O2 -std=gnu++14 -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread -o "$B/render_seq" \
  2>/dev/null
fails=0
run() { "$@" || fails=$((fails + 1)); }
render() {   # render <out> <loop beats> [env...] -- [params...]
  local out="$1" loop="$2"
  shift 2
  local envs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
  shift
  env MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=120 MPC_RENDER_LOOP="$loop" ${envs[@]+"${envs[@]}"} \
    "$B/render_seq" "$out" "$@" >/dev/null 2>&1
}

# test breaks, sorted: 0 Acid Loop (1 bar), 1 Funk Test 96 (4 bars), 2 Grid 120bpm (2 bars)
echo "== 1. switching (a 6-beat loop at 120 BPM: wraps every 3 s)"
for extra in "" "half=1" "grain=5"; do
  # shellcheck disable=SC2086
  render "$B/seq/old.wav" 6 -- break=2 $extra
  # shellcheck disable=SC2086
  render "$B/seq/new.wav" 6 -- break=0 $extra
  # shellcheck disable=SC2086
  render "$B/seq/sw.wav" 6 MPC_RENDER_SET="1.3:break=0" -- break=2 $extra
  printf "%-10s Grid -> Acid at 1.3 s:  " "${extra:-plain}"
  run python3 "$HERE/tools/check_switch.py" "$B/seq/sw.wav" "$B/seq/old.wav" "$B/seq/new.wav" 3.0
  # shellcheck disable=SC2086
  render "$B/seq/old.wav" 6 -- break=1 $extra
  # shellcheck disable=SC2086
  render "$B/seq/new.wav" 6 -- break=2 $extra
  # shellcheck disable=SC2086
  render "$B/seq/sw.wav" 6 MPC_RENDER_SET="4.4:break=2" -- break=1 $extra
  printf "%-10s Funk -> Grid at 4.4 s:  " "${extra:-plain}"
  run python3 "$HERE/tools/check_switch.py" "$B/seq/sw.wav" "$B/seq/old.wav" "$B/seq/new.wav" 6.0
done

echo "== 2. passes (an 8-beat loop: 4 s)"
for spec in "pattern=0|same" "pattern=1|same" "pattern=4|differ"; do
  IFS='|' read -r pat want <<< "$spec"
  render "$B/seq/p.wav" 8 -- break=2 vary=90 seed=3 "$pat"
  printf "vary 90 %-10s " "$pat"
  run python3 "$HERE/tools/check_passes.py" "$B/seq/p.wav" 4.0 "$want"
done
[ "$fails" = 0 ] && echo "PASSED" || { echo "FAILED ($fails)"; exit 1; }
