#!/usr/bin/env python3
"""Build an MPC keygroup program (.xpm) from a folder of pitched WAVs.

    make_keygroup.py <wav_dir> <out_dir> [--name NAME] [--template FILE.xpm] [--link]

Root note comes from the WAV's smpl chunk, else a trailing note name in the file name ("... G#3"),
else a leading MIDI number ("63 Foo.wav"). Each sample's zone runs to the midpoint between it and
its neighbours. Writes <out_dir>/<NAME>.xpm and <out_dir>/<NAME>_[ProgramData]/ (WAV copies).

The .xpm is "ACVS\\n<ver>\\nSerialisableProgramData\\njson\\nLinux\\n" + JSON, all gzipped. The
template is a keygroup program saved by the MPC itself; its first instrument is cloned per sample
and its last instrument (an unused default MPC always appends) is kept at the end.
"""
import argparse, copy, glob, gzip, json, os, re, shutil, struct, sys

NOTES = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6, 'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}
# Note names in the From Mars file names are C3 = MIDI 60 (their smpl chunks agree), not C4.
OCT = 2
DEFAULT_TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'keygroup_template.xpm')


def read_wav(path):
    """-> dict(frames, rate, root (or None), loop (start, end) or None)"""
    d = open(path, 'rb').read()
    info = dict(frames=0, rate=44100, root=None, loop=None)
    i = 12
    block = 2
    while i < len(d) - 8:
        cid, sz = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
        body = d[i + 8:i + 8 + sz]
        if cid == b'fmt ':
            _, ch, info['rate'], _, _, bits = struct.unpack('<HHIIHH', body[:16])
            block = ch * bits // 8
        elif cid == b'data':
            info['frames'] = sz // block
        elif cid == b'smpl' and sz >= 36:
            unity, nloops = struct.unpack('<I', body[12:16])[0], struct.unpack('<I', body[28:32])[0]
            info['root'] = unity
            if nloops and sz >= 60:
                info['loop'] = struct.unpack('<II', body[44:52])
        i += 8 + sz + (sz & 1)
    return info


def name_root(fn):
    stem = os.path.splitext(os.path.basename(fn))[0]
    m = re.search(r'([A-G]#?)(-?\d)$', stem.strip())
    if m:
        return (int(m.group(2)) + OCT) * 12 + NOTES[m.group(1)]
    m = re.match(r'(\d{1,3})\b', stem)
    return int(m.group(1)) if m else None


def name_root_note_only(fn):
    """Like name_root but only a trailing note name; a leading number is too often just an index."""
    stem = os.path.splitext(os.path.basename(fn))[0]
    m = re.search(r'(?<![A-Za-z])([A-G]#?)(-?\d)$', stem.strip())
    return (int(m.group(2)) + OCT) * 12 + NOTES[m.group(1)] if m else None


def write_program(zones, out_dir, name, template, ref_fn, copy_fn=None):
    """zones: [(root, base_file_name, frames, loop_start or None)] sorted by root, one per root.
    ref_fn(base) -> the path stored in the program; copy_fn(base, dst) copies a WAV into ProgramData."""
    raw = gzip.decompress(open(template, 'rb').read()).decode('utf-8')
    head, body = raw[:raw.index('{')], json.loads(raw[raw.index('{'):])
    d = body['data']
    ins_t = d['drum']['instruments']
    proto, tail = ins_t[0], ins_t[-1]
    pdir = os.path.join(out_dir, name + '_[ProgramData]')
    os.makedirs(pdir, exist_ok=True)
    instruments, samples = [], []
    for n, (root, base, frames, loop) in enumerate(zones):
        lo = 0 if n == 0 else (zones[n - 1][0] + root) // 2 + 1
        hi = 127 if n == len(zones) - 1 else (root + zones[n + 1][0]) // 2
        if copy_fn:
            copy_fn(base, os.path.join(pdir, base))
        ref = ref_fn(base)
        stem = os.path.splitext(base)[0]
        ins = copy.deepcopy(proto)
        ins['lowNote'], ins['highNote'] = lo, hi
        ins['polyphony'] = 16
        L = ins['layersv'][0]
        L.update(active=True, rootNote=root, keyTrackEnable=True, sampleName=stem, sampleFile=ref,
                 velocityStart=0, velocityEnd=127)
        L['sliceInfo'].update(Start=0, End=frames)
        L['sliceInfo'].update(LoopStart=loop or 0, LoopMode=1 if loop else 0)
        instruments.append(ins)
        samples.append({'version': 1, 'name': stem, 'path': ref, 'loadImpl': 0,
                        'metadata': {'tempo': 120.0, 'rootNote': root, 'tune': 0.0, 'key': ''}})
    d['drum']['instruments'] = instruments + [tail]
    d['keygroup']['numKeygroups'] = len(instruments)
    d['samples'] = samples
    d['name'] = name
    out = os.path.join(out_dir, name + '.xpm')
    with open(out, 'wb') as fh:
        fh.write(gzip.compress((head + json.dumps(body, indent=4)).encode('utf-8')))
    return out


def one_per_root(zones):
    uniq = {}
    for z in zones:
        uniq.setdefault(z[0], z)
    return [uniq[k] for k in sorted(uniq)]


def build(wav_dir, out_dir, name, template, link, ref_dir=None):
    zones = []
    for f in sorted(glob.glob(os.path.join(wav_dir, '*.wav'))):
        w = read_wav(f)
        root = w['root'] if w['root'] else name_root(f)
        if root is None:
            print('skip (no root note): ' + f, file=sys.stderr)
            continue
        zones.append((root, os.path.basename(f), w['frames'], w['loop'][0] if w['loop'] else None))
    zones = one_per_root(zones)
    if not zones:
        sys.exit('no pitched wavs found')
    if len(zones) > 128:
        sys.exit('too many zones')

    def copy_fn(base, dst):
        if not os.path.exists(dst):
            src = os.path.join(wav_dir, base)
            (os.symlink(os.path.abspath(src), dst) if link else shutil.copyfile(src, dst))

    out = write_program(zones, out_dir, name, template,
                        (lambda b: ref_dir.rstrip('/') + '/' + b) if ref_dir else (lambda b: b),
                        None if ref_dir else copy_fn)
    print('%s: %d keygroups, notes %d..%d' % (out, len(zones), zones[0][0], zones[-1][0]))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('wav_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--name')
    ap.add_argument('--template', default=DEFAULT_TEMPLATE)
    ap.add_argument('--ref-dir', help='reference WAVs in place: folder as the MPC sees it (abs or relative to ProgramData)')
    ap.add_argument('--link', action='store_true', help='symlink WAVs instead of copying')
    a = ap.parse_args()
    build(a.wav_dir, a.out_dir, a.name or os.path.basename(os.path.normpath(a.wav_dir)), a.template, a.link, a.ref_dir)
