#!/usr/bin/env python3
"""Check a held velocity pad:  check_vel.py plain.wav vel.wav up|down start end sweep_beats step [bpm]

Both renders hold the same repeating effect (a roll: a hit every `step` beats) on the Grid break; `vel` also holds a
velocity pad from `start` to `end` (beats). Just after each hit the level, relative to the plain render, must be
the level the pad has reached at that hit, squared: up from 0 (or down from 1) by one per `sweep_beats`, held
within 0..1. Within 0.06. Exit status 1 on a miss."""
import struct
import sys
import wave

SR = 44100


def load(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    return struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]


def main(plain_path, vel_path, way, start, end, sweep, step, bpm=120.0):
    plain, vel = load(plain_path), load(vel_path)
    spb = 60.0 / bpm
    checked, bad = 0, []
    k = 0
    while start + k * step < end - step:
        hit = start + k * step
        a = int((hit * spb + 0.004) * SR)
        b = int((hit * spb + 0.020) * SR)
        ref = max(abs(v) for v in plain[a:b])
        if ref > 1000:
            level = (hit - start) / sweep
            level = level if way == "up" else 1 - level
            level = min(1.0, max(0.0, level))
            got = max(abs(v) for v in vel[a:b]) / float(ref)
            checked += 1
            if abs(got - level * level) > 0.06:
                bad.append("hit at %.3f: %.2f, want %.2f" % (hit, got, level * level))
        k += 1
    ok = not bad and checked > 4
    print("%s %d hits checked%s" % ("ok  " if ok else "FAIL", checked, "" if ok else ": " + "; ".join(bad[:4])))
    return 0 if ok else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    sys.exit(main(a[0], a[1], a[2], *(float(x) for x in a[3:])))
