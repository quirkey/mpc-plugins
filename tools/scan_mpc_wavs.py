#!/usr/bin/env python3
"""Scan a WAV library on the MPC (over ssh) and cache each file's header facts as JSON.

    scan_mpc_wavs.py <device_dir> <cache.json> [--host mpc]

Reads only the first 4 KB of every file (BusyBox dd/hexdump), so a multi-GB library scans in minutes.
Per file: size, frames, rate, root note and first loop start (from the smpl chunk) when they sit
before the data chunk, which they do in the From Mars libraries.
"""
import argparse, json, struct, subprocess, sys

REMOTE = r'''cd "$1" && find . -type f -iname '*.wav' | while IFS= read -r f; do
  printf 'F\t%s\t%s\n' "$f" "$(wc -c < "$f")"; dd bs=4096 count=1 if="$f" 2>/dev/null | hexdump -v -e '1/1 "%02x"'; printf '\n'; done'''


def parse_header(b):
    info = dict(frames=0, rate=44100, root=None, loop=None, block=2)
    i, block = 12, 2
    while i <= len(b) - 8:
        cid, sz = b[i:i + 4], struct.unpack('<I', b[i + 4:i + 8])[0]
        body = b[i + 8:i + 8 + sz]
        if cid == b'fmt ' and len(body) >= 16:
            _, ch, info['rate'], _, _, bits = struct.unpack('<HHIIHH', body[:16])
            block = info['block'] = max(1, ch * bits // 8)
        elif cid == b'smpl' and len(body) >= 36:
            info['root'] = struct.unpack('<I', body[12:16])[0]
            if struct.unpack('<I', body[28:32])[0] and len(body) >= 60:
                info['loop'] = struct.unpack('<I', body[44:48])[0]
        elif cid == b'data':
            info['frames'] = sz // block
            break
        i += 8 + sz + (sz & 1)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('device_dir')
    ap.add_argument('cache')
    ap.add_argument('--host', default='mpc')
    ap.add_argument('--redo-empty', action='store_true', help='re-read only cached files that have frames == 0')
    a = ap.parse_args()
    script, prev = REMOTE, {}
    if a.redo_empty:
        prev = json.load(open(a.cache))
        todo = [k for k, v in prev.items() if not v['frames']]
        script = r'''cd "$1" && while IFS= read -r f; do
  printf 'F\t%s\t%s\n' "./$f" "$(wc -c < "$f")"; dd bs=4096 count=1 if="$f" 2>/dev/null | hexdump -v -e '1/1 "%02x"'; printf '\n'; done <<'LIST_END'
''' + '\n'.join(todo) + '\nLIST_END\n'
    p = subprocess.run(['ssh', a.host, "sh -s '%s'" % a.device_dir], input=script, capture_output=True, text=True)
    lines = [l for l in p.stdout.split('\n')]
    out, n = dict(prev), 0
    for k, l in enumerate(lines):
        if l.startswith('F\t'):
            _, path, size = l.split('\t')
            hexs = lines[k + 1] if k + 1 < len(lines) else ''
            info = parse_header(bytes.fromhex(hexs))
            info['size'] = int(size)
            out[path[2:]] = info
            n += 1
    json.dump(out, open(a.cache, 'w'))
    print('%d files -> %s' % (n, a.cache), file=sys.stderr)
    if p.returncode:
        print(p.stderr[-500:], file=sys.stderr)


if __name__ == '__main__':
    main()
