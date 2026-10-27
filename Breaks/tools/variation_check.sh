#!/usr/bin/env bash
# All the Breaks variation checks:  Breaks/tools/variation_check.sh
#   1. Variation.h unit test (ASan/UBSan)
#   2. the audio matches each 16th's decision (debug render of the grid break + check_variation.py)
#   3. a seed renders byte-identically twice; another seed differs
#   4. with Vary 0 the transport sync check still passes
set -euo pipefail
export MPC_RENDER_NOTES=none   # no test phrase: Breaks plays notes as pad effects
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null

echo "== 1. decisions"
g++ -O1 -g -fsanitize=address,undefined -std=gnu++17 "$HERE/tools/variation_test.cpp" -o "$B/variation_test"
"$B/variation_test" | tail -n 1

echo "== 2. audio matches decisions"
g++ -O2 -std=gnu++14 -DBREAKS_DEBUG -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread \
  -o "$B/render_dbg" 2>/dev/null
for bpm in 120 96; do
  MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=$bpm "$B/render_dbg" "$B/var.wav" break=2 vary=70 pattern=4 seed=5 \
    2> "$B/var_trace.txt" >/dev/null
  printf "%s BPM: " $bpm
  python3 "$HERE/tools/check_variation.py" "$B/var.wav" "$B/var_trace.txt" $bpm
done

echo "== 3. determinism"
r() { MPC_RENDER_DATA_DIR="$B/test_breaks" "$B/render_dbg" "$B/$1.wav" break=1 vary=60 seed=$2 2>/dev/null >/dev/null; md5 -q "$B/$1.wav" 2>/dev/null || md5sum "$B/$1.wav" | cut -d' ' -f1; }
a="$(r seed42a 42)"; b="$(r seed42b 42)"; c="$(r seed43 43)"
[ "$a" = "$b" ] && echo "ok   seed 42 twice: identical" || { echo "FAIL seed 42 twice differs"; exit 1; }
[ "$a" != "$c" ] && echo "ok   seed 43: different" || { echo "FAIL seed 43 same as 42"; exit 1; }

echo "== 4. Vary 0 keeps sync"
MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=120 MPC_RENDER_TRANSPORT=1 MPC_RENDER_LOG="$B/transport.csv" \
  "$B/render_dbg" "$B/sync.wav" break=2 2>/dev/null >/dev/null
python3 "$HERE/tools/check_sync.py" "$B/sync.wav" "$B/transport.csv"
