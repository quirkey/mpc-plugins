#!/usr/bin/env bash
# Copy the classic breaks out of a breaks collection into a staging folder, renamed with their bar count so
# Breaks never has to guess (BreakLibrary.h reads "4bar" in a name first). Copies only; the source is untouched.
#   Breaks/tools/stage_classics.sh [source dir] [staging dir]
# Bar counts marked "judgment" were picked from the file length and the record's known tempo; if one plays
# at the wrong speed, rename the file on the MPC (e.g. "Cold Sweat 3bar.wav") and press RESCAN.
# Think is part 2 of the collection's five one-bar cuts: part 1 holds a horn note across its end, which the loop cut
# off. Hot Pants's file is ~20 dB under the rest, so it is brought up with level_break.py.
# Four of the collection's cuts don't start on the one, so trim_break.py cuts them to whole bars from the downbeat
# (downbeat, BPM: fitted to the onsets, the downbeat on the kick's attack; checked against the snare on 2 and 4):
#   Mardi Gras and Funky President open with a pickup (0.81 s, 0.65 s); Synthetic Substitution's one is 71 ms in.
#   Funky President has 3.9 bars after its downbeat and I Know You Got Soul 1.87, so they keep 2 bars and 1.
set -euo pipefail
SRC="${1:-$HOME/Downloads/Breaks}"
OUT="${2:-$(cd "$(dirname "$0")/.." && pwd)/build/classics}"
A="$SRC/All The Breaks"
rm -rf "$OUT" && mkdir -p "$OUT"
put() { cp "$1" "$OUT/$2.wav"; }
put "$SRC/AmenVN_4barOrig.wav"                          "Amen Brother 4bar"
put "$SRC/Think Break Pt2.wav"                          "Think 1bar"
put "$SRC/ORIGINAL Funky Drummer.wav"                   "Funky Drummer 4bar"
put "$SRC/Apache - Part1.wav"                           "Apache 4bar"
python3 "$(dirname "$0")/level_break.py" "$SRC/120_HOT_PANTS.wav" "$OUT/Hot Pants 2bar.wav"
put "$A/James Brown - Cold Sweat.wav"                   "Cold Sweat 4bar"              # judgment: 3 or 4
trim() { python3 "$(dirname "$0")/trim_break.py" "$1" "$OUT/$2.wav" "$3" "$4" "$5"; }
trim "$A/Melvin Bliss - Synthetic Substitution.wav"     "Synthetic Substitution 2bar" 0.071 95.48 2
put "$A/Skull Snaps - It_s A New Day.wav"               "Its A New Day 2bar"
put "$A/Billy Squier - The Big Beat.wav"                "The Big Beat 4bar"
put "$A/Led Zeppelin - When The Levee Breaks.wav"       "When The Levee Breaks 2bar"
trim "$A/Bobby Byrd - I Know You Got Soul.wav"          "I Know You Got Soul 1bar"   0.032 113.35 1
trim "$A/James Brown - Funky President.wav"            "Funky President 2bar"       0.652 102.00 2
put "$A/The Soul Searchers - Ashley_s Roadclip.wav"     "Ashleys Roachclip 8bar"
trim "$A/Bob James - Take Me To Mardi Gras.wav"         "Take Me To Mardi Gras 4bar" 0.809 103.39 4
ls "$OUT" | wc -l | tr -d ' ' | xargs echo "staged in $OUT:"
du -sh "$OUT" | cut -f1
