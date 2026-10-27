// Unit test of Variation.h's Decide():  built and run by variation_check.sh (also standalone:
//   g++ -std=gnu++17 -I.. variation_test.cpp && ./a.out)
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../Variation.h"

using namespace variation;
static int fails;
#define CHECK(c, ...)                         \
    do                                        \
    {                                         \
        printf("%s ", (c) ? "ok  " : "FAIL"); \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
        if(!(c))                              \
            fails++;                          \
    } while(0)

static bool Same(const Step &a, const Step &b) { return a.kind == b.kind && a.slice == b.slice && a.reversed == b.reversed; }

int main()
{
    const long slices = 64;   // a 4-bar break
    Settings   s;
    s.swap = 60, s.repeat = 40, s.reverse = 25, s.gap = 20, s.roll = 30;

    // Vary 0: exactly the break
    s.amount = 0;
    bool plain = true;
    for(long c = -40; c < 500; c++)
    {
        Step d = Decide(7, c, slices, slices, s);
        plain &= d.kind == NORMAL && d.slice == Mod(c, slices) && !d.reversed;
    }
    CHECK(plain, "Vary 0 plays every slice in order");

    // weights all 0: exactly the break, whatever Vary says
    Settings none;
    none.amount = 1;
    plain       = true;
    for(long c = 0; c < 500; c++)
        plain &= Decide(7, c, slices, slices, none).kind == NORMAL;
    CHECK(plain, "all weights 0 plays the break");

    s.amount = 0.5f;
    // deterministic, and seeds differ
    bool same = true, differ = false;
    for(long c = 0; c < 1000; c++)
    {
        same &= Same(Decide(3, c, slices, 16, s), Decide(3, c, slices, 16, s));
        differ |= !Same(Decide(3, c, slices, 16, s), Decide(4, c, slices, 16, s));
    }
    CHECK(same, "the same seed gives the same variation");
    CHECK(differ, "another seed gives another variation");

    // the pattern repeats at its length (kinds; and whole steps when the pattern is the break's length)
    for(long plen : {16L, 32L, 64L})
    {
        bool rep = true;
        for(long c = 0; c < 400; c++)
        {
            Step a = Decide(11, c, slices, plen, s), b = Decide(11, c + plen, slices, plen, s);
            rep &= a.kind == b.kind;
            if(plen == slices)
                rep &= Same(a, b);
        }
        CHECK(rep, "pattern of %ld sixteenths repeats", plen);
    }
    // Evolve does not repeat at a bar
    bool evolves = false;
    for(long c = 0; c < 400; c++)
        evolves |= Decide(11, c, slices, 0, s).kind != Decide(11, c + 16, slices, 0, s).kind;
    CHECK(evolves, "Evolve keeps changing");

    // counts by kind over a long run, Vary 100
    s.amount = 1;
    long n[6] = {0}, swapPos = 0, gapSlice = 0;
    for(long c = 0; c < 100000; c++)
    {
        Step d = Decide(99, c, slices, 0, s);
        n[d.kind]++;
        if(d.kind == SWAP && d.slice % 4 != Mod(c, slices) % 4)
            swapPos++;
        if(d.kind == GAP && d.slice != -1)
            gapSlice++;
        if(d.slice < -1 || d.slice >= slices)
            gapSlice++;
    }
    CHECK(swapPos == 0, "Swap keeps the position in the beat");
    CHECK(gapSlice == 0, "slices in range; Gap has none");
    CHECK(n[NORMAL] < 2000, "Vary 100 varies (nearly) every 16th (%ld plain, from Repeat chains)", n[NORMAL]);
    // Repeat of a Gap is a Gap, so gaps run a little above their share; the rest follow their weights
    double total = 175.0, runs = 100000;
    CHECK(fabs(n[SWAP] / runs - 60 / total) < 0.03, "Swap share %.3f (want ~%.3f)", n[SWAP] / runs, 60 / total);
    CHECK(fabs(n[ROLL] / runs - 30 / total) < 0.03, "Roll share %.3f (want ~%.3f)", n[ROLL] / runs, 30 / total);
    CHECK(n[REPEAT] > 0 && n[REVERSE] > 0 && n[GAP] > 0, "every kind occurs");

    // Repeat only: chains are cut at 8, never runaway recursion
    Settings rep;
    rep.amount = 1, rep.repeat = 1;
    bool bounded = true;
    for(long c = 0; c < 200; c++)
    {
        Step d = Decide(5, c, slices, 0, rep);
        bounded &= d.kind == REPEAT && d.slice == Mod(c - 8, slices);
    }
    CHECK(bounded, "an all-Repeat run holds the slice from 8 sixteenths back");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails;
}
