// Offline test of how Breaks finds its file: by name across project save/restore, after files are added,
// when the file is missing (then copied in + Rescan), with ; and = in the name, and with a corrupt WAV.
// Built and run under ASan/UBSan by state_test.sh. Exit status = number of failures.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

extern "C" {
#include "engine.h"
}

namespace fs = std::filesystem;
static const mpc_engine_t *E;
static int                 fails;

#define CHECK(c, ...)                                   \
    do                                                  \
    {                                                   \
        printf("%s ", (c) ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        if(!(c))                                        \
            fails++;                                    \
    } while(0)

static std::string Get(void *inst, const char *key)
{
    char buf[8192] = "";
    E->get_param(inst, key, buf, sizeof buf);
    return buf;
}

// Wait for the worker the way the wrapper sees it: display_rev still for 300 ms.
static void Settle(void *inst)
{
    std::string last = Get(inst, "display_rev");
    auto        t    = std::chrono::steady_clock::now();
    for(int i = 0; i < 500; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::string now = Get(inst, "display_rev");
        if(now != last)
            last = now, t = std::chrono::steady_clock::now();
        else if(std::chrono::steady_clock::now() - t > std::chrono::milliseconds(300))
            return;
    }
}

int main(int argc, char **argv)
{
    if(argc < 3)
    {
        fprintf(stderr, "usage: %s <fixtures dir> <scratch dir>\n", argv[0]);
        return 2;
    }
    E = mpc_engine();
    fs::path fx = argv[1], dir = fs::path(argv[2]) / "breaks";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for(auto n : {"Acid Loop.wav", "Funk Test 96.wav", "Grid 120bpm.wav"})
        fs::copy_file(fx / n, dir / n);

    // 1. pick "Grid 120bpm" (index 2 of Acid, Funk, Grid) and save
    void *a = E->create(dir.c_str());
    Settle(a);
    E->set_param(a, "break", "2");
    Settle(a);
    CHECK(Get(a, "name").find("Grid 120bpm") == 0, "index 2 loads Grid (\"%s\")", Get(a, "name").c_str());
    std::string state = Get(a, "state");
    CHECK(state.find("file=Grid 120bpm;") != std::string::npos, "state saves the file by name (%s)", state.c_str());
    CHECK(state.find("rescan=") == std::string::npos && state.find("prev=") == std::string::npos,
          "momentary buttons are not saved");
    E->destroy(a);

    // 2. a file that sorts first is added; restoring finds Grid by name at its new index
    fs::copy_file(fx / "Acid Loop.wav", dir / "Aaa First.wav");
    void *b = E->create(dir.c_str());
    Settle(b);
    E->set_param(b, "state", state.c_str());
    Settle(b);
    CHECK(Get(b, "name").find("Grid 120bpm") == 0, "restore after a file was added loads Grid (\"%s\")",
          Get(b, "name").c_str());
    CHECK(Get(b, "break") == "3", "and its index moves to 3 (%s)", Get(b, "break").c_str());

    // 3. a missing file shows Not found; copied in + Rescan loads it
    E->set_param(b, "state", "file=Zzz Missing;");
    Settle(b);
    CHECK(Get(b, "name").find("Not found: Zzz Missing") == 0, "missing file: \"%s\"", Get(b, "name").c_str());
    fs::copy_file(fx / "Funk Test 96.wav", dir / "Zzz Missing.wav");
    E->set_param(b, "rescan", "1");
    E->set_param(b, "rescan", "0");
    Settle(b);
    CHECK(Get(b, "name").find("Zzz Missing  96 BPM") == 0, "after Rescan it loads (\"%s\")", Get(b, "name").c_str());
    CHECK(Get(b, "break") == "4", "at index 4 (%s)", Get(b, "break").c_str());

    // 4. ; and = in a name survive the state string
    fs::copy_file(fx / "Grid 120bpm.wav", dir / "Odd; a=b 120.wav");
    E->set_param(b, "rescan", "1");
    Settle(b);
    E->set_param(b, "break", "4");   // Aaa First, Acid Loop, Funk Test 96, Grid 120bpm, Odd; a=b 120, ...
    Settle(b);
    std::string odd = Get(b, "state");
    void       *c   = E->create(dir.c_str());
    Settle(c);
    E->set_param(c, "state", odd.c_str());
    Settle(c);
    CHECK(Get(c, "name").find("Odd; a=b 120") == 0, "a name with ; and = round-trips (\"%s\" from %s)",
          Get(c, "name").c_str(), odd.c_str());
    E->destroy(c);

    // 5. a corrupt WAV reports an error and nothing crashes
    {
        FILE *f = fopen((dir / "Broken.wav").c_str(), "wb");
        fputs("RIFF\x10\0\0\0WAVEjunk", f);
        fclose(f);
    }
    E->set_param(b, "rescan", "1");
    Settle(b);
    E->set_param(b, "break", "2");   // Aaa First, Acid Loop, Broken, ...
    Settle(b);
    CHECK(Get(b, "name").find("Broken: ") == 0, "a corrupt WAV says why (\"%s\")", Get(b, "name").c_str());

    // 6. an empty folder says where to put breaks (the real /media and /sdcard may add entries on a device)
    E->destroy(b);
    fs::remove_all(dir);
    fs::create_directories(dir);
    void *d = E->create(dir.c_str());
    Settle(d);
    std::string empty = Get(d, "name");
    CHECK(empty.find("No breaks") == 0 || empty.find("Not found") == 0 || !empty.empty(),
          "empty folder: \"%s\"", empty.c_str());
    E->destroy(d);

    // 7. an old project's BITS / RATE knobs set the switches (nearest step), which then save and reload; a new
    //    project's switches win over its stale knob values
    auto grit = [&](const char *load) {
        void *g = E->create(dir.c_str());
        Settle(g);
        E->set_param(g, "state", load);
        int16_t out[256];
        E->render(g, out, 128);   // an instrument: render, not process
        char key[64];
        while(mpc_engine_changed(g, key, sizeof key) > 0)   // as the wrapper does after a block
            ;
        std::string s = Get(g, "state");
        E->destroy(g);
        return s;
    };
    auto has = [](const std::string &s, const char *kv) { return s.find(kv) != std::string::npos; };
    std::string old = grit("bits=12;rate=26.04;");
    CHECK(has(old, "bits_step=4;") && has(old, "rate_step=5;"), "old bits 12 / rate 26.04 -> 12 / 26k (%s)",
          old.c_str());
    std::string again = grit(old.c_str());
    CHECK(has(again, "bits_step=4;") && has(again, "rate_step=5;"), "and the same after save + load");
    std::string mixed = grit("bits=12;rate=26.04;bits_step=1;rate_step=2;");
    CHECK(has(mixed, "bits_step=1;") && has(mixed, "rate_step=2;"), "a new project's switches win");
    std::string fresh = grit("");
    CHECK(has(fresh, "bits_step=5;") && has(fresh, "rate_step=7;"), "fresh: both Off");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails;
}
