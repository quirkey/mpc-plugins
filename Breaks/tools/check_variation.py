#!/usr/bin/env python3
"""Check that what Breaks played on each 16th matches what it decided:  check_variation.py render.wav trace.txt [bpm]

render.wav: "Grid 120bpm.wav" (make_test_breaks.py: slice s is a 1 kHz burst over the first 60% of the 16th,
at level 0.1 + 0.7 * s / 31) played from ppq 0 with no transport events. trace.txt: the stderr of a
-DBREAKS_DEBUG build ("step N kind K slice S rev R at <ppq> into <beats>"). For every 16th started on its
boundary:
  Gap                     silent after the boundary
  reversed (Reverse, or a Repeat of one)   silent at first: backwards, the burst comes last
  Roll                    the slice's burst again from the 32nd
  anything else           the decided slice's burst
Exit status 1 on any miss.
"""
import re
import struct
import sys
import wave

SR, SLICES = 44100, 32
NAMES = ["normal", "swap", "repeat", "reverse", "gap", "roll"]


def main(wav_path, trace_path, bpm=120.0):
    w = wave.open(wav_path)
    raw = w.readframes(w.getnframes())
    left = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]

    def level(f0, f1):
        seg = left[max(0, int(f0)):int(f1)]
        return max((abs(v) for v in seg), default=0) / 32768.0

    def slice_of(lv):
        return round((lv - 0.1) / 0.7 * (SLICES - 1))

    pat = re.compile(r"step (-?\d+) kind (\d) slice (-?\d+) rev (\d) at ([-\d.]+) into ([-\d.]+)")
    counts = [0] * 6
    misses = 0
    ms = SR / 1000.0
    sixteenth = 60.0 / bpm / 4 * SR
    for line in open(trace_path):
        m = pat.search(line)
        if not m:
            continue
        step, kind, s, rev, at, into = int(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5]), float(m[6])
        if into > 0.0001:
            continue
        f = at * 60.0 / bpm * SR
        if f + sixteenth >= len(left):
            break
        counts[kind] += 1
        lv = level(f + 4 * ms, f + 40 * ms)
        if kind == 4:
            ok, what = lv < 0.01, "silent (%.3f)" % lv
        elif rev:
            ok, what = lv < 0.03, "silent at first (%.3f)" % lv
        else:
            ok, what = slice_of(lv) == s, "slice %d (heard %d, %.3f)" % (s, slice_of(lv), lv)
            if ok and kind == 5:
                half = sixteenth / 2
                lv2 = level(f + half + 4 * ms, f + half + 40 * ms)
                ok, what = slice_of(lv2) == s, "rolled slice %d on the 32nd (heard %d)" % (s, slice_of(lv2))
        if not ok:
            misses += 1
            if misses <= 10:
                print("miss: step %d %s: want %s" % (step, NAMES[kind], what))
    print("checked %s; %d wrong" % (", ".join("%d %s" % (c, n) for c, n in zip(counts, NAMES)), misses))
    return 1 if misses or sum(counts) == 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 120.0))
