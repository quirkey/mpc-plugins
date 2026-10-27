#!/usr/bin/env python3
"""A break picked while playing takes over where the sequence starts again:
check_switch.py switched.wav old.wav new.wav wrap_seconds

All three rendered in the same loop; `switched` changes from the old break to the new one partway through a pass.
Up to the wrap it must be the old break's render, sample for sample, and from the wrap on the new break's (which
restarts with every pass, so its own render is what the first pass after the switch should be). 10 ms either side
of the wrap is skipped (the crossfade). Exit status 1 on a mismatch.
"""
import struct
import sys
import wave

SR = 44100


def load(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    return struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]


def main(switched, old, new, wrap):
    s, a, b = load(switched), load(old), load(new)
    cut, pad = int(wrap * SR), int(0.010 * SR)
    before = max(abs(s[i] - a[i]) for i in range(0, cut - pad))
    after = max(abs(s[i] - b[i]) for i in range(cut + pad, min(len(s), len(b))))
    apart = max(abs(a[i] - b[i]) for i in range(cut + pad, min(len(a), len(b))))   # else nothing is tested
    ok = before <= 2 and after <= 2 and apart > 1000
    print("%s before the wrap: off the old break by %d; after: off the new break by %d" % (
        "ok  " if ok else "FAIL", before, after))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])))
