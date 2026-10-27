#ifndef BIQUAD_EQ_H
#define BIQUAD_EQ_H

#include <cmath>

// ─── 4-band parametric EQ (biquad) ───────────────────────────────────────────
// Bands: low shelf 100Hz | peak 500Hz Q1.4 | peak 2kHz Q1.4 | high shelf 8kHz
// All EQ cookbook formulas from R. Bristow-Johnson.
// Cascaded biquads: each band processes the output of the previous band.

struct BiquadEq {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;

    float Process(float x) {
        float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

    void SetLowShelf(float f0, float dBgain, float sr) {
        float A    = powf(10.f, dBgain / 40.f);
        float w0   = 2.f * 3.14159265f * f0 / sr;
        float cosw = cosf(w0), sinw = sinf(w0);
        float alpha = sinw * 0.70710678f; // slope S=1
        float sqA  = sqrtf(A);
        float a0r  = 1.f / ((A+1.f) + (A-1.f)*cosw + 2.f*sqA*alpha);
        b0 =  A * ((A+1.f) - (A-1.f)*cosw + 2.f*sqA*alpha) * a0r;
        b1 = 2.f*A * ((A-1.f) - (A+1.f)*cosw) * a0r;
        b2 =  A * ((A+1.f) - (A-1.f)*cosw - 2.f*sqA*alpha) * a0r;
        a1 = -2.f * ((A-1.f) + (A+1.f)*cosw) * a0r;
        a2 = ((A+1.f) + (A-1.f)*cosw - 2.f*sqA*alpha) * a0r;
    }

    void SetHighShelf(float f0, float dBgain, float sr) {
        float A    = powf(10.f, dBgain / 40.f);
        float w0   = 2.f * 3.14159265f * f0 / sr;
        float cosw = cosf(w0), sinw = sinf(w0);
        float alpha = sinw * 0.70710678f;
        float sqA  = sqrtf(A);
        float a0r  = 1.f / ((A+1.f) - (A-1.f)*cosw + 2.f*sqA*alpha);
        b0 =  A * ((A+1.f) + (A-1.f)*cosw + 2.f*sqA*alpha) * a0r;
        b1 = -2.f*A * ((A-1.f) + (A+1.f)*cosw) * a0r;
        b2 =  A * ((A+1.f) + (A-1.f)*cosw - 2.f*sqA*alpha) * a0r;
        a1 =  2.f * ((A-1.f) - (A+1.f)*cosw) * a0r;
        a2 = ((A+1.f) - (A-1.f)*cosw - 2.f*sqA*alpha) * a0r;
    }

    void SetPeak(float f0, float Q, float dBgain, float sr) {
        float A    = powf(10.f, dBgain / 40.f);
        float w0   = 2.f * 3.14159265f * f0 / sr;
        float cosw = cosf(w0), sinw = sinf(w0);
        float alpha = sinw / (2.f * Q);
        float a0r  = 1.f / (1.f + alpha/A);
        b0 = (1.f + alpha*A) * a0r;
        b1 = (-2.f * cosw) * a0r;
        b2 = (1.f - alpha*A) * a0r;
        a1 = (-2.f * cosw) * a0r;
        a2 = (1.f - alpha/A) * a0r;
    }
};

#endif // BIQUAD_EQ_H
