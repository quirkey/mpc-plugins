#pragma once
// ─── DaisyEngine.h ────────────────────────────────────────────────────────────
// Glue between a Daisy-style float DSP class and mpc-vst-plugins' engine
// interface (wrapper/engine.h: 44.1 kHz, interleaved int16 stereo, 128-frame
// blocks, string key/value parameters).
//
// A plugin writes one class derived from DaisyDsp and ends its .cpp with
//   MPC_DAISY_ENGINE(MyDsp)
//
// The DSP sees plain floats: SetParam(index, value) gets each parameter in its
// own units from params.json (an option list gets its index), and Process()
// gets float buffers in -1..1 — the same shape as a Daisy AudioCallback.
// Parameter storage, save/restore ("state" chunk) and int16 conversion live
// here, so the DSP class never touches strings.
//
// Threads: the wrapper never calls into one instance concurrently, but
// SetParam may arrive between blocks from the screen thread. Smooth anything
// that would click (cutoff, gain) inside the DSP, as on Daisy.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
#include "engine.h"
}
#include "params.h"   // generated per plugin by gen_vst.py from params.json

#ifndef HAS_SONG_POS
#define HAS_SONG_POS 0
#endif

constexpr float kMpcSampleRate = 44100.0f;
constexpr int   kMpcBlockSize  = 128;

// Defaults are no-ops; a DSP hides the ones it needs.
struct DaisyDsp
{
    void Init(float /*sampleRate*/) {}
    void SetParam(int /*index*/, float /*value*/) {}
    void Midi(const uint8_t * /*msg*/, int /*len*/) {}
    // HAS_SONG_POS only: host song position in beats (-1 if unknown), tempo, play state.
    void SongPos(double /*ppq*/, double /*bpm*/, bool /*playing*/) {}
    // Text for a param with "dynamic_display": true, or the readout of a "display": "string" param;
    // return > 0 when handled.
    int Display(int /*index*/, char * /*buf*/, int /*len*/) { return 0; }
    // in may be null (instruments). frames is 1..128.
    void Process(const float *inL, const float *inR, float *outL, float *outR, int frames) {}

    // The plugin's data folder (vst.json "defines": {"MODULE_SUBDIR": "\"name\""} → <plugin dir>/name), or
    // null. Called before Init().
    void SetDataDir(const char * /*dir*/) {}
    // State beyond the param values, e.g. a file chosen by name: append pairs with MpcStateAppend() and
    // return the length written. Each comes back through LoadExtra() after the params are restored.
    int  SaveExtra(char * /*buf*/, int /*len*/) { return 0; }
    void LoadExtra(const char * /*key*/, const char * /*value*/) {}
    // "defines": {"HAS_DISPLAY_REV": 1}: the wrapper polls this every ~100 ms and, when it changes,
    // re-reads every value and readout. Bump it when a worker thread changes text or params.
    unsigned DisplayRev() { return 0; }
    // Params the DSP moved by itself (a file found by name on restore): report them one at a time,
    // returning true while there are more. Drained on each display_rev poll, so bump DisplayRev() too.
    bool TakeChangedParam(int & /*index*/, float & /*value*/) { return false; }
    // "<key>_on" of a "display": "string" param: 1 lights its list tile, 0 leaves it dark, -1 = not answered.
    // The wrapper polls it every 10 ms, so this runs on the audio thread: read atomics, no locks.
    int TileOn(int /*index*/) { return -1; }
    // Display-only params the DSP moves by itself, often (a waveform's columns, a meter): not saved, host sets are
    // ignored, and changes are reported one at a time from TakeMovedParam (asked every block on the audio thread,
    // through mpc_engine_changed: the host is told with audioMasterAutomate, no UpdateDisplay for a non-option param).
    bool EngineOwned(int /*index*/) { return false; }
    bool TakeMovedParam(int & /*index*/, float & /*value*/) { return false; }
};

// "key=value;" for SaveExtra(), with %, ; and = in the value escaped as %XX.
inline int MpcStateAppend(char *buf, int len, const char *key, const char *val)
{
    int n = snprintf(buf, len, "%s=", key);
    for(const char *c = val; *c && n < len - 4; c++)
    {
        if(*c == '%' || *c == ';' || *c == '=')
            n += snprintf(buf + n, len - n, "%%%02X", (unsigned char)*c);
        else
            buf[n++] = *c;
    }
    if(n >= len - 1)
        return 0;
    buf[n++] = ';';
    buf[n]   = 0;
    return n;
}

inline void MpcStateUnescape(char *s)
{
    char *w = s;
    for(char *r = s; *r; r++)
    {
        unsigned v;
        if(*r == '%' && r[1] && r[2] && sscanf(r + 1, "%2x", &v) == 1)
        {
            *w++ = (char)v;
            r += 2;
        }
        else
            *w++ = *r;
    }
    *w = 0;
}

inline float MpcParamDefault(int i)
{
    const param_t &p = PARAMS[i];
    if(p.nopts > 1)
        return roundf(p.def * (p.nopts - 1));
    return p.min + p.def * (p.max - p.min);
}

inline float MpcParamClamp(int i, float v)
{
    const param_t &p = PARAMS[i];
    if(p.nopts > 0)
    {
        int idx = (int)lroundf(v);
        return (float)(idx < 0 ? 0 : idx >= p.nopts ? p.nopts - 1 : idx);
    }
    return v < p.min ? p.min : v > p.max ? p.max : v;
}

inline int MpcParamIndex(const char *key)
{
    for(int i = 0; i < NPARAMS; i++)
        if(!strcmp(PARAMS[i].key, key))
            return i;
    return -1;
}

template <class Dsp>
struct DaisyInstance
{
    Dsp   dsp;
    float values[NPARAMS];
    float inL[kMpcBlockSize], inR[kMpcBlockSize];
    float outL[kMpcBlockSize], outR[kMpcBlockSize];

    void Set(int i, float v)
    {
        values[i] = MpcParamClamp(i, v);
        dsp.SetParam(i, values[i]);
    }
};

static inline int16_t MpcToInt16(float x)
{
    x *= 32767.0f;
    if(x > 32767.0f) return 32767;
    if(x < -32768.0f) return -32768;
    return (int16_t)lrintf(x);
}

template <class Dsp>
struct DaisyEngine
{
    using Inst = DaisyInstance<Dsp>;

    static void *Create(const char *dataDir)
    {
        // Big DSP state (reverbs, delay lines) goes on the heap here, never in the audio path.
        Inst *s = new(std::nothrow) Inst();
        if(!s)
            return nullptr;
        s->dsp.SetDataDir(dataDir);
        s->dsp.Init(kMpcSampleRate);
        for(int i = 0; i < NPARAMS; i++)
            s->Set(i, MpcParamDefault(i));
        return s;
    }

    static void Destroy(void *inst) { delete static_cast<Inst *>(inst); }

    static void Midi(void *inst, const uint8_t *msg, int len)
    {
        static_cast<Inst *>(inst)->dsp.Midi(msg, len);
    }

    // "state" is "key=value;key=value;..." so presets survive params being appended later. Param values
    // come first; any other key is the DSP's own (SaveExtra), unescaped and handed to LoadExtra.
    static void SetState(Inst *s, const char *val)
    {
        char        buf[1024];
        const char *p = val;
        while(*p)
        {
            const char *end = strchr(p, ';');
            size_t      n   = end ? (size_t)(end - p) : strlen(p);
            if(n < sizeof buf)
            {
                memcpy(buf, p, n);
                buf[n]   = 0;
                char *eq = strchr(buf, '=');
                if(eq)
                {
                    *eq   = 0;
                    int i = MpcParamIndex(buf);
                    if(i >= 0 && PARAMS[i].popup_of < 0 && !s->dsp.EngineOwned(i))
                        s->Set(i, (float)atof(eq + 1));
                    else if(i < 0)
                    {
                        MpcStateUnescape(eq + 1);
                        s->dsp.LoadExtra(buf, eq + 1);
                    }
                }
            }
            p += n;
            if(*p == ';')
                p++;
        }
    }

    static void SetParam(void *inst, const char *key, const char *val)
    {
        Inst *s = static_cast<Inst *>(inst);
        if(!strcmp(key, "state"))
        {
            SetState(s, val);
            return;
        }
        int i = MpcParamIndex(key);
        if(i >= 0 && !s->dsp.EngineOwned(i))
            s->Set(i, (float)atof(val));
    }

    static int GetParam(void *inst, const char *key, char *buf, int len)
    {
        Inst *s = static_cast<Inst *>(inst);
        if(!strcmp(key, "state"))
        {
            int n = 0;
            for(int i = 0; i < NPARAMS && n < len; i++)
                if(PARAMS[i].popup_of < 0 && !PARAMS[i].string_display && !PARAMS[i].momentary
                   && !s->dsp.EngineOwned(i))
                    n += snprintf(buf + n, len - n, "%s=%g;", PARAMS[i].key, s->values[i]);
            if(n < len)
                n += s->dsp.SaveExtra(buf + n, len - n);
            return n < len ? n : 0;
        }
        if(!strcmp(key, "display_rev"))
        {
            int   i;
            float v;
            while(s->dsp.TakeChangedParam(i, v))
                if(i >= 0 && i < NPARAMS)
                    s->values[i] = MpcParamClamp(i, v);
            return snprintf(buf, len, "%u", s->dsp.DisplayRev());
        }
        size_t      klen = strlen(key);
        const char *sfx  = "_display";
        if(klen > 8 && !strcmp(key + klen - 8, sfx))
        {
            char base[64];
            snprintf(base, sizeof base, "%.*s", (int)(klen - 8), key);
            int i = MpcParamIndex(base);
            return i >= 0 ? s->dsp.Display(i, buf, len) : 0;
        }
        int i = MpcParamIndex(key);
        if(i < 0 && klen > 3 && !strcmp(key + klen - 3, "_on"))
        {
            char base[64];
            snprintf(base, sizeof base, "%.*s", (int)(klen - 3), key);
            int t  = MpcParamIndex(base);
            int on = t >= 0 && PARAMS[t].string_display ? s->dsp.TileOn(t) : -1;
            return on < 0 ? 0 : snprintf(buf, len, "%d", on ? 1 : 0);
        }
        if(i < 0)
            return 0;
        if(PARAMS[i].string_display)
            return s->dsp.Display(i, buf, len);
        return snprintf(buf, len, "%g", s->values[i]);
    }

    static void Render(void *inst, int16_t *out, int frames)
    {
        Inst *s = static_cast<Inst *>(inst);
        s->dsp.Process(nullptr, nullptr, s->outL, s->outR, frames);
        for(int i = 0; i < frames; i++)
        {
            out[2 * i]     = MpcToInt16(s->outL[i]);
            out[2 * i + 1] = MpcToInt16(s->outR[i]);
        }
    }

    static void ProcessFx(void *inst, const int16_t *in, int16_t *out, int frames)
    {
        Inst *s = static_cast<Inst *>(inst);
        for(int i = 0; i < frames; i++)
        {
            s->inL[i] = in[2 * i] * (1.0f / 32768.0f);
            s->inR[i] = in[2 * i + 1] * (1.0f / 32768.0f);
        }
        s->dsp.Process(s->inL, s->inR, s->outL, s->outR, frames);
        for(int i = 0; i < frames; i++)
        {
            out[2 * i]     = MpcToInt16(s->outL[i]);
            out[2 * i + 1] = MpcToInt16(s->outR[i]);
        }
    }

    static void SongPos(void *inst, double ppq, double bpm, int playing, int)
    {
        static_cast<Inst *>(inst)->dsp.SongPos(ppq, bpm, playing != 0);
    }

    // mpc_engine_changed(): the next param the DSP moved (TakeMovedParam), its key into `key`; 0 when none are left.
    static int Moved(void *inst, char *key, int len)
    {
        Inst *s = static_cast<Inst *>(inst);
        int   i;
        float v;
        while(s->dsp.TakeMovedParam(i, v))
            if(i >= 0 && i < NPARAMS)
            {
                s->values[i] = MpcParamClamp(i, v);
                return snprintf(key, len, "%s", PARAMS[i].key);
            }
        return 0;
    }

    static const mpc_engine_t *Get()
    {
        static mpc_engine_t e = [] {
            mpc_engine_t t{};
            t.create    = Create;
            t.destroy   = Destroy;
            t.midi      = Midi;
            t.set_param = SetParam;
            t.get_param = GetParam;
            t.render    = Render;
#ifdef PLUG_EFFECT
            t.process = ProcessFx;   // NULL for instruments, per engine.h
#endif
            t.song_pos  = HAS_SONG_POS ? SongPos : nullptr;
            return t;
        }();
        return &e;
    }
};

// mpc_engine_changed() is an optional weak symbol in engine.h, defined here for every DSP (macOS's linker won't
// leave a weak symbol undefined, so the host test fails to link on a Mac without it): it reports what the DSP's
// TakeMovedParam() hands over, and nothing (0) for a DSP without engine-owned params.
#define MPC_DAISY_ENGINE(DspClass)                                                              \
    extern "C" const mpc_engine_t *mpc_engine(void) { return DaisyEngine<DspClass>::Get(); }    \
    extern "C" int mpc_engine_changed(void *inst, char *key, int len)                          \
    {                                                                                           \
        return DaisyEngine<DspClass>::Moved(inst, key, len);                                    \
    }
