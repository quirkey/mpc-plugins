// render — offline audition of an MPC plugin engine, no device needed.
//
// Links against a plugin's engine sources (not the VST wrapper) and drives
// mpc_engine() the way the wrapper does: 44.1 kHz, 128-frame int16 blocks.
//
//   render out.wav [key=value ...]
//
// Environment: MPC_RENDER_DATA_DIR (passed to create(), the plugin folder's data dir), MPC_RENDER_BPM,
// MPC_RENDER_TRANSPORT=1 (the stop/loop/tempo-step/locate timeline below), MPC_RENDER_LOG=file.csv
// (frame,ppq,bpm,playing per block), MPC_RENDER_NOTES (see NotesFromEnv), MPC_RENDER_LOOP=beats (a plain
// loop from ppq 0), MPC_RENDER_SET="seconds:key=value,..." (params changed during the render).
//
// Instruments play a short test phrase (chords, a run, sustained low note), or MPC_RENDER_NOTES.
// Effects (PLUG_EFFECT) process a synthesized drum-and-bass loop at 120 BPM,
// with the song position fed to song_pos() when the engine provides it.
// Prints peak/RMS and how long processing took relative to real time
// (host CPU, so only a rough guide: the MPC's Cortex-A17 is several times
// slower — bench on the device with mpc-vst-plugins/tools/bench.sh).
//
// Built by build.sh render <Plugin>.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include "engine.h"
}
#include "params.h"

static const int   kSr    = 44100;
static const int   kBlock = 128;
static const float kBpm   = 120.0f;

struct Event
{
    double  beat;
    uint8_t msg[3];
};

static void WriteWav(const char *path, const std::vector<int16_t> &pcm)
{
    FILE *f = fopen(path, "wb");
    if(!f)
    {
        perror(path);
        exit(1);
    }
    uint32_t data = pcm.size() * 2, riff = 36 + data, fmtLen = 16, rate = kSr, byteRate = kSr * 4;
    uint16_t pcmFmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f), fwrite(&pcmFmt, 2, 1, f), fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f), fwrite(&byteRate, 4, 1, f), fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

// MPC_RENDER_NOTES instead of the phrase: "none", or held notes "seconds:note:length,..." (velocity 100), e.g. pad
// holds for an engine that maps notes to actions. Times are seconds from the start of the render.
static std::vector<Event> NotesFromEnv(const char *spec)
{
    std::vector<Event> ev;
    if(!strcmp(spec, "none"))
        return ev;
    for(const char *p = spec; *p;)
    {
        double t = 0, len = 0;
        int    n = 0, used = 0;
        if(sscanf(p, "%lf:%d:%lf%n", &t, &n, &len, &used) != 3)
            break;
        double at = t * kBpm / 60.0, end = (t + len) * kBpm / 60.0;   // events are timed in beats at kBpm
        ev.push_back({at, {0x90, (uint8_t)n, 100}});
        ev.push_back({end, {0x80, (uint8_t)n, 0}});
        p += used;
        if(*p == ',')
            p++;
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) { return a.beat < b.beat; });
    return ev;
}

static std::vector<Event> Phrase()
{
    if(const char *spec = getenv("MPC_RENDER_NOTES"))
        return NotesFromEnv(spec);
    std::vector<Event> ev;
    auto note = [&](double at, double len, int n, int vel) {
        ev.push_back({at, {0x90, (uint8_t)n, (uint8_t)vel}});
        ev.push_back({at + len, {0x80, (uint8_t)n, 0}});
    };
    // Am - F - C - G strums, a fast run, then a held low note
    int chords[4][4] = {{57, 60, 64, 69}, {53, 57, 60, 65}, {48, 55, 60, 64}, {55, 59, 62, 67}};
    for(int c = 0; c < 4; c++)
        for(int k = 0; k < 4; k++)
            note(c * 2.0 + k * 0.04, 1.8, chords[c][k], 70 + 15 * (k == 0));
    int run[] = {69, 72, 76, 79, 81, 79, 76, 72};
    for(int i = 0; i < 8; i++)
        note(8.0 + i * 0.25, 0.2, run[i], 50 + i * 9);
    note(10.0, 3.0, 40, 120);
    return ev;
}

// Kick on the beat, closed hat on 8ths, saw bass on 16ths: something to filter.
static void DrumLoop(int64_t frame, float *l, float *r)
{
    double t    = (double)frame / kSr;
    double beat = t * kBpm / 60.0;
    double inB  = beat - floor(beat);
    double tb   = inB * 60.0 / kBpm;
    float  kick = sinf(2 * M_PI * (50 + 120 * expf(-tb * 40)) * tb) * expf(-tb * 8) * 0.8f;
    double in8  = fmod(beat * 2, 1.0) * 30.0 / kBpm;
    float  hat  = ((rand() / (float)RAND_MAX) * 2 - 1) * expf(-in8 * 60) * 0.15f;
    int    step = (int)floor(beat * 4) % 16;
    int    seq[16] = {33, 33, 45, 33, 36, 33, 45, 31, 33, 33, 45, 33, 38, 36, 31, 28};
    double f       = 440.0 * pow(2.0, (seq[step] - 69) / 12.0);
    double in16    = fmod(beat * 4, 1.0) * 15.0 / kBpm;
    float  saw     = (float)(2.0 * fmod(t * f, 1.0) - 1.0) * expf(-in16 * 6) * 0.35f;
    *l = *r = kick + hat + saw;
}

// MPC_RENDER_TRANSPORT=1: a song timeline with the host behaviour docs/MIDI_TIMING.md measured on a Force.
// Stopped for 0.5 s, then play from ppq 2.37 (mid-16th) in a 6-beat loop [0, 6) that wraps only at block
// starts (so the straddling block reports a pre-wrap position, like MPC). At 3.5 s the position lurches
// 0.3 beat ahead and at 6 s steps back 0.07 beat (a fast and a slow tempo change). Stopped from 9 to 10 s,
// then play from a locate to ppq 1.1; at 12 s a locate to ppq 4.1 while playing. The loop length is not a
// multiple of a 2-bar break, so a player that kept counting through a wrap (Breaks before 0.8) fails check_sync.py.
struct Transport
{
    bool   lurched = false, steppedBack = false, located = false, jumped = false;
    double pos     = 2.37;

    void Block(double t, double bpm, double &ppq, bool &playing)
    {
        playing = (t >= 0.5 && t < 9.0) || t >= 10.0;
        if(!playing)
        {
            ppq = pos;
            return;
        }
        if(t >= 10.0 && !located)
            located = true, pos = 1.1;
        if(t >= 12.0 && !jumped)
            jumped = true, pos = 4.1;
        if(t >= 3.5 && !lurched)
            lurched = true, pos += 0.3;
        if(t >= 6.0 && !steppedBack)
            steppedBack = true, pos -= 0.07;
        if(pos >= 6.0)
            pos -= 6.0;
        ppq = pos;
        pos += kBlock * bpm / 60.0 / kSr;
    }
};

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "usage: %s out.wav [key=value ...]\n", argv[0]);
        return 2;
    }
    const mpc_engine_t *eng  = mpc_engine();
    void               *inst = eng->create(getenv("MPC_RENDER_DATA_DIR"));
    for(int a = 2; a < argc; a++)
    {
        char  buf[128];
        snprintf(buf, sizeof buf, "%s", argv[a]);
        char *eq = strchr(buf, '=');
        if(!eq)
            continue;
        *eq = 0;
        eng->set_param(inst, buf, eq + 1);
    }
    // An engine with a worker thread (file scans, loads) settles first: wait until its display_rev has
    // been still for 300 ms (at most 5 s), as the screen would before anyone presses play.
    auto settle = [&] {
        char last[32] = "", now[32];
        auto still    = std::chrono::steady_clock::now();
        for(int i = 0; i < 500; i++)
        {
            now[0] = 0;
            eng->get_param(inst, "display_rev", now, sizeof now);
            if(strcmp(now, last))
            {
                snprintf(last, sizeof last, "%s", now);
                still = std::chrono::steady_clock::now();
            }
            else if(std::chrono::steady_clock::now() - still > std::chrono::milliseconds(300))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        char name[128];
        if(eng->get_param(inst, "name", name, sizeof name) > 0)
            printf("name: %s\n", name);
    };
    settle();
    // MPC_RENDER_SET="seconds:key=value,...": params set as the render reaches each time (then settled as above,
    // so a file the engine loads is ready by the next block, however fast the render runs)
    std::vector<std::pair<double, std::string>> sets;
    if(const char *spec = getenv("MPC_RENDER_SET"))
        for(const char *s = spec; *s;)
        {
            const char *end = strchr(s, ',');
            std::string item(s, end ? end - s : strlen(s));
            size_t      colon = item.find(':');
            if(colon != std::string::npos)
                sets.push_back({atof(item.c_str()), item.substr(colon + 1)});
            s = end ? end + 1 : s + item.size();
        }
    size_t       nextSet  = 0;
    const double loop     = getenv("MPC_RENDER_LOOP") ? atof(getenv("MPC_RENDER_LOOP")) : 0;
    double       loopPos  = 0;
    const double bpm      = getenv("MPC_RENDER_BPM") ? atof(getenv("MPC_RENDER_BPM")) : kBpm;
    const bool   scenario = getenv("MPC_RENDER_TRANSPORT") != nullptr;
    FILE        *log      = getenv("MPC_RENDER_LOG") ? fopen(getenv("MPC_RENDER_LOG"), "w") : nullptr;
    Transport    tp;

#ifdef PLUG_EFFECT
    const bool effect = true;
    const double seconds = 16.0;
#else
    const bool effect = false;
    const double seconds = 14.0;
#endif
    std::vector<Event>   ev = effect ? std::vector<Event>() : Phrase();
    std::vector<int16_t> pcm;
    int64_t total = (int64_t)(seconds * kSr), frame = 0;
    size_t  next  = 0;
    int16_t in[kBlock * 2], out[kBlock * 2];
    double  busy = 0, sumSq = 0;
    int     peak = 0;

    while(frame < total)
    {
        double beatEnd = (double)(frame + kBlock) / kSr * kBpm / 60.0;
        while(next < ev.size() && ev[next].beat < beatEnd)
            eng->midi(inst, ev[next++].msg, 3);
        for(int i = 0; i < kBlock; i++)
        {
            float l = 0, r = 0;
            if(effect)
                DrumLoop(frame + i, &l, &r);
            in[2 * i]     = (int16_t)(l * 32767);
            in[2 * i + 1] = (int16_t)(r * 32767);
        }
        while(nextSet < sets.size() && sets[nextSet].first <= (double)frame / kSr)
        {
            std::string kv = sets[nextSet++].second;
            size_t      eq = kv.find('=');
            if(eq != std::string::npos)
                eng->set_param(inst, kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
            settle();
        }
        double ppq     = (double)frame / kSr * bpm / 60.0;
        bool   playing = true;
        if(scenario)
            tp.Block((double)frame / kSr, bpm, ppq, playing);
        else if(loop > 0)
        {
            // MPC_RENDER_LOOP=beats: a loop [0, beats) that wraps at block starts, like MPC (the block that
            // straddles the end reports the pre-wrap position)
            if(loopPos >= loop)
                loopPos -= loop;
            ppq = loopPos;
            loopPos += kBlock * bpm / 60.0 / kSr;
        }
        if(log)
            fprintf(log, "%lld,%.6f,%.3f,%d\n", (long long)frame, ppq, bpm, playing ? 1 : 0);
        auto t0 = std::chrono::steady_clock::now();
        if(eng->song_pos)
            eng->song_pos(inst, ppq, bpm, playing ? 1 : 0, kBlock);
        if(effect)
            eng->process(inst, in, out, kBlock);
        else
            eng->render(inst, out, kBlock);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for(int i = 0; i < kBlock * 2; i++)
        {
            int a = abs(out[i]);
            peak  = a > peak ? a : peak;
            sumSq += (double)out[i] * out[i];
            pcm.push_back(out[i]);
        }
        frame += kBlock;
    }
    eng->destroy(inst);
    if(log)
        fclose(log);
    WriteWav(argv[1], pcm);

    double rms = sqrt(sumSq / pcm.size()) / 32768.0;
    printf("%s: %.1f s, peak %.1f dBFS, rms %.1f dBFS, host CPU %.2f%% of real time\n",
           argv[1], seconds, 20 * log10(peak / 32768.0 + 1e-9), 20 * log10(rms + 1e-9),
           100.0 * busy / seconds);
    return 0;
}
