// imber_dsp.hpp — shared primitives for the imber/sylla pair: seeded RNG,
// urn picker, bounded drunk walker, the scale table everything snaps to,
// and small DSP helpers used by the generators and the engine.
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

// -------------------------------------------------------------- scale ---

// The tonal glue: everything the generators pitch snaps to one table, so
// random material stays musical and two instances always agree. The table
// is selectable per module, defaulting to minor pentatonic on D, which is
// what the whole library was tuned against.
//
// Scale list and order mirror pages64's Scales.hpp. Never reorder it:
// modules serialize the scale by index.
struct ScaleDef {
    const char* name;
    int size;
    int deg[12];
};

static const ScaleDef kScales[] = {
    {"Major",            7, {0, 2, 4, 5, 7, 9, 11}},
    {"Natural minor",    7, {0, 2, 3, 5, 7, 8, 10}},
    {"Harmonic minor",   7, {0, 2, 3, 5, 7, 8, 11}},
    {"Dorian",           7, {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",         7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",           7, {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",       7, {0, 2, 4, 5, 7, 9, 10}},
    {"Major pentatonic", 5, {0, 2, 4, 7, 9}},
    {"Minor pentatonic", 5, {0, 3, 5, 7, 10}},
    {"Blues",            6, {0, 3, 5, 6, 7, 10}},
    {"Whole tone",       6, {0, 2, 4, 6, 8, 10}},
    {"Chromatic",       12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {"Hijaz",            7, {0, 1, 4, 5, 7, 8, 10}},
    {"Byzantine",        7, {0, 1, 4, 5, 7, 8, 11}},
    {"Hirajoshi",        5, {0, 2, 3, 7, 8}},
};
static const int kScaleCount = (int)(sizeof(kScales) / sizeof(kScales[0]));
static const int kDefaultScale = 8;    // minor pentatonic
static const int kDefaultRoot = 2;     // D, i.e. MIDI 50 with the octave below
static const int kRootOctave = 48;     // root note = kRootOctave + 0..11

inline const char* noteName(int i) {
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names[((i % 12) + 12) % 12];
}

// What a render is pitched to. It travels inside the Rng because the Rng is
// already the per-render context handed to all two dozen generators, and it
// has to be per-render rather than global: sylla's worker and imber's bank
// builder render on their own threads.
struct Tuning {
    int root;            // MIDI note of the scale root
    const int* deg;      // scale degrees, in semitones above the root
    int nDeg;
    int chord[4];        // the four chord-voice offsets
    Tuning()
        : root(kRootOctave + kDefaultRoot),
          deg(kScales[kDefaultScale].deg),
          nDeg(kScales[kDefaultScale].size) {
        chord[0] = 0; chord[1] = 3; chord[2] = 7; chord[3] = 10;   // m7 stack
    }
};

// The chord voices are the scale notes nearest an m7 template, ties going
// upward (brighter). On the default minor pentatonic that lands exactly on
// {0, 3, 7, 10}, so the default tuning reproduces every sound rendered
// before the scale was selectable.
inline Tuning makeTuning(int scaleIdx, int rootNote) {
    static const int tmpl[4] = {0, 3, 7, 10};
    Tuning t;
    if (scaleIdx < 0 || scaleIdx >= kScaleCount)
        scaleIdx = kDefaultScale;
    t.root = kRootOctave + ((rootNote % 12) + 12) % 12;
    t.deg = kScales[scaleIdx].deg;
    t.nDeg = kScales[scaleIdx].size;
    for (int v = 0; v < 4; v++) {
        int best = t.deg[0], bd = 128;
        for (int i = 0; i < t.nDeg; i++) {
            int d = t.deg[i] - tmpl[v];
            if (d < 0) d = -d;
            if (d <= bd) { bd = d; best = t.deg[i]; }
        }
        t.chord[v] = best;
    }
    return t;
}

// ---------------------------------------------------------------- RNG ---

// xorshift64* — one instance per domain (bank, constellation, timing,
// voices) so reseeding the bank never disturbs the timing feel
struct Rng {
    uint64_t s;
    Tuning tune;
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

// ------------------------------------------------------------- pitches ---

inline float midiToFreq(float m) {
    return 440.f * std::pow(2.f, (m - 69.f) / 12.f);
}

// random scale note as a frequency, octaves relative to the root octave
inline float pickFreq(Rng& rng, int octLo, int octHi) {
    int oct = rng.irange(octLo, octHi);
    const Tuning& t = rng.tune;
    int deg = t.deg[rng.irange(0, t.nDeg - 1)];
    return midiToFreq((float)(t.root + 12 * oct + deg));
}

inline float chordFreq(Rng& rng, int voice, int oct) {
    const Tuning& t = rng.tune;
    return midiToFreq((float)(t.root + 12 * oct + t.chord[voice & 3]));
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

// quantise to a precomputed level count. Callers whose bit depth only
// moves at control rate (or on sample load) should hold the level and use
// this, rather than paying a pow() per sample inside crush().
inline float crushLv(float x, float lv) {
    return std::floor(x * lv + 0.5f) / lv;
}

inline float crush(float x, float bits) {
    return crushLv(x, std::pow(2.f, bits));
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
