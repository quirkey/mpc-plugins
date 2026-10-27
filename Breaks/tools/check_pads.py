#!/usr/bin/env python3
"""Check a pad effect held over a window:  check_pads.py render.wav <fx> <t0> <t1> [bpm] [hz]

render.wav: "Grid 120bpm.wav" (make_test_breaks.py: 32 slices, slice s a 1 kHz burst over the first 60% of its 16th
at level 0.1 + 0.7 * s / 31) played from ppq 0 with no transport events, Grain 1/16, Half off, Vary 0, and the
pad held from t0 to t1 seconds. k0 is the 16th the pad went down in. On every 16th k the level 4-40 ms after it
says what plays (16ths whose 4-40 ms straddle the release are skipped):
  outside the hold     slice k mod 32 (the break kept its place underneath: "slip")
  reverse              silent at first (backwards, the burst comes last)
  gap                  silent
  stutter              slice k0 (the one playing when the pad went down), every 16th
  roll / triplet       that slice too (retriggered faster than the 16ths)
  half                 slice (k0 + (k - k0) / 2) mod 32: half speed from the 16th the pad went down in
  flam                 slice k, plus (once the pad was down 25 ms before it) its grace note: that slice at half
                       level 2-20 ms before the 16th
  none                 slice k (an octave pad on its own)
hz: the burst's pitch inside the hold (1000 normally, 2000 an octave up, 500 down), from zero crossings 4-34 ms
after each held 16th; checked for every effect but gap and reverse.
Exit status 1 on any miss.
"""
import struct
import sys
import wave

SR, SLICES = 44100, 32


def main(path, fx, t0, t1, bpm=120.0, hz=None):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    left = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]

    def level(a, b):
        seg = left[max(0, int(a * SR)):int(b * SR)]
        return max((abs(v) for v in seg), default=0) / 32768.0

    def pitch(a, b):
        seg = left[int(a * SR):int(b * SR)]
        flips = sum(1 for x, y in zip(seg, seg[1:]) if (x < 0) != (y < 0))
        return flips / 2.0 / (b - a)

    def slice_of(lv, gain=1.0):
        return round((lv / gain - 0.1) / 0.7 * (SLICES - 1))

    six = 60.0 / bpm / 4
    k0 = int(t0 / six + 1e-9)
    checked = misses = 0
    k = 1
    while (k + 1) * six < len(left) / SR:
        t = k * six
        inside = t >= t0 - 0.001 and t + 0.040 <= t1
        if not inside and t0 - 0.001 <= t < t1 - 0.001:
            k += 1   # straddles the release
            continue
        lv = level(t + 0.004, t + 0.040)
        if not inside or fx in ("flam", "none"):
            want, ok = k % SLICES, slice_of(lv) == k % SLICES
            what = "slice %d, heard %d" % (want, slice_of(lv))
            if ok and inside and fx == "flam" and t - 0.025 >= t0:
                g = level(t - 0.020, t - 0.002)
                ok = slice_of(g, 0.5) == k % SLICES
                what = "grace note of slice %d at half level, heard %d (%.3f)" % (want, slice_of(g, 0.5), g)
        elif fx in ("reverse", "gap"):
            ok, what = lv < 0.03, "silent at first (%.3f)" % lv
        elif fx in ("stutter", "roll", "triplet"):
            want = k0 % SLICES
            ok, what = slice_of(lv) == want, "held slice %d, heard %d" % (want, slice_of(lv))
        elif fx == "half":
            want = (k0 + (k - k0) // 2) % SLICES
            ok, what = slice_of(lv) == want, "slice %d at half speed, heard %d" % (want, slice_of(lv))
        else:
            sys.exit("unknown effect " + fx)
        if ok and inside and hz and fx not in ("gap", "reverse"):
            f = pitch(t + 0.004, t + 0.034)
            ok = abs(f / hz - 1) < 0.1
            what = "a %g Hz burst, heard %.0f Hz" % (hz, f)
        checked += 1
        if not ok:
            misses += 1
            if misses <= 8:
                print("miss: 16th %d (%.3f s, %s): want %s" % (k, t, "held" if inside else "free", what))
        k += 1
    print("%-8s %d sixteenths checked, %d wrong" % (fx, checked, misses))
    return 1 if misses or not checked else 0


if __name__ == "__main__":
    a = sys.argv
    sys.exit(main(a[1], a[2], float(a[3]), float(a[4]), float(a[5]) if len(a) > 5 else 120.0,
                  float(a[6]) if len(a) > 6 else None))
