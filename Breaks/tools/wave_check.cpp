// The waveform's columns, through the engine interface as the wrapper sees it (built and run by wave_check.sh):
//   wave_check <test_breaks dir>
// On the Grid break (32 sixteenths, each a burst louder than the last) the 128 columns rise from left to right, four
// per slice; while it plays, the playing slice's four columns are lit (+16) and no others; stopped, none are. The
// columns are reported through mpc_engine_changed (each change once), are not saved, and ignore host sets.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include "engine.h"
}

static const mpc_engine_t *eng;
static void               *inst;
static int                 fails;

static std::string Get(const char *key)
{
    char buf[256] = "";
    int  n        = eng->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf, n) : std::string();
}

static int Col(int c)
{
    char key[16];
    snprintf(key, sizeof key, "wave_%d", c + 1);
    return atoi(Get(key).c_str());
}

static void Fail(const char *what)
{
    printf("FAIL %s\n", what);
    fails++;
}

// Drain mpc_engine_changed as the wrapper does each block; returns how many keys it named.
static int Drain()
{
    char key[64];
    int  n = 0;
    while(mpc_engine_changed(inst, key, sizeof key) > 0)
    {
        if(strncmp(key, "wave_", 5))
            Fail("mpc_engine_changed named a key that isn't a column");
        n++;
    }
    return n;
}

static void Block(double ppq, bool playing)
{
    int16_t out[256];
    eng->song_pos(inst, ppq, 120.0, playing ? 1 : 0, 128);
    eng->render(inst, out, 128);
}

int main(int argc, char **argv)
{
    if(argc < 2)
        return 2;
    eng  = mpc_engine();
    inst = eng->create(argv[1]);
    eng->set_param(inst, "break", "2");
    for(int i = 0; i < 300 && Get("name").find("Grid") == std::string::npos; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(Get("name").find("Grid") == std::string::npos)
    {
        printf("FAIL the Grid break didn't load (%s)\n", Get("name").c_str());
        return 1;
    }
    Block(-1, false);   // stopped: the levels arrive, nothing lit
    int named = Drain();
    if(named < 32)
        Fail("the new break's columns were not reported");
    if(Drain() != 0)
        Fail("columns reported twice");
    int prev = -1;
    for(int s = 0; s < 32; s++)
    {
        int a = Col(4 * s), b = Col(4 * s + 3);
        if(a >= 16 || b >= 16)
            Fail("a column lit while stopped");
        if(a < prev || a < 1)
            printf("FAIL slice %d: column level %d after %d (should rise)\n", s, a, prev), fails++;
        prev = a;
    }
    if(Col(124) != 15)
        printf("FAIL the loudest slice's column is %d, not 15\n", Col(124)), fails++;

    // playing: the slice under the song position is lit, and only its four columns
    double ppq = 0;
    for(int b = 0; b < 1200; b++, ppq += 128 * 2.0 / 44100)
    {
        Block(ppq, true);
        Drain();
        if(b % 37 != 20)
            continue;
        int  s   = (int)((ppq + 128 * 2.0 / 44100) * 4) % 32;   // the slice playing at the end of the block
        int  lit = 0;
        bool ok  = true;
        for(int c = 0; c < 128; c++)
            if(Col(c) >= 16)
            {
                lit++;
                ok = ok && c / 4 == s;
            }
        if(lit != 4 || !ok)
            printf("FAIL at ppq %.3f (slice %d): %d columns lit, right ones %d\n", ppq, s, lit, ok), fails++;
    }
    Block(ppq, false);
    Drain();
    for(int c = 0; c < 128; c++)
        if(Col(c) >= 16)
        {
            Fail("a column still lit after stop");
            break;
        }
    int before = Col(10);
    eng->set_param(inst, "wave_11", "31");
    if(Col(10) != before)
        Fail("a host set moved a column");
    if(Get("state").find("wave_") != std::string::npos)
        Fail("state saves the columns");
    eng->destroy(inst);
    printf(fails ? "wave: FAILED (%d)\n" : "wave: ok (Grid columns rise, the playing slice lit, not saved)\n", fails);
    return fails ? 1 : 0;
}
