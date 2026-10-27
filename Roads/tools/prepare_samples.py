#!/usr/bin/env python3
"""
Prepare MDA EPiano sample data for Daisy QSPI flash.

Downloads mdaEPianoData.h from the mda-lv2 source, parses the epianoData[]
short array, applies the same crossfade loops as the original plugin, and
writes a raw binary file ready for dfu-util.

Usage:
    python3 prepare_samples.py          # produces epiano_samples.bin
    make flash-samples                  # flashes it (hold BOOT + press RESET first)
"""

import urllib.request
import re
import struct
import sys

DATA_URL = "https://gitlab.com/drobilla/mda-lv2/-/raw/main/src/mdaEPianoData.h"

# First 28 keygroups that get crossfade treatment (pos, end, loop)
KGRP_XFADE = [
    (      0,   8476,  4400),
    (   8477,  16248,  4903),
    (  16249,  34565,  6398),
    (  34566,  41384,  3938),
    (  41385,  45760,  1633),
    (  45761,  65211,  5245),
    (  65212,  72897,  2937),
    (  72898,  78626,  2203),
    (  78627, 100387,  6368),
    ( 100388, 116297, 10452),
    ( 116298, 127661,  5217),
    ( 127662, 144113,  3099),
    ( 144114, 152863,  4284),
    ( 152864, 173107,  3916),
    ( 173108, 192734,  2937),
    ( 192735, 204598,  4732),
    ( 204599, 218995,  4733),
    ( 218996, 233801,  2285),
    ( 233802, 248011,  4098),
    ( 248012, 265287,  4099),
    ( 265288, 282255,  3609),
    ( 282256, 293776,  2446),
    ( 293777, 312566,  6278),
    ( 312567, 330200,  2283),
    ( 330201, 348889,  2689),
    ( 348890, 365675,  4370),
    ( 365676, 383661,  5225),
    ( 383662, 393372,  2811),
]

def apply_crossfades(waves):
    """
    Port of the MDA EPiano crossfade loop (mdaEPiano.cpp constructor).
    Smoothly blends each loop end back to loop start over 50 samples.
    Modifies waves in-place.
    """
    for pos, end, loop in KGRP_XFADE:
        p0 = end
        p1 = end - loop
        xf = 1.0
        while xf > 0.0:
            blended = int((1.0 - xf) * waves[p0] + xf * waves[p1])
            # Clamp to int16 range
            waves[p0] = max(-32768, min(32767, blended))
            p0 -= 1
            p1 -= 1
            xf -= 0.02
    return waves

def main():
    print(f"Downloading {DATA_URL} ...")
    with urllib.request.urlopen(DATA_URL) as resp:
        src = resp.read().decode('utf-8')

    # Strip C comments and find the array body
    src_stripped = re.sub(r'//[^\n]*', '', src)
    src_stripped = re.sub(r'/\*.*?\*/', '', src_stripped, flags=re.DOTALL)

    m = re.search(r'epianoData\s*\[\s*\]\s*=\s*\{([^}]+)\}', src_stripped, re.DOTALL)
    if not m:
        print("ERROR: could not find epianoData[] in downloaded file")
        sys.exit(1)

    nums = list(map(int, re.findall(r'-?\d+', m.group(1))))
    print(f"Parsed {len(nums)} samples ({len(nums)*2} bytes)")

    if len(nums) < 422408:
        print(f"WARNING: expected at least 422408 samples, got {len(nums)}")

    print("Applying crossfade loops...")
    apply_crossfades(nums)

    out_path = "epiano_samples.bin"
    with open(out_path, 'wb') as f:
        for v in nums:
            f.write(struct.pack('<h', v))

    print(f"Written: {out_path} ({len(nums)*2} bytes)")
    print()
    print("Flash to Daisy (hold BOOT button, press RESET, then run):")
    print(f"  dfu-util -a 1 -s 0x90010000:leave -D {out_path}")

if __name__ == '__main__':
    main()
