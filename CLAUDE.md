# CLAUDE.md

MPC OS standalone plugins (Linux VST2 on the MPC's ARM CPU): Breaks, SamplerFilter and Roads, built with the
`mpc-vst-plugins` toolkit (submodule). Moved here from `quirkey/DaisyProjects` `MPC/` in 2026-10; the Roads firmware
stays there. See README.md for the build commands and layout. Requires Docker.

- Every change: `./build.sh test <Plugin>` and `./build.sh armtest <Plugin>` must PASS, plus the plugin's own checks
  (Breaks: `tools/state_test.sh`, `regress_check.sh`, `seq_check.sh`, `variation_check.sh`, `stretch_check.sh`,
  `shape_check.sh`, `pads_check.sh`; SamplerFilter: `tools/filter_check.sh`; Roads: `tools/vib_check.sh`).
- Params are saved by index: append only, never reorder or remove (rename to "(unused)"). Commit `params.lock`.
- DaisySP users need `"-include", "common/daisysp_armv7a.h"` first in vst.json cflags (ARMv8 `vmaxnm` in `dsp.h` is a
  SIGILL on the Cortex-A17; QEMU hides it). Roads also builds with `-fno-jump-tables`.
- Roads' voices are a copy of the DaisyProjects Roads firmware: voice fixes go into both by hand.
- The device: `ssh mpc` (MPC One+, Gen1, MPC OS 3.9.1); plugins in `/media/MPCDATA/Synths/`; crash logs
  `journalctl -u acvs`. Installing restarts MPC: ask first. Batch: stop `acvs`, `install.sh -y -n -t
  /media/MPCDATA/Synths` per zip, start `acvs`.
- Breaks: never ship or commit break recordings (`breaks/` is the user's folder); `/media/MPCDATA/Breaks` on the device
  is read-only for us.
- Catalog: release zips carry `--repo/--license/--id` when `SPDX` is set in build.sh; skins, screenshots and names may
  not use another company's name or logo (mpc-vst-plugins `catalog/README.md`).
