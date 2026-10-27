# Roads

A 16-voice electric piano instrument for MPC OS: Paul Kellett's mda ePiano (33 sampled keygroups and its tine / reed
modelling), with tine, reed and clav characters, treble and drive, tremolo / auto-pan, chorus, a note-triggered envelope
filter and a 3-band EQ. Ported from the Roads firmware for Daisy, without its delay, reverb, pedal wah and cabinet IR:
use MPC's own delay and reverb, and a cab / amp plugin on the track.

## Pages

**PIANO**
- **Character**: the voice: SeventyThree (tines, the classic electric piano bell), Wurli (reeds, a buzzier, more nasal
  tone) or Clav (bright and percussive). Only the names changed in this repo (1.0 to 1.2 called the first one by the
  maker's name), so projects load as before.
- **Voice**: Decay, Release, Hardness (shifts which sample keygroups play: harder is brighter).
- **Output**: Level.
- **Tone**: Treble, Drive, then the EQ's Low, Mid and High (dB).

**FX**
- **Vibrato**: the Pan / Trem switch (auto-pan or tremolo), Speed (0.3 to 8 Hz), Depth (0 to 100 %), and Chorus.
- **Envelope filter**: Env Sense (how far each note opens the filter; 0 is off), Env Peak, Env Freq (where it rests),
  Env Range, and the LP / BP / HP switch.

Each page's Q-Links follow the page's layout.

## Files

- `Roads.cpp`: the engine, a copy of the Daisy firmware's voice code (fixes go into both by hand).
- `epiano_samples.bin`: the sample data, shipped in the plugin's `samples/` folder (vst.json `"extra"`). Made by
  `tools/prepare_samples.py` from the mda ePiano's sample table.
- `tools/vib_check.sh`: checks that old projects still load: 1.0's single Pan / Trem knob becomes the switch and Depth,
  and 1.1's cabinet settings change nothing while its EQ still applies.
- Built with `-fno-jump-tables`: a `switch` jump table in `.text` once tripped the build's ARMv8 instruction check.

## History

- 1.2: the cabinet IR is gone (its parameters stay as "(unused)" so projects still load); the EQ moved to PIANO.
- 1.1: cabinet IRs from WAV files.
- 1.0: first release.

## Credits

The voice algorithm and sample data are from the mda ePiano by Paul Kellett (mda VST plug-ins, released as open source;
the sample table is taken from [mda-lv2](https://gitlab.com/drobilla/mda-lv2)). The chorus and state-variable filter are
[DaisySP](https://github.com/electro-smith/DaisySP)'s.
