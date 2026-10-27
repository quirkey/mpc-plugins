#pragma once
// ─── Crusher.h ────────────────────────────────────────────────────────────────
// The old-sampler grit shared by Breaks and SamplerFilter: a sample-and-hold
// decimator (RATE, kHz: the S950 / SP-1200 sample rate) then bit depth (BITS).
// RATE 44+ kHz and BITS 15.5+ are off. Stereo, linked: both channels are held
// on the same clock. Header-only; no allocation.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>

struct Crusher
{
    void SetRate(float khz, float sr) { inc_ = khz >= 44.0f ? 1.0f : khz * 1000.0f / sr; }
    void SetBits(float bits) { q_ = bits >= 15.5f ? 0.0f : exp2f(bits - 1.0f); }   // 0 = off

    void Process(float &l, float &r)
    {
        phase_ += inc_;
        if(phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
            holdL_ = l;
            holdR_ = r;
        }
        l = holdL_;
        r = holdR_;
        if(q_ > 0)
        {
            l = roundf(l * q_) / q_;
            r = roundf(r * q_) / q_;
        }
    }

  private:
    float inc_ = 1, phase_ = 0, q_ = 0, holdL_ = 0, holdR_ = 0;
};

// The BITS / RATE switches (params.json "bits_step" / "rate_step", appended after the old continuous "bits" /
// "rate" knobs, which projects saved before the switches still set): crunchiest first, Off last, like the knobs
// turned. Option lists are saved by index: append only.
namespace crushstep
{
static const float kBits[]    = {4, 6, 8, 10, 12, 16};
static const float kRateKhz[] = {4, 8, 11.025f, 16, 22.05f, 26.04f, 32, 44.1f};   // 26.04: the SP-1200
constexpr int      kBitsN = sizeof kBits / sizeof kBits[0], kRateN = sizeof kRateKhz / sizeof kRateKhz[0];

inline int Clamp(int i, int n) { return i < 0 ? 0 : i >= n ? n - 1 : i; }

// An old knob value -> the nearest switch position (rate by ratio: 6 kHz is nearer 8 than 4)
inline int NearestBits(float bits)
{
    if(bits >= 15.5f)
        return kBitsN - 1;
    int best = 0;
    for(int i = 1; i < kBitsN - 1; i++)
        if(fabsf(kBits[i] - bits) < fabsf(kBits[best] - bits))
            best = i;
    return best;
}
inline int NearestRate(float khz)
{
    if(khz >= 44.0f)
        return kRateN - 1;
    int best = 0;
    for(int i = 1; i < kRateN - 1; i++)
        if(fabsf(logf(kRateKhz[i] / khz)) < fabsf(logf(kRateKhz[best] / khz)))
            best = i;
    return best;
}
} // namespace crushstep
