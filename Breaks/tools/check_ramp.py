#!/usr/bin/env python3
"""Check a ramp pad held over a window:  check_ramp.py render.wav <up|down> <t0> <t1> <st_per_beat> [bpm] [oct]

render.wav: the Grid break (make_test_breaks.py: each 16th a 1 kHz burst) played from ppq 0, Grain 1/16, Vary 0,
LENGTH Free, a ramp pad held from t0 (on a 16th) to t1. Each 16th restarts the slice, so each 16th inside the hold
plays at the ramp's pitch at that 16th: 1 kHz * 2^(oct + semis / 12), semis = +-st_per_beat * beats since t0,
held at +-24. Outside the hold: 1 kHz. The pitch is read 3-15 ms after each 16th, crossing to crossing.
Exit status 1 on any miss (over 1.5 %).
"""
import struct
import sys
import wave

SR = 44100


def main(path, way, t0, t1, rate, bpm=120.0, octave=0.0):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    left = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]

    def pitch(a, b):
        seg = left[int(a * SR):int(b * SR)]
        ups = [i + x / float(x - y) for i, (x, y) in enumerate(zip(seg, seg[1:])) if x < 0 <= y]
        return (len(ups) - 1) * SR / (ups[-1] - ups[0]) if len(ups) > 2 else 0.0

    six = 60.0 / bpm / 4
    sign = 1 if way == "up" else -1
    checked = misses = 0
    k = 4
    while (k + 1) * six < len(left) / SR:
        t = k * six
        k += 1
        if t < t1 <= t + 0.015 + 0.002:
            continue   # straddles the release
        inside = t0 - 0.001 <= t < t1
        semis = 0.0
        if inside:
            semis = max(-24.0, min(24.0, sign * rate * (t - t0) * bpm / 60.0))
        want = 1000.0 * 2 ** (octave * inside + semis / 12.0)
        got = pitch(t + 0.003, t + 0.015)
        checked += 1
        if abs(got / want - 1) > 0.015:
            misses += 1
            print("  16th at %.3f s: %.0f Hz, want %.0f (%+.2f st)" % (t, got, want, semis))
    print("%s %-4s %s-%s %s st/beat: %s (%d 16ths)" % ("ok  " if not misses else "FAIL", way, t0, t1, rate,
                                                       "pitch follows the ramp" if not misses else "%d off" % misses,
                                                       checked))
    return 1 if misses else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    sys.exit(main(a[0], a[1], float(a[2]), float(a[3]), float(a[4]), *(float(x) for x in a[5:])))
