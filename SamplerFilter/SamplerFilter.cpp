// SamplerFilter — Akai S950 style filter and sampler grit for MPC OS, as a track effect
//
// The S950's filter is reported to be a National MF6-50: a 6th-order Butterworth switched-capacitor low-pass,
// 36 dB/octave, no resonance, its cutoff set by a clock at about 50x the cutoff. Pulled down it darkens and thickens
// rather than squelching. Its "bandwidth" setting is the same kind of filter in front of the converter, with the
// sample rate at 2.5x the bandwidth: low rates sound muffled rather than aliased.
//
// The chain, per channel:
//   DRIVE   an input preamp pushed into a soft clip (the S950's grit came partly from driving its input)
//   TRACK   on: the bandwidth filter, the same 6-pole low-pass at 0.4 x RATE, before the sampling
//   RATE    sample-and-hold decimation, then BITS: Breaks' grit (common/Crusher.h), identical
//   FILTER  the 6-pole low-pass (100 = off)
//   MIX, LEVEL
// The 6-pole is three state-variable (TPT) sections at the Butterworth Qs, which stay well-behaved at low cutoffs
// and while the cutoff moves. The MF6's own clock artifacts (the clock falls in the audio band below ~880 Hz) are
// not modelled.
//
// Params (params.json order — never reorder, MPC projects save by index):
//   0 Level    dB (param 0 must be a plain knob: the wrapper defers it around MPC's STOP toggle)
//   1 Filter   0..100 -> 20 Hz..20 kHz (exponential; 100 = off)
//   2 Bits     the old BITS knob, 16..4 (16 = off): projects saved with it set the switch (7) to the nearest step
//   3 Rate     the old RATE knob, kHz (44.1 = off): sets the switch (8) likewise
//   4 Track    Off / On: the bandwidth filter before the sampling, at 0.4 x Rate
//   5 Drive    input preamp drive
//   6 Mix      dry/wet
//   7 Bits     switch: 4 / 6 / 8 / 10 / 12 / Off (common/Crusher.h crushstep)
//   8 Rate     switch: 4k / 8k / 11k / 16k / 22k / 26k (SP-1200) / 32k / Off

#include <atomic>
#include <cmath>
#include "../common/DSPUtils.h"
#include "../common/Crusher.h"
#include "../common/DaisyEngine.h"

enum Param
{
    P_LEVEL,
    P_FILTER,
    P_BITS,
    P_RATE,
    P_TRACK,
    P_DRIVE,
    P_MIX,
    P_BITS_STEP,
    P_RATE_STEP,
};

static constexpr int   kCtlRate  = 8;         // cutoffs recomputed every 8 samples
static constexpr float kMinHz    = 20.0f;
static constexpr float kOctRange = 9.9658f;   // log2(20000 / 20)

// One 2-pole low-pass section, Zavalishin / Cytomic TPT state-variable form.
struct Svf
{
    float ic1 = 0, ic2 = 0;

    float Process(float v0, float a1, float a2, float a3)
    {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1      = 2.0f * v1 - ic1;
        ic2      = 2.0f * v2 - ic2;
        return v2;
    }
};

// A 6th-order Butterworth low-pass (the MF6's response): three sections at Q 0.518, 0.707, 1.932.
struct SixPole
{
    static constexpr float kK[3] = {1.93185f, 1.41421f, 0.51764f};   // 1 / Q
    Svf                    s[3];
    float                  a1[3] = {1, 1, 1}, a2[3] = {0, 0, 0}, a3[3] = {0, 0, 0};

    void SetCutoff(float hz, float sr)
    {
        float g = tanf(3.14159265f * fminf(hz, 0.45f * sr) / sr);
        for(int i = 0; i < 3; i++)
        {
            a1[i] = 1.0f / (1.0f + g * (g + kK[i]));
            a2[i] = g * a1[i];
            a3[i] = g * a2[i];
        }
    }

    float Process(float x)
    {
        for(int i = 0; i < 3; i++)
            x = s[i].Process(x, a1[i], a2[i], a3[i]);
        return x;
    }
};
constexpr float SixPole::kK[3];

class SamplerFilterDsp : public DaisyDsp
{
  public:
    void Init(float sr)
    {
        sr_     = sr;
        smooth_ = 1.0f - expf(-(float)kCtlRate / (0.020f * sr));   // ~20 ms glide on the cutoffs
        fade_   = 1.0f / (0.005f * sr);                             // 5 ms in and out of a filter
        crush_.SetRate(rateKhz_, sr);
        UpdateCutoffs(true);
    }

    void SetParam(int i, float v)
    {
        switch(i)
        {
            case P_LEVEL: level_ = powf(10.0f, v / 20.0f); break;
            case P_FILTER: filter_ = v; break;
            case P_BITS:   // an old project's knob: the nearest switch position, told to the host (TakeMovedParam)
                if(crushstep::NearestBits(v) != bitsStep_.load())   // only a move: every new instance sets it too
                {
                    SetBitsStep(crushstep::NearestBits(v));
                    bitsMoved_.store(true);
                }
                break;
            case P_RATE:
                if(crushstep::NearestRate(v) != rateStep_.load())
                {
                    SetRateStep(crushstep::NearestRate(v));
                    rateMoved_.store(true);
                }
                break;
            case P_BITS_STEP: SetBitsStep((int)v); break;
            case P_RATE_STEP: SetRateStep((int)v); break;
            case P_TRACK: track_ = v > 0.5f; break;
            case P_DRIVE:
                drive_ = v * 0.01f;
                driveGain_ = powf(10.0f, drive_ * 24.0f / 20.0f);   // up to +24 dB into the clip
                driveTrim_ = 1.0f / sqrtf(driveGain_);
                break;
            case P_MIX: mix_ = v * 0.01f; break;
        }
    }

    int Display(int i, char *buf, int len)
    {
        switch(i)
        {
            case P_FILTER:
            {
                if(filter_ >= 99.95f)
                    return snprintf(buf, len, "Off");
                float hz = FilterHz();
                return hz < 1000.0f ? snprintf(buf, len, "%.0f Hz", hz) : snprintf(buf, len, "%.1f kHz", hz * 0.001f);
            }
            case P_BITS: return bits_ >= 15.5f ? snprintf(buf, len, "Off") : snprintf(buf, len, "%.0f-bit", bits_);
            case P_RATE: return rateKhz_ >= 44.0f ? snprintf(buf, len, "Off") : snprintf(buf, len, "%.1f kHz", rateKhz_);
        }
        return 0;
    }

    void Process(const float *inL, const float *inR, float *outL, float *outR, int frames)
    {
        for(int i = 0; i < frames; i++)
        {
            if(ctl_++ % kCtlRate == 0)
                UpdateCutoffs(false);
            float dl = inL[i], dr = inR[i], l = dl, r = dr;
            if(drive_ > 0)
            {
                l = tanhf(l * driveGain_) * driveTrim_;
                r = tanhf(r * driveGain_) * driveTrim_;
            }
            // the bandwidth filter, faded in and out (and run warm) so TRACK and RATE changes don't click
            float aaTarget = track_ && rateKhz_ < 44.0f ? 1.0f : 0.0f;
            aaWet_ += fminf(fmaxf(aaTarget - aaWet_, -fade_), fade_);
            if(aaWet_ > 0)
            {
                float fl = aa_[0].Process(l), fr = aa_[1].Process(r);
                l += aaWet_ * (fl - l);
                r += aaWet_ * (fr - r);
            }
            crush_.Process(l, r);
            float fTarget = filter_ < 99.95f ? 1.0f : 0.0f;
            fWet_ += fminf(fmaxf(fTarget - fWet_, -fade_), fade_);
            if(fWet_ > 0)
            {
                float fl = lp_[0].Process(l), fr = lp_[1].Process(r);
                l += fWet_ * (fl - l);
                r += fWet_ * (fr - r);
            }
            outL[i] = softLimit((dl + mix_ * (l - dl)) * level_);
            outR[i] = softLimit((dr + mix_ * (r - dr)) * level_);
        }
    }

    // An old knob moved a switch: its position now, so a later set of the switch itself isn't undone.
    bool TakeMovedParam(int &index, float &value)
    {
        if(bitsMoved_.exchange(false))
        {
            index = P_BITS_STEP;
            value = (float)bitsStep_.load();
            return true;
        }
        if(rateMoved_.exchange(false))
        {
            index = P_RATE_STEP;
            value = (float)rateStep_.load();
            return true;
        }
        return false;
    }

  private:
    void SetBitsStep(int i)
    {
        i = crushstep::Clamp(i, crushstep::kBitsN);
        bitsStep_.store(i);
        bits_ = crushstep::kBits[i];
        crush_.SetBits(bits_);
    }
    void SetRateStep(int i)
    {
        i = crushstep::Clamp(i, crushstep::kRateN);
        rateStep_.store(i);
        rateKhz_ = crushstep::kRateKhz[i];
        crush_.SetRate(rateKhz_, sr_);
    }

    float FilterHz() const { return kMinHz * exp2f(filter_ * 0.01f * kOctRange); }

    // The two cutoffs glide (in octaves) toward FILTER and 0.4 x RATE.
    void UpdateCutoffs(bool jump)
    {
        float lpOct = log2f(fminf(FilterHz(), 20000.0f));
        float aaOct = log2f(fminf(rateKhz_ * 400.0f, 20000.0f));
        lpOct_ += jump ? lpOct - lpOct_ : smooth_ * (lpOct - lpOct_);
        aaOct_ += jump ? aaOct - aaOct_ : smooth_ * (aaOct - aaOct_);
        for(int c = 0; c < 2; c++)
        {
            lp_[c].SetCutoff(exp2f(lpOct_), sr_);
            aa_[c].SetCutoff(exp2f(aaOct_), sr_);
        }
    }

    SixPole  lp_[2], aa_[2];
    Crusher  crush_;
    float    sr_ = kMpcSampleRate, smooth_ = 1, fade_ = 1;
    float    level_ = 1, filter_ = 100, bits_ = 16, rateKhz_ = 44.1f, drive_ = 0, driveGain_ = 1, driveTrim_ = 1;
    float    mix_ = 1, lpOct_ = 14.29f, aaOct_ = 14.29f, aaWet_ = 0, fWet_ = 0;
    bool     track_ = false;
    unsigned ctl_   = 0;
    std::atomic<int>  bitsStep_{crushstep::kBitsN - 1}, rateStep_{crushstep::kRateN - 1};   // the switches
    std::atomic<bool> bitsMoved_{false}, rateMoved_{false};   // set by an old knob, to report
};

MPC_DAISY_ENGINE(SamplerFilterDsp)
