// Roads — MDA ePiano instrument for MPC OS
//
// Ported from the Daisy Roads firmware (Roads/Roads.cpp): the same 33-keygroup ePiano sample voices with the tine /
// reed oscillator, Rhodes / Wurli / Clav characters, treble boost, pan / tremolo LFO, the note-triggered envelope
// filter, chorus and the 3-band EQ. Left out on purpose: the tape delay, reverb and expression-pedal wah (MPC has its
// own delay and reverb, and nothing to plug a pedal into), and the cabinet IR (1.1 had one; a cab / amp plugin on the
// track does it better).
//
// The firmware is a copy, not shared code: fixes to the voices go into both by hand.
//
// Samples: epiano_samples.bin (Roads/tools/prepare_samples.py) in the plugin folder's samples/, read once per instance.
//
// Params (params.json order — never reorder, MPC projects save by index):
//   0 Level      output level (dB)
//   1 Character  Rhodes / Wurli / Clav
//   2 Decay      3 Release   4 Hardness (keygroup offset)   5 Treble   6 Drive (the firmware's OD)
//   7 Speed      LFO rate (0.3 .. 8 Hz)   8 Pan / Trem (old: -100 pan .. +100 tremolo; the firmware's DEPTH around 0.5)
//   9 Chorus
//  10 Sense      envelope filter amount (0 = off)   11 Peak   12 Freq (resting)   13 Range   14 Type LP / BP / HP
//  15 / 16       unused (the cab and its mix)   17-19 Low / Mid / High (dB, always on, as the firmware)
//  20 - 22       unused (the cab's PREV / NEXT and name)
//  23 Vibrato    Pan / Trem   24 Depth (0..100 %; these two replace 8, which now only loads old projects)
//  25            unused (the cab IR)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "dsp.h"
#include "chorus.h"
#include "svf.h"
#include "../common/DSPUtils.h"
#include "../common/BiquadEq.h"
#include "../common/DaisyEngine.h"

using namespace daisysp;

enum Param
{
    P_LEVEL,
    P_CHARACTER,
    P_DECAY,
    P_RELEASE,
    P_HARDNESS,
    P_TREBLE,
    P_DRIVE,
    P_RATE,
    P_MOD,
    P_CHORUS,
    P_SENSE,
    P_PEAK,
    P_FREQ,
    P_RANGE,
    P_FTYPE,
    P_UNUSED_CAB,
    P_UNUSED_CABMIX,
    P_LOW,
    P_MID,
    P_HIGH,
    P_UNUSED_CAB_PREV,
    P_UNUSED_CAB_NEXT,
    P_UNUSED_CAB_NAME,
    P_VIB_MODE,
    P_DEPTH,
    P_UNUSED_CAB_IR,
};

static constexpr int   kVoices    = 16;       // the firmware's NVOICES
static constexpr int   kSustained = 128;      // note value of a voice held by the sustain pedal
static constexpr float kSilence   = 0.0001f;  // envelope below this frees the voice
static constexpr int   kWaveLen   = 422414;   // samples in epiano_samples.bin
static constexpr float kVolume    = 0.07f;    // -9 dB from the firmware (its volume pot at noon), where a 4-note
                                              // velocity-100 chord ran into the limiter
static constexpr float kBendRange = 2.0f;     // semitones
static constexpr float kVibrato   = 0.02f;    // pitch depth at full mod wheel
static constexpr float kWahAmpGain = 2.5f;    // summed-voice amplitude to roughly 0..1

// 33 groups, 3 per root (low / mid / high velocity). pos / end / loop index the wave data.
struct Kgrp
{
    int root, high, pos, end, loop;
};

static const Kgrp kKgrp[33] = {
    {36, 39, 0, 8476, 4400},          {36, 39, 8477, 16248, 4903},      {36, 39, 16249, 34565, 6398},
    {43, 45, 34566, 41384, 3938},     {43, 45, 41385, 45760, 1633},     {43, 45, 45761, 65211, 5245},
    {48, 51, 65212, 72897, 2937},     {48, 51, 72898, 78626, 2203},     {48, 51, 78627, 100387, 6368},
    {55, 57, 100388, 116297, 10452},  {55, 57, 116298, 127661, 5217},   {55, 57, 127662, 144113, 3099},
    {60, 63, 144114, 152863, 4284},   {60, 63, 152864, 173107, 3916},   {60, 63, 173108, 192734, 2937},
    {67, 69, 192735, 204598, 4732},   {67, 69, 204599, 218995, 4733},   {67, 69, 218996, 233801, 2285},
    {72, 75, 233802, 248011, 4098},   {72, 75, 248012, 265287, 4099},   {72, 75, 265288, 282255, 3609},
    {79, 81, 282256, 293776, 2446},   {79, 81, 293777, 312566, 6278},   {79, 81, 312567, 330200, 2283},
    {84, 87, 330201, 348889, 2689},   {84, 87, 348890, 365675, 4370},   {84, 87, 365676, 383661, 5225},
    {91, 93, 383662, 393372, 2811},   {91, 93, 383662, 393372, 2811},   {91, 93, 393373, 406045, 4522},
    {96, 999, 406046, 414486, 2306},  {96, 999, 406046, 414486, 2306},  {96, 999, 414487, 422408, 2169},
};

struct Voice
{
    int32_t delta;     // 16.16 playback speed
    int32_t frac;      // fractional position (low 16 bits)
    int32_t pos;       // sample index
    int32_t end;       // loop end
    int32_t loop;      // loop length
    float   env;       // amplitude envelope
    float   dec;       // per-sample envelope multiplier
    float   outl, outr;
    int32_t note;      // MIDI note, or kSustained
    int32_t key;       // MIDI note, kept while sustained (the firmware released sustained notes as note 64)
    float   sinP, cosP, tineW, tineEnv, tineDec;   // tine / reed / pick oscillator
};

class RoadsDsp : public DaisyDsp
{
  public:
    ~RoadsDsp() { free(waves_); }

    void SetDataDir(const char *dir) { snprintf(dataDir_, sizeof dataDir_, "%s", dir ? dir : ""); }

    void Init(float sr)
    {
        ifs_ = 1.0f / sr;
        LoadWaves();

        wah_.Init(sr);
        chorus_.Init(sr);
        // The firmware's chorus: fixed settings, the knob sets the wet mix only
        chorus_.SetLfoFreq(0.5f, 0.57f);
        chorus_.SetDelayMs(15.0f, 23.0f);
        chorus_.SetLfoDepth(0.5f);
        chorus_.SetFeedback(0.1f);
        chorus_.SetPan(0.0f, 1.0f);

        wahRelFast_  = expf(-ifs_ / 0.050f);
        wahRelSlow_  = expf(-ifs_ / 0.500f);
        wahFollowCo_ = 1.0f - expf(-ifs_ / 0.010f);
        memset(voices_, 0, sizeof voices_);
    }

    void SetParam(int i, float v)
    {
        switch(i)
        {
            case P_LEVEL: level_ = powf(10.0f, v / 20.0f); break;
            case P_CHARACTER:
                character_ = (int)v;
                reeds_     = character_ == 1 ? 1.0f : 0.0f;
                clav_      = character_ == 2 ? 1.0f : 0.0f;
                break;
            case P_DECAY: decay_ = v * 0.01f; break;
            case P_RELEASE: rel_ = v * 0.01f; break;
            case P_HARDNESS: size_ = (int)(12.0f * v * 0.01f - 6.0f); break;
            case P_TREBLE:
            {
                float t = v * 0.01f;
                treb_   = 4.0f * t * t - 1.0f;
                tfrq_   = 1.0f - expf(-ifs_ * (t > 0.5f ? 14000.0f : 5000.0f));
                break;
            }
            case P_DRIVE: od_ = 1.8f * v * 0.01f; break;
            case P_RATE: dlfo_ = 6.283f * ifs_ * RateHz(v); break;
            case P_MOD:
            {
                // an old project's bipolar knob (-100 pan .. +100 tremolo): the switch and DEPTH from it, told to the
                // host (TakeMovedParam). Only a change counts, so a new project's later switch / DEPTH values win.
                int   mode  = v < 0.0f ? 0 : v > 0.0f ? 1 : vibMode_;
                float depth = fabsf(v) * 0.01f;
                if(mode != vibMode_)
                {
                    vibMode_ = mode;
                    modeMoved_.store(true);
                }
                if(fabsf(depth - depth_) > 1e-4f)
                {
                    depth_ = depth;
                    depthMoved_.store(true);
                }
                UpdateVibrato();
                break;
            }
            case P_VIB_MODE:
                vibMode_ = (int)v;
                UpdateVibrato();
                break;
            case P_DEPTH:
                depth_ = v * 0.01f;
                UpdateVibrato();
                break;
            case P_CHORUS: chorusMix_ = v * 0.01f; break;
            case P_SENSE: wahSens_ = v * 0.01f; break;
            case P_PEAK: wah_.SetRes(0.5f + v * 0.01f * 0.45f); break;
            case P_FREQ: wahFreq_ = v * 0.01f; break;
            case P_RANGE: wahRange_ = v * 0.01f; break;
            case P_FTYPE: wahType_ = (int)v; break;
            // the cab's params (15, 16, 20-22, 25) are unused: an old project's values are ignored
            case P_LOW:
                eqLowL_.SetLowShelf(200.0f, v, 1.0f / ifs_);
                eqLowR_.SetLowShelf(200.0f, v, 1.0f / ifs_);
                break;
            case P_MID:
                eqMidL_.SetPeak(1200.0f, 1.0f, v, 1.0f / ifs_);
                eqMidR_.SetPeak(1200.0f, 1.0f, v, 1.0f / ifs_);
                break;
            case P_HIGH:
                eqHighL_.SetHighShelf(5000.0f, v, 1.0f / ifs_);
                eqHighR_.SetHighShelf(5000.0f, v, 1.0f / ifs_);
                break;
        }
    }

    int Display(int i, char *buf, int len)
    {
        switch(i)
        {
            case P_RATE:
            {
                // the value the host passes isn't kept here; recover it from the increment
                float hz = dlfo_ / (6.283f * ifs_);
                return snprintf(buf, len, hz < 10.0f ? "%.2f Hz" : "%.1f Hz", hz);
            }
            case P_MOD:
                if(fabsf(lmod_) < 0.005f)
                    return snprintf(buf, len, "Off");
                return snprintf(buf, len, "%s %d", lmod_ < 0.0f ? "Pan" : "Trem", (int)lroundf(fabsf(lmod_) * 100.0f));
            case P_SENSE:
                if(wahSens_ <= 0.01f)
                    return snprintf(buf, len, "Off");
                return snprintf(buf, len, "%d", (int)lroundf(wahSens_ * 100.0f));
            case P_FREQ: return snprintf(buf, len, "%d Hz", (int)lroundf(50.0f + wahFreq_ * 950.0f));
        }
        return 0;
    }

    bool TakeMovedParam(int &index, float &value)
    {
        // the old knob moved the switch / DEPTH: their values now
        if(modeMoved_.exchange(false))
        {
            index = P_VIB_MODE;
            value = (float)vibMode_;
            return true;
        }
        if(depthMoved_.exchange(false))
        {
            index = P_DEPTH;
            value = depth_ * 100.0f;
            return true;
        }
        return false;
    }

    void Midi(const uint8_t *m, int len)
    {
        if(len < 2)
            return;
        uint8_t st = m[0] & 0xF0;
        if(st == 0x90 && len >= 3 && m[2] > 0)
            NoteOn(m[1], m[2]);
        else if(st == 0x80 || (st == 0x90 && len >= 3))
            NoteOff(m[1]);
        else if(st == 0xB0 && len >= 3)
        {
            if(m[1] == 64)
                Sustain(m[2] >= 64);
            else if(m[1] == 1)
                modWheel_ = m[2] / 127.0f;
            else if(m[1] == 120 || m[1] == 123)
                AllOff(m[1] == 120);
        }
        else if(st == 0xE0 && len >= 3)
        {
            float semis = ((m[2] << 7 | m[1]) - 8192) / 8192.0f * kBendRange;
            bendRatio_  = expf(semis * 0.05776226505f);
        }
    }

    void Process(const float *, const float *, float *outL, float *outR, int frames)
    {
        float tl = tl_, tr = tr_, lfo0 = lfo0_, lfo1 = lfo1_;
        float od = od_, treb = treb_, tfrq = tfrq_, lmod = lmod_, rmod = rmod_, dlfo = dlfo_;

        for(int s = 0; s < frames; s++)
        {
            float pitchRatio = bendRatio_ * (1.0f + modWheel_ * kVibrato * lfo1);
            float l = 0.0f, r = 0.0f;

            for(int v = 0; v < active_; v++)
            {
                Voice *V = &voices_[v];
                V->frac += (int32_t)((float)V->delta * pitchRatio);
                V->pos += V->frac >> 16;
                V->frac &= 0xFFFF;
                if(V->pos > V->end)
                    V->pos -= V->loop;

                int32_t s0     = waves_[V->pos];
                int32_t s1     = waves_[V->pos + 1];
                int32_t interp = s0 + ((V->frac * (s1 - s0)) >> 16);
                float   x      = V->env * (float)interp / 32768.0f;
                V->env *= V->dec;

                // "magic circle" quadrature oscillator: bounded without normalising
                float tW   = V->tineW * pitchRatio;
                float tSin = V->sinP + tW * V->cosP;
                float tCos = V->cosP - tW * tSin;
                V->sinP    = tSin;
                V->cosP    = tCos;
                x += V->tineEnv * (tSin + 0.5f * reeds_ * 2.0f * tSin * tCos);   // Wurli adds the 2nd harmonic
                V->tineEnv *= V->tineDec;

                // Overdrive per character: Rhodes asymmetric (MDA), Wurli soft clip, Clav tanh. Unlike the firmware,
                // Wurli and Clav get makeup gain (the firmware's dropped 6 and 14 dB at DRIVE 60)
                // (only the selected one is computed: the firmware blended all three, a tanhf per voice per sample)
                if(od > 0.0f)
                {
                    if(character_ == 1)
                        x = x * (1.0f + od) / (1.0f + od * fabsf(x));
                    else if(character_ == 2)
                        x = tanhf(x * (1.0f + 3.0f * od));
                    else if(x > 0.0f)
                    {
                        x -= od * x * x;
                        if(x < -V->env)
                            x = -V->env;
                    }
                }

                l += V->outl * x;
                r += V->outr * x;
            }

            // Treble boost: first-order highpass complement
            tl += tfrq * (l - tl);
            tr += tfrq * (r - tr);
            l += treb * (l - tl);
            r += treb * (r - tr);

            // Quadrature LFO: tremolo / autopan
            lfo0 += dlfo * lfo1;
            lfo1 -= dlfo * lfo0;
            l += l * lmod * lfo1;
            r += r * rmod * lfo1;

            // Envelope filter (Mutron style): opened by note-on, closes faster as the notes get quieter
            if(wahSens_ > 0.01f)
            {
                float mono = (l + r) * 0.5f;
                wahFollow_ += wahFollowCo_ * (fabsf(mono) - wahFollow_);
                float ampNorm = fminf(wahFollow_ * kWahAmpGain, 1.0f);
                wahEnv_ *= wahRelFast_ + ampNorm * (wahRelSlow_ - wahRelFast_);
                // every 4th sample: Svf::SetFreq is a sinf and a powf, and the envelope moves slowly
                if((s & 3) == 0)
                {
                    float cutoff = (50.0f + wahFreq_ * 950.0f) * (1.0f + wahEnv_ * wahRange_ * 15.0f);
                    wah_.SetFreq(fminf(cutoff, 8000.0f));
                }
                wah_.Process(mono);
                float w = wahType_ == 0 ? wah_.Low() : wahType_ == 2 ? wah_.High() : wah_.Band();
                l = r = w;
            }

            // Chorus: GetLeft / Right are 0.25x the input; squared mix, as the firmware
            if(chorusMix_ > 0.001f)
            {
                chorus_.Process(l);
                float m = chorusMix_ * chorusMix_;
                l       = l * (1.0f - m) + chorus_.GetLeft() * 4.0f * m;
                r       = r * (1.0f - m) + chorus_.GetRight() * 4.0f * m;
            }

            // Tone EQ, always on (as the firmware), then the output level below
            preL_[s] = eqHighL_.Process(eqMidL_.Process(eqLowL_.Process(softLimit(l))));
            preR_[s] = eqHighR_.Process(eqMidR_.Process(eqLowR_.Process(softLimit(r))));
        }

        tl_   = fabsf(tl) < 1e-10f ? 0.0f : tl;
        tr_   = fabsf(tr) < 1e-10f ? 0.0f : tr;
        lfo0_ = lfo0;
        lfo1_ = lfo1;

        // Free silent voices: swap them to the end of the active list
        for(int v = 0; v < active_;)
            if(voices_[v].env < kSilence)
                voices_[v] = voices_[--active_];
            else
                v++;

        for(int s = 0; s < frames; s++)
        {
            gain_ += 0.002f * (level_ - gain_);
            outL[s] = softLimit(preL_[s] * gain_);
            outR[s] = softLimit(preR_[s] * gain_);
        }
    }

  private:
    // SPEED 0.3 .. 8 Hz, exponential (the firmware's 0.07 .. 37 Hz went too slow and too fast; noon is ~1.5 Hz either way)
    static float RateHz(float v) { return 0.3f * powf(8.0f / 0.3f, v * 0.01f); }

    // the firmware's LFO depth: both sides together for tremolo, the right one inverted for autopan
    void UpdateVibrato()
    {
        lmod_ = vibMode_ == 0 ? -depth_ : depth_;
        rmod_ = depth_;
    }

    void LoadWaves()
    {
        // zeroed first, so a missing file plays the tines only and never reads garbage
        waves_ = (short *)calloc(kWaveLen + 1, sizeof(short));   // +1 guard sample for the interpolation
        if(!waves_)
            return;
        char        path[600];
        const char *env = getenv("ROADS_SAMPLES");
        const char *tries[3];
        snprintf(path, sizeof path, "%s/epiano_samples.bin", dataDir_);
        tries[0] = dataDir_[0] ? path : nullptr;
        tries[1] = env;
        tries[2] = "Roads/epiano_samples.bin";   // offline tests run from the repo root
        for(const char *t : tries)
        {
            FILE *f = t ? fopen(t, "rb") : nullptr;
            if(!f)
                continue;
            size_t n = fread(waves_, sizeof(short), kWaveLen, f);
            fclose(f);
            if(n == (size_t)kWaveLen)
            {
                loaded_ = true;
                break;
            }
            memset(waves_, 0, (kWaveLen + 1) * sizeof(short));
        }
        if(!loaded_)
            fprintf(stderr, "Roads: epiano_samples.bin not found in %s\n", dataDir_);
    }

    void NoteOn(int note, int velocity)
    {
        if(!waves_)
            return;
        const float velsens = 1.5f;
        const float width   = 0.015f;

        // Envelope filter trigger
        float trig = fminf(1.0f, velocity / 127.0f * (0.5f + wahSens_ * 1.5f));
        if(trig > wahEnv_)
            wahEnv_ = trig;

        int vl;
        if(active_ < kVoices)
            vl = active_++;
        else
        {
            vl = 0;   // steal the quietest
            for(int v = 1; v < kVoices; v++)
                if(voices_[v].env < voices_[vl].env)
                    vl = v;
        }
        Voice &V = voices_[vl];

        int k = 0;
        while(note > kKgrp[k].high + size_)
            k += 3;
        float l = 32000.0f * ifs_ * expf(0.05776226505f * (float)(note - kKgrp[k].root));
        V.delta = (int32_t)(65536.0f * l);
        V.frac  = 0;
        if(velocity > 48)
            k++;
        if(velocity > 80)
            k++;
        V.pos  = kKgrp[k].pos;
        V.end  = kKgrp[k].end - 1;
        V.loop = kKgrp[k].loop;

        V.env = (3.0f + 2.0f * velsens) * powf(0.0078f * (float)velocity, velsens);
        if(note > 60)
            V.env *= expf(0.01f * (float)(60 - note));

        // Clav plucks die out much faster than the piano body
        int   dn   = note < 44 ? 44 : note;
        float bias = character_ == 2 ? 3.0f : 0.0f;
        V.dec      = expf(-ifs_ * expf(-1.0f + bias + 0.03f * (float)dn - 2.0f * decay_));

        int pn = note < 12 ? 12 : (note > 108 ? 108 : note);
        V.outr = kVolume + kVolume * width * (float)(pn - 60);
        V.outl = 2.0f * kVolume - V.outr;
        V.note = note;
        V.key  = note;

        // Tine: a sine at the note, ~80 ms, with the voice's own velocity
        float hz  = 440.0f * expf(0.05776226505f * (float)(note - 69));
        V.tineW   = 2.0f * 3.14159265f * hz * ifs_;
        V.sinP    = 0.0f;
        V.cosP    = 1.0f;
        V.tineEnv = 0.5f * V.env * velocity / 127.0f;
        V.tineDec = expf(-ifs_ * expf(3.5f - 2.0f * decay_));
        if(character_ == 2)
        {
            // Clav: a short pick "clack" at 3x the fundamental instead (kept low enough for the oscillator)
            float pick = fminf(hz * 3.0f, 9000.0f);
            V.tineW    = 2.0f * 3.14159265f * pick * ifs_;
            V.tineEnv  = 0.35f * V.env * velocity / 127.0f;
            V.tineDec  = expf(-ifs_ * expf(5.5f));
        }
    }

    void Release(Voice &V) { V.dec = expf(-ifs_ * expf(6.0f + 0.01f * (float)V.key - 5.0f * rel_)); }

    void NoteOff(int note)
    {
        for(int v = 0; v < active_; v++)
            if(voices_[v].note == note)
            {
                if(sustain_)
                    voices_[v].note = kSustained;
                else
                    Release(voices_[v]);
            }
    }

    void Sustain(bool on)
    {
        sustain_ = on;
        if(!on)
            for(int v = 0; v < active_; v++)
                if(voices_[v].note == kSustained)
                {
                    voices_[v].note = -1;
                    Release(voices_[v]);
                }
    }

    // CC 123 releases every note; CC 120 (all sound off) silences them
    void AllOff(bool now)
    {
        sustain_ = false;
        if(now)
            active_ = 0;
        else
            for(int v = 0; v < active_; v++)
            {
                voices_[v].note = -1;
                Release(voices_[v]);
            }
    }

    short *waves_  = nullptr;
    bool   loaded_ = false;
    char   dataDir_[512] = "";
    float  ifs_          = 1.0f / kMpcSampleRate;

    Voice voices_[kVoices];
    int   active_  = 0;
    bool  sustain_ = false;

    int   character_ = 0;
    float reeds_ = 0.0f, clav_ = 0.0f;
    float decay_ = 0.5f, rel_ = 0.5f;
    int   size_  = 0;
    float treb_ = 0.0f, tfrq_ = 0.0f, od_ = 0.0f;
    float lmod_ = 0.0f, rmod_ = 0.0f, dlfo_ = 0.0f;
    int   vibMode_ = 1;      // 0 pan, 1 tremolo
    float depth_   = 0.0f;
    std::atomic<bool> modeMoved_{false}, depthMoved_{false};   // set by the old knob, for TakeMovedParam
    float tl_ = 0.0f, tr_ = 0.0f, lfo0_ = 0.0f, lfo1_ = 1.0f;
    float bendRatio_ = 1.0f, modWheel_ = 0.0f;

    Svf   wah_;
    float wahSens_ = 0.0f, wahFreq_ = 0.1f, wahRange_ = 0.6f;
    int   wahType_ = 0;
    float wahEnv_ = 0.0f, wahFollow_ = 0.0f;
    float wahRelFast_ = 0.0f, wahRelSlow_ = 0.0f, wahFollowCo_ = 0.0f;

    Chorus chorus_;
    float  chorusMix_ = 0.0f;

    BiquadEq eqLowL_, eqMidL_, eqHighL_, eqLowR_, eqMidR_, eqHighR_;
    float level_ = 1.0f, gain_ = 1.0f;

    float preL_[kMpcBlockSize], preR_[kMpcBlockSize];
};

MPC_DAISY_ENGINE(RoadsDsp)
