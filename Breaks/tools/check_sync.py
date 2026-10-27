#!/usr/bin/env python3
"""Check that Breaks played the right slice on every 16th:  check_sync.py render.wav transport.csv [advance]

advance: break 16ths per song 16th (1, or 0.5 for Half time: slice floor(count * 0.5) plays, its second song
16th from the slice's middle, which the burst over its first 60% still covers just after the boundary).

For a render of "Grid 120bpm.wav" (make_test_breaks.py: 32 slices, slice s is a 1 kHz burst at level
0.1 + 0.7 * s / 31) under MPC_RENDER_TRANSPORT, with MPC_RENDER_LOG=transport.csv.

The model being checked (Breaks.cpp): the break starts over with the sequence. Play, a loop wrap and a locate
(a jump of over a beat) start it from its top at that bar (count = song 16ths since the bar); in between every
16th boundary the song position crosses advances the count by however many 16ths it moved, and a step back never
counts. A jump back is remembered as the loop: from then on the loop end's own 16th (in the block that straddles
it, still reported pre-wrap) already plays the loop start's slice, unless no wrap follows (the scenario's locate
back at 12 s is learned as a loop that never comes again: that 16th is put back on the next block). The slice
played is floor(count * advance) mod 32, read back from the burst's level just after each boundary.
Starts that land mid-slice (Play, a lurch past a boundary) are not measured. Also checks the output is
silent while stopped. Exit status 1 on any miss.
"""
import math
import struct
import sys
import wave

SLICES, SR = 32, 44100


def main(wav_path, csv_path, advance=1.0):
    w = wave.open(wav_path)
    raw = w.readframes(w.getnframes())
    left = struct.unpack("<%dh" % (len(raw) // 2), raw)[0::2]
    blocks = [tuple(float(x) for x in line.split(",")) for line in open(csv_path) if line.strip()]

    def level(f0, f1):
        seg = left[max(0, int(f0)):int(f1)]
        return max((abs(v) for v in seg), default=0) / 32768.0

    checked = misses = silent_bad = wraps = 0
    last_k = count = next_ppq = None
    loop_end = loop_start = None
    predicted = False
    from_bar = lambda k: k - k // 16 * 16
    for i, (frame, ppq, bpm, playing) in enumerate(blocks):
        n = (blocks[i + 1][0] if i + 1 < len(blocks) else frame + 128) - frame
        if not playing:
            stopped_for = 0
            while i - stopped_for - 1 >= 0 and not blocks[i - stopped_for - 1][3]:
                stopped_for += 1
            # the engine fades out over 5 ms (220 frames) on stop; after that it must be silent
            if stopped_for * 128 >= 0.010 * SR and level(frame, frame + n) > 0.01:
                silent_bad += 1
            last_k = count = next_ppq = None
            predicted = False
            continue
        step = bpm / 60.0 / SR
        k0 = math.floor(ppq * 4 + 1e-9)
        if last_k is None:
            last_k, count = k0, from_bar(k0)    # Play: from the top at this bar, starts mid-slice
        elif abs(ppq - next_ppq) > 1.0:
            wraps += 1
            if predicted:
                last_k = k0                     # the wrap the loop end already restarted for
            else:
                if ppq < next_ppq:              # a jump back: the loop
                    loop_end, loop_start = round(next_ppq * 4), k0
                last_k, count = k0, from_bar(k0)
            predicted = False
        elif k0 > last_k:
            count += k0 - last_k                # a lurch past a boundary: counted, starts mid-slice
            last_k = k0
        k = last_k + 1
        while k / 4.0 < ppq + n * step:
            f = frame + (k / 4.0 - ppq) / step
            count += 1
            if k == loop_end:
                nxt = blocks[i + 1] if i + 1 < len(blocks) else None
                if nxt and nxt[3] and abs(nxt[1] - (ppq + n * step)) > 1.0 and math.floor(nxt[1] * 4 + 1e-9) == loop_start:
                    count, predicted = from_bar(loop_start), True
                else:
                    loop_end = None             # restarted, then put back on the next block
            want = math.floor(count * advance) % SLICES
            last_k = k
            k += 1
            # the level is read 4-40 ms after the boundary; skip it if the transport jumped inside that window
            w0, w1 = f + 0.004 * SR, f + 0.040 * SR
            jumped = False
            for fr, pq, bp, pl in blocks[i + 1:]:
                if fr >= w1:
                    break
                expect = ppq + (fr - frame) * step
                if not pl or abs(pq - expect) > 0.01:
                    jumped = True
                    break
            if jumped:
                continue
            lv = level(w0, w1)
            got = round((lv - 0.1) / 0.7 * (SLICES - 1))
            checked += 1
            if got != want:
                misses += 1
                if misses <= 10:
                    print("miss: at %.3f s (song 16th %d): slice %d, want %d (level %.3f)" % (f / SR, k - 1, got, want, lv))
        next_ppq = ppq + n * step
    print("%d sixteenths checked across %d wraps/locates, %d wrong; %d stopped blocks not silent"
          % (checked, wraps, misses, silent_bad))
    return 1 if misses or silent_bad or checked == 0 or wraps == 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0))
