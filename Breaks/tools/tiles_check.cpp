// PADS page tiles, through the engine interface as the wrapper sees it (built and run by pads_check.sh):
// "pad_N" is the tile's label, "pad_N_on" lights it while its effect is held, from either pad layout (chromatic
// from 36, MPC's default C major scale from 72), an unmapped tile lights while its pad is pressed, a note from
// neither layout lights none, and keys that aren't tiles don't answer "_on".
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "engine.h"
}

static const mpc_engine_t *eng;
static void               *inst;
static int                 fails;

static std::string Get(const char *key)
{
    char buf[128] = "";
    int  n        = eng->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf, n) : std::string("<none>");
}

static void Expect(const char *key, const char *want, const char *when)
{
    std::string got = Get(key);
    if(got != want)
    {
        printf("FAIL %s: %s = %s, want %s\n", when, key, got.c_str(), want);
        fails++;
    }
}

static void Note(int status, int note)
{
    uint8_t m[3] = {(uint8_t)status, (uint8_t)note, (uint8_t)(status == 0x90 ? 100 : 0)};
    eng->midi(inst, m, 3);
}

// Every tile's _on: lit tiles (1-based) "1", the rest "0".
static void Lit(std::initializer_list<int> lit, const char *when)
{
    for(int t = 1; t <= 16; t++)
    {
        bool on = false;
        for(int l : lit)
            on |= l == t;
        char key[16];
        snprintf(key, sizeof key, "pad_%d_on", t);
        Expect(key, on ? "1" : "0", when);
    }
}

int main()
{
    eng  = mpc_engine();
    inst = eng->create(nullptr);
    const char *labels[16] = {"REVERSE", "GAP", "FLAM", "TRIPLET", "ROLL", "STUTTER", "HALF",    "OCT DOWN",
                              "RAMP DOWN", "RAMP UP", "-", "OCT UP",  "SPEED DOWN", "SPEED UP", "VEL DOWN", "VEL UP"};
    for(int t = 1; t <= 16; t++)
    {
        char key[16];
        snprintf(key, sizeof key, "pad_%d", t);
        Expect(key, labels[t - 1], "labels");
    }
    Lit({}, "nothing held");
    Note(0x90, 36);   // pad 1 (chromatic): reverse
    Lit({1}, "pad 1 held");
    Note(0x90, 79);   // pad 5 on the scale layout (G5): roll, the newest, wins
    Lit({5}, "pad 1 + scale pad 5 held");
    Note(0x80, 79);
    Lit({1}, "scale pad 5 released, pad 1 still held");
    Note(0x90, 36);   // velocity-0 note-on is a note-off
    Note(0x90, 36);
    Note(0x80, 36);
    Lit({}, "all released");
    Note(0x90, 46);   // pad 11: no effect, its tile lights
    Lit({11}, "pad 11 held");
    Note(0x80, 46);
    Note(0x90, 93);   // scale pad 13 (A6): speed down
    Lit({13}, "scale pad 13 held");
    Note(0x80, 93);
    Note(0x90, 60);   // in neither layout: no tile, the readout says the note
    Lit({}, "note 60 held");
    Expect("name", "No breaks: add WAVs to the plugin's breaks folder   [note 60: no effect]", "readout");
    Note(0x80, 60);
    Note(0x90, 73);   // C#5, off the scale: likewise
    Lit({}, "note 73 held");
    Note(0x80, 73);
    Lit({}, "all released");
    Note(0x90, 40);   // pad 5 roll, then pad 12 octave up: both lit (the octave rides on the effect)
    Note(0x90, 47);
    Lit({5, 12}, "pads 5 + 12 held");
    Expect("name", "No breaks: add WAVs to the plugin's breaks folder   [ROLL + OCT UP]", "readout");
    Note(0x90, 43);   // pad 8 octave down, the newest octave pad, wins
    Lit({5, 8}, "pads 5 + 12 + 8 held");
    Note(0x80, 40);
    Lit({8}, "pad 5 released");
    Note(0x80, 43);
    Lit({12}, "pad 8 released");
    Note(0x80, 47);
    Lit({}, "all released");
    Note(0x90, 40);   // pad 5 roll, pad 12 octave up, pad 10 ramp up: all three ride together
    Note(0x90, 47);
    Note(0x90, 45);
    Lit({5, 10, 12}, "pads 5 + 12 + 10 held");
    Expect("name", "No breaks: add WAVs to the plugin's breaks folder   [ROLL + OCT UP + RAMP UP]", "readout");
    Note(0x90, 44);   // pad 9 ramp down, the newest ramp pad, wins
    Lit({5, 9, 12}, "pads 5 + 12 + 10 + 9 held");
    Note(0x80, 40);
    Note(0x80, 47);
    Note(0x80, 44);
    Lit({10}, "only pad 10 still held");
    Expect("name", "No breaks: add WAVs to the plugin's breaks folder   [RAMP UP]", "readout");
    Note(0x80, 45);
    Lit({}, "all released");
    Note(0x90, 86);   // scale pad 9 (D6) and pad 12 (G6): ramp down, octave up
    Note(0x90, 91);
    Lit({9, 12}, "scale pads 9 + 12 held");
    Note(0x80, 86);
    Note(0x80, 91);
    Note(0x90, 40);   // roll, octave up, ramp up, speed up, velocity down: all five ride together
    Note(0x90, 47);
    Note(0x90, 45);
    Note(0x90, 49);
    Note(0x90, 50);
    Lit({5, 10, 12, 14, 15}, "pads 5 + 12 + 10 + 14 + 15 held");
    Expect("name", "No breaks: add WAVs to the plugin's breaks folder   [ROLL + OCT UP + RAMP UP + SPEED UP + VEL DOWN]",
           "readout");
    Note(0x90, 48);   // speed down and velocity up, the newest of their kinds, win
    Note(0x90, 51);
    Lit({5, 10, 12, 13, 16}, "and 13 + 16");
    Note(0xB0, 123);
    Lit({}, "all notes off");
    Note(0x90, 42);   // pad 7: half
    Note(0xB0, 123);  // all notes off
    Lit({}, "all notes off");
    Expect("level_on", "<none>", "not a tile");
    Expect("name_on", "<none>", "a text param that isn't a tile");
    std::string state = Get("state");
    if(state.find("pad_") != std::string::npos)
    {
        printf("FAIL state saves the tiles: %s\n", state.c_str());
        fails++;
    }
    eng->destroy(inst);
    printf(fails ? "tiles: FAILED (%d)\n" : "tiles: ok\n", fails);
    return fails ? 1 : 0;
}
