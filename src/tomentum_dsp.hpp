// tomentum_dsp.hpp — the four-stage transistor fuzz, free of Rack headers so
// test/tomentum_probe can measure it directly.
//
// Circuit values follow ElectroSmash's analysis of the USA V3 Big Muff Pi
// (https://www.electrosmash.com/big-muff-pi-analysis): four common-emitter
// stages, the middle two clipped by antiparallel silicon diodes in their
// collector-base feedback, with the passive tone network between the third
// and fourth. See doc/tomentum.md for what is and is not modelled.

#pragma once

#include <algorithm>
#include <cmath>

namespace tomentum {

static const float kPi = 3.14159265358979323846f;

static inline float clampf(float x, float lo, float hi) {
    return std::max(lo, std::min(x, hi));
}

// ── published operating points ──────────────────────────────────────────────
// Stage gains are the *measured* figures from the analysis, not -Rf/Ri: the
// transistors' open-loop gain is only about 66, so the feedback pair falls
// well short of the 470k/10k the resistors suggest.
static const float kBoostGain = 6.84f;    // 16.7 dB, simulated not calculated
static const float kClip1Gain = 14.1f;    // 23 dB
static const float kClip2Gain = 17.8f;    // 25 dB
static const float kRecoverGain = 4.47f;  // 13 dB, offsetting the tone stack

// Both clipping stages' pole pairs are published; the booster's and the
// recovery stage's are not, and these are plausible values for the coupling
// and Miller capacitances rather than measurements.
static const float kBoostHp = 20.f, kBoostLp = 8000.f;
static const float kClip1Hp = 55.f, kClip1Lp = 1780.f;
static const float kClip2Hp = 94.f, kClip2Lp = 1170.f;
static const float kRecoverHp = 20.f, kRecoverLp = 8000.f;

// The feedback resistor the diodes sit across.
static const float kRf = 470e3f;

// The sustain pot cannot reach zero: a series resistance keeps a trickle in.
static const float kSustainFloor = 0.035f;

// ── tone stack, the values that set its two corners ─────────────────────────
// bass leg 39k + 10n -> 408 Hz, treble leg 3.9n + 22k -> 1855 Hz, 100k pot.
// Rs is the collector resistance driving the network and RL the volume pot
// loading it; both change the insertion loss and are part of why the pedal is
// dark.
static const float kToneRb = 39e3f, kToneCb = 10e-9f;
static const float kToneRt = 22e3f, kToneCt = 3.9e-9f;
static const float kTonePot = 100e3f, kToneRs = 15e3f, kToneRl = 100e3f;

// ── transistor rails ────────────────────────────────────────────────────────
// A 9 V supply with the collector sitting near the middle. The clipping
// stages never get near this (their diodes clamp at well under a volt) but
// the booster and the recovery stage do, and asymmetrically: the collector
// bottoms out a couple of hundred millivolts above ground and runs out of
// current before it reaches the rail going up.
static const float kRailHi = 3.6f, kRailLo = 4.3f;

// Asymptotic soft clip, strictly increasing, unit slope at each knee.
static inline float railClip(float x, float hi, float lo) {
    if (x > hi) { const float t = x - hi; return hi + hi * t / (hi + t); }
    if (x < -lo) { const float t = -lo - x; return -lo - lo * t / (lo + t); }
    return x;
}

// ── one-pole TPT filters ────────────────────────────────────────────────────
struct OnePoleLP {
    float g = 0.1f, z = 0.f;
    void set(float sr, float fc) {
        const float w = std::tan(kPi * clampf(fc, 1.f, 0.45f * sr) / sr);
        g = w / (1.f + w);
    }
    void reset() { z = 0.f; }
    float process(float x) {
        const float v = (x - z) * g;
        const float y = v + z;
        z = y + v;
        return y;
    }
};

struct OnePoleHP {
    OnePoleLP lp;
    void set(float sr, float fc) { lp.set(sr, fc); }
    void reset() { lp.reset(); }
    float process(float x) { return x - lp.process(x); }
};

struct DCBlock {
    float xz = 0.f, yz = 0.f, r = 0.9995f;
    void set(float sr) { r = 1.f - 2.f * kPi * 8.f / sr; }
    void reset() { xz = yz = 0.f; }
    float process(float x) {
        yz = x - xz + r * yz;
        xz = x;
        return yz;
    }
};

// ── the diode clipper ───────────────────────────────────────────────────────
// The 1 uF capacitor in series with each diode pair is there to block the DC
// bias, and with 470k across it its corner is a third of a hertz: at audio it
// is a short. The feedback clipper is therefore *memoryless*, and the whole
// stage is the static solution of
//
//     v + Rf * Id(v) = w,        Id(v) = 2 Is sinh(v / nVt),   w = -gain * in
//
// which is worth knowing, because it means the transfer curve can be solved
// once into a table instead of Newton-iterated every sample. The index is
// companded as sign(w) sqrt(|w|), which puts millivolt resolution across the
// knee and still covers a stage input that has run away to tens of volts.
struct DiodeTable {
    static const int kSize = 4096;
    float v[kSize + 1];
    float tmax = 8.f;      // sqrt of the largest |w| the table covers
    float scale = 0.f;     // index per unit of the companded axis

    static float current(float v, float Is, float nVt) {
        // 2 Is sinh(v/nVt), with the exponent held to something finite
        const float a = clampf(v / nVt, -60.f, 60.f);
        return Is * (std::exp(a) - std::exp(-a));
    }

    void build(float Is, float nVt, float wmax) {
        tmax = std::sqrt(wmax);
        scale = 0.5f * kSize / tmax;
        float guess = 0.f;
        for (int i = 0; i <= kSize; i++) {
            const float t = -tmax + i * (2.f * tmax / kSize);
            const float w = (t < 0.f ? -1.f : 1.f) * t * t;
            // Newton from the previous grid point; f is strictly increasing
            // and convex away from zero, so this converges in a few steps.
            float x = guess;
            for (int it = 0; it < 60; it++) {
                const float a = clampf(x / nVt, -60.f, 60.f);
                const float ep = std::exp(a), em = std::exp(-a);
                const float f = x + kRf * Is * (ep - em) - w;
                const float fp = 1.f + kRf * Is * (ep + em) / nVt;
                const float step = f / fp;
                x -= clampf(step, -0.5f, 0.5f);
                if (std::fabs(step) < 1e-9f) break;
            }
            v[i] = x;
            guess = x;
        }
    }

    inline float lookup(float w) const {
        const float t = (w < 0.f ? -1.f : 1.f) * std::sqrt(std::fabs(w));
        const float p = clampf((t + tmax) * scale, 0.f, (float)kSize - 1.001f);
        const int i = (int)p;
        const float f = p - i;
        return v[i] + f * (v[i + 1] - v[i]);
    }
};

// Diode choices. Silicon is the pedal's own 1N4148, with the Shockley
// parameters fitted in the DAFx-22 Wasp paper that vespae already uses.
// Germanium and LED are the two mods people actually do; "lifted" is the
// third, and removes the pair altogether, leaving the transistor to clip
// against its own supply.
enum DiodeType { DIODE_SILICON, DIODE_GERMANIUM, DIODE_LED, DIODE_LIFTED };
static const int kDiodeTypes = 3;   // LIFTED needs no table

struct DiodeSpec { float Is, nVt; };
static const DiodeSpec kDiodes[kDiodeTypes] = {
    {2.52e-9f, 1.752f * 25.85e-3f},   // 1N4148, ~0.6 V
    {5.0e-7f, 45.0e-3f},              // germanium, ~0.35 V: earlier, softer
    {6.0e-12f, 90.0e-3f},             // red LED, ~1.7 V: louder, cleaner
};

// Built once for the whole plugin: they depend on nothing the user can turn,
// so the cost of solving them lands on the first module ever created and
// never again. Function-local statics are thread-safe in C++11.
inline const DiodeTable* diodeTables() {
    static DiodeTable* tables = []() {
        DiodeTable* t = new DiodeTable[kDiodeTypes];
        for (int i = 0; i < kDiodeTypes; i++)
            t[i].build(kDiodes[i].Is, kDiodes[i].nVt, 64.f);
        return t;
    }();
    return tables;
}

// ── the passive tone stack ──────────────────────────────────────────────────
// Two shunt-terminated branches mixed by the pot, solved as one biquad rather
// than approximated by two independent first-order filters — which is what
// makes the notch land in the right place, move with the knob, and take the
// right amount of level with it. H(s) comes from nodal analysis of
//
//   Vi -Rs- V1 -Rb- B -Cb- gnd,  V1 -Ct- T -Rt- gnd,  pot from B to T,
//   wiper -> out, loaded by RL
//
// t = 0 puts the wiper on the bass node, t = 1 on the treble node.
struct ToneStack {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;

    void reset() { x1 = x2 = y1 = y2 = 0.f; }

    void set(float t, float sr) {
        t = clampf(t, 0.f, 1.f);
        const float Rb = kToneRb, Cb = kToneCb, Rt = kToneRt, Ct = kToneCt;
        const float Rp = kTonePot, Rs = kToneRs, RL = kToneRl;
        const float Rp2 = Rp * Rp;

        const float n2 = -Cb * Ct * RL * Rb * Rp * Rt * t;
        const float n1 = -Ct * RL * Rt * (Rb + Rp);
        const float n0 = RL * (Rp * t - Rp - Rt);

        const float d2 = Cb * Ct * RL * (Rb * (Rp * (-Rs - Rt) - Rs * Rt) - Rp * Rs * Rt)
            + t * (Cb * Ct * t * (Rb * Rp2 * (Rs + Rt) + Rp2 * Rs * Rt)
                   + Cb * Ct * (Rb * Rp * (Rp * (-Rs - Rt) - Rs * Rt) - Rp2 * Rs * Rt));
        const float d1 = Cb * RL * (Rb * (-Rp - Rt) - Rp * Rs - Rs * Rt)
            + Ct * (RL * (Rb * (-Rs - Rt) + Rp * (-Rs - Rt))
                    + Rb * (Rp * (-Rs - Rt) - Rs * Rt) - Rp * Rs * Rt)
            + t * (Cb * (Rb * Rp * (-Rp - Rt) + Rp * (-Rp * Rs - Rs * Rt))
                   + Ct * (Rb * Rp * (Rs + Rt) + Rp2 * (-Rs - Rt))
                   + t * (Cb * (Rb * Rp2 + Rp2 * Rs) + Ct * Rp2 * (Rs + Rt)));
        const float d0 = RL * (-Rb - Rp - Rs - Rt) + Rb * (-Rp - Rt) - Rp * Rs - Rs * Rt
            + t * (Rb * Rp + Rp2 * t + Rp * (-Rp + Rs - Rt));

        // bilinear transform, s -> c (1 - z^-1) / (1 + z^-1)
        const float c = 2.f * sr;
        const float c2 = c * c;
        const float A0 = d2 * c2 + d1 * c + d0;
        const float inv = 1.f / A0;
        b0 = (n2 * c2 + n1 * c + n0) * inv;
        b1 = (2.f * n0 - 2.f * n2 * c2) * inv;
        b2 = (n2 * c2 - n1 * c + n0) * inv;
        a1 = (2.f * d0 - 2.f * d2 * c2) * inv;
        a2 = (d2 * c2 - d1 * c + d0) * inv;
    }

    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// ── one channel of pedal ────────────────────────────────────────────────────
struct Voice {
    OnePoleHP boostHp, clip1Hp, clip2Hp, recoverHp;
    OnePoleLP boostLp, clip1Lp, clip2Lp, recoverLp;
    ToneStack tone;
    DCBlock dc;
    float clipEnv = 0.f;      // how hard the diodes are working, for the LED

    void setRate(float sr) {
        boostHp.set(sr, kBoostHp);     boostLp.set(sr, kBoostLp);
        clip1Hp.set(sr, kClip1Hp);     clip1Lp.set(sr, kClip1Lp);
        clip2Hp.set(sr, kClip2Hp);     clip2Lp.set(sr, kClip2Lp);
        recoverHp.set(sr, kRecoverHp); recoverLp.set(sr, kRecoverLp);
        dc.set(sr);
    }

    void reset() {
        boostHp.reset(); clip1Hp.reset(); clip2Hp.reset(); recoverHp.reset();
        boostLp.reset(); clip1Lp.reset(); clip2Lp.reset(); recoverLp.reset();
        tone.reset(); dc.reset();
        clipEnv = 0.f;
    }

    struct Params {
        float sustain = 0.65f;
        float mids = 0.f;
        float volume = 0.7f;
        float bias = 0.f;         // operating-point offset, in volts at the stage
        const DiodeTable* table = nullptr;   // null = diodes lifted
    };

    // One stage of feedback-clipped gain.
    float clipStage(float x, float gain, float bias, OnePoleHP& hp, OnePoleLP& lp,
                    const DiodeTable* table, float& clipAmt) {
        const float filtered = lp.process(hp.process(x));
        const float w = -(gain * filtered) + bias;
        if (!table) return railClip(w, kRailHi, kRailLo);
        const float v = table->lookup(w);
        const float aw = std::fabs(w);
        if (aw > 1e-6f) clipAmt = std::max(clipAmt, 1.f - std::fabs(v) / aw);
        return v;
    }

    // input in pedal volts, output in pedal volts
    float process(float in, const Params& p) {
        // input booster: coupling, gain, its own rails, Miller rolloff
        float x = railClip(kBoostGain * boostHp.process(in), kRailHi, kRailLo);
        x = boostLp.process(x);

        // sustain sets how hard the pair is driven, not where they clip
        x *= kSustainFloor + (1.f - kSustainFloor) * p.sustain;

        float clipAmt = 0.f;
        x = clipStage(x, kClip1Gain, p.bias, clip1Hp, clip1Lp, p.table, clipAmt);
        x = clipStage(x, kClip2Gain, -p.bias, clip2Hp, clip2Lp, p.table, clipAmt);
        clipEnv += (clipAmt - clipEnv) * 0.001f;

        // the passive network, plus the mids mod: blending the pre-tone signal
        // back in fills the notch, which is what a tone-bypass switch does
        // 0.18 is the difference between the network's passband loss and its
        // notch, so at full the scoop is filled rather than merely dented.
        const float pre = x;
        x = tone.process(x) + p.mids * 0.18f * pre;

        // recovery stage and volume
        x = recoverLp.process(recoverHp.process(x));
        x = railClip(kRecoverGain * x, kRailHi, kRailLo);
        return dc.process(x) * p.volume;
    }
};

} // namespace tomentum
