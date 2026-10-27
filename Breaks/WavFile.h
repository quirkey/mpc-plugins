#pragma once
// ─── WavFile.h ────────────────────────────────────────────────────────────────
// Minimal WAV reader for Breaks. Worker thread only: it allocates and reads files.
//
// Reads PCM 16/24/32-bit and float 32-bit, mono or stereo (more channels: the
// first two), plain or WAVE_FORMAT_EXTENSIBLE, skipping unknown chunks (LIST,
// bext, ...) with RIFF's pad byte after odd sizes. An ACID "acid" chunk gives
// the loop's beat count and tempo when the file has one.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct WavData
{
    std::vector<float> l, r;   // r == l for mono files
    float              sampleRate = 0;
    int                frames     = 0;
    int                acidBeats  = 0;   // 0 = no acid chunk
    float              acidTempo  = 0;
};

namespace wavfile
{
inline uint32_t U32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

inline float Sample(const uint8_t *p, int fmt, int bits)
{
    if(fmt == 3)
    {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    switch(bits)
    {
        case 8: return (p[0] - 128) / 128.0f;
        case 16: return (int16_t)U16(p) / 32768.0f;
        case 24: return (int32_t)((uint32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0f;
        case 32: return (int32_t)U32(p) / 2147483648.0f;
    }
    return 0;
}

// Returns an empty string on success, else why it failed.
inline const char *Load(const char *path, WavData &out, size_t maxBytes = 32u << 20)
{
    FILE *f = fopen(path, "rb");
    if(!f)
        return "can't open";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size < 44 || (size_t)size > maxBytes)
    {
        fclose(f);
        return size < 44 ? "too short" : "too big";
    }
    std::vector<uint8_t> buf((size_t)size);
    size_t               got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if(got != buf.size())
        return "read error";
    const uint8_t *d = buf.data();
    if(memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return "not a WAV";

    int            fmt = 0, ch = 0, bits = 0;
    uint32_t       rate = 0;
    const uint8_t *data = nullptr;
    uint32_t       dataLen = 0;
    for(size_t pos = 12; pos + 8 <= buf.size();)
    {
        const uint8_t *c   = d + pos;
        uint32_t       len = U32(c + 4);
        size_t         avail = buf.size() - pos - 8;
        if(len > avail)
            len = (uint32_t)avail;   // truncated file: use what is there
        if(!memcmp(c, "fmt ", 4) && len >= 16)
        {
            fmt  = U16(c + 8);
            ch   = U16(c + 10);
            rate = U32(c + 12);
            bits = U16(c + 22);
            if(fmt == 0xFFFE && len >= 40)
                fmt = U16(c + 32);   // SubFormat GUID's first two bytes: 1 = PCM, 3 = float
        }
        else if(!memcmp(c, "data", 4))
        {
            data    = c + 8;
            dataLen = len;
        }
        else if(!memcmp(c, "acid", 4) && len >= 24)
        {
            out.acidBeats = (int)U32(c + 8 + 12);
            memcpy(&out.acidTempo, c + 8 + 20, 4);
        }
        pos += 8 + len + (len & 1);
    }
    if(!data || !ch || !rate)
        return "no audio data";
    if(!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
        return "unsupported format";

    int bytes  = bits / 8;
    int stride = bytes * ch;
    out.frames = (int)(dataLen / stride);
    if(out.frames < 64)
        return "too short";
    out.sampleRate = (float)rate;
    out.l.resize(out.frames);
    out.r.resize(out.frames);
    for(int i = 0; i < out.frames; i++)
    {
        const uint8_t *p = data + (size_t)i * stride;
        out.l[i]         = Sample(p, fmt, bits);
        out.r[i]         = ch > 1 ? Sample(p + bytes, fmt, bits) : out.l[i];
    }
    return "";
}
} // namespace wavfile
