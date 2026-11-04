#!/usr/bin/env python3
"""Generate MPC keygroup programs from Ableton Core Library multisamples, using Ableton's own presets for the map.

    make_keygroups_from_ableton.py <core_library> <device_samples_dir> <out_dir> [--only NAME ...] [--list]

<core_library> is Live's "Core Library" folder (inside the app bundle). <device_samples_dir> is where its Samples/
folder was copied on the MPC (e.g. "/media/MPCDATA/Ableton/Live 11 Core Library"): programs reference the samples
there in place. Writes <out_dir>/<Folder>.xpm plus an empty <Folder>_[ProgramData]/ per Samples/Multisamples folder.

For each folder, the preset (.adv / .adg) whose samplers map the most of its files gives each sample's key range,
velocity range, root key, fine tune and loop. Racks spread layers over chains (the Grand Piano's four velocity
layers), so every sampler in that preset that maps the folder is merged, largest first, skipping any that overlaps
what is already taken (the Upright's key-clack and pedal-noise samplers). Keys are then cut into segments at every
range edge, one keygroup per segment and one layer per velocity range; where Ableton crossfades two overlapping
ranges, each note goes to the part with the nearer root so a note never plays twice on the MPC.
"""
import argparse, collections, copy, gzip, json, os, re, sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_keygroup as mk

SKIP = ('Drum Machines/',)  # kit pieces with tune variants, not pitched instruments
NOTES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def val(e, path):
    return e.find(path).get('Value')


def parts_of(sampler):
    out = []
    for p in sampler.iter('MultiSamplePart'):
        loop = None
        if val(p, 'SustainLoop/Mode') == '1':  # forward; 2 is back-and-forth, which the MPC can't do
            ls, le = int(val(p, 'SustainLoop/Start')), int(val(p, 'SustainLoop/End'))
            if 0 < ls < le:
                loop = (ls, le)
        out.append(dict(path=val(p, 'SampleRef/FileRef/RelativePath'),
                        key=(int(val(p, 'KeyRange/Min')), int(val(p, 'KeyRange/Max'))),
                        vel=(max(1, int(val(p, 'VelocityRange/Min'))), int(val(p, 'VelocityRange/Max'))),
                        root=int(val(p, 'RootKey')), detune=int(round(float(val(p, 'Detune')))),
                        frames=int(val(p, 'SampleRef/DefaultDuration')), loop=loop))
    return out


def scan_presets(lib, folders):
    """-> {folder: (preset path, [samplers' part lists])} for the preset mapping the most of each folder's files."""
    best = {}
    for dp, _, fs in os.walk(lib):
        for f in fs:
            if not f.endswith(('.adv', '.adg')):
                continue
            p = os.path.join(dp, f)
            try:
                root = ET.fromstring(gzip.open(p).read())
            except Exception:
                continue
            samplers = collections.defaultdict(list)
            for s in list(root.iter('MultiSampler')) + list(root.iter('OriginalSimpler')):
                ps = parts_of(s)
                for d in {os.path.dirname(x['path']) for x in ps} & folders:
                    samplers[d].append([x for x in ps if os.path.dirname(x['path']) == d])
            for d, lists in samplers.items():
                n = len({x['path'] for ps in lists for x in ps})
                if d not in best or n > best[d][0]:
                    best[d] = (n, p, lists)
    return {d: (p, lists) for d, (n, p, lists) in best.items()}


def merge(lists):
    """Largest sampler first; skip a sampler whose key x velocity rectangles overlap what is already taken."""
    taken = []
    def hit(a, b):
        return a['key'][0] <= b['key'][1] and b['key'][0] <= a['key'][1] and \
            a['vel'][0] <= b['vel'][1] and b['vel'][0] <= a['vel'][1]
    for ps in sorted(lists, key=len, reverse=True):
        if len({x['path'] for x in ps}) < len(ps) / 2:
            continue  # one file sliced over many keys (noises), not a multisample
        if any(hit(a, b) for a in ps for b in taken):
            continue
        taken += ps
    return taken


def zones(parts):
    """-> [(lo, hi, [part per velocity layer])]: keys cut at every range edge, nearest root wins an overlap."""
    owner = {}
    for note in range(128):
        for vel in {p['vel'] for p in parts}:
            cands = [p for p in parts if p['key'][0] <= note <= p['key'][1] and p['vel'] == vel]
            if cands:
                w = min(cands, key=lambda p: (abs(p['root'] - note), p['root'], p['path']))
                # parts stacked on the same range and root all sound (Smooth Synth's "- 1" and "- 2")
                owner[(note, vel)] = sorted((p for p in cands if p['key'] == w['key'] and p['root'] == w['root']),
                                            key=lambda p: p['path'])
    out = []
    for note in range(128):
        layer = tuple(sorted((v, id(p)) for (n, v), ps in owner.items() if n == note for p in ps))
        ps = [p for (n, v), group in sorted(owner.items(), key=lambda kv: kv[0]) if n == note for p in group]
        if out and out[-1][3] == layer:
            out[-1][1] = note
        elif ps:
            out.append([note, note, ps, layer])
    # stretch the outer keygroups to the keyboard's ends, as the MPC's own auto-map does
    if out:
        out[0][0], out[-1][1] = 0, 127
    return [(lo, hi, ps) for lo, hi, ps, _ in out]


def name_note(path):
    """The note in an Ableton sample name (C3 = 60): "Saw-C0", "Mallet Guitar Bb0", "GrandPiano A# 1" (= A#-1)."""
    stem = os.path.splitext(os.path.basename(path))[0]
    m = re.search(r'(?<![A-Za-z])([A-G])(?:([#b])\s?|)(-?\d)(?!\d)', stem)
    if not m:
        return None
    octave = int(m.group(3))
    if m.group(2) and ' ' in m.group(0):
        octave = -octave
    return (octave + 2) * 12 + NOTES[m.group(1)] + {'#': 1, 'b': -1, None: 0}[m.group(2)]


def by_name(parts):
    """The preset re-pitches the samples (a sound-design patch): map each file at the note in its name instead, all
    files sharing a note layered by the preset's velocity ranges (Ground Plate's takes), zones split halfway between
    notes."""
    files = {}
    for p in parts:
        files.setdefault(p['path'], dict(p, root=name_note(p['path']), detune=0))
    roots = sorted({p['root'] for p in files.values()})
    out = []
    for n, r in enumerate(roots):
        lo = 0 if n == 0 else (roots[n - 1] + r) // 2 + 1
        hi = 127 if n == len(roots) - 1 else (r + roots[n + 1]) // 2
        out.append((lo, hi, sorted((p for p in files.values() if p['root'] == r), key=lambda p: p['path'])))
    return out


def repitched(parts):
    """True when every sample is named by note and the preset plays some of them more than a semitone away from it."""
    notes = [(name_note(p['path']), p['root']) for p in parts]
    return all(n is not None for n, _ in notes) and any(abs(n - r) > 1 for n, r in notes)


def write(zs, out_dir, name, template, ref):
    raw = gzip.decompress(open(template, 'rb').read()).decode('utf-8')
    head, body = raw[:raw.index('{')], json.loads(raw[raw.index('{'):])
    d = body['data']
    proto, tail = d['drum']['instruments'][0], d['drum']['instruments'][-1]
    blank = copy.deepcopy(proto['layersv'][1])
    os.makedirs(os.path.join(out_dir, name + '_[ProgramData]'), exist_ok=True)
    instruments, samples = [], {}
    for lo, hi, ps in zs:
        if len(ps) > len(proto['layersv']):
            sys.exit('%s: %d layers at %d..%d, the MPC has %d' % (name, len(ps), lo, hi, len(proto['layersv'])))
        ins = copy.deepcopy(proto)
        ins['lowNote'], ins['highNote'], ins['polyphony'] = lo, hi, 16
        ins['layersv'] = [copy.deepcopy(blank) for _ in proto['layersv']]
        for L, p in zip(ins['layersv'], ps):
            L.update(copy.deepcopy(proto['layersv'][0]))
            stem = os.path.splitext(os.path.basename(p['path']))[0]
            end = p['loop'][1] if p['loop'] and p['loop'][1] < p['frames'] else p['frames']
            L.update(active=True, rootNote=p['root'], keyTrackEnable=True, fineTune=p['detune'], sampleName=stem,
                     sampleFile=ref(p['path']), velocityStart=p['vel'][0], velocityEnd=p['vel'][1])
            L['sliceInfo'].update(Start=0, End=end, LoopStart=p['loop'][0] if p['loop'] else 0,
                                  LoopMode=1 if p['loop'] else 0)
            samples.setdefault(ref(p['path']), {'version': 1, 'name': stem, 'path': ref(p['path']), 'loadImpl': 0,
                                                'metadata': {'tempo': 120.0, 'rootNote': p['root'], 'tune': 0.0,
                                                             'key': ''}})
        instruments.append(ins)
    if len(instruments) > 128:
        sys.exit('%s: %d keygroups' % (name, len(instruments)))
    d['drum']['instruments'] = instruments + [tail]
    d['keygroup']['numKeygroups'] = len(instruments)
    d['samples'] = list(samples.values())
    d['name'] = name
    with open(os.path.join(out_dir, name + '.xpm'), 'wb') as fh:
        fh.write(gzip.compress((head + json.dumps(body, indent=4)).encode('utf-8')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('core_library')
    ap.add_argument('device_samples_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--only', nargs='*', help='folder names to build (default: all)')
    ap.add_argument('--list', action='store_true', help='only print each program and any root / name mismatch')
    ap.add_argument('--refs', help='write every referenced device path to this file')
    ap.add_argument('--template', default=mk.DEFAULT_TEMPLATE)
    a = ap.parse_args()
    lib = a.core_library
    folders = set()
    for dp, _, fs in os.walk(os.path.join(lib, 'Samples', 'Multisamples')):
        rel = os.path.relpath(dp, lib)
        if any(f.lower().endswith(('.wav', '.aif', '.aiff')) for f in fs) and not any(s in rel for s in SKIP):
            folders.add(rel)
    found = scan_presets(lib, folders)
    refs = set()
    ref = lambda p: a.device_samples_dir.rstrip('/') + '/' + p[len('Samples/'):]
    for d in sorted(folders):
        name = os.path.basename(d)
        if a.only and name not in a.only:
            continue
        if d not in found:
            print('%-28s no preset: skipped' % name)
            continue
        preset, lists = found[d]
        parts = merge(lists)
        named = repitched(parts)
        zs = by_name(parts) if named else zones(parts)
        used = {p['path'] for _, _, ps in zs for p in ps}
        print('%-28s %3d keygroups  %d layers  %2d/%-2d files  loops %-3s  %s %s' % (
            name, len(zs), max(len(ps) for _, _, ps in zs), len(used),
            len([f for f in os.listdir(os.path.join(lib, d)) if not f.endswith('.asd')]),
            'yes' if any(p['loop'] for p in parts) else 'no', 'BY NAME, not' if named else 'from',
            os.path.basename(preset)))
        for _, _, ps in zs:
            for p in ps:
                n = name_note(p['path'])
                if n is not None and n != p['root']:
                    print('    root %d, name %d: %s' % (p['root'], n, os.path.basename(p['path'])))
        refs |= {ref(p) for p in used}
        if not a.list:
            write(zs, a.out_dir, name, a.template, ref)
    if a.refs:
        open(a.refs, 'w').write('\n'.join(sorted(refs)) + '\n')


if __name__ == '__main__':
    main()
