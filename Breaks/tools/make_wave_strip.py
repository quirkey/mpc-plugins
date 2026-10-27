#!/usr/bin/env python3
"""The waveform column filmstrips:  make_wave_strip.py [out_dir]   (default: ../images)

One column of the BREAKS page waveform, W x H per frame, 32 frames top to bottom: frames 0-15 are levels 0-15 of
a bar mirrored about the centre line (dim), 16-31 the same lit (the playing slice). Each frame carries its own
strip of the dark LCD, so 128 columns side by side make one panel. The panel is 1216 px across, so columns are 9
and 10 px wide in turn: wave_col9.png and wave_col10.png, the same bar in each. layout.conf places them as
`meter ... frames=32`; Breaks.cpp's wave_N params pick the frame (level, + 16 while playing).
"""
import os
import sys

from PIL import Image, ImageDraw

H, LEVELS = 120, 16
LCD, MID = (26, 31, 22), (44, 53, 40)
DIM, LIT = (88, 128, 80), (125, 255, 106)   # unlit bar; lit = the S950 green of the knob arcs
BAR = 6                                      # bar width (the rest is the gap to the next column)


def frame(w, level, lit):
    im = Image.new("RGB", (w, H), LCD)
    d = ImageDraw.Draw(im)
    cy = H // 2
    d.line([(0, cy), (w - 1, cy)], fill=MID)
    half = 1 + round(level * (H / 2 - 6) / (LEVELS - 1))
    x0 = 2
    d.rectangle([x0, cy - half, x0 + BAR - 1, cy + half - 1], fill=LIT if lit else DIM)
    return im


def main(out_dir):
    for w in (9, 10):
        strip = Image.new("RGB", (w, H * LEVELS * 2), LCD)
        for i in range(LEVELS * 2):
            strip.paste(frame(w, i % LEVELS, i >= LEVELS), (0, i * H))
        out = os.path.join(out_dir, "wave_col%d.png" % w)
        strip.save(out)
        print("wrote %s (%dx%d, %d frames)" % (out, strip.width, strip.height, LEVELS * 2))


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "images"))
