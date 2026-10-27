#!/usr/bin/env bash
# Pad effect checks:  Breaks/tools/pads_check.sh
#   0. the PADS page tiles: labels, and "pad_N_on" lit while its effect is held (tiles_check.cpp); the waveform's
#      columns: levels, the playing slice lit, reported once, not saved (wave_check.cpp)
#   1. each effect (LENGTH Free: exactly while held; then LENGTH 1/4's snapped start and whole-beat end)
#      each effect, held over 2-3 s of the Grid break: right inside the hold, and the break back in its place after
#      (check_pads.py); pads pressed on 16ths, and pressed and released mid-16th
#   2. no clicks: every effect held on the tone break and on a real one, with and without grains (check_clicks.py)
#   3. the same pad performance renders identically twice
# Pads 1-7 = reverse, gap, flam, triplet, roll, stutter, half; 8 / 12 = octave down / up; 9 / 10 = ramp down / up;
# 13 / 14 = speed down / up; 15 / 16 = velocity down / up.
# Pad n sends 35 + n on chromatic pads (here) and SCALE[n - 1] on MPC's default layout (a C major scale from C5).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
MV="$MPC/mpc-vst-plugins"
mkdir -p "$B/pads"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
g++ -O2 -std=gnu++14 -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread -o "$B/render_pads" \
  2>/dev/null
g++ -O2 -std=gnu++14 -DBREAKS_DEBUG -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$MPC/tools/render.cpp" -lpthread \
  -o "$B/render_pads_dbg" 2>/dev/null
fails=0
run() { "$@" || fails=$((fails + 1)); }
render() {   # render <out> <notes> <data> <bpm> [params...]
  local out="$1" notes="$2" data="$3" bpm="$4"
  shift 4
  MPC_RENDER_NOTES="$notes" MPC_RENDER_DATA_DIR="$data" MPC_RENDER_BPM=$bpm "$B/render_pads" "$out" "$@" >/dev/null 2>&1
}
FX=(reverse gap flam triplet roll stutter half octdown rampdown rampup - octup speeddown speedup veldown velup)
SCALE=(72 74 76 77 79 81 83 84 86 88 89 91 93 95 96 98)

echo "== 0. PADS page tiles (labels, lit while held)"
g++ -O1 -std=gnu++14 -fsanitize=address,undefined -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$HERE/tools/tiles_check.cpp" \
  -lpthread -o "$B/tiles_check" 2>/dev/null
run "$B/tiles_check"
g++ -O1 -std=gnu++14 -fsanitize=address,undefined -I"$B" -I"$MV/wrapper" "$HERE/Breaks.cpp" "$HERE/tools/wave_check.cpp" \
  -lpthread -o "$B/wave_check" 2>/dev/null
run "$B/wave_check" "$B/test_breaks"

echo "== 1. effects (Grid break, 120 BPM, held 2.0-3.0 s)"
for i in 0 1 2 3 4 5 6; do
  render "$B/pads/fx.wav" "2.0:$((36 + i)):1.0" "$B/test_breaks" 120 break=2 pad_len=0
  run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" "${FX[$i]}" 2.0 3.0
done
echo "-- the same from MPC's default pad layout (a C major scale from C5)"
for i in 0 1 4 6; do
  render "$B/pads/fx.wav" "2.0:${SCALE[$i]}:1.0" "$B/test_breaks" 120 break=2 pad_len=0
  run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" "${FX[$i]}" 2.0 3.0
done
render "$B/pads/fx.wav" "2.0:${SCALE[9]}:1.0" "$B/test_breaks" 120 break=2 pad_len=0 ramp=1
run python3 "$HERE/tools/check_ramp.py" "$B/pads/fx.wav" up 2.0 3.0 3
echo "-- held mid-16th (2.03-2.97 s): the 16ths after it are still back in place"
for i in 0 1 5 6; do
  render "$B/pads/fx.wav" "2.03:$((36 + i)):0.94" "$B/test_breaks" 120 break=2 pad_len=0
  run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" "${FX[$i]}" 2.03 2.97
done

echo "-- octave pads (8 = down, 12 = up), alone and held with an effect (roll + down, stutter + up)"
render "$B/pads/fx.wav" "2.0:43:1.0" "$B/test_breaks" 120 break=2 pad_len=0
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" none 2.0 3.0 120 500
render "$B/pads/fx.wav" "2.03:47:0.94" "$B/test_breaks" 120 break=2 pad_len=0
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" none 2.03 2.97 120 2000
render "$B/pads/fx.wav" "2.0:40:1.0,2.0:43:1.0" "$B/test_breaks" 120 break=2 pad_len=0
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" roll 2.0 3.0 120 500
render "$B/pads/fx.wav" "2.0:41:1.0,2.0:47:1.0" "$B/test_breaks" 120 break=2 pad_len=0
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" stutter 2.0 3.0 120 2000
echo "-- octave pressed mid-hold of half (the half-speed place carries on, not restarted)"
render "$B/pads/fx.wav" "2.0:42:1.0,2.53:43:0.47" "$B/test_breaks" 120 break=2 pad_len=0
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" half 2.0 3.0

echo "-- ramp pads (9 = down, 10 = up): the pitch moves each 16th from where the pad went down, held at 2 octaves"
render "$B/pads/fx.wav" "2.0:45:1.0" "$B/test_breaks" 120 break=2 pad_len=0 ramp=1
run python3 "$HERE/tools/check_ramp.py" "$B/pads/fx.wav" up 2.0 3.0 3
render "$B/pads/fx.wav" "2.0:44:1.5" "$B/test_breaks" 120 break=2 pad_len=0 ramp=3
run python3 "$HERE/tools/check_ramp.py" "$B/pads/fx.wav" down 2.0 3.5 12
render "$B/pads/fx.wav" "2.0:45:1.0,2.0:43:1.0" "$B/test_breaks" 120 break=2 pad_len=0 ramp=2
run python3 "$HERE/tools/check_ramp.py" "$B/pads/fx.wav" up 2.0 3.0 6 120 -1
render "$B/pads/fx.wav" "2.0:45:1.0" "$B/test_breaks" 120 break=2 pad_len=3 ramp=1
run python3 "$HERE/tools/check_ramp.py" "$B/pads/fx.wav" up 2.0 3.0 3

echo "-- speed pads (13 = down, 14 = up): the hits come faster / slower, doubling or halving every RAMP octave's time"
# held 2.0-4.0 s = beats 4-8, pressed on a 16th (LENGTH 1/16) so the first hit is the pad's own 16th; alone (from
# 16ths), with a roll (32nds) and with triplets
for spec in "40,49|up|8|3|12" "40,48|down|8|3|12" "49|up|4|2|6" "48|down|4|2|6" "39,49|up|6|1|3" "40,49|up|8|4|24"; do
  IFS='|' read -r notes way base ramp semis <<< "$spec"
  held=""
  for n in ${notes//,/ }; do held="$held${held:+,}2.0:$n:2.0"; done
  MPC_RENDER_NOTES="$held" MPC_RENDER_DATA_DIR="$B/test_breaks" MPC_RENDER_BPM=120 "$B/render_pads_dbg" "$B/pads/fx.wav" \
    break=2 pad_len=1 ramp=$ramp >/dev/null 2>"$B/pads/speed.txt"
  printf "notes %-6s speed %-4s from %s/beat: " "$notes" "$way" "$base"
  run python3 "$HERE/tools/check_speed.py" "$B/pads/speed.txt" "$way" "$base" 4.0 8.0 "$semis"
done
echo "-- velocity pads (15 = down, 16 = up): each roll hit's level sweeps up from silence / down to it, per RAMP time"
render "$B/pads/plain.wav" "2.0:40:2.0" "$B/test_breaks" 120 break=2 pad_len=1 ramp=1
for spec in "51|up" "50|down"; do
  IFS='|' read -r note way <<< "$spec"
  render "$B/pads/fx.wav" "2.0:40:2.0,2.0:$note:2.0" "$B/test_breaks" 120 break=2 pad_len=1 ramp=1
  printf "roll + velocity %-4s " "$way"
  run python3 "$HERE/tools/check_vel.py" "$B/pads/plain.wav" "$B/pads/fx.wav" "$way" 4.0 8.0 4.0 0.125
done

echo "-- LENGTH 1/4 (120 BPM: 16ths every 0.125 s): a press snaps to the nearest 16th, a release plays on to whole beats"
# press, release -> the window it should sound in: 2.03 (early in 16th 16) counts from 2.0 (its first 30 ms
# already played, so that 16th is not checked); 2.09 (late in it) at 2.125;
# held 0.27 s -> one beat; a tap -> one beat; held 0.6 s from 2.0 -> two beats
for spec in "2.03:0.27:2.03:2.5" "2.09:0.11:2.125:2.625" "2.09:0.01:2.125:2.625" "2.0:0.6:2.0:3.0"; do
  IFS=: read -r t len w0 w1 <<< "$spec"
  for i in 0 4 5 6; do
    render "$B/pads/fx.wav" "$t:$((36 + i)):$len" "$B/test_breaks" 120 break=2 pad_len=3
    printf "pressed %s for %s: " "$t" "$len"
    run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" "${FX[$i]}" "$w0" "$w1"
  done
done
echo "-- LENGTH 1/4: a second pad pressed while the first is held switches at once (on its own nearest 16th)"
render "$B/pads/fx.wav" "2.0:38:0.5,2.26:40:0.5" "$B/test_breaks" 120 break=2 pad_len=3
run python3 "$HERE/tools/check_pads.py" "$B/pads/fx.wav" roll 2.25 3.25

echo "== 2. clicks"
DATA="$B/classics"
[ -d "$DATA" ] && ls "$DATA"/*.wav >/dev/null 2>&1 || DATA="$B/test_breaks"
for spec in "$B/test_breaks:2:100" "$DATA:0:120"; do
  IFS=: read -r data brk bpm <<< "$spec"
  for grain in 0 5; do
    render "$B/pads/plain.wav" none "$data" $bpm break=$brk grain=$grain
    # octave up plays at twice the rate (steeper edges), so its reference has PITCH at +12; octave down's
    # steepest edges are the plain playback around the hold
    render "$B/pads/up.wav" none "$data" $bpm break=$brk grain=$grain pitch=12
    for i in 0 1 2 3 4 5 6 7 8 9 11 12 13 14 15; do
      ref="$B/pads/plain.wav"; [ $i = 9 ] || [ $i = 11 ] && ref="$B/pads/up.wav"
      printf "%-30s " "$(basename "$data") grain $grain ${FX[$i]}"
      render "$B/pads/g.wav" "1.03:$((36 + i)):1.5,4.0:${SCALE[$i]}:2.0" "$data" $bpm break=$brk grain=$grain
      run python3 "$HERE/tools/check_clicks.py" "$ref" "$B/pads/g.wav"
    done
    for combo in "40,49:roll+speedup" "40,48:roll+speeddown" "40,51:roll+velup" "40,49,51:roll+speedup+velup"; do
      IFS=: read -r notes label <<< "$combo"
      held=""
      for n in ${notes//,/ }; do held="$held${held:+,}1.03:$n:2.5"; done
      printf "%-30s " "$(basename "$data") grain $grain $label"
      render "$B/pads/g.wav" "$held" "$data" $bpm break=$brk grain=$grain ramp=2
      run python3 "$HERE/tools/check_clicks.py" "$B/pads/plain.wav" "$B/pads/g.wav"
    done
  done
done

echo "== 3. determinism"
SCENE="0.5:40:1.0,0.7:47:0.6,2.0:36:0.5,3.1:41:0.9,3.3:43:0.4,4.0:42:2.0,4.5:45:1.0,6.0:38:0.5,6.2:44:0.5,7.0:39:1.0"
render "$B/pads/d1.wav" "$SCENE" "$DATA" 120 break=0 grain=4
render "$B/pads/d2.wav" "$SCENE" "$DATA" 120 break=0 grain=4
if cmp -s "$B/pads/d1.wav" "$B/pads/d2.wav"; then echo "ok   the same pad performance renders identically"
else echo "FAIL the same pad performance differs"; fails=$((fails + 1)); fi
[ "$fails" = 0 ] && echo "PASSED" || { echo "FAILED ($fails)"; exit 1; }
