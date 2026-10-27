// Breaks — tempo-locked break player for MPC OS
//
// Plays a drum break (a WAV from your Breaks folder) in time with the song: the
// loop is cut into 16th-note slices and each slice is retriggered on the song's
// 16th grid, so the break always follows MPC's tempo. PITCH repitches the slices
// themselves, sampler style (faster/higher or slower/lower) without moving the
// grid. BITS and RATE are the S950/SP-1200 style grit. Plays whenever the
// transport plays; no notes needed.
//
// The break runs through its whole length regardless of the MPC sequence: a
// 4-bar break over a 1-bar loop keeps going bar 1, 2, 3, 4. Play lines it up
// with the song (bar 1 of the song = start of the break); after that it counts
// 16ths. Timing follows mpc-vst-plugins docs/MIDI_TIMING.md: boundaries come
// from the song position (k = floor(ppq * 4)), the count advances by however
// many 16ths k moved (a fast tempo change can skip one), never goes back on a
// step-back, and a loop wrap or locate (a jump of over a beat) only re-reads k
// without counting: the 16th at a loop end already fired in the block that
// straddles it, which MPC reports with the pre-wrap position.
//
// Pads: notes hold effects for as long as they are held (see Fx below), the newest held pad winning. The break
// keeps its place underneath, so letting go lands where it would have been. Notes apply at the start of the
// next block; one within 5 ms before a 16th applies on it.
//
// Threads: a worker scans folders and loads WAVs; a loaded break reaches the
// audio thread through an atomic pointer and old ones go back the same way to
// be freed. The audio thread never allocates, locks or touches files.
//
// Params (params.json order — append only, projects save by index):
//   0 Level      dB (param 0 must be a plain knob: the wrapper defers it around MPC's STOP toggle)
//   1 Pitch      semitones, repitches the slices
//   2 Bits       16..4 bit depth
//   3 Rate       sample rate of the sample-and-hold decimator, kHz
//   4 Bars       Auto / 1 / 2 / 4 / 8 / 16: how long the loop is (Auto: name "4bar", ACID, name BPM, length)
//   5 Break      index into the sorted break list (the file itself is saved by name)
//   6 Prev / 7 Next   step the Break index
//   8 Name       readout: name, tempo, bars or status
//   9 Rescan     look for new files
//  10 Vary       chance a 16th is varied (Variation.h)
//  11-14 Swap / Repeat / Reverse / Gap   relative weights of the variations (VARY never rolls: Variation.h's
//                ROLL stays for rolls played live, its weight 0 here)
//  15 Pattern    the variation repeats every: Break (its length) / 1 / 2 / 4 bars / Evolve (never)
//  16 Variation  the seed: each number is a different variation, saved with the project
//  17 Prev Var / 18 New Var   step the seed
//  19 Half       half time: the break advances half a 16th per song 16th, so each break 16th spans two
//  20 Grain      1/16 (each song 16th restarts the slice; the classic behaviour) or a shorter grain: within
//                each 16th the break is re-cut every grain, S950 time-stretch style
//
// Time-stretch: the break has its own position B, in break 16ths, that each song 16th advances by 1 (by 0.5
// in half time). Every song 16th restarts playback at B's place in the break; with a grain shorter than a 16th,
// playback also restarts every grain, at where B has moved to by then (so the break keeps the song's pace)
// while each grain plays at the PITCH rate. Grain 1/16 with Half off is exactly the plain slice player.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BreakLibrary.h"
#include "Variation.h"
#include "WavFile.h"
#include "../common/DSPUtils.h"
#include "../common/Crusher.h"
#include "../common/DaisyEngine.h"

enum Param
{
    P_LEVEL,
    P_PITCH,
    P_BITS,
    P_RATE,
    P_BARS,
    P_BREAK,
    P_PREV,
    P_NEXT,
    P_NAME,
    P_RESCAN,
    P_VARY,
    P_SWAP,
    P_REPEAT,
    P_REVERSE,
    P_GAP,
    P_PATTERN,
    P_SEED,
    P_VAR_PREV,
    P_VAR_NEW,
    P_HALF,
    P_GRAIN,
    P_PAD_1,   // P_PAD_1 .. P_PAD_16: the PADS page's tiles (label + lit while held), pad 1 = C
    P_PAD_16 = P_PAD_1 + 15,
    P_PAD_LEN,
    P_WAVE_1,   // P_WAVE_1 .. P_WAVE_128: the waveform's columns (engine-owned): level 0-15, + 16 while playing
    P_WAVE_128 = P_WAVE_1 + 127,
    P_RAMP,
    P_ATTACK,    // each slice's envelope, in % of its 16th (of a roll hit's length, ...): the fade in,
    P_GATE,      // where it starts to end,
    P_RELEASE,   // and the fade out from there
    P_BITS_STEP,   // the BITS / RATE switches (common/Crusher.h crushstep); P_BITS / P_RATE are the old knobs,
    P_RATE_STEP,   // which still set them (to the nearest position) for projects saved before
};

static const int kWaveCols   = 128;
static const int kWaveLevels = 16;

static const int kBarsOption[]    = {0, 1, 2, 4, 8, 16};   // P_BARS index -> bars (0 = auto)
static const int kPatternOption[] = {-1, 16, 32, 64, 0};   // P_PATTERN -> 16ths (-1 = the break, 0 = never)
// P_GRAIN -> grain: 0 = 1/16 (no grains), < 0 = that many beats (1/32 = 0.125), > 0 = milliseconds
static const float kGrainOption[] = {0, -0.125f, 80, 50, 30, 20, 12, 6};
// P_PAD_LEN -> song 16ths (0 = free): a pad effect starts on the nearest 16th (a press in the second half of one
// waits for the next) and, once let go, plays on to a whole number of these from where it started
static const int kPadLenOption[] = {0, 1, 2, 4, 8, 16};
// P_RAMP -> semitones per beat a held ramp pad moves the pitch (an octave in 2 bars, 1, half a bar, a beat, half)
static const float kRampOption[] = {1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
static const float kRampMax      = 24.0f;   // semitones: a ramp holds there
// SPEED pads: hits per beat stay within these (a beat .. a 128th); the rate doubles (or halves) per RAMP octave
static const double kSpeedMin = 1.0, kSpeedMax = 32.0;

// Pad effects: held pads change what plays for as long as they are held. The break keeps its place underneath
// ("slip"): letting go lands where it would have been. Pads are mapped by pad number (PadForNote: the notes of
// MPC's two pad layouts): pads 1-7 are the effects, 8 and 12 the octave pads (down, up), 9 and 10 the ramp pads
// (down, up), 13 and 14 the speed pads, 15 and 16 the velocity pads, 11 free. An octave pad shifts the pitch on top
// of whichever effect is held, so OCT UP + ROLL is a pitched roll; a ramp pad moves it further each grain
// (each 16th, each roll hit) for as long as it is held, RAMP semitones per beat, so RAMP UP + ROLL is a riser.
// A speed pad repeats the slice at a rate that doubles (SPEED UP) or halves (DOWN) every RAMP octave's time, from
// the held roll's (32nds, triplets) or 16ths; a velocity pad sweeps each hit's level up from silence or down to
// it over the same time. So SPEED UP + ROLL is a roll that speeds up, VEL UP + ROLL one that swells.
// Among the effects (and between the two pads of each kind) the newest held one wins; the two pads of a kind carry
// on from where the other got to.
enum Fx
{
    FX_NONE = -1,
    FX_REVERSE,   // every slice backwards
    FX_GAP,       // silence
    FX_FLAM,      // a quieter grace hit of each next slice just before its 16th
    FX_TRIPLET,   // the held slice retriggered on 16th triplets
    FX_ROLL,      // the held slice retriggered on 32nds
    FX_STUTTER,   // the held slice replayed every 16th
    FX_HALF,      // half speed from where the pad was pressed (quarter with HALF TIME on)
    FX_OCT_DOWN,  // modifiers, held alongside an effect: an octave down (half rate, like the PITCH knob)
    FX_OCT_UP,    // an octave up
    FX_RAMP_DOWN, // the pitch falls each grain, from where the pad went down
    FX_RAMP_UP,   // rises
    FX_SPEED_DOWN, // the held slice repeated slower and slower
    FX_SPEED_UP,   // faster and faster
    FX_VEL_DOWN,   // each hit quieter, down to silence
    FX_VEL_UP,     // louder, up from silence
    FX_COUNT,
};
static const char *const kFxName[FX_COUNT] = {"REVERSE", "GAP",  "FLAM",     "TRIPLET", "ROLL",     "STUTTER",
                                              "HALF",    "OCT DOWN", "OCT UP", "RAMP DOWN", "RAMP UP",
                                              "SPEED DOWN", "SPEED UP", "VEL DOWN", "VEL UP"};
static const double      kFlamSeconds      = 0.025;   // grace note lead
static const float       kFlamGain         = 0.5f;

// The pad (0-15) a note comes from, or -1: a track's pads send their pad layout's notes, which on MPC are either
// the default Notes layout, a C major scale from C5 (pad 1 = 72 ... pad 16 = 98, white keys only), or chromatic
// from bank A's 36 (36-51).
static int PadForNote(int note)
{
    if(note >= 36 && note <= 51)
        return note - 36;
    static const int kDegree[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};   // C major scale degree, -1 off it
    if(note >= 72 && note <= 98 && kDegree[note % 12] >= 0)
        return (note / 12 - 6) * 7 + kDegree[note % 12];
    return -1;
}

static int FxForPad(int pad)
{
    static const int kPadFx[16] = {FX_REVERSE, FX_GAP,       FX_FLAM,    FX_TRIPLET, FX_ROLL, FX_STUTTER,
                                   FX_HALF,    FX_OCT_DOWN,  FX_RAMP_DOWN, FX_RAMP_UP, FX_NONE, FX_OCT_UP,
                                   FX_SPEED_DOWN, FX_SPEED_UP, FX_VEL_DOWN, FX_VEL_UP};
    return pad >= 0 && pad < 16 ? kPadFx[pad] : FX_NONE;
}

static int FxForNote(int note) { return FxForPad(PadForNote(note)); }

static bool IsOctave(int fx) { return fx == FX_OCT_DOWN || fx == FX_OCT_UP; }
static bool IsRamp(int fx) { return fx == FX_RAMP_DOWN || fx == FX_RAMP_UP; }
static bool IsSpeed(int fx) { return fx == FX_SPEED_DOWN || fx == FX_SPEED_UP; }
static bool IsVel(int fx) { return fx == FX_VEL_DOWN || fx == FX_VEL_UP; }

// A loaded break. Built by the worker, read-only once published.
struct Break
{
    WavData     wav;
    std::string name;
    int         autoBeats = 4;
    float       autoBpm   = 0;
};

struct Voice
{
    const Break *brk    = nullptr;
    double       pos    = 0;   // file frames
    double       rate   = 1;   // file frames per output frame
    float        gain   = 0;
    float        target = 0;   // fade towards
    float        step   = 0;   // per frame
    bool         wrap   = false;   // past either end, carry on from the other (stretching runs past the file's end)
    double       envStart = 0, envBeats = 0.25;   // its segment's song position and length (ATTACK / GATE / RELEASE)
    float        env    = 1;       // that envelope now

    bool Active() const { return brk && (gain > 0 || target > 0); }

    void Read(float &l, float &r)
    {
        int n = brk->wav.frames;
        if(wrap && n > 1 && (pos < 0 || pos >= n - 1))
        {
            pos = fmod(pos, (double)(n - 1));
            if(pos < 0)
                pos += n - 1;
        }
        int i = (int)pos;
        if(pos < 0 || i >= n - 1)
        {
            brk = nullptr;
            return;
        }
        float f = (float)(pos - i);
        l += gain * env * (brk->wav.l[i] + f * (brk->wav.l[i + 1] - brk->wav.l[i]));
        r += gain * env * (brk->wav.r[i] + f * (brk->wav.r[i + 1] - brk->wav.r[i]));
        pos += rate;
        if(gain != target)
        {
            gain += step;
            if((step > 0 && gain >= target) || (step < 0 && gain <= target))
                gain = target;
        }
        if(gain <= 0 && target <= 0)
            brk = nullptr;
    }
};

class BreaksDsp : public DaisyDsp
{
  public:
    ~BreaksDsp()
    {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if(worker_.joinable())
            worker_.join();   // before dlclose can unmap this code
        delete cur_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
        for(int i = 0; i < staleCount_; i++)
            delete stale_[i];
    }

    void SetDataDir(const char *dir) { dataDir_ = dir ? dir : ""; }

    void Init(float sr)
    {
        sr_ = sr;
        try
        {
            worker_ = std::thread([this] { WorkerMain(); });
        }
        catch(...)
        {
            std::lock_guard<std::mutex> g(mu_);
            scanned_ = true;
            SetStatusLocked("Can't start the file loader");
        }
    }

    void SetParam(int i, float v)
    {
        if(i >= P_PAD_1 && i <= P_PAD_16)
        {
            // A PADS-page tile touched: every touch and every lift reaches here once, as the skin's toggle flipped
            // from whatever the tile's light last set it to (the wrapper drops MPC's echo of the light), so the
            // value says nothing and each call flips the tile between held and let go, played as its pad's note
            // (chromatic, from 36). The defaults set at creation, before any audio, aren't touches.
            if(!started_)
                return;
            int     n       = i - P_PAD_1;
            uint8_t note[3] = {(uint8_t)((screenHeld_[n] = !screenHeld_[n]) ? 0x90 : 0x80), (uint8_t)(36 + n), 100};
            Midi(note, 3);
            return;
        }
        switch(i)
        {
            case P_BREAK:
            {
                std::lock_guard<std::mutex> g(mu_);
                breakIndex_ = (int)v;
                if(scanned_ && !list_.empty() && breakIndex_ >= (int)list_.size())
                {
                    breakIndex_ = (int)list_.size() - 1;   // past the last break (SELECT, the wheel) stays on it
                    movedBreak_.store(breakIndex_);   // told to the host, so its knob stops there too
                }
                if(breakIndex_ < (int)list_.size())
                    wantName_ = list_[breakIndex_].name;
                else
                    wantName_.clear();   // resolved after the scan
                byName_ = !wantName_.empty();
                PublishLocked();
                break;
            }
            case P_BARS: barsOpt_ = (int)v; break;
            case P_PITCH: pitchRatio_ = exp2f(v / 12.0f); break;
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
            case P_LEVEL: level_ = powf(10.0f, v / 20.0f); break;
            case P_VARY: var_.amount = v * 0.01f; break;
            case P_SWAP: var_.swap = v; break;
            case P_REPEAT: var_.repeat = v; break;
            case P_REVERSE: var_.reverse = v; break;
            case P_GAP: var_.gap = v; break;
            case P_PATTERN: pattern_ = kPatternOption[(int)v]; break;
            case P_SEED: seed_ = (uint32_t)v; break;
            case P_HALF: half_ = v > 0.5f; break;
            case P_PAD_LEN:
            {
                int o   = (int)v;
                padLen_ = o >= 0 && o < (int)(sizeof kPadLenOption / sizeof kPadLenOption[0]) ? kPadLenOption[o] : 0;
                break;
            }
            case P_ATTACK: attack_ = v * 0.01f; break;
            case P_GATE: gate_ = v * 0.01f; break;
            case P_RELEASE: release_ = v * 0.01f; break;
            case P_RAMP:
            {
                int o      = (int)v;
                rampSpeed_ = o >= 0 && o < (int)(sizeof kRampOption / sizeof kRampOption[0]) ? kRampOption[o] : 3.0f;
                break;
            }
            case P_GRAIN:
            {
                int g  = (int)v;
                grain_ = g >= 0 && g < (int)(sizeof kGrainOption / sizeof kGrainOption[0]) ? kGrainOption[g] : 0;
                break;
            }
            case P_RESCAN:
                if(v > 0.5f)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    rescan_ = true;
                    cv_.notify_all();
                }
                break;
            case P_PREV:
            case P_NEXT:
                if(Pressed(press_[i == P_NEXT], v))
                    StepBreak(i == P_NEXT ? 1 : -1);
                break;
            case P_VAR_PREV:
            case P_VAR_NEW:
                if(Pressed(press_[2 + (i == P_VAR_NEW)], v))
                {
                    long s = (long)seed_ + (i == P_VAR_NEW ? 1 : -1);
                    seed_  = (uint32_t)(s < 0 ? 0 : s > 999 ? 999 : s);
                    movedSeed_.store((int)seed_);
                }
                break;
        }
        if(i == P_BREAK || i == P_PREV || i == P_NEXT)
            cv_.notify_all();
    }

    // PREV / NEXT / PREV VAR / NEW VAR: MPC sends a button's 1 twice per tap, on the press and ~0.1 s later on the
    // release (traced on an MPC One, 3.9.1), so a 1 counts when the last value was 0 or 0.3 s after the last press.
    struct Press
    {
        float  last = 0;
        double at   = -1;
    };
    Press press_[4];

    static bool Pressed(Press &p, float v)
    {
        double now  = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool   fire = v > 0.5f && (p.last <= 0.5f || now - p.at >= 0.3);
        p.last      = v;
        if(fire)
            p.at = now;
        return fire;
    }

    // One break on, stopping at either end; the new index goes back to the host (TakeMovedParam) so SELECT
    // and the data wheel carry on from it.
    void StepBreak(int d)
    {
        std::lock_guard<std::mutex> g(mu_);
        if(list_.empty())
            return;
        int n = breakIndex_ + d, last = (int)list_.size() - 1;
        n     = n < 0 ? 0 : n > last ? last : n;
        if(n == breakIndex_)
            return;
        breakIndex_ = n;
        wantName_   = list_[n].name;
        byName_     = true;
        movedBreak_.store(n);
        PublishLocked();
    }

    // Display, DisplayRev and TakeChangedParam run on MPC's audio thread (the wrapper's housekeeping), so
    // they never take mu_: the worker formats the readout into one of a few fixed slots and publishes it.
    int Display(int i, char *buf, int len)
    {
        if(i >= P_PAD_1 && i <= P_PAD_16)
        {
            int fx = FxForPad(i - P_PAD_1);
            return snprintf(buf, len, "%s", fx != FX_NONE ? kFxName[fx] : "-");
        }
        switch(i)
        {
            case P_NAME:
            {
                int pad = shownPad_.load(), oct = shownOct_.load(), ramp = shownRamp_.load();
                int speed = shownSpeed_.load(), vel = shownVel_.load();
                const char *t = text_[textSlot_.load()];
                if(pad >= 100)
                    return snprintf(buf, len, "%s   [note %d: no effect]", t, pad - 100);
                char parts[96] = "";
                int  n         = 0;
                for(int f : {pad, oct, ramp, speed, vel})
                    if(f >= 0)
                        n += snprintf(parts + n, sizeof parts - n, "%s%s", n ? " + " : "", kFxName[f]);
                return n ? snprintf(buf, len, "%s   [%s]", t, parts) : snprintf(buf, len, "%s", t);
            }
            case P_BITS: return bits_ >= 15.5f ? snprintf(buf, len, "Off") : snprintf(buf, len, "%.0f-bit", bits_);
            case P_RATE:
                return rateKhz_ >= 44.0f ? snprintf(buf, len, "Off") : snprintf(buf, len, "%.1f kHz", rateKhz_);
        }
        return 0;
    }

    unsigned DisplayRev() { return rev_.load(); }

    // A PADS tile lights while the effect it shows is the one playing (pads 1 and 13 both light for reverse),
    // or, for an unmapped tile, while its note is the pad pressed.
    int TileOn(int i)
    {
        if(i < P_PAD_1 || i > P_PAD_16)
            return -1;
        int n = i - P_PAD_1, shown = shownPad_.load(), fx = FxForPad(n);
        if(IsOctave(fx))
            return shownOct_.load() == fx;
        if(IsRamp(fx))
            return shownRamp_.load() == fx;
        if(IsSpeed(fx))
            return shownSpeed_.load() == fx;
        if(IsVel(fx))
            return shownVel_.load() == fx;
        if(fx != FX_NONE)
            return shown == fx;
        return shown >= 100 && PadForNote(shown - 100) == n;
    }

    bool TakeChangedParam(int &index, float &value)
    {
        int c = changedIndex_.exchange(-1);
        if(c < 0)
            return false;
        index = P_BREAK;
        value = (float)c;
        return true;
    }

    int SaveExtra(char *buf, int len)
    {
        std::lock_guard<std::mutex> g(mu_);
        return wantName_.empty() ? 0 : MpcStateAppend(buf, len, "file", wantName_.c_str());
    }

    void LoadExtra(const char *key, const char *value)
    {
        if(strcmp(key, "file"))
            return;
        {
            std::lock_guard<std::mutex> g(mu_);
            wantName_ = value;
            byName_   = true;   // its index may differ from the saved one if files were added since
            if(scanned_)
                ResolveIndexLocked();
            PublishLocked();
        }
        cv_.notify_all();
    }

    // Pads (MIDI notes) hold effects. Runs on the audio thread, between blocks: no allocation, no locks.
    void Midi(const uint8_t *msg, int len)
    {
        if(len < 3)
            return;
        int status = msg[0] & 0xF0;
        if(status == 0xB0 && (msg[1] == 120 || msg[1] == 123))   // all sound / all notes off
        {
            heldCount_ = 0;
            for(auto &h : screenHeld_)
                h = false;
            UpdateFx(-1);
            return;
        }
        bool on  = status == 0x90 && msg[2] > 0;
        bool off = status == 0x80 || (status == 0x90 && msg[2] == 0);
        if(!on && !off)
            return;
        int note = msg[1], at = -1;
        for(int i = 0; i < heldCount_; i++)
            if(heldNote_[i] == note)
                at = i;
        if(on)
        {
            if(at >= 0 || heldCount_ == kHeld)   // a note already held (overlapping sequenced notes): ignore
                return;
            heldNote_[heldCount_] = note;
            heldFx_[heldCount_++] = FxForNote(note);
        }
        else
        {
            if(at < 0)
                return;
            for(int i = at; i < heldCount_ - 1; i++)
                heldNote_[i] = heldNote_[i + 1], heldFx_[i] = heldFx_[i + 1];
            heldCount_--;
        }
        UpdateFx(on ? note : -1);
    }

    void SongPos(double ppq, double bpm, bool playing)
    {
        ppq_     = ppq;
        bpm_     = bpm;
        playing_ = playing && ppq >= 0 && bpm > 0;
    }

    void Process(const float *, const float *, float *outL, float *outR, int frames)
    {
        started_ = true;
        if(!playing_)
        {
            if(anchored_)
            {
                for(auto &v : voices_)
                    Fade(v, 0.0f, 0.005f);
                anchored_  = false;
                rollArmed_ = false;
                segOn_     = false;
            }
        }
        else
        {
            double ppqPerFrame = bpm_ / 60.0 / sr_;
            if(!anchored_)
            {
                // Play: the break starts from its top, starting mid-slice if play starts mid-16th
                lastK_     = (long)floor(ppq_ * 4.0 + 1e-9);
                restarts_  = 0;
                predicted_ = false;
                SeqStart(lastK_);
                if(releasePending_)
                {
                    releasePending_ = false;
                    CommitFx();
                }
                fxDirty_  = false;
                fxAtBoundary_ = false;
                fxEntering_ = false;
                fxByPress_  = false;
                fxStartK_   = lastK_;
                nowPpq_     = ppq_;
                if(fx_ != FX_NONE)
                    EnterFx(ppq_, true);
                Trigger(lastK_ / 4.0, ppq_ - lastK_ / 4.0, false, true);
                anchored_ = true;
            }
            else
            {
                bool jump = fabs(ppq_ - nextPpq_) > 1.0;
                long newK = (long)floor(ppq_ * 4.0 + 1e-9);
                if(predicted_ && !(jump && newK == loopStartK_))
                {
                    // the loop end passed without the wrap (the loop was changed): back to where the break was
                    predicted_ = false;
                    loopEndK_  = -1;
                    restarts_--;
                    breakPos_ += predSaved_;
                    if(fx_ == FX_HALF)
                        fxPos_ = breakPos_;
                    if(!jump && cur_)
                        Trigger(lastK_ / 4.0, ppq_ - lastK_ / 4.0, true);
                }
                if(jump)
                {
                    rollArmed_ = false;   // its 32nd belonged to the old position
                    fxStartK_  = newK;
                    if(releasePending_)   // its end belonged to the old position too
                    {
                        releasePending_ = false;
                        fxByPress_      = false;
                        CommitFx();
                    }
                    if(predicted_)
                    {
                        // the wrap the loop end already restarted the break for
                        predicted_ = false;
                        lastK_     = newK;
                    }
                    else
                    {
                        // a loop wrap seen for the first time, or a locate (Play Start while playing): the
                        // sequence starts again here. A jump back is remembered as the loop, so the next pass
                        // restarts the break on the loop end's own 16th (the block that straddles it still
                        // reports the old position) instead of a block late.
                        if(ppq_ < nextPpq_)
                        {
                            loopEndK_   = lround(nextPpq_ * 4.0);
                            loopStartK_ = newK;
                        }
                        lastK_ = newK;
                        restarts_++;
                        SeqStart(newK);
                        Trigger(newK / 4.0, ppq_ - newK / 4.0, false, true);
                    }
                }
                if(fxDirty_)
                    ApplyFx(ppq_);
            }
            for(int i = 0; i < frames; i++)
            {
                double p       = ppq_ + i * ppqPerFrame;
                long   k       = (long)floor(p * 4.0 + 1e-9);
                nowPpq_        = p;
                bool   started = true;
                if(k > lastK_)
                {
                    breakPos_ += (k - lastK_) * BaseAdvance();
                    if(fx_ == FX_HALF)
                        fxPos_ += (k - lastK_) * BaseAdvance() * 0.5;
                    lastK_       = k;
                    bool seqStart = false;
                    if(k == loopEndK_)
                    {
                        // the loop's end: the host wraps here (reported from the next block on)
                        double counted = breakPos_;
                        restarts_++;
                        SeqStart(loopStartK_);
                        predSaved_ = counted - breakPos_;
                        predicted_ = seqStart = true;
                    }
                    if(releasePending_ && k >= fxEndK_)
                    {
                        releasePending_ = false;
                        fxByPress_      = false;
                        fxStartK_       = k;   // what the pads still hold starts here
                        CommitFx();
                        if(fxDirty_)
                            fxDirty_ = false, fxAtBoundary_ = true;
                    }
                    if(fxAtBoundary_)
                    {
                        fxAtBoundary_ = false;
                        if(fxEntering_)
                        {
                            fxEntering_ = false;
                            EnterFx(p, true);
                            if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)
                                fxRollG_ = (long)floor(p / fxRollBeats_ + 1e-9) - 1;   // first hit on this 16th
                        }
                    }
                    Trigger(k / 4.0, p - k / 4.0, false, seqStart);
                }
                else if(rollArmed_ && p >= rollPpq_)
                {
                    rollArmed_ = false;
                    // a Roll's second hit, on the 32nd
                    StartSegment(rollSlice_, false, 0.0, rollPpq_, 0.0, 0.0003f, 0.125);
                }
                else
                    started = false;
                if(speed_ != FX_NONE && fx_ != FX_GAP)
                {
                    if(speedPending_)
                    {
                        // now, or on the 16th a press just before it lands on (as the effects do); with a roll
                        // held, from its next grid point. Alone, the slice it repeats is taken at the first hit.
                        speedPending_  = false;
                        double next16  = (lastK_ + 1) / 4.0;
                        speedOn16th_   = next16 - p < 0.005 * bpm_ / 60.0;
                        speedStartPpq_ = speedOn16th_ ? next16 : p;
                        bool roll      = fx_ == FX_TRIPLET || fx_ == FX_ROLL;
                        speedNextPpq_  = roll && !speedOn16th_ ? fmax(p, (fxRollG_ + 1) * fxRollBeats_) : speedStartPpq_;
                        speedCapture_  = !roll && fx_ != FX_STUTTER;
                    }
                    if(p >= speedNextPpq_ && cur_)
                    {
                        if(speedCapture_)
                        {
                            speedCapture_ = false;
                            CaptureSlice(speedOn16th_ || !segOn_);
                        }
                        speedMul_         = SpeedMul(p);
                        double interval   = 1.0 / (SpeedBase() * speedMul_);
                        StartSegment(capSlice_, capRev_, 0.0, p, 0.0, 0.0003f, interval);
#ifdef BREAKS_DEBUG
                        fprintf(stderr, "speed hit %.6f next %.6f slice %ld\n", p, p + interval, capSlice_);
#endif
                        speedNextPpq_ = p + interval;
                        started       = true;
                    }
                }
                else if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)
                {
                    long g = (long)floor(p / fxRollBeats_ + 1e-9);
                    if(g > fxRollG_ && cur_)
                    {
                        fxRollG_ = g;
                        StartSegment(capSlice_, capRev_, 0.0, g * fxRollBeats_, 0.0, 0.0003f, fxRollBeats_);
                        started = true;
                    }
                }
                else if(fx_ == FX_FLAM && !flamDone_ && cur_ && p >= (lastK_ + 1) / 4.0 - kFlamSeconds * bpm_ / 60.0)
                {
                    flamDone_ = true;
                    Grace(p);
                    started = true;
                }
                if(!started && segOn_ && grainBeats_ > 0 && p >= nextGrainPpq_)
                    NextGrain();
                RenderFrame(outL[i], outR[i]);
            }
            nextPpq_ = ppq_ + frames * ppqPerFrame;
            UpdateWave();
            Retire();
            return;
        }
        for(int i = 0; i < frames; i++)
            RenderFrame(outL[i], outR[i]);
        UpdateWave();
        Retire();
    }

  private:
    void RenderFrame(float &outL, float &outR)
    {
        float l = 0, r = 0;
        bool  shaped = attack_ > 0 || gate_ < 1;
        for(auto &v : voices_)
            if(v.brk)
            {
                v.env = shaped ? Envelope(v) : 1.0f;
                v.Read(l, r);
            }
        crush_.Process(l, r);   // sample-and-hold decimation, then bit depth: the old-sampler grit
        outL = softLimit(l * level_);
        outR = softLimit(r * level_);
    }

    // ATTACK / GATE / RELEASE: a voice's level for where the song is in its segment (x, as a share of its length):
    // up from 0 over the first ATTACK, and from GATE down to 0 over RELEASE (at least 3 ms, so a gate doesn't
    // click), silent after that until the next segment cuts in. GATE 100% has nothing to release.
    float Envelope(const Voice &v) const
    {
        double x = (nowPpq_ - v.envStart) / v.envBeats;
        float  e = 1.0f;
        if(attack_ > 0 && x < attack_)
            e = x > 0 ? (float)(x / attack_) : 0.0f;
        if(gate_ < 1 && x > gate_)
        {
            double minRel = 0.003 * bpm_ / 60.0 / v.envBeats;
            double rel    = release_ > minRel ? release_ : minRel;
            e             = fminf(e, x < gate_ + rel ? (float)(1.0 - (x - gate_) / rel) : 0.0f);
        }
        return e;
    }

    void Fade(Voice &v, float target, float seconds)
    {
        v.target = target;
        float n  = seconds * sr_;
        v.step   = (target - v.gain) / (n < 1 ? 1 : n);
    }

    long Slices() const
    {
        int beats = kBarsOption[barsOpt_] ? kBarsOption[barsOpt_] * 4 : cur_->autoBeats;
        return (long)beats * 4;
    }

    double BaseAdvance() const { return half_ ? 0.5 : 1.0; }   // break 16ths per song 16th (HALF TIME)
    double Advance() const { return fx_ == FX_HALF ? BaseAdvance() * 0.5 : BaseAdvance(); }   // as played now

    // ---- waveform ----

    // Worker: a loaded break's waveform, kWaveCols columns of RMS level (0..kWaveLevels-1, against the break's
    // loudest column; RMS rather than peak, so kicks and snares stand above the hats and ghost notes), handed to the
    // audio thread.
    void PublishWave(const WavData &w)
    {
        float rms[kWaveCols] = {}, top = 1e-9f;
        for(int c = 0; c < kWaveCols; c++)
        {
            int    a = (int)((long long)w.frames * c / kWaveCols), b = (int)((long long)w.frames * (c + 1) / kWaveCols);
            double sum = 0;
            for(int i = a; i < b; i++)
                sum += 0.5 * ((double)w.l[i] * w.l[i] + (double)w.r[i] * w.r[i]);
            rms[c] = b > a ? (float)sqrt(sum / (b - a)) : 0.0f;
            top    = fmaxf(top, rms[c]);
        }
        for(int c = 0; c < kWaveCols; c++)
            waveLevel_[c].store((uint8_t)lroundf(rms[c] / top * (kWaveLevels - 1)), std::memory_order_relaxed);
        waveRev_.fetch_add(1, std::memory_order_release);
    }

    // Audio thread, each block: the columns as they should show (the newest break's levels, the playing slice's
    // columns lit); a changed column is reported to the host by TakeMovedParam.
    void UpdateWave()
    {
        unsigned r = waveRev_.load(std::memory_order_acquire);
        if(r != waveSeen_)
        {
            waveSeen_ = r;
            for(int c = 0; c < kWaveCols; c++)
                waveLv_[c] = waveLevel_[c].load(std::memory_order_relaxed);
        }
        int lo = -1, hi = -1;
        if(playing_ && anchored_ && segOn_ && cur_)
        {
            long n  = Slices(), sl = ((segSlice_ % n) + n) % n;
            lo      = (int)(sl * kWaveCols / n);
            hi      = (int)((sl + 1) * kWaveCols / n);
            hi      = hi > lo ? hi : lo + 1;
        }
        for(int c = 0; c < kWaveCols; c++)
        {
            int v = waveLv_[c] + (c >= lo && c < hi ? kWaveLevels : 0);
            if(v != waveShown_[c])
                waveShown_[c] = v, waveDirty_[c] = true;
        }
    }

  public:
    bool EngineOwned(int i) { return i >= P_WAVE_1 && i <= P_WAVE_128; }

    bool TakeMovedParam(int &index, float &value)
    {
        // an old knob moved a switch: its position now, so a later set of the switch itself isn't undone
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
        int b = movedBreak_.exchange(-1), s = movedSeed_.exchange(-1);
        if(b >= 0 || s >= 0)
        {
            if(b >= 0 && s >= 0)
                movedSeed_.store(s);   // the next call
            index = b >= 0 ? P_BREAK : P_SEED;
            value = (float)(b >= 0 ? b : s);
            return true;
        }
        for(int c = 0; c < kWaveCols; c++)
            if(waveDirty_[c])
            {
                waveDirty_[c] = false;
                index         = P_WAVE_1 + c;
                value         = (float)waveShown_[c];
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

    // ---- pad effects ----

    // The held pads changed: the newest held effect, and the newest held octave pad, apply from the next block
    // (Midi runs between blocks).
    void UpdateFx(int pressedNote)
    {
        int fx = FX_NONE, oct = FX_NONE, ramp = FX_NONE, speed = FX_NONE, vel = FX_NONE, unmapped = -1;
        for(int i = 0; i < heldCount_; i++)
        {
            int f = heldFx_[i];
            if(IsOctave(f))
                oct = f;
            else if(IsRamp(f))
                ramp = f;
            else if(IsSpeed(f))
                speed = f;
            else if(IsVel(f))
                vel = f;
            else if(f != FX_NONE)
                fx = f;
            else
                unmapped = heldNote_[i];
        }
        bool mods  = oct != FX_NONE || ramp != FX_NONE || speed != FX_NONE || vel != FX_NONE;
        int  shown = fx != FX_NONE ? fx : (!mods && unmapped >= 0 ? 100 + unmapped : -1);
        if(pressedNote >= 0 && FxForNote(pressedNote) == FX_NONE)
            shown = 100 + pressedNote;
        wantFx_ = fx, wantOct_ = oct, wantRamp_ = ramp, wantShown_ = shown;
        wantSpeed_ = speed, wantVel_ = vel;
        if(padLen_ == 0 || pressedNote >= 0 || !anchored_)
        {
            releasePending_ = false;
            fxByPress_      = pressedNote >= 0;
            CommitFx();
            return;
        }
        // let go, with a LENGTH: the effect plays on to the end of its length, a whole number of LENGTHs from
        // the 16th it started on (at least one), and what the pads still hold takes over there
        if(!releasePending_)
        {
            double r        = nextPpq_ * 4.0;   // song 16ths, at the next block
            long   n        = (long)ceil((r - fxStartK_) / padLen_ - 1e-9);
            fxEndK_         = fxStartK_ + (n < 1 ? 1 : n) * padLen_;
            releasePending_ = true;
        }
    }

    // What the held pads want becomes what plays (and what the tiles and readout show).
    void CommitFx()
    {
        if(wantShown_ != shownPad_.load() || wantOct_ != shownOct_.load() || wantRamp_ != shownRamp_.load()
           || wantSpeed_ != shownSpeed_.load() || wantVel_ != shownVel_.load())
        {
            shownPad_.store(wantShown_);
            shownOct_.store(wantOct_);
            shownRamp_.store(wantRamp_);
            shownSpeed_.store(wantSpeed_);
            shownVel_.store(wantVel_);
            rev_++;
        }
        if(wantFx_ != fx_)
        {
            fx_         = wantFx_;
            fxDirty_    = true;
            fxEntering_ = true;
        }
        float octMul = wantOct_ == FX_OCT_UP ? 2.0f : wantOct_ == FX_OCT_DOWN ? 0.5f : 1.0f;
        if(octMul != octMul_)
        {
            octMul_  = octMul;
            fxDirty_ = true;   // restart what plays now at the new pitch
        }
        if(wantRamp_ != ramp_)
        {
            // a ramp starts from the pitch it finds (0, or where the other ramp pad got to) at its first grain;
            // let go, what plays now restarts at the plain pitch
            rampBase_    = ramp_ != FX_NONE && wantRamp_ != FX_NONE ? rampSemis_ : 0.0f;
            rampPending_ = wantRamp_ != FX_NONE;
            if(wantRamp_ == FX_NONE)
            {
                rampSemis_ = 0.0f;
                fxDirty_   = true;
            }
            ramp_ = wantRamp_;
        }
        if(wantSpeed_ != speed_)
        {
            // a speed pad starts from the held roll's rate (or where the other speed pad got to) at the next frame;
            // let go, the roll goes back to its grid, or the break carries on from where it is
            speedMulBase_ = speed_ != FX_NONE && wantSpeed_ != FX_NONE ? speedMul_ : 1.0;
            speedPending_ = wantSpeed_ != FX_NONE;
            bool off      = wantSpeed_ == FX_NONE;
            speed_        = wantSpeed_;
            if(off)
            {
                speedMul_ = 1.0;
                if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)
                    fxRollG_ = (long)floor(nowPpq_ / fxRollBeats_ + 1e-9);
                else
                    fxDirty_ = true;
            }
        }
        if(wantVel_ != vel_)
        {
            // from full (down) or silence (up), or where the other velocity pad got to; let go, full again
            velBase_    = vel_ != FX_NONE && wantVel_ != FX_NONE ? velLevel_ : wantVel_ == FX_VEL_UP ? 0.0 : 1.0;
            velPending_ = wantVel_ != FX_NONE;
            vel_        = wantVel_;
            if(vel_ == FX_NONE)
                velLevel_ = 1.0;
        }
    }

    // The speed pads: a song position's rate multiplier (from the base), stepping on from where it started.
    double SpeedMul(double p)
    {
        double d   = (p - speedStartPpq_) * rampSpeed_ / 12.0;   // RAMP octaves since it started
        double mul = speedMulBase_ * exp2(speed_ == FX_SPEED_UP ? d : -d);
        double base = SpeedBase();
        return mul * base > kSpeedMax ? kSpeedMax / base : mul * base < kSpeedMin ? kSpeedMin / base : mul;
    }

    // hits per beat the speed pads start from: the held roll's, else 16ths
    double SpeedBase() const { return fx_ == FX_ROLL ? 8.0 : fx_ == FX_TRIPLET ? 6.0 : 4.0; }

    // The velocity pads: the level of a hit starting now (0..1, squared into gain), from where it started.
    float VelGain()
    {
        if(vel_ == FX_NONE)
            return 1.0f;
        if(velPending_)
        {
            velPending_  = false;
            velStartPpq_ = nowPpq_;
        }
        double d  = (nowPpq_ - velStartPpq_) * rampSpeed_ / 12.0;   // a full sweep per RAMP octave's time
        double l  = velBase_ + (vel_ == FX_VEL_UP ? d : -d);
        velLevel_ = l < 0 ? 0 : l > 1 ? 1 : l;
        return (float)(velLevel_ * velLevel_);
    }

    // A held ramp pad: the semitones the grain starting now plays at (its first grain, where it starts from).
    float RampSemis()
    {
        if(ramp_ == FX_NONE)
            return 0.0f;
        if(rampPending_)
        {
            rampPending_   = false;
            rampStartPpq_  = nowPpq_;
        }
        float d    = (float)(nowPpq_ - rampStartPpq_) * rampSpeed_;
        float st   = rampBase_ + (ramp_ == FX_RAMP_UP ? d : -d);
        rampSemis_ = st > kRampMax ? kRampMax : st < -kRampMax ? -kRampMax : st;
        return rampSemis_;
    }

    // What the break plays at break position `pos` (the variation's choice for its 16th).
    variation::Step DecideAt(double pos) const
    {
        long slices = Slices();
        long plen   = pattern_ < 0 ? slices : pattern_;
        // EVOLVE never repeats: each pass of the sequence (the break restarts with it) varies differently
        return variation::Decide(seed_, (long)floor(pos), slices, plen, var_, 0, pattern_ == 0 ? restarts_ : 0);
    }

    // The sequence starts (again) at song 16th k (Play, a loop wrap, a locate): the break from its top at the bar,
    // so a start mid-bar is that far into the break.
    void SeqStart(long k)
    {
        long bar  = (k >= 0 ? k : k - 15) / 16 * 16;
        breakPos_ = (k - bar) * BaseAdvance();
        if(fx_ == FX_HALF)
            fxPos_ = breakPos_;
    }

    // An effect starts at song position `p`: Stutter and the rolls hold the slice playing now (at a 16th, the one
    // starting), Half starts from this 16th's place in the break.
    void EnterFx(double p, bool atBoundary)
    {
        flamDone_ = false;
        if(!cur_)
            return;
        if(fx_ == FX_STUTTER || fx_ == FX_TRIPLET || fx_ == FX_ROLL)
            CaptureSlice(atBoundary);
        if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)
        {
            fxRollBeats_ = fx_ == FX_TRIPLET ? 1.0 / 6.0 : 0.125;
            fxRollG_     = (long)floor(p / fxRollBeats_ + 1e-9);   // the next grid point is the first hit
        }
        if(fx_ == FX_HALF)
            fxPos_ = breakPos_;
    }

    // The slice a repeating effect holds: the one playing, or at a 16th the one starting.
    void CaptureSlice(bool atBoundary)
    {
        if(segOn_ && !atBoundary)
            capSlice_ = segSlice_, capRev_ = segRev_;
        else
        {
            variation::Step d = DecideAt(breakPos_);
            capSlice_         = d.slice >= 0 ? d.slice : (long)floor(breakPos_) % Slices();
            capRev_           = d.reversed != (fx_ == FX_REVERSE);
        }
    }

    // Apply an effect change at block start `p`: restart what plays now, mid-16th, under the new effect (the
    // break kept its place, so letting go of a pad lands where it would have been). A change within 5 ms of the
    // next 16th waits for it, so a quantized pad note a block early doesn't restart twice.
    void ApplyFx(double p)
    {
        fxDirty_ = false;
        nowPpq_  = p;
        bool press = fxByPress_;
        fxByPress_ = false;
        if(padLen_ && press)
        {
            // a press on the nearest 16th: early in one, as if pressed on it; in its second half, on the next
            fxStartK_ = lastK_;
            if(p * 4.0 - lastK_ >= 0.5)
            {
                fxStartK_     = lastK_ + 1;
                fxAtBoundary_ = true;
                return;
            }
        }
        else if((lastK_ + 1) / 4.0 - p < 0.005 * bpm_ / 60.0)
        {
            fxAtBoundary_ = true;   // the next 16th enters it
            return;
        }
        if(fxEntering_)
        {
            fxEntering_ = false;
            EnterFx(p, false);
            if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)   // a roll starts on its next grid point
                return;
        }
        else if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)   // an octave change: the roll's next hit takes it
            return;
        if(!cur_)
            return;
        double boundary = lastK_ / 4.0;
        Trigger(boundary, p > boundary ? p - boundary : 0.0, true);
    }

    // Flam: a quieter hit of the next 16th's slice just before it (the grace note).
    void Grace(double p)
    {
        double          pos = breakPos_ + BaseAdvance();
        variation::Step d   = DecideAt(pos);
        if(d.kind == variation::GAP)
            return;
        startGain_ = kFlamGain;
        StartSegment(d.slice, d.reversed, pos - floor(pos), p, 0.0, 0.0003f, (lastK_ + 1) / 4.0 - p);
        startGain_ = 1.0f;
    }

    // A song 16th starts at song position `boundary`; `intoBeats` past it (0 except on Play, or `mid`: a pad
    // effect changed during it). The break is at breakPos_: its 16th floor(breakPos_) plays, from the fraction of
    // it already passed (half time's second song 16th). What plays is the variation's choice for that break 16th
    // (Variation.h); it is decided again on the second half, with the same answer, but only its first half arms a
    // Roll. A held pad effect changes that.
    void Trigger(double boundary, double intoBeats, bool mid = false, bool seqStart = false)
    {
        // A newly loaded break takes over where the sequence starts (Play, a loop wrap, a locate), so it starts
        // from its top on the sequence's; the first one at once
        if(!mid && (seqStart || !cur_) && staleCount_ < kStale)
            if(Break *nb = pending_.exchange(nullptr))
            {
                if(cur_)
                    stale_[staleCount_++] = cur_;
                cur_ = nb;
            }
        rollArmed_ = false;
        flamDone_  = false;
        if(!cur_)
        {
            Fade(voices_[active_], 0.0f, 0.003f);
            segOn_ = false;
            return;
        }
        float fadeIn = intoBeats > 0.001 ? 0.003f : 0.0003f;
        if(speed_ != FX_NONE && fx_ != FX_GAP)
            return;   // the speed pad's hits play
        switch(fx_)
        {
            case FX_TRIPLET:
            case FX_ROLL: return;   // the roll grid plays
            case FX_GAP:
                Fade(voices_[active_], 0.0f, 0.003f);
                segOn_ = false;
                return;
            case FX_STUTTER: StartSegment(capSlice_, capRev_, 0.0, boundary, intoBeats, fadeIn); return;
            default: break;
        }
        double          pos    = fx_ == FX_HALF ? fxPos_ : breakPos_;
        long            step   = (long)floor(pos);
        double          frac   = pos - step;   // 0, or a fraction in half time
        variation::Step d      = DecideAt(pos);
#ifdef BREAKS_DEBUG
        if(frac == 0 && fx_ == FX_NONE && !mid)
            fprintf(stderr, "step %ld kind %d slice %ld rev %d at %.4f into %.4f\n", step, (int)d.kind, d.slice,
                    (int)d.reversed, boundary, intoBeats);
#endif
        if(d.kind == variation::GAP)
        {
            Fade(voices_[active_], 0.0f, 0.003f);
            segOn_ = false;
            return;
        }
        StartSegment(d.slice, d.reversed != (fx_ == FX_REVERSE), frac, boundary, intoBeats, fadeIn);
        if(fx_ == FX_NONE && !mid && frac == 0 && d.kind == variation::ROLL && intoBeats < 0.125)
        {
            rollArmed_ = true;
            rollPpq_   = boundary + 0.125;
            rollSlice_ = d.slice;
            segBeats_  = 0.125;   // the envelope covers each of the two hits
            voices_[active_].envBeats = segBeats_;
        }
    }

    // Play slice `s` of the current break from `frac` of the way in (backwards from its end when reversed),
    // a stretch segment that starts at song position `boundary`, `intoBeats` past it. Without grains that is one
    // voice playing on at the PITCH rate (the classic slice player); with grains NextGrain() restarts it every
    // grain at where the break has moved to by then.
    void StartSegment(long s, bool reversed, double frac, double boundary, double intoBeats, float fadeIn,
                      double beats = 0.25)
    {
        segBeats_    = beats > 1e-6 ? beats : 1e-6;   // the envelope's length: a 16th, a roll hit, a grace note
        segSlice_    = s;
        segRev_      = reversed;
        segOn_       = true;
        segGain_     = startGain_ * VelGain();
        segPpq_      = boundary;
        segSliceLen_ = (double)cur_->wav.frames / Slices();
        segFrac_     = frac * segSliceLen_;
        // break frames per output frame: one song 16th covers Advance() break 16ths
        segStretch_ = Advance() * segSliceLen_ / (60.0 / bpm_ / 4.0 * sr_);
        grainBeats_ = grain_ == 0 ? 0.0 : grain_ < 0 ? -grain_ : grain_ / 1000.0 * bpm_ / 60.0;
        segGrainFrames_ = grainBeats_ * 60.0 / bpm_ * sr_;
        grainFade_      = grainBeats_ > 0 ? fminf(0.003f, (float)(segGrainFrames_ / sr_ / 4.0)) : 0.003f;
        // output frames since the boundary; the epsilon in floor(ppq * 4 + 1e-9) can make it a hair below 0,
        // which on slice 0 put the start before the file and silenced the voice
        double elapsed = intoBeats > 0 ? intoBeats * 60.0 / bpm_ * sr_ : 0.0;
        long   g       = 0;
        if(grainBeats_ > 0 && segGrainFrames_ > 0)
        {
            g = (long)floor(elapsed / segGrainFrames_);   // Play mid-16th: the grain we are in
            elapsed -= g * segGrainFrames_;
        }
        grainIdx_     = g;
        nextGrainPpq_ = boundary + (g + 1) * grainBeats_;
        StartVoice(segFrac_ + g * segGrainFrames_ * segStretch_, elapsed, fadeIn, 0.003f, true);
    }

    // The next grain of the segment, unless it would start within its fade of the next restart: the next song
    // 16th, or a held roll's next hit.
    void NextGrain()
    {
        grainIdx_++;
        double start  = segPpq_ + grainIdx_ * grainBeats_;
        nextGrainPpq_ = start + grainBeats_;
        double fadeBeats = grainFade_ * bpm_ / 60.0;
        double restart   = (lastK_ + 1) / 4.0;
        if(speed_ != FX_NONE && fx_ != FX_GAP)
            restart = speedNextPpq_;
        else if(fx_ == FX_TRIPLET || fx_ == FX_ROLL)
            restart = fmin(restart, (fxRollG_ + 1) * fxRollBeats_);
        if(start > restart - fadeBeats)
            return;
        StartVoice(segFrac_ + grainIdx_ * segGrainFrames_ * segStretch_, 0.0, grainFade_, grainFade_, false);
    }

    // Start the segment's slice `offset` break frames in, plus `elapsed` output frames of playback, on the other
    // voice, the old one fading out under it.
    void StartVoice(double offset, double elapsed, float fadeIn, float fadeOut, bool segmentStart)
    {
        Voice &oldV = voices_[active_];
        if(oldV.brk)
            Fade(oldV, 0.0f, fadeOut);
        // the first free voice other than the playing one (in plain playback, simply the other of the first two);
        // when restarts come faster than fades (rolls, grains, pads) the quietest instead of cutting one off
        int next = -1;
        for(int j = 0; j < kVoices && next < 0; j++)
            if(j != active_ && !voices_[j].brk)
                next = j;
        if(next < 0)
        {
            next = active_ == 0 ? 1 : 0;
            for(int j = 0; j < kVoices; j++)
                if(j != active_ && voices_[j].gain < voices_[next].gain)
                    next = j;
        }
        active_  = next;
        Voice &v = voices_[active_];

        long   s    = segSlice_;
        double rate = cur_->wav.sampleRate / sr_ * pitchRatio_ * octMul_;
        if(ramp_ != FX_NONE)
            rate *= exp2f(RampSemis() / 12.0f);
        v.brk       = cur_;
        v.wrap      = half_ || grainBeats_ > 0 || fx_ != FX_NONE || octMul_ != 1.0f || ramp_ != FX_NONE;
        if(segRev_)
        {
            // from the slice's end backwards; that end is usually mid-decay, so fade in a little longer
            v.rate = -rate;
            v.pos  = (s + 1) * segSliceLen_ - 1 - offset - elapsed * rate;
            if(segmentStart)
                fadeIn = 0.0025f;
        }
        else
        {
            v.rate = rate;
            v.pos  = s * segSliceLen_ + offset + elapsed * rate;
        }
        v.gain = 0;
        Fade(v, segGain_, fadeIn);
        v.envStart = segPpq_;
        v.envBeats = segBeats_;
    }

    // Breaks no voice reads any more go back to the worker to be freed.
    void Retire()
    {
        for(int i = 0; i < staleCount_;)
        {
            Break *b     = stale_[i];
            bool   inUse = false;
            for(auto &v : voices_)
                inUse |= v.brk == b;
            if(!inUse && retired_.load() == nullptr)
            {
                retired_.store(b);
                stale_[i]           = stale_[--staleCount_];
                stale_[staleCount_] = nullptr;
            }
            else
                i++;
        }
    }

    // One step per pass: a scan, a load, or freeing what the audio thread retired. Anything that can throw
    // (allocation, a huge file) is caught per pass: an exception escaping a thread would abort MPC. Frees
    // and big destructors run after mu_ is released, since the audio thread may want mu_ (SetParam).
    void WorkerMain()
    {
        bool        first = true;
        std::string loadedName;
        for(;;)
        {
            Break                  *toFree[2] = {nullptr, nullptr};
            std::vector<BreakEntry> oldList;
            bool                    failed = false;
            try
            {
                std::unique_lock<std::mutex> lk(mu_);
                if(stop_)
                    break;
                if(first || rescan_)
                {
                    first   = false;
                    rescan_ = false;
                    if(!status_.empty())
                        loadedName.clear();   // retry a missing or unreadable file after a rescan
                    lk.unlock();
                    const char *media = getenv("BREAKS_MEDIA_ROOT");   // tests point it at a fixture
                    auto        found = breaklib::Scan(dataDir_, media ? media : "/media");
                    lk.lock();
                    oldList.swap(list_);
                    list_.swap(found);
                    scanned_ = true;
                    ResolveIndexLocked();
                    PublishLocked();
                }
                else if(!wantName_.empty() && wantName_ != loadedName)
                {
                    std::string want = wantName_, path;
                    for(auto &e : list_)
                        if(e.name == want)
                            path = e.path;
                    loadedName = want;
                    if(path.empty())
                        SetStatusLocked(("Not found: " + want).c_str());
                    else
                    {
                        lk.unlock();
                        std::unique_ptr<Break> b(new Break());
                        const char            *err = wavfile::Load(path.c_str(), b->wav);
                        if(!*err)
                        {
                            b->name     = want;
                            double secs = b->wav.frames / b->wav.sampleRate;
                            b->autoBeats = breaklib::GuessBeats(secs, b->wav.acidBeats, b->wav.acidTempo, want,
                                                                &b->autoBpm);
                            PublishWave(b->wav);
                        }
                        lk.lock();
                        if(*err)
                            SetStatusLocked((want + ": " + err).c_str());
                        else
                        {
                            // "4 bars" for whole bars, else "13 beats": an odd length is not a bug
                            char info[64];
                            int  n = b->autoBeats % 4 ? b->autoBeats : b->autoBeats / 4;
                            snprintf(info, sizeof info, "  %.0f BPM  %d %s%s", b->autoBpm, n,
                                     b->autoBeats % 4 ? "beat" : "bar", n == 1 ? "" : "s");
                            shown_ = breaklib::DisplayName(want) + info;
                            status_.clear();
                            statusText_[0] = 0;
                            // a newer load replaces one the audio thread never took
                            toFree[0] = pending_.exchange(b.release());
                            PublishLocked();
                        }
                    }
                }
                else if(!(toFree[1] = retired_.exchange(nullptr)))
                    cv_.wait_for(lk, std::chrono::milliseconds(50));
            }
            catch(...)
            {
                failed = true;
            }
            if(failed)
            {
                std::lock_guard<std::mutex> g(mu_);
                SetStatusLocked("Out of memory loading this break");
            }
            delete toFree[0];
            delete toFree[1];
            if(failed)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // Readout text for the current state into the next slot, then tell the wrapper (display_rev). Fixed
    // buffers, no allocation, so it is also safe in the out-of-memory path. Under mu_.
    void PublishLocked()
    {
        int   next = (textSlot_.load() + 1) % kTextSlots;
        char *t    = text_[next];
        if(!scanned_)
            snprintf(t, kTextLen, "Scanning...");
        else if(!statusText_[0] && list_.empty())
            snprintf(t, kTextLen, "No breaks: add WAVs to the plugin's breaks folder");
        else if(statusText_[0])
            snprintf(t, kTextLen, "%s", statusText_);
        else
            snprintf(t, kTextLen, "%s", shown_.c_str());
        textSlot_.store(next);
        rev_++;
    }

    void SetStatusLocked(const char *s)
    {
        snprintf(statusText_, sizeof statusText_, "%s", s);
        try
        {
            status_ = s;
        }
        catch(...)
        {
        }
        PublishLocked();
    }

    // After a scan: a break known by name (a project restore, or one already playing when files are
    // added) keeps its file and moves its index; one chosen by index before the list existed gets its
    // name. A missing name stays as it is and shows "Not found".
    void ResolveIndexLocked()
    {
        if(list_.empty())
            return;
        if(byName_ && !wantName_.empty())
        {
            for(size_t i = 0; i < list_.size(); i++)
                if(list_[i].name == wantName_ && (int)i != breakIndex_)
                {
                    breakIndex_ = (int)i;
                    changedIndex_.store((int)i);
                }
            return;
        }
        if(breakIndex_ >= (int)list_.size())
        {
            breakIndex_ = (int)list_.size() - 1;
            changedIndex_.store(breakIndex_);
        }
        wantName_ = list_[breakIndex_].name;
        byName_   = true;
    }

    // Worker / screen side, under mu_
    std::mutex              mu_;
    std::condition_variable cv_;
    std::thread             worker_;
    bool                    stop_ = false, rescan_ = false, scanned_ = false, byName_ = false;
    std::string             dataDir_, wantName_, status_, shown_;
    char                    statusText_[96] = "";
    std::vector<BreakEntry> list_;
    int                     breakIndex_ = 0;

    // Read lock-free from the audio thread (Display / DisplayRev / TakeChangedParam)
    static constexpr int  kTextSlots = 4, kTextLen = 96;
    char                  text_[kTextSlots][kTextLen] = {"Scanning..."};
    std::atomic<int>      textSlot_{0}, changedIndex_{-1};
    std::atomic<int>      movedBreak_{-1}, movedSeed_{-1};   // stepped by PREV / NEXT, to report (TakeMovedParam)
    std::atomic<int>      bitsStep_{crushstep::kBitsN - 1}, rateStep_{crushstep::kRateN - 1};   // the switches
    std::atomic<bool>     bitsMoved_{false}, rateMoved_{false};   // set by an old knob, to report
    std::atomic<unsigned> rev_{0};

    // Hand-over between worker and audio
    std::atomic<Break *> pending_{nullptr}, retired_{nullptr};

    // Audio side
    static constexpr int kStale = 4;
    Break               *cur_ = nullptr;
    Break               *stale_[kStale] = {};
    int                  staleCount_    = 0;
    static constexpr int kVoices = 4;
    Voice                voices_[kVoices];
    int                  active_    = 0;
    float                startGain_ = 1, segGain_ = 1;   // a new segment's level (a flam's grace note is quieter)
    double               segBeats_  = 0.25;          // its length, for the envelope
    float                attack_ = 0, gate_ = 1, release_ = 0;   // ATTACK / GATE / RELEASE, as shares of it
    float                sr_ = kMpcSampleRate;
    double               ppq_ = -1, bpm_ = 120, nextPpq_ = 0;
    bool                 playing_ = false, anchored_ = false;
    long                 lastK_    = 0;   // last song 16th seen
    long                 loopEndK_ = -1, loopStartK_ = 0;   // the loop, learned from its first wrap (16ths)
    bool                 predicted_ = false;   // the break restarted on the loop end; its wrap is due
    double               predSaved_ = 0;       // how far that moved the break back, if the wrap never comes
    uint32_t             restarts_  = 0;       // passes of the sequence since Play (EVOLVE varies each)
    double               breakPos_ = 0;   // where the break is, in break 16ths
    bool                 half_     = false;
    float                grain_    = 0;   // kGrainOption
    // the playing stretch segment (StartSegment)
    bool                 segOn_ = false, segRev_ = false;
    long                 segSlice_ = 0, grainIdx_ = 0;
    double               segPpq_ = 0, segSliceLen_ = 1, segFrac_ = 0, segStretch_ = 1, segGrainFrames_ = 0;
    double               grainBeats_ = 0, nextGrainPpq_ = 0;
    float                grainFade_  = 0.003f;
    int                  barsOpt_ = 0;
    float                pitchRatio_ = 1;
    Crusher              crush_;
    float                level_ = 1, bits_ = 16, rateKhz_ = 44.1f;
    variation::Settings  var_;
    uint32_t             seed_    = 0;
    long                 pattern_ = -1;   // 16ths, -1 = the break's length, 0 = never repeat
    bool                 rollArmed_ = false;
    double               rollPpq_   = 0;
    long                 rollSlice_ = 0;

    // Pad effects (Midi): held pads in press order, the newest one's effect applies
    static constexpr int kHeld = 8;
    int                  heldNote_[kHeld] = {}, heldFx_[kHeld] = {};
    int                  heldCount_ = 0;
    int                  fx_        = FX_NONE;
    bool                 fxDirty_   = false;           // apply fx_ at the next block start
    bool                 fxAtBoundary_ = false;        // ...or at the next 16th, when the change came just before it
    bool                 fxEntering_ = false;          // fx_ itself changed (not just the octave): EnterFx when applied
    float                octMul_     = 1.0f;           // the held octave pad: 0.5, 1 or 2 times the playback rate
    int                  padLen_     = 4;              // LENGTH in song 16ths (0 = free)
    int                  wantFx_ = FX_NONE, wantOct_ = FX_NONE, wantRamp_ = FX_NONE, wantShown_ = -1;   // the held pads
    int                  ramp_        = FX_NONE;      // the held ramp pad
    int                  wantSpeed_ = FX_NONE, wantVel_ = FX_NONE, speed_ = FX_NONE, vel_ = FX_NONE;   // speed / velocity
    bool                 speedPending_ = false, velPending_ = false;   // starting at the next frame / hit
    bool                 speedOn16th_ = false, speedCapture_ = false;   // it starts on a 16th; take the slice at its first hit
    double               speedStartPpq_ = 0, speedMulBase_ = 1, speedMul_ = 1, speedNextPpq_ = 0;
    double               velStartPpq_ = 0, velBase_ = 1, velLevel_ = 1;
    float                rampSpeed_   = 3.0f;         // RAMP: semitones per beat
    float                rampBase_    = 0, rampSemis_ = 0;   // where it started from; the latest grain's
    bool                 rampPending_ = false;        // its first grain is still to come
    double               rampStartPpq_ = 0, nowPpq_ = 0;   // that grain's song position; the frame playing now
    bool                 fxByPress_      = false;      // the pending change is a press (snaps to the nearest 16th)
    bool                 releasePending_ = false;      // let go: wantFx_ etc. take over at the 16th fxEndK_
    long                 fxStartK_ = 0, fxEndK_ = 0;   // the song 16th the effect started on / ends on
    std::atomic<uint8_t> waveLevel_[kWaveCols] = {};   // worker -> audio: the newest break's column levels
    std::atomic<unsigned> waveRev_{0};
    unsigned             waveSeen_ = 0;
    int                  waveLv_[kWaveCols] = {}, waveShown_[kWaveCols] = {};   // audio: levels; shown (+16 lit)
    bool                 waveDirty_[kWaveCols] = {};   // shown changed, not yet reported to the host
    long                 capSlice_  = 0;               // Stutter / rolls: the slice held
    bool                 capRev_    = false;
    double               fxPos_     = 0;               // Half pad: the break's position, in break 16ths
    double               fxRollBeats_ = 0.125;         // Triplet / Roll: retrigger every
    long                 fxRollG_   = 0;               // last roll grid point played
    bool                 flamDone_  = false;           // this 16th's grace note has started
    std::atomic<int>     shownOct_{FX_NONE};          // readout / tiles: the held octave pad
    std::atomic<int>     shownRamp_{FX_NONE};         // and ramp pad
    std::atomic<int>     shownSpeed_{FX_NONE}, shownVel_{FX_NONE};   // and speed and velocity pads
    std::atomic<int>     shownPad_{-1};                // readout: an effect, or 100 + an unmapped note
    bool                 screenHeld_[16] = {};         // PADS-page tiles held by touch
    bool                 started_        = false;      // audio has run: tile sets from then on are touches
};

MPC_DAISY_ENGINE(BreaksDsp)

#ifdef WRAP_TRACE
// A diagnostic build ("defines": {"WRAP_TRACE": 1}): every setParameter MPC sends (S) and each changed read-back (G),
// except the waveform columns, with the ms since load, into /tmp/breaks_trace.log (stops at 2 MB).
#include <time.h>
extern "C" void wrap_trace(int kind, int idx, float v)
{
    static FILE           *log;
    static long            bytes;
    static struct timespec t0;
    static float           last[NPARAMS];
    if(!log)
    {
        log = fopen("/tmp/breaks_trace.log", "a");
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for(auto &l : last)
            l = -2;
    }
    if(!log || bytes > (2L << 20) || idx < 0 || idx >= NPARAMS || !strncmp(PARAMS[idx].key, "wave_", 5))
        return;
    if(kind == 1)
    {
        if(fabsf(v - last[idx]) < 1e-6f)
            return;
        last[idx] = v;
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long ms = (t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
    bytes += fprintf(log, "%ld %c %s %.6f %.4f\n", ms, kind ? 'G' : 'S', PARAMS[idx].key, v,
                     PARAMS[idx].min + v * (PARAMS[idx].max - PARAMS[idx].min));
    fflush(log);
}
#endif
