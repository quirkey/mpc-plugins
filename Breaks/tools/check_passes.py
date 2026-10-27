#!/usr/bin/env python3
"""Do the passes of a loop play the same:  check_passes.py render.wav pass_seconds same|differ

Compares the second pass of the loop with the third (the first starts from the render's settling). `same`: the
break restarts with each pass, so they match sample for sample; `differ`: EVOLVE varies each pass differently.
Exit status 1 when it isn't so."""
import struct
import sys
import wave

SR = 44100


def main(path, seconds, want):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    x = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]
    n = int(seconds * SR)
    # 10 ms in from each wrap, past the restart's crossfade
    a, b = x[n + 441:2 * n - 441], x[2 * n + 441:3 * n - 441]
    off = max(abs(p - q) for p, q in zip(a, b))
    ok = (off <= 2) == (want == "same")
    print("%s passes 2 and 3 differ by up to %d (want %s)" % ("ok  " if ok else "FAIL", off, want))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], float(sys.argv[2]), sys.argv[3]))
