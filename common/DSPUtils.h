#pragma once
// ─── DSPUtils.h ───────────────────────────────────────────────────────────────
// Portable DSP utility functions shared across Daisy firmware projects.
// No dependencies on libDaisy, DaisySP, or STM32 headers — include anywhere.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>

// ── Padé approximant tanh ─────────────────────────────────────────────────────
// 3rd/3rd rational approximation — faster than libm tanhf; good accuracy for
// |x| < 3.  Useful for oversampled filter cores and waveshaping inside the
// audio callback where libm cost is unwanted.
inline float fastTanh(float x)
{
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// ── Wavefolder ────────────────────────────────────────────────────────────────
// Folds signal back on itself at ±1 boundaries using sin() for smooth folds
// with no derivative discontinuities.  amount 0→1 maps input drive 1×→8×.
// Output is normalized by drive so level stays consistent as amount increases;
// the inherent sin() warmth (~π/2 small-signal gain) is intentional character.
inline float wavefold(float x, float amount)
{
    float drive = 1.0f + amount * 7.0f;
    return sinf(x * drive * 1.5707963f) / drive;
}

// ── Tube saturator ────────────────────────────────────────────────────────────
// Asymmetric soft-clip saturation with unity-gain normalisation.
// The DC bias gives even-order (2nd) harmonic character, like a triode tube.
//
// Usage — call Setup() once per block, Process() per sample:
//   TubeSaturator sat;
//   sat.Setup(drive);    // drive in [0, 1]
//   out = sat.Process(in);
struct TubeSaturator
{
    float preGain  = 1.0f;
    float bias     = 0.0f;
    float biasClip = 0.0f;
    float normInv  = 1.0f;

    // drive: 0 = clean pass-through, 1 = maximum saturation
    void Setup(float drive)
    {
        preGain  = 1.0f + drive * 2.0f;    // 1→3× pre-gain
        bias     = drive * 0.1f;             // asymmetric DC bias
        biasClip = clip(bias);
        normInv  = (drive > 0.001f)
            ? 1.0f / (clip(preGain + bias) - biasClip)
            : 1.0f;
    }

    inline float Process(float x) const
    {
        return (clip(x * preGain + bias) - biasClip) * normInv;
    }

private:
    // Cubic soft-clip (mirrors daisysp::SoftClip / SoftLimit — kept private
    // here to avoid collisions when daisysp is also in scope via using-directive).
    static inline float clip(float x)
    {
        if (x < -3.0f) return -1.0f;
        if (x >  3.0f) return  1.0f;
        return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }
};

// ── Soft pre-amp limiter ──────────────────────────────────────────────────────
// Transparent (unity gain) for |x| ≤ knee.  Above the knee, uses the same
// cubic soft-clip formula as TubeSaturator to smoothly approach ±1.0 over the
// next `span` of input range, then hard-clips above that.
// Use at gain-staging boundaries (e.g. pre-cab) to prevent FIR state overflow.
inline float softLimit(float x, float knee = 0.9f, float span = 1.0f)
{
    if (x > knee) {
        float t = (x - knee) * (3.0f / span);
        if (t >= 3.0f) return 1.0f;
        return knee + (1.0f - knee) * t * (27.0f + t * t) / (27.0f + 9.0f * t * t);
    }
    if (x < -knee) {
        float t = (-x - knee) * (3.0f / span);
        if (t >= 3.0f) return -1.0f;
        return -knee - (1.0f - knee) * t * (27.0f + t * t) / (27.0f + 9.0f * t * t);
    }
    return x;
}

// ── 2× oversampled tube saturator ────────────────────────────────────────────
// Runs TubeSaturator at 2× by linearly interpolating a midpoint sample, then
// averaging the two outputs (2-tap FIR decimation). This significantly reduces
// aliasing from the nonlinearity — harmonics that would fold back into the
// audio band are attenuated before downsampling. Rejection is gentle (~-3dB
// at 24kHz); sufficient for most material, especially mid-range fundamentals.
//
// xPrev carries state across audio blocks, so declare instances static or at
// file scope — NOT as block-local variables.
//
// Stereo: use separate L/R instances so xPrev doesn't cross channels.
struct TubeSaturatorOS2
{
    TubeSaturator sat;
    float xPrev = 0.0f;

    void Setup(float drive) { sat.Setup(drive); }

    inline float Process(float x)
    {
        float mid = 0.5f * (xPrev + x);
        xPrev = x;
        return 0.5f * (sat.Process(mid) + sat.Process(x));
    }
};
