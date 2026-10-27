#!/usr/bin/env bash
# One-time setup for building the plugins (see README.md). Safe to re-run.
#   - fetches the pinned submodules: the mpc-vst-plugins toolkit and DaisySP
#   - checks Docker can run the armhf build image (QEMU emulation of linux/arm/v7)
#   - pre-pulls the images tools/build_port.sh uses
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ok()   { printf '  \033[32mok\033[0m   %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAILED=1; }
FAILED=0

echo "Submodules"
git -C "$HERE" submodule update --init --depth 1 mpc-vst-plugins DaisySP
ok "mpc-vst-plugins at $(git -C "$HERE/mpc-vst-plugins" rev-parse --short HEAD)"
if [ -f "$HERE/DaisySP/Source/Effects/chorus.cpp" ]; then
  ok "DaisySP at $(git -C "$HERE/DaisySP" rev-parse --short HEAD)"
else
  fail "DaisySP not found (git submodule update --init DaisySP)"
fi

echo "Host tools"
for t in python3 gcc g++ docker; do
  command -v "$t" >/dev/null && ok "$t" || fail "$t not found${t/docker/ (install Docker Desktop)}"
done

echo "Docker"
if docker info >/dev/null 2>&1; then
  ok "daemon running"
  if out="$(docker run --rm --platform linux/arm/v7 arm32v7/gcc:11-bullseye sh -c 'uname -m; ldd --version | head -1' 2>&1)"; then
    ok "armhf build image runs: $(echo "$out" | tr '\n' ' ')"
  else
    fail "cannot run linux/arm/v7 containers (Docker Desktop emulates them; on Linux run:
         docker run --privileged --rm tonistiigi/binfmt --install arm,arm64)"
  fi
  for img in gcc:12 python:3.11-slim; do
    docker image inspect "$img" >/dev/null 2>&1 || docker pull -q "$img" >/dev/null
    ok "$img"
  done
else
  fail "Docker daemon not running (start Docker Desktop)"
fi

python3 -c 'import PIL' 2>/dev/null && ok "Pillow (for skin previews)" ||
  echo "  note Pillow missing: 'pip3 install pillow' for mpc-vst-plugins/tools/studio.py preview"

[ "$FAILED" = 0 ] && echo "Ready: ./build.sh all Roads" || { echo "Fix the FAIL lines above and re-run."; exit 1; }
