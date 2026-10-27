#!/usr/bin/env python3
"""Test WAVs for Breaks (standard library only):  make_test_breaks.py <out dir>

  Grid 120bpm.wav     2 bars at 120 BPM, 44.1 kHz 16-bit stereo. Each of the 32 sixteenths is a 1 kHz burst whose
                      level encodes its slice number (0.1 + 0.7 * s / 31), so check_sync.py can read back which slice
                      played on every step.
  Funk Test 96.wav    4 bars at 96 BPM (tempo only in the name), 48 kHz 24-bit mono, WAVE_FORMAT_EXTENSIBLE, with a
                      LIST chunk before the data: a synthetic kick/snare/hat groove.
  Acid Loop.wav       1 bar at 100 BPM, 44.1 kHz float stereo, tempo and beats only in an ACID chunk, after an
                      odd-sized chunk that needs RIFF's pad byte.
"""
import math
import os
import random
import struct
import sys


def chunk(tag, data):
    pad = b"\0" if len(data) & 1 else b""
    return tag + struct.pack("<I", len(data)) + data + pad


def riff(chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def fmt_pcm(ch, rate, bits):
    return chunk(b"fmt ", struct.pack("<HHIIHH", 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits))


def fmt_extensible(ch, rate, bits, subformat):
    guid = struct.pack("<H", subformat) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
    return chunk(b"fmt ", struct.pack("<HHIIHHHHI", 0xFFFE, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits,
                                      22, bits, 0x4 if ch == 1 else 0x3) + guid)


def grid(path):
    rate, bpm, slices = 44100, 120.0, 32
    step = int(rate * 60 / bpm / 4)
    frames = []
    for s in range(slices):
        amp = 0.1 + 0.7 * s / (slices - 1)
        for i in range(step):
            on = i < step * 0.6
            env = min(1.0, i / 20.0, (step * 0.6 - i) / 20.0) if on else 0.0
            v = int(32767 * amp * env * math.sin(2 * math.pi * 1000 * i / rate))
            frames.append(struct.pack("<hh", v, v))
    open(path, "wb").write(riff([fmt_pcm(2, rate, 16), chunk(b"data", b"".join(frames))]))


def funk(path):
    rate, bpm, bars = 48000, 96.0, 4
    n = int(round(rate * bars * 4 * 60 / bpm))
    step = 60 / bpm / 4
    rng = random.Random(1)
    kick = {0, 6, 10}
    snare = {4, 12}
    out = bytearray()
    for i in range(n):
        t = i / rate
        k = int(t / step)
        tt = t - k * step
        s16 = k % 16
        v = 0.0
        if s16 in kick:
            v += 0.7 * math.sin(2 * math.pi * (45 + 90 * math.exp(-tt * 30)) * tt) * math.exp(-tt * 9)
        if s16 in snare:
            v += 0.45 * (rng.random() * 2 - 1) * math.exp(-tt * 22) + 0.2 * math.sin(2 * math.pi * 190 * tt) * math.exp(-tt * 18)
        v += 0.12 * (rng.random() * 2 - 1) * math.exp(-tt * 70)
        x = max(-8388608, min(8388607, int(v * 8388607)))
        out += struct.pack("<i", x)[:3]
    info = chunk(b"LIST", b"INFO" + chunk(b"ISFT", b"make_test_breaks\0"))
    open(path, "wb").write(riff([fmt_extensible(1, rate, 24, 1), info, chunk(b"data", bytes(out))]))


def acid(path):
    rate, bpm, beats = 44100, 100.0, 4
    n = int(round(rate * beats * 60 / bpm))
    out = bytearray()
    for i in range(n):
        t = i / rate
        tb = (t * bpm / 60) % 1 * 60 / bpm
        v = 0.6 * math.sin(2 * math.pi * 60 * tb) * math.exp(-tb * 8)
        out += struct.pack("<ff", v, v)
    fmt = chunk(b"fmt ", struct.pack("<HHIIHH", 3, 2, rate, rate * 8, 8, 32))
    acid_chunk = chunk(b"acid", struct.pack("<IHHfIHHf", 1, 60, 0x8000, 0.0, beats, 4, 4, bpm))
    open(path, "wb").write(riff([fmt, chunk(b"junk", b"odd"), acid_chunk, chunk(b"data", bytes(out))]))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    grid(os.path.join(out, "Grid 120bpm.wav"))
    funk(os.path.join(out, "Funk Test 96.wav"))
    acid(os.path.join(out, "Acid Loop.wav"))
    print("wrote 3 test breaks to", out)
