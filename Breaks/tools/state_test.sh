#!/usr/bin/env bash
# Build and run Breaks/tools/state_test.cpp against the engine under ASan/UBSan:  Breaks/tools/state_test.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MPC="$(cd "$HERE/.." && pwd)"
ROOT="$MPC"
MV="$MPC/mpc-vst-plugins"
B="$HERE/build"
mkdir -p "$B"
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
python3 "$HERE/tools/make_test_breaks.py" "$B/test_breaks" >/dev/null
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
cd "$ROOT"
g++ $SAN -std=gnu++17 -I"$B" -I"$MV/wrapper" -c Breaks/Breaks.cpp -o "$B/state_engine.o"
g++ $SAN -std=gnu++17 -I"$B" -I"$MV/wrapper" -c Breaks/tools/state_test.cpp -o "$B/state_test.o"
g++ $SAN "$B/state_engine.o" "$B/state_test.o" -lpthread -o "$B/state_test"
"$B/state_test" "$B/test_breaks" "$B/state_scratch"
