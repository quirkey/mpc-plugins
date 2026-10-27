#!/usr/bin/env bash
# Grain and Half time checks:  Breaks/tools/stretch_check.sh
#   1. timing: under MPC's transport quirks, the right slice starts on every 16th, for Grain 1/16 / 1/32 / 20 ms /
#      6 ms, with Half off and on (check_sync.py; half time plays each slice over two song 16ths)
#   2. variations still land per break 16th in half time (check_variation.py on the debug trace)
#   3. grain splices don't click (check_clicks.py), on a real break when the classics are staged
# Grain option indexes: 0 = 1/16, 1 = 1/32, 5 = 20 ms, 7 = 6 ms.
set -euo pipefail
export MPC_RENDER_NOTES=none   # no test phrase: Breaks plays notes as pad effects
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B/stretch"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
g++ -O2 -std=gnu++14 -DBREAKS_DEBUG -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread \
  -o "$B/render_dbg" 2>/dev/null
fails=0
run() { "$@" || fails=$((fails + 1)); }

echo "== 1. timing (Grid break, MPC transport)"
for half in 0 1; do
  adv=1; [ $half = 1 ] && adv=0.5
  for grain in 0 1 5 7; do
    printf "half %s grain %s: " $half $grain
    MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=120 MPC_RENDER_TRANSPORT=1 MPC_RENDER_LOG="$B/stretch/t.csv" \
      "$B/render_dbg" "$B/stretch/sync.wav" break=2 half=$half grain=$grain 2>/dev/null >/dev/null
    run python3 "$HERE/tools/check_sync.py" "$B/stretch/sync.wav" "$B/stretch/t.csv" $adv
  done
done

echo "== 2. variations in half time"
for grain in 0 5; do
  printf "grain %s: " $grain
  MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=120 "$B/render_dbg" "$B/stretch/var.wav" break=2 vary=70 \
    pattern=4 seed=5 half=1 grain=$grain 2> "$B/stretch/var.txt" >/dev/null
  run python3 "$HERE/tools/check_variation.py" "$B/stretch/var.wav" "$B/stretch/var.txt" 120
done

echo "== 3. clicks"
DATA="$B/classics"
[ -d "$DATA" ] && ls "$DATA"/*.wav >/dev/null 2>&1 || DATA="$B/test_breaks"
for bpm in 90 120; do
  MPC_RENDER_DATA_DIR="$DATA" MPC_RENDER_BPM=$bpm "$B/render_dbg" "$B/stretch/plain.wav" break=0 2>/dev/null >/dev/null
  for p in "grain=5" "grain=7" "half=1 grain=5" "half=1 grain=7" "half=1"; do
    printf "%s BPM %-16s " $bpm "$p"
    MPC_RENDER_DATA_DIR="$DATA" MPC_RENDER_BPM=$bpm "$B/render_dbg" "$B/stretch/g.wav" break=0 $p 2>/dev/null >/dev/null
    run python3 "$HERE/tools/check_clicks.py" "$B/stretch/plain.wav" "$B/stretch/g.wav"
  done
done
[ "$fails" = 0 ] && echo "PASSED" || { echo "FAILED ($fails)"; exit 1; }
