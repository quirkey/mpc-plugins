#pragma once
// ─── daisysp_armv7a.h ─────────────────────────────────────────────────────────
// Force-included (vst.json cflags: "-include", "common/daisysp_armv7a.h")
// into every translation unit of a plugin that uses DaisySP.
//
// DaisySP's dsp.h implements fmax()/fmin() (and so fclamp()) with inline
// vmaxnm/vminnm assembly whenever __arm__ is defined. Those are ARMv8 / FPv5
// instructions: fine on the Daisy's Cortex-M7, but the MPC's Cortex-A17 is
// ARMv7-A, so the first call kills MPC with SIGILL (illegal instruction).
// QEMU emulates them, so offline ARM tests don't catch it.
//
// Including dsp.h here first, with __arm__ hidden, makes its include guard
// lock in DaisySP's portable C versions for the whole translation unit.
// The system headers dsp.h pulls in are included before the #undef, so they
// still see the real target.
// ─────────────────────────────────────────────────────────────────────────────

// vst.json cflags reach C sources too; DaisySP is C++ only, so do nothing there.
#ifdef __cplusplus
#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>

#pragma push_macro("__arm__")
#undef __arm__
#include "dsp.h"
#pragma pop_macro("__arm__")
#endif
