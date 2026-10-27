#!/usr/bin/env python3
"""A trimmed break must loop without a click:  check_seam.py file.wav [...]

Compares the jump from the last sample back to the first (the loop seam) with the material's own sample-to-sample
jumps: fails when the seam's is over the 99.9th percentile of the file's.
"""
import struct
import sys
import wave


def check(path):
    w = wave.open(path)
    ch, sw = w.getnchannels(), w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw == 2:
        s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    else:
        s = [int.from_bytes(raw[i:i + 3], "little", signed=True) >> 8 for i in range(0, len(raw), 3)]
    worst_ok = True
    parts = []
    for c in range(ch):
        x = s[c::ch]
        jumps = sorted(abs(x[i + 1] - x[i]) for i in range(len(x) - 1))
        p999 = jumps[int(len(jumps) * 0.999)]
        seam = abs(x[0] - x[-1])
        worst_ok = worst_ok and seam <= p999
        parts.append("seam %d / p99.9 %d" % (seam, p999))
    print("%s %s: %s" % ("ok  " if worst_ok else "FAIL", path.split("/")[-1], ", ".join(parts)))
    return worst_ok


if __name__ == "__main__":
    sys.exit(0 if all([check(p) for p in sys.argv[1:]]) else 1)
