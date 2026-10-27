# mpc-plugins

Plugins that run **inside** an MPC / Force in standalone mode (MPC OS's own VST host, on the device's ARM CPU), built
with [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
([guide](https://sd88me.github.io/mpc-vst-plugins/build.html)), pinned here as the `mpc-vst-plugins/` submodule. The DSP
started life as firmware for [Daisy](https://electro-smith.com/daisy) boards.

| Plugin | Kind | What it is |
|--------|------|------------|
| [Breaks](Breaks) | Instrument | Plays a drum break in time with the song while the transport runs: 16th slices retriggered on the song grid, through the whole break whatever the loop length. Pitch, grit, time-stretch grains, half time, seeded variations and pad effects |
| [SamplerFilter](SamplerFilter) | Effect | A vintage-sampler 6-pole low-pass (no resonance) with the bit-depth and sample-rate grit from Breaks |
| [Roads](Roads) | Instrument | A 16-voice electric piano (tine, reed and clav characters) with tremolo / auto-pan, chorus, a note-triggered envelope filter and a 3-band EQ |

Each folder's README covers the plugin's controls. Releases (one zip per plugin, tagged `<plugin>-vX.Y.Z`) are on the
[Releases](../../releases) page.

## Installing

**This needs root SSH access to the device**, i.e. a modded unit; stock MPC OS can't load third-party plugins.
The builds are for Gen1 devices (MPC Live / Live II, One, X, Key 61, Force: `armv7`).

```bash
unzip Roads-1.2.1-mpc-armv7.zip -d /tmp
scp -r /tmp/Roads-1.2.1 root@<mpc-ip>:/tmp/
ssh root@<mpc-ip> sh /tmp/Roads-1.2.1/install.sh     # stops MPC, backs up MPC.settings, installs, restarts
```

Or drop the zip into the [MPC plugin installer app](https://github.com/sd88me/mpc-vst-plugins/releases/latest). Then
add the plugin to a track from the plugin browser. The zip's `INSTALL.md` has the details.

## Building

Needs Docker Desktop (it emulates the MPC's 32-bit ARM), Python 3 and a host compiler (Xcode command line tools).

```bash
./setup.sh                       # once: submodules (toolkit, DaisySP), Docker and the build images
./build.sh test    Roads         # offline host test under ASan/UBSan: params, MIDI → audio, save/restore, threads
./build.sh armtest Roads         # the same test built for the device, on an emulated ARMv7 CPU (catches SIGILL)
./build.sh render  Roads         # Roads/build/render.wav + peak/RMS/CPU: listen before touching the device
./build.sh render  SamplerFilter filter=40 bits_step=2   # any params as key=value
./build.sh arm     Roads         # device build: Roads/build/roads.so, skin/, pluginlist-entry.xml (+ ARMv7 ISA check)
./build.sh release Roads 1.2.1   # Roads/dist/Roads-1.2.1-mpc-armv7.zip with install.sh / uninstall.sh / INSTALL.md
./build.sh preview Roads         # the skin, as PNGs of each page (Roads/build/preview_N.png)
```

`test` and `armtest` must both pass before a build goes to a device: a crash in a plugin takes MPC down. `render` plays
a test phrase into instruments and a 120 BPM drum loop through effects (with song position, so synced things behave as
on the MPC); `MPC_RENDER_TRANSPORT=1` adds MPC's measured transport quirks, `MPC_RENDER_DATA_DIR`, `_BPM` and `_LOG` set
the data folder, tempo and a per-block log. The host CPU figure is your computer's; the device is several times slower,
so check on hardware with `mpc-vst-plugins/tools/bench.sh <so> <mpc-ip>` (its docs/BENCH.md). `tools/screenshot.sh
--plugin` grabs the device's screen. Each plugin also has its own checks in its `tools/` (see its README).

## How it fits together

MPC OS has a JUCE plugin host that loads **Linux VST2** `.so` files built for its ARM CPU. The toolkit supplies a
hand-written VST2 wrapper (no Steinberg SDK) around a small C engine interface (`mpc-vst-plugins/wrapper/engine.h`), a
generator that turns a parameter list into the plugin's native touchscreen page and Q-Link map, and Docker images with
the right compiler and glibc.

`common/DaisyEngine.h` adapts that interface to how the Daisy code is written, so a plugin is one class plus
`params.json` / `vst.json` / `layout.conf`:

```cpp
#include "../common/DaisyEngine.h"

class MyDsp : public DaisyDsp {
  public:
    void Init(float sr);                        // like main()'s setup: sr is always 44100
    void SetParam(int index, float value);      // params.json order; value in its own units (options: the index)
    void Midi(const uint8_t *msg, int len);     // instruments: note on/off, CC, bend
    void Process(const float *inL, const float *inR, float *outL, float *outR, int frames);   // like AudioCallback
};
MPC_DAISY_ENGINE(MyDsp)
```

Optional: `SongPos(ppq, bpm, playing)` (with `"defines": {"HAS_SONG_POS": 1}`) for tempo sync, and
`Display(index, buf, len)` for live value text (a param with `"dynamic_display": true`).

| Folder | What's in it |
|--------|--------------|
| `common/` | `DaisyEngine.h` (the adapter), `Crusher.h` (Breaks' and SamplerFilter's BITS / RATE), `DSPUtils.h`, `BiquadEq.h`, `daisysp_armv7a.h` |
| `tools/` | `render.cpp` (offline renders), `armtest.sh`, `screenshot.sh`, `knob_gallery.py`, keygroup helpers |
| `DaisySP/` | [DaisySP](https://github.com/electro-smith/DaisySP) (MIT), submodule; Roads uses its chorus and SVF |
| `mpc-vst-plugins/` | the toolkit, submodule |

### Differences from the Daisy

| | Daisy | MPC OS plugin |
|---|---|---|
| Sample rate / block | 48 kHz | **44.1 kHz, 128 frames**, always |
| Controls | knobs, switches, encoder | parameters → on-screen knobs / buttons + Q-Links + automation; smooth anything zipper-prone |
| Memory | SRAM / SDRAM / QSPI | ordinary heap; allocate in `Init()`, **never** in `Process()` |
| Instances | one | several at once: all state in the class, no globals |
| CPU | 480 MHz M7, yours alone | 4× Cortex-A17 @ 1.8 GHz, shared with MPC: ~2.9 ms per block for everything |
| Crash | the board reboots | **MPC crashes**: test offline first |

**A plugin that uses DaisySP must start its `vst.json` cflags with `"-include", "common/daisysp_armv7a.h"`.** DaisySP's
`dsp.h` writes `fmax` / `fmin` as inline ARMv8 `vmaxnm` / `vminnm` assembly on any ARM target; the Cortex-A17 is ARMv7
and dies with SIGILL on them, while plain QEMU runs them. `build.sh arm` refuses a `.so` with ARMv8-only instructions and
`build.sh armtest` runs the host test on an emulated ARMv7 CPU; only these two catch it.

### Rules for changes

- **Never reorder or remove params** once a version is on a device: projects store values by index. Append, and rename
  a retired one "(unused)". `params.lock` (written by the toolkit) fails the build if a key moves: commit it.
- Keep each plugin's `uid` and `so` name fixed across versions.
- Versions: bump Z for fixes, Y for new parameters or pages, X only if parameter indices change.

## Licence

See [LICENSE](LICENSE). DaisySP is MIT. Roads' sample data and voice algorithm come from Paul Kellett's mda ePiano
(see [Roads](Roads)).
