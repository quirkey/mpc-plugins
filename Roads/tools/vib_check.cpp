// Offline check of Roads' old-project params: the vibrato's old Pan / Trem knob sets the switch and DEPTH (told to the
// host), and the removed cab's params (1.0 / 1.1 projects) change nothing, while the EQ that shared its page still
// loads. Built and run by vib_check.sh. Exit status = failures.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "engine.h"
}

static const mpc_engine_t *E;
static int                 fails;

#define CHECK(c, ...)                         \
    do                                        \
    {                                         \
        printf("%s ", (c) ? "ok  " : "FAIL"); \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
        if(!(c))                              \
            fails++;                          \
    } while(0)

static std::string Get(void *inst, const char *key)
{
    char buf[256] = "";
    E->get_param(inst, key, buf, sizeof buf);
    return buf;
}

// What the wrapper does after a block: the params the engine moved
static std::string Moved(void *inst)
{
    char        key[64];
    std::string all;
    while(mpc_engine_changed(inst, key, sizeof key) > 0)
        all += std::string(key) + "=" + Get(inst, key) + ";";
    return all;
}

// A chord into a fresh instance with `state` loaded; the largest sample difference against the same with `other`
static int Diff(const char *state, const char *other)
{
    void *a = E->create(nullptr), *b = E->create(nullptr);
    E->set_param(a, "state", state);
    E->set_param(b, "state", other);
    for(void *v : {a, b})
        for(int n : {48, 55, 60, 64})
        {
            uint8_t on[3] = {0x90, (uint8_t)n, 100};
            E->midi(v, on, 3);
        }
    int     worst = 0;
    int16_t oa[256], ob[256];
    for(int blk = 0; blk < 400; blk++)
    {
        E->render(a, oa, 128);
        E->render(b, ob, 128);
        for(int i = 0; i < 256; i++)
            worst = std::max(worst, std::abs(oa[i] - ob[i]));
    }
    E->destroy(a);
    E->destroy(b);
    return worst;
}

int main()
{
    E = mpc_engine();

    // The vibrato: an old project's bipolar Pan / Trem knob sets the switch and DEPTH (told to the host), which then
    // save and reload; a new project's switch / DEPTH win over its stale knob value
    auto vib = [&](const char *load) {
        void *v = E->create(nullptr);
        E->set_param(v, "state", load);
        Moved(v);
        std::string st = Get(v, "state");
        E->destroy(v);
        return st;
    };
    auto has = [](const std::string &st, const char *kv) { return st.find(kv) != std::string::npos; };
    std::string fresh = vib("");
    CHECK(has(fresh, "vib_mode=1;") && has(fresh, "depth=0;"), "fresh: Trem, depth 0");
    std::string pan = vib("mod=-40;");
    CHECK(has(pan, "vib_mode=0;") && has(pan, "depth=40;"), "old knob -40 -> Pan 40%% (%s)", pan.c_str());
    std::string trem = vib("mod=75;");
    CHECK(has(trem, "vib_mode=1;") && has(trem, "depth=75;"), "old knob +75 -> Trem 75%%");
    std::string again = vib(pan.c_str());
    CHECK(has(again, "vib_mode=0;") && has(again, "depth=40;"), "and the same after save + load");
    std::string mixed = vib("mod=-40;vib_mode=1;depth=20;");
    CHECK(has(mixed, "vib_mode=1;") && has(mixed, "depth=20;"), "a new project's switch and DEPTH win");

    // The removed cab: a 1.1 project's cab settings load without a move and change nothing; its EQ still does
    void       *s = E->create(nullptr);
    const char *old11 = "cab=4;cabmix=100;cab_ir=3;ir=04_Twin_Reverb_-_Balanced.wav;low=6;";
    E->set_param(s, "state", old11);
    std::string m = Moved(s), st = Get(s, "state");
    CHECK(m.empty() && has(st, "low=6;") && !has(st, "ir=04"), "a 1.1 project's cab loads quietly, its EQ kept (%s)",
          st.c_str());
    E->destroy(s);
    int same = Diff("cab=4;cabmix=100;cab_ir=3;ir=04_Twin_Reverb_-_Balanced.wav;", "");
    CHECK(same == 0, "and its cab settings change nothing (largest difference %d)", same);
    int eq = Diff("low=6;", "");
    CHECK(eq > 100, "while the EQ does (largest difference %d)", eq);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails;
}
