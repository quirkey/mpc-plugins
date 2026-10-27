#!/usr/bin/env python3
"""Generate keygroup programs for every pitched multi-sample folder found by scan_mpc_wavs.py.

    make_keygroups_from_scan.py <cache.json> <device_root> <out_dir> [--min-roots 4] [--list]

<device_root> is the scanned folder as the MPC sees it (e.g. "/media/MPCDATA/From Mars For MPC").
Programs reference the WAVs in place (absolute paths), so <out_dir> only holds .xpm files and
empty _[ProgramData] folders, laid out as <out_dir>/<Library>/<Instrument>.xpm.
"""
import argparse, collections, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_keygroup as mk


def program_name(rel_dir):
    parts = [p for p in rel_dir.split('/') if p and p.upper() != 'WAV']
    lib = re.sub(r'\s+From Mars.*$', '', parts[0]).strip() or parts[0]
    rest = [re.sub(r'^\d+\.\s*', '', p) for p in parts[1:]]
    return lib, ' - '.join([lib] + rest)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache')
    ap.add_argument('device_root')
    ap.add_argument('out_dir')
    ap.add_argument('--min-roots', type=int, default=4)
    ap.add_argument('--list', action='store_true', help='only list the folders that qualify')
    ap.add_argument('--template', default=mk.DEFAULT_TEMPLATE)
    a = ap.parse_args()
    cache = json.load(open(a.cache))
    groups = collections.defaultdict(list)
    for rel, info in cache.items():
        groups[os.path.dirname(rel)].append((rel, info))
    made = 0
    for rel_dir in sorted(groups):
        zones = []
        for rel, info in groups[rel_dir]:
            base = os.path.basename(rel)
            root = info['root'] if info['root'] is not None else mk.name_root_note_only(base)
            if root is None or root > 127 or not info['frames']:
                continue
            zones.append((root, base, info['frames'], info['loop']))
        zones = mk.one_per_root(zones)
        if len(zones) < a.min_roots or len(zones) > 128:
            continue
        lib, name = program_name(rel_dir)
        made += 1
        if a.list:
            print('%-60s %3d zones  notes %d..%d' % (name, len(zones), zones[0][0], zones[-1][0]))
            continue
        out = os.path.join(a.out_dir, lib)
        os.makedirs(out, exist_ok=True)
        ref_dir = a.device_root.rstrip('/') + '/' + rel_dir
        mk.write_program(zones, out, name, a.template, lambda b, r=ref_dir: r + '/' + b)
    print('%d programs' % made, file=sys.stderr)


if __name__ == '__main__':
    main()
