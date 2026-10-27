#!/usr/bin/env python3
"""Grain splices must not click:  check_clicks.py plain.wav grained.wav [ratio]

Compares the largest sample-to-sample jump of a grained render with the same scene played plainly (Grain 1/16):
a splice without its crossfade jumps by up to the whole signal swing, far more than the material itself does.
Fails when the grained render's largest jump is over `ratio` (default 1.5) times the plain one's.
"""
import struct
import sys
import wave


def max_jump(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    best = 0
    for ch in (s[0::2], s[1::2]):
        best = max(best, max(abs(ch[i + 1] - ch[i]) for i in range(len(ch) - 1)))
    return best / 32768.0


def main(plain, grained, ratio=1.5):
    a, b = max_jump(plain), max_jump(grained)
    ok = b <= a * ratio
    print("%s largest jump %.3f (plain %.3f, limit %.3f)" % ("ok  " if ok else "FAIL", b, a, a * ratio))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.5))
