#!/usr/bin/env bash
# Old projects must sound the same:  Breaks/tools/regress_check.sh [git-ref]   (default HEAD)
# Renders the same scenes with this tree's Breaks.cpp and with <git-ref>'s, at default Grain and Half, and
# compares the WAVs byte for byte: tempos around and away from the breaks', MPC's transport quirks, variations.
# The classics (tools/stage_classics.sh, build/classics) are used when staged, else the generated test breaks.
set -euo pipefail
export MPC_RENDER_NOTES=none   # no test phrase: Breaks plays notes as pad effects
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
REPO="$MPC"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
REF="${1:-HEAD}"
OLD="$B/regress_ref"
mkdir -p "$B/regress"

python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
g++ -O2 -std=gnu++14 -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread -o "$B/render_new"

# the reference: its Breaks sources and params.json in a scratch folder laid out like this repo (common/, tools/)
rm -rf "$OLD"
mkdir -p "$OLD/Breaks/build"
( cd "$REPO" && git archive "$REF" Breaks common tools/render.cpp ) | tar -x -C "$OLD"
python3 "$MV/tools/gen_vst.py" "$OLD/Breaks/vst.json" --params-h >/dev/null
g++ -O2 -std=gnu++14 -I"$OLD/Breaks/build" -I"$MV/wrapper" "$OLD/Breaks/Breaks.cpp" "$OLD/tools/render.cpp" \
  -lpthread -o "$B/render_old"

DATA="$B/classics"
[ -d "$DATA" ] && ls "$DATA"/*.wav >/dev/null 2>&1 || DATA="$B/test_breaks"
md() { md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1; }
fails=0
scene() {   # scene <name> <env...> -- <params...>
  local name="$1"; shift
  local env=()
  while [ "$1" != "--" ]; do env+=("$1"); shift; done
  shift
  env MPC_RENDER_DATA_DIR="$DATA" ${env[@]+"${env[@]}"} "$B/render_old" "$B/regress/$name.old.wav" "$@" >/dev/null 2>&1
  env MPC_RENDER_DATA_DIR="$DATA" ${env[@]+"${env[@]}"} "$B/render_new" "$B/regress/$name.new.wav" "$@" >/dev/null 2>&1
  if [ "$(md "$B/regress/$name.old.wav")" = "$(md "$B/regress/$name.new.wav")" ]; then
    echo "ok   $name"
  else
    echo "FAIL $name differs from $REF"
    fails=$((fails + 1))
  fi
}
for bpm in 77 96 120 141; do
  for brk in 0 1 2; do
    scene "bpm${bpm}_break${brk}" MPC_RENDER_BPM=$bpm -- break=$brk
  done
done
scene transport_120 MPC_RENDER_BPM=120 MPC_RENDER_TRANSPORT=1 -- break=0
scene transport_93 MPC_RENDER_BPM=93 MPC_RENDER_TRANSPORT=1 -- break=1
scene vary_120 MPC_RENDER_BPM=120 -- break=0 vary=70 seed=5 pattern=4
scene vary_96 MPC_RENDER_BPM=96 MPC_RENDER_TRANSPORT=1 -- break=2 vary=90 seed=17 swap=100
scene pitch_grit MPC_RENDER_BPM=110 -- break=1 pitch=-5 bits=8 rate=26.04 bars=3
echo "data: $DATA"
[ "$fails" = 0 ] && echo "PASSED: default Grain/Half render identically to $REF" || { echo "FAILED ($fails)"; exit 1; }
