# Breaks

An instrument for MPC OS that plays a drum break in time with the song while the transport runs: no notes needed. The
break is cut into 16th-note slices and each slice is retriggered on the song's 16th grid, so it always follows MPC's
tempo. It runs through its whole length whatever the sequence's loop: a 4-bar break over a 1-bar loop keeps going bar 1,
2, 3, 4. Pressing play lines it up with the song (bar 1 of the song is the start of the break).

## Your breaks

Breaks ships with no breaks. Put WAVs in the plugin folder's `breaks/` (the installer keeps it on upgrade), or in any
folder on a drive with "break" in its name; press **RESCAN** after adding files. Name a file with its bar count
(`Amen 4bar.wav`) and that is used first; otherwise Breaks works it out from ACID tags, a BPM in the name or the length.
A project saves its break by file name.

## Pages

**BREAKS**
- **Break**: Select / Prev / Next, the name readout (name, tempo, bars or status), Bars (Auto / 1 / 2 / 4 / 8 / 16), Rescan,
  and the waveform with the playing slice lit.
- **Sound**: Pitch (semitones; repitches the slices, sampler style, without moving the grid), Level.
- **Stretch**: Grain (1/16 restarts each slice every 16th, the classic sound; a shorter grain re-cuts within each 16th,
  time-stretch style) and Half Time.
- **Grit**: Bits and Rate (kHz), the same as SamplerFilter's.

**VARIATION**
- **Pattern**: Vary (the chance a 16th is varied), Repeats Every (the break's length / 1 / 2 / 4 bars / Evolve, which never
  repeats), Variation (the seed: each number is a different variation, saved with the project) with New and Prev.
- **Variation**: the relative weights of Swap, Repeat, Reverse and Gap.
- **Shape**: each slice's Attack, Gate and Release, in % of its 16th.

**PADS**: each pad holds an effect for as long as it is held; the break keeps its place underneath, so letting go lands
where it would have been. Reverse, Gap, Flam, Triplet, Roll, Stutter, Half, Octave down / up, Ramp down / up, Speed
down / up and Velocity down / up. Pads map from either of MPC's pad layouts. **Length** quantizes them (a press lands on
the nearest 16th, a release plays on to whole lengths; Free is exactly while held) and **Ramp** sets how fast the ramp,
speed and velocity pads move.

## Files

- `Breaks.cpp`: the engine. A worker thread scans folders and loads WAVs; a loaded break reaches the audio thread through
  an atomic pointer, so the audio thread never allocates, locks or touches files.
- `BreakLibrary.h` (finding breaks and their bar counts), `Variation.h`, `WavFile.h`.
- `tools/`: checks, each runnable on its own:
  - `state_test.sh`: project save and restore by file name.
  - `regress_check.sh [git-ref]`: old projects render byte for byte the same as at `git-ref`.
  - `seq_check.sh`, `variation_check.sh`, `stretch_check.sh`, `shape_check.sh`, `pads_check.sh`: timing on every 16th,
    variations, grains and half time, slice shapes, pad effects.
  - `probe_breaks`: what Breaks makes of a folder of WAVs.
  - `stage_classics.sh`: stages a set of classic breaks from your own collection into `build/classics` for testing, cut
    to whole bars from the downbeat with `trim_break.py`. Those recordings are not part of this repo or its releases.
