#!/usr/bin/env python3
"""Check a held speed pad's hits:  check_speed.py trace.txt up|down base start end ramp_semis_per_beat [bpm]

trace.txt: render_dbg's stderr ("speed hit <ppq> next <ppq> slice <n>" per hit). The pad was held from song
position `start` to `end` (beats); hits start at `base` per beat and the rate doubles (up) or halves (down) every
12 / ramp beats, held between 1 and 32 per beat. Each gap must be what the rate at its hit says (within a frame),
the gaps must shrink (up) or grow (down) until the limit, every hit repeats one slice, and the hits must cover the
hold. Exit status 1 on a miss."""
import sys


def main(path, way, base, start, end, ramp, bpm=120.0):
    hits = []
    for line in open(path):
        f = line.split()
        if len(f) >= 6 and f[0] == "speed" and f[1] == "hit":
            hits.append((float(f[2]), float(f[4]), int(f[6])))
    frame = bpm / 60.0 / 44100
    bad = []
    if len(hits) < 3:
        bad.append("only %d hits" % len(hits))
    else:
        if abs(hits[0][0] - start) > 2 * frame:
            bad.append("first hit at %.4f, pad at %.4f" % (hits[0][0], start))
        if hits[-1][1] < end - 0.5:
            bad.append("hits stop at %.4f, held to %.4f" % (hits[-1][1], end))
        if len({h[2] for h in hits}) != 1:
            bad.append("slices %s" % sorted({h[2] for h in hits}))
        prev = None
        for p, nxt, _ in hits:
            d = (p - start) * ramp / 12.0
            rate = base * 2 ** (d if way == "up" else -d)
            rate = min(32.0, max(1.0, rate))
            gap = nxt - p
            if abs(gap - 1.0 / rate) > frame:
                bad.append("hit at %.4f: gap %.5f, want %.5f" % (p, gap, 1.0 / rate))
            if prev is not None and (gap > prev + 1e-9 if way == "up" else gap < prev - 1e-9):
                bad.append("hit at %.4f: gap %.5f after %.5f" % (p, gap, prev))
            prev = gap
    first, last = (hits[0][1] - hits[0][0], hits[-1][1] - hits[-1][0]) if hits else (0, 0)
    print("%s %d hits, gaps %.4f -> %.4f beat%s" % ("ok  " if not bad else "FAIL", len(hits), first, last,
                                                    "" if not bad else ": " + "; ".join(bad[:4])))
    return 1 if bad else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    sys.exit(main(a[0], a[1], *(float(x) for x in a[2:])))
