#!/usr/bin/env python3
"""Bring a quiet break up to the others' level:  level_break.py in.wav out.wav [rms_dbfs]

Gain to the target RMS (default -18 dBFS, where Amen and Think sit; the classics run from -18 to -10), then a
look-ahead peak limiter (stereo linked, 1.5 ms look-ahead, 60 ms release) holds peaks at -0.3 dBFS, where the other
classics are limited.
16- or 24-bit PCM in, the same out; other chunks are dropped. Prints the gain and how much the loudest peak was held.
"""
import collections
import math
import struct
import sys
import wave

CEIL = 10 ** (-0.3 / 20)


def main(src, dst, target=-18.0):
    w = wave.open(src)
    ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    full = float(1 << (8 * sw - 1))
    if sw == 2:
        x = [v / full for v in struct.unpack("<%dh" % (len(raw) // 2), raw)]
    elif sw == 3:
        x = [int.from_bytes(raw[i:i + 3], "little", signed=True) / full for i in range(0, len(raw), 3)]
    else:
        sys.exit("16- or 24-bit PCM only")
    rms = math.sqrt(sum(v * v for v in x) / len(x))
    gain = 10 ** (target / 20) / rms
    x = [v * gain for v in x]
    frames = len(x) // ch
    need = [min(1.0, CEIL / max(max(abs(x[i * ch + c]) for c in range(ch)), 1e-12)) for i in range(frames)]

    look = int(0.0015 * sr)
    gmin, q = [0.0] * frames, collections.deque()   # least gain needed over the next `look` frames
    for i in range(frames + look):
        if i < frames:
            while q and need[q[-1]] >= need[i]:
                q.pop()
            q.append(i)
        k = i - look
        if k >= 0:
            while q[0] < k:
                q.popleft()
            gmin[k] = need[q[0]]
    rel = 1.0 / (0.060 * sr)
    held, g = [0.0] * frames, 1.0
    for i in range(frames):                          # back up slowly after a peak
        g = min(gmin[i], g + rel)
        held[i] = g
    out, acc, win = [], 0.0, collections.deque()     # averaged over the look-ahead: no step in the gain
    for i in range(frames):
        win.append(held[i])
        acc += held[i]
        if len(win) > look:
            acc -= win.popleft()
        gi = acc / len(win)
        for c in range(ch):
            out.append(max(-CEIL, min(CEIL, x[i * ch + c] * gi)))

    o = wave.open(dst, "wb")
    o.setnchannels(ch)
    o.setsampwidth(sw)
    o.setframerate(sr)
    top = full - 1
    if sw == 2:
        o.writeframes(struct.pack("<%dh" % len(out), *(int(round(v * top)) for v in out)))
    else:
        o.writeframes(b"".join(int(round(v * top)).to_bytes(3, "little", signed=True) for v in out))
    o.close()
    print("%s: %+.1f dB, peaks held by up to %.1f dB" % (dst.split("/")[-1], 20 * math.log10(gain),
                                                         -20 * math.log10(min(held))))


if __name__ == "__main__":
    a = sys.argv[1:]
    main(a[0], a[1], *(float(v) for v in a[2:]))
