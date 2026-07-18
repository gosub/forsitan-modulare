// imber_dsp.hpp — shared primitives for the imber/sylla pair: seeded RNG,
// urn picker, bounded drunk walker, the one scale table everything snaps
// to, and small DSP helpers used by the generators and the engine.
//
// Pure C++11, no Rack dependencies — the offline harness compiles this
// standalone. Everything lives in structs / inline functions so the
// headers can be included from more than one module without ODR trouble.
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace imber_dsp {

static const float kPi = 3.14159265358979f;
static const float kTau = 6.28318530717959f;

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// ---------------------------------------------------------------- RNG ---

// xorshift64* — one instance per domain (bank, constellation, timing,
// voices) so reseeding the bank never disturbs the timing feel
struct Rng {
    uint64_t s;
    Rng() : s(0x9e3779b97f4a7c15ull) {}
    void seed(uint64_t v) { s = v ? v : 0x9e3779b97f4a7c15ull; next(); next(); }
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    float uniform() { return (float)((next() >> 40) * (1.0 / 16777216.0)); }
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }
    // inclusive
    int irange(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + (int)(next() % (uint64_t)(hi - lo + 1));
    }
    float bipolar() { return uniform() * 2.f - 1.f; }
    bool chance(float p) { return uniform() < p; }
};

// random-without-immediate-repeat picker (Haiku's urn model, small state)
struct Urn {
    int last;
    Urn() : last(-1) {}
    int pick(Rng& rng, int n) {
        if (n <= 1) return 0;
        int v = rng.irange(0, n - 1);
        if (v == last)
            v = (v + 1 + rng.irange(0, n - 2)) % n;
        last = v;
        return v;
    }
};

// bounded random walk — the "drunk" at the heart of Haiku's timing
struct Drunk {
    float v;
    Drunk() : v(0.f) {}
    float step(Rng& rng, float stepSize, float lo, float hi) {
        v = clampf(v + rng.bipolar() * stepSize, lo, hi);
        return v;
    }
};

// -------------------------------------------------------------- scale ---

// one pentatonic-minor table on D — the tonal glue; everything the
// generators pitch goes through here (tuned by ear, not recovered)
static const int kScaleRoot = 50;                       // D3
static const int kScaleDegrees[5] = {0, 3, 5, 7, 10};   // minor pentatonic
static const int kChordDegrees[4] = {0, 3, 7, 10};      // m7 stack

inline float midiToFreq(float m) {
    return 440.f * std::pow(2.f, (m - 69.f) / 12.f);
}

// random scale note as a frequency, octaves relative to the root octave
inline float pickFreq(Rng& rng, int octLo, int octHi) {
    int oct = rng.irange(octLo, octHi);
    int deg = kScaleDegrees[rng.irange(0, 4)];
    return midiToFreq((float)(kScaleRoot + 12 * oct + deg));
}

inline float chordFreq(Rng& rng, int voice, int oct) {
    int deg = kChordDegrees[voice & 3];
    return midiToFreq((float)(kScaleRoot + 12 * oct + deg));
}

// ------------------------------------------------------------ filters ---

struct OnePoleLp {
    float z;
    OnePoleLp() : z(0.f) {}
    void setTau(float fc, float sr) { a = 1.f - std::exp(-kTau * fc / sr); }
    float process(float x) { z += a * (x - z); return z; }
    void reset() { z = 0.f; }
    float a = 0.5f;
};

struct OnePoleHp {
    OnePoleLp lp;
    void setTau(float fc, float sr) { lp.setTau(fc, sr); }
    float process(float x) { return x - lp.process(x); }
    void reset() { lp.reset(); }
};

// RBJ biquad, direct form 1
struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    Biquad() : b0(1), b1(0), b2(0), a1(0), a2(0), x1(0), x2(0), y1(0), y2(0) {}
    void reset() { x1 = x2 = y1 = y2 = 0.f; }
    void setLp(float fc, float q, float sr) {
        float w = kTau * clampf(fc, 10.f, 0.45f * sr) / sr;
        float c = std::cos(w), s = std::sin(w), alpha = s / (2.f * q);
        float a0 = 1.f + alpha;
        b0 = (1.f - c) * 0.5f / a0; b1 = (1.f - c) / a0; b2 = b0;
        a1 = -2.f * c / a0; a2 = (1.f - alpha) / a0;
    }
    void setHp(float fc, float q, float sr) {
        float w = kTau * clampf(fc, 10.f, 0.45f * sr) / sr;
        float c = std::cos(w), s = std::sin(w), alpha = s / (2.f * q);
        float a0 = 1.f + alpha;
        b0 = (1.f + c) * 0.5f / a0; b1 = -(1.f + c) / a0; b2 = b0;
        a1 = -2.f * c / a0; a2 = (1.f - alpha) / a0;
    }
    // constant skirt gain bandpass (Haiku's BPF flavor)
    void setBp(float fc, float q, float sr) {
        float w = kTau * clampf(fc, 10.f, 0.45f * sr) / sr;
        float c = std::cos(w), s = std::sin(w), alpha = s / (2.f * q);
        float a0 = 1.f + alpha;
        b0 = s * 0.5f / a0; b1 = 0.f; b2 = -b0;
        a1 = -2.f * c / a0; a2 = (1.f - alpha) / a0;
    }
    float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// ------------------------------------------------------- misc helpers ---

inline float softLimit(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

inline float smoothstepf(float t) {
    t = clampf(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// reflect the signal past ±thresh (wavefolder)
inline float fold(float x, float thresh) {
    if (thresh < 1e-4f) return 0.f;
    while (x > thresh || x < -thresh) {
        if (x > thresh) x = 2.f * thresh - x;
        if (x < -thresh) x = -2.f * thresh - x;
    }
    return x;
}

// quantize to 2^bits levels
inline float crush(float x, float bits) {
    float lv = std::pow(2.f, bits);
    return std::floor(x * lv + 0.5f) / lv;
}

// ------------------------------------------------ buffer-level helpers ---

inline void fadeEdges(std::vector<float>& b, float sr, float ms) {
    int n = (int)b.size();
    int f = std::min(n / 2, (int)(ms * 0.001f * sr));
    for (int i = 0; i < f; i++) {
        float g = 0.5f - 0.5f * std::cos(kPi * i / f);
        b[i] *= g;
        b[n - 1 - i] *= g;
    }
}

inline void normalizePeak(std::vector<float>& b, float peak) {
    float m = 0.f;
    for (size_t i = 0; i < b.size(); i++)
        m = std::max(m, std::fabs(b[i]));
    if (m > 1e-9f) {
        float g = peak / m;
        for (size_t i = 0; i < b.size(); i++)
            b[i] *= g;
    }
}

inline void safetyClip(std::vector<float>& b) {
    for (size_t i = 0; i < b.size(); i++) {
        float x = b[i];
        if (!(x > -1e6f && x < 1e6f)) x = 0.f;   // catches NaN/inf too
        b[i] = softLimit(x);
    }
}

// the shared lo-fi grunge pass: gentle drive, a whisper of hiss, and on
// some buffers a light bit/SR bruise
inline void dirtify(std::vector<float>& b, Rng& rng, float sr, float amount) {
    if (amount <= 0.f) return;
    float drive = 1.f + amount * rng.range(0.2f, 0.8f);
    float hiss = amount * 0.0015f;
    bool bruise = rng.chance(0.4f * amount);
    float bits = rng.range(7.f, 10.f);
    int hold = rng.irange(1, 3);
    int hc = 0;
    float held = 0.f;
    for (size_t i = 0; i < b.size(); i++) {
        float x = std::tanh(b[i] * drive) / std::tanh(drive);
        if (bruise) {
            if (hc == 0) held = crush(x, bits);
            if (++hc >= hold) hc = 0;
            x = held;
        }
        b[i] = x + rng.bipolar() * hiss;
    }
}

} // namespace imber_dsp
