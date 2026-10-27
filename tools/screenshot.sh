#!/usr/bin/env bash
# Screenshot of the MPC's screen as it is right now, converted locally (Pillow) instead of in the toolkit's
# Chromium image:
#   tools/screenshot.sh out.png [--plugin] [ssh-target]     (target defaults to "mpc", see ~/.ssh/config)
# --plugin crops to the plugin area (1280x628). Uses the toolkit's drmgrab (read-only DRM scanout read), built
# for 32-bit ARM in Docker on first use; see mpc-vst-plugins/tools/screenshot.sh.
set -euo pipefail
[ $# -ge 1 ] || { sed -n 2,6p "$0"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MV="$HERE/mpc-vst-plugins"
OUT="$1"; CROP="${2:-}"; TARGET="${3:-mpc}"
[ "$CROP" = "--plugin" ] || { [ -n "$CROP" ] && TARGET="$CROP"; CROP=""; }
BIN="$MV/tools/build/drmgrab"
if [ ! -x "$BIN" ] || [ "$MV/tools/drmgrab.c" -nt "$BIN" ]; then
  mkdir -p "$MV/tools/build"
  docker run --rm --platform linux/arm/v7 -v "$MV/tools":/t -w /t arm32v7/gcc:12 bash -c \
    'apt-get -qq update >/dev/null && apt-get -qq install -y libdrm-dev >/dev/null && gcc -O2 -static -I/usr/include/libdrm -o build/drmgrab drmgrab.c'
fi
RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT
ssh "$TARGET" 'cat > /tmp/drmgrab && chmod +x /tmp/drmgrab && { /tmp/drmgrab /dev/dri/card0 || /tmp/drmgrab /dev/dri/card1; }; rc=$?; rm -f /tmp/drmgrab; exit $rc' \
  < "$BIN" > "$RAW"
python3 - "$RAW" "$OUT" "$CROP" <<'EOF'
import sys
from PIL import Image
raw, out, crop = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(raw, 'rb').read()
hdr, px = data.split(b'\n', 1)
w, h, p = map(int, hdr.split()[:3])
im = Image.frombuffer('RGBA', (w, h), px[:p * h], 'raw', 'BGRA', p, 1).convert('RGB')
if h > w:
    im = im.rotate(270, expand=True)
if crop == '--plugin':
    im = im.crop((0, 110, 1280, 738))
im.save(out, optimize=True)
print(out, im.size)
EOF
