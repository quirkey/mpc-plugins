#!/usr/bin/env bash
# Builds Roads' engine with tools/vib_check.cpp (no VST wrapper) under ASan/UBSan and runs it:
#   Roads/tools/vib_check.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MV="$HERE/../../mpc-vst-plugins"
eval "$(python3 "$MV/tools/gen_vst.py" "$HERE/../vst.json" --shell)"
python3 "$MV/tools/gen_vst.py" "$HERE/../vst.json" --params-h >/dev/null
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
OUT="$ROOT/$PORT/build/vib_check"
cd "$ROOT"
objs=()
for f in $SOURCES "$PORT/tools/vib_check.cpp"; do
  o="$PORT/build/vib_$(basename "$f").o"
  g++ $SAN -std=gnu++11 $CFLAGS -I"$PORT/build" -I"$MV/wrapper" -c "$f" -o "$o" 2>&1 | grep -E "error" || true
  objs+=("$o")
done
g++ $SAN "${objs[@]}" $LIBS -lpthread -o "$OUT"
"$OUT"
