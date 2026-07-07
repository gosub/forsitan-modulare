#pragma once
// draen_engines.hpp — the drone engines and their registry.
//
// Each engine is a faithful C++ port of a dronecaster SynthDef
// (github.com/northern-information/dronecaster, GPL-3.0), keeping the original
// author credit. The common contract mirrors the SC synths: given a fundamental
// `hz` and amplitude `amp`, render one stereo sample. The host (dræn) owns the
// fade envelope and engine switching, exactly as dronecaster's SynthSocket does.

#include "draen_ugens.hpp"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace draen {

struct DroneEngine {
    virtual ~DroneEngine() {}
    virtual const char* name() const = 0;
    // (re)seed and clear all state; called when this engine becomes active.
    virtual void init(uint32_t seed) = 0;
    // render one stereo sample. `st` is the sample time (1/sampleRate).
    virtual void process(float hz, float amp, float st, float& outL, float& outR) = 0;
};

// ── Sine — @northern-information. { |hz,amp| (SinOsc.ar(hz)*amp).dup } ─────────
struct SineEngine : DroneEngine {
    SinOsc osc;
    const char* name() const override { return "sine"; }
    void init(uint32_t) override { osc.reset(); }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = osc.process(hz, st) * amp;
    }
};

// ── Square — @taubaland. { |hz,amp| (Pulse.ar(hz,0.5)*amp).dup } ──────────────
struct SquareEngine : DroneEngine {
    BlPulse osc;
    const char* name() const override { return "square"; }
    void init(uint32_t) override { osc.reset(); }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = osc.process(hz, 0.5f, st) * amp;
    }
};

// ── Triangle — @taubaland. { |hz,amp| (LFTri.ar(hz)*amp).dup } ────────────────
struct TriangleEngine : DroneEngine {
    LFTri osc;
    const char* name() const override { return "triangle"; }
    void init(uint32_t) override { osc.reset(); }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = osc.process(hz, st) * amp;
    }
};

// ── Supersaw — @cfdrake. Five band-passed saws, spread with Splay. ────────────
//   Splay.ar(Array.fill(5, { |i|
//     BPF.ar(Saw.ar(hz*i + SinOsc.kr(0.1*i,0,0.5)),
//            100 + (i*100) + SinOsc.kr(0.05*i, mul:100), 2) }), 1) * amp
struct SupersawEngine : DroneEngine {
    static constexpr int N = 5;
    BlSaw  saw[N];
    SinOsc freqLfo[N];      // SinOsc.kr(0.1*i, 0, 0.5): saw detune
    SinOsc bpfLfo[N];       // SinOsc.kr(0.05*i, mul:100): filter sweep
    Biquad bpf[N];
    const char* name() const override { return "supersaw"; }
    void init(uint32_t) override {
        for (int i = 0; i < N; ++i) {
            saw[i].reset(); freqLfo[i].reset(); bpfLfo[i].reset(); bpf[i].reset();
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float ch[N];
        for (int i = 0; i < N; ++i) {
            float detune = freqLfo[i].process(0.1f * i, st) * 0.5f;
            float sawHz  = hz * i + detune;
            float s      = saw[i].process(sawHz, st);
            float center = 100.f + i * 100.f + bpfLfo[i].process(0.05f * i, st) * 100.f;
            ch[i] = bpf[i].bpf(s, center, 2.f, st);   // SC rq = 2
        }
        splay(ch, N, 1.f, 0.f, l, r);
        l *= amp; r *= amp;
    }
};

// ── harm's way — @moonblind. 16 harmonics, each slowly amplitude-modulated. ───
//   Splay.ar((SinOsc.ar(hz*n)/n) * SinOsc.kr({Rand(0.001,0.02)}!16),
//            0.5, amp, SinOsc.kr(0.001))
struct HarmsWayEngine : DroneEngine {
    static constexpr int N = 16;
    SinOsc osc[N];
    SinOsc am[N];
    SinOsc centerOsc;
    float amRate[N] = {};
    const char* name() const override { return "harm's way"; }
    void init(uint32_t seed) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            osc[i].reset();
            // random AM start phase (the rates are already random): avoids the
            // degenerate all-zero start where every slow modulator sits at sin(0)
            am[i].reset(rng.uniform());
            amRate[i] = linlin(rng.uniform(), 0.f, 1.f, 0.001f, 0.02f);
        }
        centerOsc.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float ch[N];
        for (int i = 0; i < N; ++i) {
            int n = i + 1;
            ch[i] = osc[i].process(hz * n, st) / n * am[i].process(amRate[i], st);
        }
        float center = centerOsc.process(0.001f, st);
        splay(ch, N, 0.5f, center, l, r);
        const float makeup = 2.5f;   // normalise toward the other engines' level
        l *= amp * makeup; r *= amp * makeup;
    }
};

// ── THX — @infinitedigits. The Deep Note: 12 saws sweep from a random cluster ─
// to a target chord as `amp` rises (amp doubles as the sweep position, exactly
// as in the original). earslap.com/article/recreating-the-thx-deep-note.html
struct ThxEngine : DroneEngine {
    static constexpr int V = 12;
    BlSaw   saw[V];
    Biquad  bpf[V];
    LFNoise2 initN[V], destN[V];
    float fund[V] = {}, sweepF[V] = {}, panPos[V] = {};
    const char* name() const override { return "thx"; }
    void init(uint32_t seed) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < V; ++i) {
            fund[i]   = linlin(rng.uniform(), 0.f, 1.f, 100.f, 500.f);
            sweepF[i] = linlin(rng.uniform(), 0.f, 1.f, 0.2f, 2.f);
            panPos[i] = linlin(rng.uniform(), 0.f, 1.f, -0.6f, 0.6f);
            saw[i].reset(); bpf[i].reset();
            initN[i].reset(seed + i * 7 + 1);
            destN[i].reset(seed + i * 13 + 3);
        }
        std::sort(fund, fund + V, std::greater<float>());   // descending
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = 0.f; r = 0.f;
        for (int t = 1; t < V; ++t) {                        // voices 1..11
            float ir = fund[t] + initN[t].process(0.5f, st) * (6.f * (V - (t + 1)));
            float fp = hz * std::pow(2.f, std::round(t / 2.f) - 3.f);
            float dr = fp + destN[t].process(0.1f, st) * (t / 2.f);
            float sw = std::pow(amp, sweepF[t]);
            float freq = (1.f - sw) * ir + sw * dr;
            float s = bpf[t].blowpass(saw[t].process(freq, st), freq * 6.f, 0.6f, st);
            float lv = std::pow(1.f - 1.f / (t + 1), 4.f) + 0.4f;
            float pl, pr; pan2(s, panPos[t], lv, pl, pr);
            l += pl / V; r += pr / V;
        }
        // amp is the sweep position (above), not level; a fixed makeup gain
        // brings the Deep Note up to the other engines' output level
        const float makeup = 18.f;
        l = l / 10.f * makeup; r = r / 10.f * makeup;
    }
};

// ── Hecker — @infinitedigits. Two stereo banks of 16 filtered noise voices ────
// slowly morphing between white and pink, band-passed around the fundamental.
struct HeckerEngine : DroneEngine {
    struct Voice {
        WhiteNoise wn; PinkNoise pn;
        LFNoise0 nSel, nCut, nRq;
        Lag lSel, lCut, lRq;
        Biquad lpf, bpf;
        void reset(uint32_t seed) {
            wn.reset(seed); pn.reset(seed * 2654435761u + 1u);
            nSel.reset(seed + 11u); nCut.reset(seed + 23u); nRq.reset(seed + 37u);
            lSel.reset(); lCut.reset(); lRq.reset();
            lpf.reset(); bpf.reset();
        }
        float process(float hz, float st) {
            float sel = lSel.process(nSel.process(0.1f, st), 10.f, st) * 0.5f + 0.5f;
            float s = selectx(sel, wn.process(), pn.process());
            float cutoff = linexp(lCut.process(nCut.process(0.1f, st), 10.f, st), -1.f, 1.f, 20.f, 20000.f);
            s = lpf.lpf(s, cutoff, st);
            float rq = linexp(lRq.process(nRq.process(0.1f, st), 10.f, st), -1.f, 1.f, 0.0001f, 0.02f);
            return bpf.bpf(s, hz, rq, st);
        }
    };
    Voice left[16], right[16];
    const char* name() const override { return "hecker"; }
    void init(uint32_t seed) override {
        for (int i = 0; i < 16; ++i) {
            left[i].reset(seed + i * 101u + 1u);
            right[i].reset(seed + i * 211u + 1009u);
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sl = 0.f, sr = 0.f;
        for (int i = 0; i < 16; ++i) { sl += left[i].process(hz, st); sr += right[i].process(hz, st); }
        const float makeup = 3.f;    // normalise toward the other engines' level
        l = std::tanh(sl * 100.f) * amp / 8.f * makeup;
        r = std::tanh(sr * 100.f) * amp / 8.f * makeup;
    }
};

// ── registry ─────────────────────────────────────────────────────────────────
// Phase 1 roster: the four engines that need only Tier-1 UGENs. Grows as the
// UGEN library fills out (see project notes / CHANGELOG).
inline std::vector<std::unique_ptr<DroneEngine>> makeEngines() {
    std::vector<std::unique_ptr<DroneEngine>> v;
    v.emplace_back(new SineEngine());
    v.emplace_back(new SquareEngine());
    v.emplace_back(new TriangleEngine());
    v.emplace_back(new SupersawEngine());
    v.emplace_back(new HarmsWayEngine());
    v.emplace_back(new ThxEngine());
    v.emplace_back(new HeckerEngine());
    return v;
}

}  // namespace draen
