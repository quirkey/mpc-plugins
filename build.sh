#!/usr/bin/env bash
# Build, test and audition the MPC OS plugins in this folder.
#
#   ./build.sh test   <Plugin>            offline x86 test under ASan/UBSan (mpc-vst-plugins tools/test_port.sh)
#   ./build.sh arm    <Plugin>            device build: .so + skin + plugin-list entry in <Plugin>/build/ (Docker)
#   ./build.sh render <Plugin> [k=v ...]  render <Plugin>/build/render.wav offline (instrument phrase / effect loop)
#   ./build.sh armtest <Plugin>           the same host test built for the device and run on an emulated ARMv7 CPU
#   ./build.sh all    <Plugin>            test, armtest, then arm
#   ./build.sh release <Plugin> [1.0.0]   arm build, then an installable zip in <Plugin>/dist/ (tools/release.py)
#   ./build.sh preview <Plugin>           skin + <Plugin>/build/preview_N.png per page (no Docker unless "art": "html")
#
# <Plugin> is a folder here with a vst.json (Breaks, Roads, ...). See README.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MV="$HERE/mpc-vst-plugins"
CMD="${1:-}"; PLUG="${2:-}"
[ -n "$CMD" ] && [ -n "$PLUG" ] || { sed -n '2,12p' "$0"; exit 2; }
CFG="$HERE/$PLUG/vst.json"
[ -f "$CFG" ] || { echo "no $CFG" >&2; exit 1; }
[ -f "$MV/tools/build_port.sh" ] || { echo "mpc-vst-plugins missing: run ./setup.sh" >&2; exit 1; }

# For the catalog manifest in release zips (mpc-vst-plugins docs/RELEASING.md): the repo and its licence (an SPDX id,
# matching LICENSE). Without a licence the zip still builds, but isn't listable.
REPO_SLUG="quirkey/mpc-plugins"
SPDX="MIT"

render() {
  shift 2
  eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"
  python3 "$MV/tools/gen_vst.py" "$CFG" --params-h >/dev/null
  local out="$ROOT/$PORT/build/render" objs=()
  ( cd "$ROOT"
    for f in $SOURCES "$HERE/tools/render.cpp"; do
      o="$PORT/build/render_$(basename "$f").o"
      case "$f" in
        *.cpp|*.cc) g++ -O2 -std=gnu++14 $CFLAGS -I"$PORT/build" -I"$MV/wrapper" -c "$f" -o "$o" ;;
        *) gcc -O2 -std=gnu11 $CFLAGS -I"$PORT/build" -I"$MV/wrapper" -c "$f" -o "$o" ;;
      esac
      echo "$o"
    done > "$PORT/build/render_objs"
    g++ $(cat "$PORT/build/render_objs") $LIBS -lpthread -o "$out" )
  "$out" "$ROOT/$PORT/build/render.wav" "$@"
}

# The device build, then a check that the .so has no ARMv8-only VFP instructions: the Gen1 MPC's Cortex-A17 is
# ARMv7-A and dies with SIGILL on them, but QEMU runs them, so offline tests can't catch it (DaisySP's dsp.h
# inline-asm vmaxnm/vminnm did exactly this; see common/daisysp_armv7a.h). The code is Thumb-2, and the .so is
# stripped of the ARM/Thumb mapping symbols, so it is decoded as Thumb.
arm() {
  eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"
  "$MV/tools/build_port.sh" "$CFG" armv7
  local bad
  bad="$(docker run --rm --platform linux/arm/v7 -v "$ROOT/$PORT/build":/b:ro arm32v7/gcc:11-bullseye \
    sh -c "objdump -d -M force-thumb '/b/$SO' | grep -E 'vmaxnm|vminnm|vrint[a-z]|vsel[a-z]|vcvt[anpm]\.'" || true)"
  if [ -n "$bad" ]; then
    echo "$bad" | head -5 >&2
    echo "FAIL $SO has $(echo "$bad" | wc -l | tr -d ' ') ARMv8-only instruction(s): it would crash a Gen1 MPC (SIGILL)." >&2
    echo "     DaisySP sources need \"-include\", \"common/daisysp_armv7a.h\" first in vst.json cflags." >&2
    exit 1
  fi
  echo "ok   $SO: no ARMv8-only instructions (Gen1 Cortex-A17 safe)"
}

# The host test built with the device's compiler and flags, run on QEMU's cortex-a15 (ARMv7-A, like the MPC).
armtest() {
  eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"
  python3 "$MV/tools/gen_vst.py" "$CFG" --params-h >/dev/null
  docker run --rm --platform linux/arm/v7 -e QEMU_CPU=cortex-a15 -v "$ROOT":/b -v "$HERE/tools":/t:ro \
    arm32v7/gcc:11-bullseye bash /t/armtest.sh "$PORT" "$SOURCES" "$CFLAGS" "$CFLAGS_ARM"
}

# vst.json "user_data": ["breaks"] names folders inside the plugin folder that hold the user's own files:
# the installer keeps them on upgrade (release.py --user-data) and the zip ships them empty.
release() {
  eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"
  local b="$ROOT/$PORT/build" skin ud=()
  arm
  skin="$(ls -d "$b"/skin/*/)"
  for d in $(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("user_data", [])))' "$CFG"); do
    ud+=(--user-data "$d")
  done
  # vst.json "extra": ["Roads/epiano_samples.bin:samples/epiano_samples.bin"]: SRC (from the repo root) shipped at
  # DEST inside the plugin folder
  for e in $(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("extra", [])))' "$CFG"); do
    ud+=(--extra "$ROOT/$e")
  done
  # the catalog: id = the folder name in lower case; vst.json "about" is INSTALL.md's one line; <Plugin>/bench.txt is
  # the device's `tools/bench.sh ... -j` result, when committed
  local id about
  id="$(echo "$PORT" | tr '[:upper:]' '[:lower:]')"
  about="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("about", ""))' "$CFG")"
  [ -f "$ROOT/$PORT/bench.txt" ] && ud+=(--bench "$ROOT/$PORT/bench.txt")
  if [ -n "$SPDX" ]; then
    ud+=(--id "$id" --repo "$REPO_SLUG" --license "$SPDX")
  else
    echo "note: no SPDX licence set in build.sh, so this zip can't be listed in the catalog" >&2
  fi
  python3 "$MV/tools/release.py" --so "$b/$SO" --skin "${skin%/}" --entry "$b/pluginlist-entry.xml" \
    --version "${3:-1.0.0}" --about "$about" ${ud[@]+"${ud[@]}"} -o "$ROOT/$PORT/dist"
  [ -z "$SPDX" ] || python3 "$MV/tools/catalog_check.py" "$ROOT/$PORT/dist/$PORT-${3:-1.0.0}-mpc-armv7.zip" --catalog
}

# The skin and a PNG of each page, for iterating on layout.conf: the same renderer as the device build, so the PNGs
# are the pixels the MPC shows. Native and ~2 s with the stock renderer; with vst.json "art": "html" (knob looks,
# art_css) the browser renderer in the toolkit's Chromium image, as build_port.sh runs it (~15 s).
# (html_art has no sized-button drawing: give `button ... w= h=` an img= look there.)
preview() {
  eval "$(python3 "$MV/tools/gen_vst.py" "$CFG" --shell)"
  local b="$ROOT/$PORT/build" art="$ROOT/$PORT/build/host_shadow_art"
  mkdir -p "$b"
  rm -f "$b"/preview_*.png
  if [ "$ART" = html ]; then
    docker build -q -t mpc-vst-html-art "$MV/tools/html_art" >/dev/null
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/w -v "$MV":/mv:ro -w /w mpc-vst-html-art \
      python3 /mv/tools/gen_vst.py "$PORT/vst.json" > "$b/preview.log"
  else
    if [ ! -x "$art" ] || [ "$MV/tools/shadow_art.c" -nt "$art" ]; then
      cc -O2 -I"$MV/tools/vendor/force-shadow/tools" -o "$art" "$MV/tools/shadow_art.c" -lm
    fi
    env SHADOW_ART="$art" ${TITLE_FONT:+SHADOW_TITLE_FONT="$ROOT/$PORT/$TITLE_FONT"} \
      python3 "$MV/tools/gen_vst.py" "$CFG" > "$b/preview.log"
  fi
  grep -v '^params.h\|^skin:' "$b/preview.log" || true
  python3 "$MV/tools/studio.py" preview "$(ls -d "$b"/skin/*/)Plugin Skins" -o "$b/preview_%d.png"
  ls "$b"/preview_*.png
}

case "$CMD" in
  test)   "$MV/tools/test_port.sh" "$CFG" ;;
  preview) preview ;;
  arm)    arm ;;
  render) render "$@" ;;
  armtest) armtest ;;
  all)    "$MV/tools/test_port.sh" "$CFG" && armtest && arm ;;
  release) release "$@" ;;
  *)      sed -n '2,12p' "$0"; exit 2 ;;
esac
