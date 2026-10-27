#pragma once
// ─── Variation.h ──────────────────────────────────────────────────────────────
// What Breaks plays on each 16th: the break's own slice, or a variation of it.
//
// A pure function of (seed, the break's running 16th, the settings): no state,
// no allocation, safe on the audio thread. The same seed always gives the same
// variation, so a project reloads exactly as it was saved, PREV/NEW step back
// and forth through variations, and a sequence can later be exported by
// evaluating it ahead of time.
//
// Per 16th, with probability `amount`, one action is picked by weight:
//   Swap     another slice from the same position in the beat (keeps the groove)
//   Repeat   play again what the previous 16th played (stutters, chains up to 8)
//   Reverse  this slice backwards
//   Gap      silence
//   Roll     this slice twice, the second on the 32nd
// The variation repeats every `patternLen` 16ths (the break's length by
// default), or never with patternLen = 0 ("Evolve").
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace variation
{
enum Kind
{
    NORMAL,
    SWAP,
    REPEAT,
    REVERSE,
    GAP,
    ROLL,
};

struct Settings
{
    float amount = 0;                                       // 0..1: chance a 16th varies
    float swap = 0, repeat = 0, reverse = 0, gap = 0, roll = 0;   // relative weights, >= 0
};

struct Step
{
    Kind kind;
    long slice;      // slice to play; -1 for GAP
    bool reversed;   // REVERSE, or a REPEAT of one
};

inline long Mod(long a, long n) { return ((a % n) + n) % n; }

// splitmix64 over (seed, position)
inline uint64_t Hash(uint32_t seed, long pos)
{
    uint64_t z = ((uint64_t)seed << 32) ^ (uint64_t)(int64_t)pos;
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// `salt` (0 = none) gives each pass of a never-repeating pattern its own choices, the positions starting over.
inline Step Decide(uint32_t seed, long count, long slices, long patternLen, const Settings &s, int depth = 0,
                   uint32_t salt = 0)
{
    long natural = Mod(count, slices > 0 ? slices : 1);
    Step normal  = {NORMAL, natural, false};
    float total  = s.swap + s.repeat + s.reverse + s.gap + s.roll;
    if(s.amount <= 0 || total <= 0 || slices < 1)
        return normal;
    long     pos = patternLen > 0 ? Mod(count, patternLen) : count;
    uint64_t h   = Hash(seed ^ salt * 0x9E3779B9u, pos);
    float    u1  = (float)(h & 0xFFFFFF) / 16777216.0f;
    float    u2  = (float)((h >> 24) & 0xFFFFFF) / 16777216.0f;
    uint32_t r3  = (uint32_t)(h >> 48);
    if(u1 >= s.amount)
        return normal;
    float pick = u2 * total;
    if((pick -= s.swap) < 0)
    {
        long groups = slices / 4 > 0 ? slices / 4 : 1;   // slices at this position within a beat
        return {SWAP, Mod((long)(r3 % groups) * 4 + natural % 4, slices), false};
    }
    if((pick -= s.repeat) < 0)
    {
        if(depth >= 8)
            return normal;
        Step prev = Decide(seed, count - 1, slices, patternLen, s, depth + 1, salt);
        return prev.kind == GAP ? prev : Step{REPEAT, prev.slice, prev.reversed};
    }
    if((pick -= s.reverse) < 0)
        return {REVERSE, natural, true};
    if((pick -= s.gap) < 0)
        return {GAP, -1, false};
    return {ROLL, natural, false};
}
} // namespace variation
