# SamplerFilter

A track effect for MPC OS modelled on the filter and grit of late-80s rack samplers: a 6-pole (36 dB/octave) Butterworth
low-pass with no resonance, which darkens and thickens rather than squelching, plus the bit-depth and sample-rate
reduction from [Breaks](../Breaks).

## Controls

- **Filter**: the 6-pole low-pass, 20 Hz to 20 kHz (100 is off).
- **Bits**: 4 / 6 / 8 / 10 / 12 / Off.
- **Rate (kHz)**: 4 / 8 / 11 / 16 / 22 / 26 / 32 / Off: sample-and-hold decimation.
- **Track**: On puts the sampler's bandwidth filter (the same low-pass at 0.4 × Rate) before the sampling, so low rates
  sound muffled rather than aliased.
- **Drive**: an input preamp pushed into a soft clip.
- **Mix**, **Level**.

The chain, per channel: Drive, Track, Rate then Bits, Filter, Mix and Level. The filter is three state-variable sections
at the Butterworth Qs, which stay well-behaved at low cutoffs and while the cutoff moves.

## Files

- `SamplerFilter.cpp`: the engine; Bits / Rate are `common/Crusher.h`, shared with Breaks, so the two sound the same.
- `tools/filter_check.sh`: the filter's response against a 6th-order Butterworth, the old Bits / Rate knobs mapping to
  the switches, then a drum loop through a few settings (no clicks or NaN, levels, CPU).
- `tools/skin_post.py`: post-processes the generated skin.
