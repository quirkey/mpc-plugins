// SamplerFilter's response, through the engine API (mpc_engine) as the wrapper drives it:  built by filter_check.sh
//   1. FILTER at 1 kHz and at 50 Hz: -3 dB at the cutoff, flat below, about 36 dB down an octave above and 60+ two
//      octaves above (6th-order Butterworth), and no peak anywhere below the cutoff (no resonance)
//   2. everything off: the input comes out bit for bit
//   3. the BITS / RATE switches: exactly common/Crusher.h (Breaks' grit) on the same input
//   4. TRACK: a 9 kHz tone at RATE 11 kHz folds to 2 kHz; the bandwidth filter takes that alias down by 25+ dB
//   5. old projects: the old BITS / RATE knobs set the switches (nearest step), saved and reloaded
//   6. DRIVE: louder and still bounded at full drive
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/DSPUtils.h"
#include "../../common/Crusher.h"
extern "C" {
#include "engine.h"
}

static const int   kSr = 44100, kBlock = 128;
static int         fails;
static const mpc_engine_t *eng;

static void Check(bool ok, const char *what, double got)
{
    printf("%s %-58s %.2f\n", ok ? "ok  " : "FAIL", what, got);
    if(!ok)
        fails++;
}

// Run `in` (mono, both channels) through a fresh instance with `params`; returns the left output.
static std::vector<float> Run(const std::vector<float> &in, const std::vector<std::pair<std::string, std::string>> &params)
{
    void *inst = eng->create(nullptr);
    for(auto &p : params)
        eng->set_param(inst, p.first.c_str(), p.second.c_str());
    std::vector<float> out(in.size());
    int16_t            bi[kBlock * 2], bo[kBlock * 2];
    for(size_t f = 0; f + kBlock <= in.size(); f += kBlock)
    {
        for(int i = 0; i < kBlock; i++)
            bi[2 * i] = bi[2 * i + 1] = (int16_t)lrintf(in[f + i] * 32767.0f);
        eng->process(inst, bi, bo, kBlock);
        for(int i = 0; i < kBlock; i++)
            out[f + i] = bo[2 * i] / 32768.0f;
    }
    eng->destroy(inst);
    return out;
}

static std::vector<float> Sine(double hz, double amp = 0.25, double seconds = 1.0)
{
    std::vector<float> s((size_t)(seconds * kSr) / kBlock * kBlock);
    for(size_t i = 0; i < s.size(); i++)
        s[i] = (float)(amp * sin(2 * M_PI * hz * i / kSr));
    return s;
}

// Level at `hz` (Hann-windowed DFT bin) over the last half second, in dB against a full-scale `amp` sine.
static double LevelDb(const std::vector<float> &x, double hz, double amp = 0.25)
{
    size_t n = kSr / 2, a = x.size() - n;
    double re = 0, im = 0, wsum = 0;
    for(size_t i = 0; i < n; i++)
    {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        re += w * x[a + i] * cos(2 * M_PI * hz * i / kSr);
        im += w * x[a + i] * sin(2 * M_PI * hz * i / kSr);
        wsum += w;
    }
    return 20 * log10(2 * sqrt(re * re + im * im) / wsum / amp + 1e-12);
}

static std::string Knob(double hz) { return std::to_string(log2(hz / 20.0) / log2(1000.0) * 100.0); }

int main()
{
    eng = mpc_engine();
    char buf[96];

    printf("== 1. the 6-pole low-pass\n");
    for(double fc : {1000.0, 50.0})
    {
        auto at = [&](double hz) { return LevelDb(Run(Sine(hz), {{"filter", Knob(fc)}}), hz); };
        double peak = -1e9;
        for(double r = 0.05; r <= 0.95; r += 0.05)
            peak = fmax(peak, at(fc * r));
        snprintf(buf, sizeof buf, "fc %.0f Hz: no peak below the cutoff (max dB)", fc);
        Check(peak < 0.1, buf, peak);
        snprintf(buf, sizeof buf, "fc %.0f Hz: half the cutoff flat (dB)", fc);
        double half = at(fc / 2);
        Check(fabs(half) < 0.1, buf, half);
        snprintf(buf, sizeof buf, "fc %.0f Hz: -3 dB at the cutoff", fc);
        double c = at(fc);
        Check(fabs(c + 3.01) < 0.3, buf, c);
        snprintf(buf, sizeof buf, "fc %.0f Hz: an octave up, about -36 dB", fc);
        double o1 = at(fc * 2);
        Check(fabs(o1 + 36.1) < 1.5, buf, o1);
        snprintf(buf, sizeof buf, "fc %.0f Hz: two octaves up, below -60 dB", fc);
        double o2 = at(fc * 4);
        Check(o2 < -60, buf, o2);
    }

    printf("== 2. all off: bit for bit\n");
    {
        std::vector<float> in = Sine(437.0, 0.5);
        for(size_t i = 0; i < in.size(); i += 97)
            in[i] *= -0.3f;   // not just a sine
        auto   out  = Run(in, {});
        double diff = 0;
        for(size_t i = 0; i < in.size(); i++)   // through the engine's int16 in (/ 32768) and out (* 32767)
            diff = fmax(diff, fabs(out[i] - lrintf(lrintf(in[i] * 32767.0f) / 32768.0f * 32767.0f) / 32768.0f));
        Check(diff == 0, "defaults: output = input (max difference)", diff);
    }

    printf("== 3. BITS / RATE switches = common/Crusher.h\n");
    for(auto br : std::vector<std::pair<int, int>>{{2, 2}, {4, 5}, {0, 0}, {5, 1}})   // bits_step, rate_step
    {
        std::vector<float> in = Sine(523.0, 0.6);
        for(size_t i = 0; i < in.size(); i++)
            in[i] += (float)(0.2 * sin(2 * M_PI * 3100.0 * i / kSr));
        auto out = Run(in, {{"bits_step", std::to_string(br.first)}, {"rate_step", std::to_string(br.second)}});
        float   bits = crushstep::kBits[br.first], khz = crushstep::kRateKhz[br.second];
        Crusher c;
        c.SetBits(bits);
        c.SetRate(khz, kSr);
        double diff = 0;
        for(size_t i = 0; i < in.size(); i++)
        {
            float l = lrintf(in[i] * 32767.0f) / 32768.0f, r = l;
            c.Process(l, r);
            float want = (float)lrintf(fmaxf(fminf(softLimit(l), 1.0f), -1.0f) * 32767.0f) / 32768.0f;
            diff       = fmax(diff, fabs(out[i] - want));
        }
        snprintf(buf, sizeof buf, "bits %.0f rate %.2f kHz: as Breaks (max difference)", bits, khz);
        Check(diff < 1.0 / 32768, buf, diff);
    }

    printf("== 4. TRACK: the bandwidth filter before the sampling\n");
    {
        auto   in  = Sine(9000.0);   // at 11.025 kHz (rate_step 2) it folds to 2.025 kHz
        double off = LevelDb(Run(in, {{"rate_step", "2"}}), 2025.0);
        double on  = LevelDb(Run(in, {{"rate_step", "2"}, {"track", "1"}}), 2025.0);
        Check(off > -12, "rate 11 kHz, a 9 kHz tone: its 2 kHz alias (dB)", off);
        Check(on < off - 25, "TRACK on: the alias (dB)", on);
        double low = LevelDb(Run(Sine(1000.0), {{"rate_step", "2"}, {"track", "1"}}), 1000.0);
        Check(low > -1.5, "TRACK on: 1 kHz, well inside the bandwidth (dB)", low);
    }

    printf("== 5. old projects: the old BITS / RATE knobs set the switches\n");
    {
        auto state = [](const char *load) {
            void   *inst = eng->create(nullptr);
            int16_t bi[kBlock * 2] = {}, bo[kBlock * 2];
            eng->set_param(inst, "state", load);
            eng->process(inst, bi, bo, kBlock);
            char key[64];
            while(mpc_engine_changed(inst, key, sizeof key) > 0)   // as the wrapper does after a block
                ;
            char out[2048] = "";
            eng->get_param(inst, "state", out, sizeof out);
            eng->destroy(inst);
            return std::string(out);
        };
        auto has = [](const std::string &s, const char *kv) { return s.find(kv) != std::string::npos; };
        std::string a = state("bits=12;rate=26.04;");
        Check(has(a, "bits_step=4;") && has(a, "rate_step=5;"), "bits 12, rate 26.04 -> 12, 26k (saved)", 0);
        std::string b = state(a.c_str());
        Check(has(b, "bits_step=4;") && has(b, "rate_step=5;"), "saved again, loaded again: the same", 0);
        std::string c = state("bits=7.4;rate=6;");
        Check(has(c, "bits_step=2;") && has(c, "rate_step=1;"), "bits 7.4, rate 6 -> 8, 8k (nearest)", 0);
        std::string d = state("bits=12;rate=26.04;bits_step=1;rate_step=2;");
        Check(has(d, "bits_step=1;") && has(d, "rate_step=2;"), "a new project: its switches win", 0);
        std::string e = state("");
        Check(has(e, "bits_step=5;") && has(e, "rate_step=7;"), "fresh: both Off", 0);
    }

    printf("== 6. DRIVE\n");
    {
        auto   in  = Sine(200.0, 0.05);
        double dry = LevelDb(Run(in, {}), 200.0, 0.05);
        auto   out = Run(in, {{"drive", "100"}});
        double wet = LevelDb(out, 200.0, 0.05), peak = 0;
        for(float v : out)
            peak = fmax(peak, fabs(v));
        Check(wet > dry + 6, "full drive on a quiet tone: louder (dB)", wet - dry);
        auto loud = Run(Sine(200.0, 0.9), {{"drive", "100"}});
        for(float v : loud)
            peak = fmax(peak, fabs(v));
        Check(peak <= 1.0, "full drive on a loud tone: bounded (peak)", peak);
    }

    printf(fails ? "filter: FAILED (%d)\n" : "filter: ok\n", fails);
    return fails ? 1 : 0;
}
