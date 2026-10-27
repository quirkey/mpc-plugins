#!/usr/bin/env python3
"""Check ATTACK / GATE / RELEASE:  check_shape.py plain.wav shaped.wav <step> <x:level,...> [bpm] [t0 t1]

Both renders of the Grid break (make_test_breaks.py: each 16th a 1 kHz burst over its first 60%) from ppq 0, the
second with an envelope. Every segment `step` beats long (0.25 = a 16th, 0.125 = a roll's 32nds), at each share x
of it, the shaped peak over 1 ms around x divided by the plain one should be `level` (within 0.06): what the envelope
is there. Segments 4 onwards (past the start-up) up to the end of the render are checked, or only those from t0
to t1 seconds.
Exit status 1 on any miss.
"""
import struct
import sys
import wave

SR = 44100


def load(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    return struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]


def main(plain_path, shaped_path, step, spec, bpm=120.0, t0=0.0, t1=1e9):
    plain, shaped = load(plain_path), load(shaped_path)
    want = [tuple(float(v) for v in item.split(":")) for item in spec.split(",")]
    seg = step * 60.0 / bpm
    checked = misses = 0
    k = 4
    while (k + 1) * seg * SR < min(len(plain), len(shaped)):
        if not t0 - 1e-6 <= k * seg < t1 - seg + 1e-6:
            k += 1
            continue
        for x, level in want:
            c = int((k + x) * seg * SR)
            a, b = c - SR // 2000, c + SR // 2000
            ref = max(abs(v) for v in plain[a:b])
            if ref < 1000:
                continue   # silent there in the plain render (the burst's off part)
            got = max(abs(v) for v in shaped[a:b]) / float(ref)
            checked += 1
            if abs(got - level) > 0.06:
                misses += 1
                if misses <= 6:
                    print("  segment %d at %.2f: level %.2f, want %.2f" % (k, x, got, level))
        k += 1
    ok = misses == 0 and checked > 0
    print("%s %-40s %d points%s" % ("ok  " if ok else "FAIL", spec, checked, "" if ok else ", %d off" % misses))
    return 0 if ok else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    sys.exit(main(a[0], a[1], float(a[2]), a[3], *(float(x) for x in a[4:])))
