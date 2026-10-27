#!/usr/bin/env python3
"""Cut a break to exactly N bars from its downbeat:  trim_break.py in.wav out.wav downbeat_s bpm bars

Breaks plays a file's first sample on the one and stretches the whole file over its bar count, so a file that opens
with a pickup, or that isn't a whole number of bars, plays off the beat. This keeps [downbeat - 3 ms, + bars * 4 beats):
the 3 ms before the attack keep its front, and the same 3 ms at the end lead back into it. When the source ends before
the last bar does, the rest is taken from the same place one bar earlier (it leads into a downbeat too), joined with
a 5 ms crossfade. 16- or 24-bit PCM in, the same out; other chunks are dropped. Prints what it cut and filled.
"""
import struct
import sys
import wave

PRE = 0.003
XFADE = 0.005


def main(src, dst, downbeat, bpm, bars):
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
    bar = int(round(4 * 60.0 / bpm * sr))
    start = int(round((downbeat - PRE) * sr))
    length = int(round(bars * 4 * 60.0 / bpm * sr))
    if start < 0 or start >= n:
        sys.exit("downbeat outside the file")
    have = min(length, n - start)
    fill = length - have
    if fill > bar // 16:                             # more than a 16th missing: too much to patch
        sys.exit("the file ends %.3f s before the last bar does: use fewer bars" % (fill / sr))

    def frame(i):
        return x[i * ch:(i + 1) * ch]

    out = []
    xf = int(XFADE * sr) if fill else 0
    for i in range(length):
        src_i = start + i
        if i < have - xf:
            out.extend(frame(src_i))
        elif i < have:                                   # crossfade into the copy from one bar earlier
            a = (i - (have - xf)) / float(xf)
            out.extend((1 - a) * p + a * q for p, q in zip(frame(src_i), frame(src_i - bar)))
        else:
            out.extend(frame(src_i - bar))

    o = wave.open(dst, "wb")
    o.setnchannels(ch)
    o.setsampwidth(sw)
    o.setframerate(sr)
    top = full - 1
    clip = [max(-1.0, min(top / full, v)) for v in out]
    if sw == 2:
        o.writeframes(struct.pack("<%dh" % len(clip), *(int(round(v * full)) for v in clip)))
    else:
        o.writeframes(b"".join(int(round(v * full)).to_bytes(3, "little", signed=True) for v in clip))
    o.close()
    print("%s: from %.3f s, %d bars at %.2f BPM (%.3f s)%s" % (
        dst.split("/")[-1], start / float(sr), bars, bpm, length / float(sr),
        ", last %.0f ms from the bar before" % (fill * 1000.0 / sr) if fill else ""))


if __name__ == "__main__":
    a = sys.argv[1:]
    main(a[0], a[1], float(a[2]), float(a[3]), int(a[4]))
