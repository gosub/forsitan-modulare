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
    virtual void init(uint32_t seed, float sampleRate) = 0;
    // render one stereo sample. `st` is the sample time (1/sampleRate).
    virtual void process(float hz, float amp, float st, float& outL, float& outR) = 0;
};

// ── Sine — @northern-information. { |hz,amp| (SinOsc.ar(hz)*amp).dup } ─────────
struct SineEngine : DroneEngine {
    SinOsc osc;
    const char* name() const override { return "sine"; }
    void init(uint32_t, float) override { osc.reset(); }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = osc.process(hz, st) * amp;
    }
};

// ── Square — @taubaland. { |hz,amp| (Pulse.ar(hz,0.5)*amp).dup } ──────────────
struct SquareEngine : DroneEngine {
    BlPulse osc;
    const char* name() const override { return "square"; }
    void init(uint32_t, float) override { osc.reset(); }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = osc.process(hz, 0.5f, st) * amp;
    }
};

// ── Triangle — @taubaland. { |hz,amp| (LFTri.ar(hz)*amp).dup } ────────────────
struct TriangleEngine : DroneEngine {
    LFTri osc;
    const char* name() const override { return "triangle"; }
    void init(uint32_t, float) override { osc.reset(); }
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
    void init(uint32_t, float) override {
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
    void init(uint32_t seed, float) override {
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
    void init(uint32_t seed, float) override {
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
    void init(uint32_t seed, float) override {
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

// ── Coil — @infinitedigits. "Traversing the tunnels of goats." ───────────────
// 12 voices of Dust-triggered events: each fires an AR envelope with random
// attack/release, crossfades a feedback-sine against noise, band-limits and
// micro-delays it, pans it with a moving envelope, all fed into the shared
// reverb. Slow, cavernous, ever-shifting.
struct CoilEngine : DroneEngine {
    static constexpr int V = 12;
    static constexpr float detuning = 0.5f;
    struct Voice {
        Dust dPulse, dHit;
        Impulse imp;
        LFNoise0 nAtk, nRel;
        Latch latchAtk, latchRel;
        Trig trigEnv;
        BPEnv env, env2, env3;
        TChoose chPanL, chPanR, chWhich;
        SinOsc detuneLfo, fbLfo;
        SinOscFB oscFB;
        WhiteNoise wn;
        Biquad blp;
        DelayC delayc;
        LFNoise1 delayMod;
        float delayRate = 7.f;
        void init(uint32_t seed, float sr) {
            Rng rng; rng.seed(seed);
            dPulse.reset(seed + 1u); dHit.reset(seed + 2u);
            imp.reset();
            nAtk.reset(seed + 3u); nRel.reset(seed + 4u);
            latchAtk.reset(); latchRel.reset(); trigEnv.reset();
            env.reset(); env2.reset(); env3.reset();
            chPanL.reset(seed + 5u); chPanR.reset(seed + 6u); chWhich.reset(seed + 7u);
            detuneLfo.reset();
            fbLfo.reset(rng.uniform());          // SinOsc.kr(0.2, Rand(0,2pi))
            oscFB.reset();
            wn.reset(seed + 8u);
            blp.reset();
            delayc.dl.init(0.05f, sr);
            delayMod.reset(seed + 9u);
            delayRate = 5.f + rng.uniform() * 5.f;   // Rand(5,10)
        }
        void process(float hz, int i, float st, float sr, float& outL, float& outR) {
            static const float arrPan[2] = {-1.f, 1.f};
            static const float arrWhich[2] = {0.f, 1.f};
            float pulse = dPulse.process(0.5f, st) + imp.process(0.f, st);
            float pulseHit = dHit.process(1.f, st);
            float atk = latchAtk.process(linlin(nAtk.process(0.2f, st), -1.f, 1.f, 0.5f, 3.f), pulse);
            float rel = latchRel.process(linlin(nRel.process(0.2f, st), -1.f, 1.f, 0.1f, 3.f), pulse);
            float gate = trigEnv.process(pulseHit, atk + rel, st);
            float lv[3] = {0.f, 1.f, 0.f}, tm[2] = {atk, rel};
            float e = env.process(gate, lv, tm, 2, true, st);
            float pl = chPanL.process(pulse, arrPan, 2);
            float pr = chPanR.process(pulse, arrPan, 2);
            float ws = chWhich.process(pulse, arrWhich, 2);
            float lv2[3] = {0.f, pl, pr}, tm2[2] = {0.001f, atk + rel};
            float e2 = env2.process(gate, lv2, tm2, 2, false, st);
            float lv3[3] = {0.f, ws, 1.f - ws}, tm3[2] = {0.001f, atk + rel};
            float e3 = env3.process(gate, lv3, tm3, 2, false, st);
            float midi = cpsmidi(hz);
            float det = linlin(detuneLfo.process(0.1f * i, st), -1.f, 1.f, midi - detuning * i, midi + detuning * i);
            float fb = linlin(fbLfo.process(0.2f, st), -1.f, 1.f, 0.f, 0.5f);
            float snd1 = oscFB.process(hz + det, fb, st);
            float snd2 = wn.process() * 0.1f;
            float snd = selectx(e3, snd1, snd2);
            snd = blp.blowpass(snd, hz * 6.f, 0.6f, st);
            float dt = (0.02f + 0.01f * delayMod.process(delayRate, st)) / 15.f;
            snd = delayc.process(snd, dt * sr);
            snd *= e;
            pan2(snd, e2, 1.f, outL, outR);
        }
    };
    Voice voices[V];
    SchroederReverb reverb;
    const char* name() const override { return "coil"; }
    void init(uint32_t seed, float sr) override {
        for (int i = 0; i < V; ++i) voices[i].init(seed + i * 40009u + 1u, sr);
        reverb.init(seed + 99991u, sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float dryL = 0.f, dryR = 0.f;
        for (int i = 0; i < V; ++i) {
            float vl, vr; voices[i].process(hz, i, st, sr, vl, vr);
            dryL += vl; dryR += vr;
        }
        dryL *= amp; dryR *= amp;
        float rvL, rvR; reverb.process(dryL, dryR, st, sr, rvL, rvR);
        const float makeup = 3.f;   // -20 dBamp original is quiet; lift to roster level
        l = (dryL + 0.05f * rvL) * 0.1f * makeup;
        r = (dryR + 0.05f * rvR) * 0.1f * makeup;
    }
};

// ── Sachiko — @infinitedigits. "High-tone space-cutting." ────────────────────
// 4 voices of DPW pulses, each modulated by a bank of very slow wandering
// triangle LFOs, resonant-lowpassed, panned, and run through a long per-voice
// comb; summed, saturated into a global Moog ladder, then the shared reverb.
struct SachikoEngine : DroneEngine {
    static constexpr int V = 4;
    // one slow triangle LFO whose rate is itself slowly randomised (SC:
    // LFTri.kr(LFNoise0.kr(rrand(1/60,1/3)).range(1/60,1/3)))
    struct ModTri {
        LFNoise0 rateNoise; LFTri tri; float rateHz = 0.1f;
        void reset(uint32_t seed, float rHz) { rateNoise.reset(seed); tri.reset(); rateHz = rHz; }
        float process(float st) {
            float f = linlin(rateNoise.process(rateHz, st), -1.f, 1.f, 1.f / 60.f, 1.f / 3.f);
            return tri.process(f, st);
        }
    };
    struct Voice {
        ModTri mod[8];
        BlPulse pulse;
        AttackEnv env;
        Biquad rlpf;
        CombC combL, combR;
        LeakDC dcL, dcR;
        LFNoise0 combModNoise; Lag combModLag;
        float combBase = 0.35f, combDecay = 10.f, combRate = 0.1f;
        void init(uint32_t seed, float sr) {
            Rng rng; rng.seed(seed);
            for (int k = 0; k < 8; ++k) {
                float rHz = linlin(rng.uniform(), 0.f, 1.f, 1.f / 60.f, 1.f / 3.f);
                mod[k].reset(seed + k * 17u + 1u, rHz);
            }
            pulse.reset();
            env.reset(linlin(rng.uniform(), 0.f, 1.f, 1.f, 10.f));   // Env.asr(rrand(1,10))
            rlpf.reset();
            combL.dl.init(0.6f, sr); combR.dl.init(0.6f, sr);
            dcL.reset(); dcR.reset();
            combModNoise.reset(seed + 200u); combModLag.reset();
            combBase  = linlin(rng.uniform(), 0.f, 1.f, 0.2f, 0.5f);   // rrand(0.2,0.5)
            combDecay = linlin(rng.uniform(), 0.f, 1.f, 5.f, 15.f);    // rrand(5,15)
            combRate  = linlin(rng.uniform(), 0.f, 1.f, 1.f / 60.f, 1.f / 3.f);
        }
        void process(float hz, float st, float sr, float& outL, float& outR) {
            float modAmp   = linlin(mod[0].process(st), -1.f, 1.f, 0.2f, 0.5f);
            float modWidth = linlin(mod[1].process(st), -1.f, 1.f, 0.2f, 0.8f);
            float midi = cpsmidi(hz);
            float modFreq = midicps(linlin(mod[2].process(st), -1.f, 1.f, midi - 0.5f, midi + 0.5f));
            float s = pulse.process(modFreq, modWidth, st) * modAmp;
            s *= env.process(st);
            float cutoff = linexp(mod[4].process(st), -1.f, 1.f, hz, 20000.f);
            float rq = linlin(mod[5].process(st), -1.f, 1.f, 0.01f, 1.f);
            s = rlpf.rlpf(s, cutoff, rq, st);
            float pan = linlin(mod[6].process(st), -1.f, 1.f, -0.5f, 0.5f);
            float pL, pR; pan2(s, pan, 1.f, pL, pR);
            float dtMod = linlin(combModLag.process(combModNoise.process(combRate, st), 0.5f, st), -1.f, 1.f, -0.2f, 0.f);
            float dt = rack::clamp(combBase + dtMod, 0.001f, 0.5f);
            float g = combFeedback(dt, combDecay);
            outL = dcL.process(pL + combL.process(pL, dt * sr, g));
            outR = dcR.process(pR + combR.process(pR, dt * sr, g));
        }
    };
    Voice voices[V];
    MoogFF moogL, moogR;
    Lag moogLag; LFNoise0 moogNoise;
    SchroederReverb reverb;
    const char* name() const override { return "sachiko"; }
    void init(uint32_t seed, float sr) override {
        for (int i = 0; i < V; ++i) voices[i].init(seed + i * 60013u + 1u, sr);
        moogL.reset(); moogR.reset();
        moogLag.reset(); moogNoise.reset(seed + 5u);
        reverb.init(seed + 88883u, sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float sumL = 0.f, sumR = 0.f;
        for (int i = 0; i < V; ++i) { float vl, vr; voices[i].process(hz, st, sr, vl, vr); sumL += vl; sumR += vr; }
        sumL *= 0.70710678f; sumR *= 0.70710678f;   // Splay of a stereo pair ~ 1/sqrt(2)
        float cutoff = linexp(moogLag.process(moogNoise.process(0.25f, st), 4.f, st), -1.f, 1.f, hz * 10.f, 18000.f);
        float mL = moogL.process(std::tanh(sumL), cutoff, 1.2f, st);   // MoogFF gain ~2 -> moderate res
        float mR = moogR.process(std::tanh(sumR), cutoff, 1.2f, st);
        float rvL, rvR; reverb.process(mL, mR, st, sr, rvL, rvR);
        const float makeup = 2.f;
        l = (mL + 0.01f * rvL) * amp / 2.f * makeup;
        r = (mR + 0.01f * rvR) * amp / 2.f * makeup;
    }
};

// ── Starlids — @infinitedigits. "Symphonic, meek, radiant." ─────────────────
// A PWM sub-oscillator plus 12 sawtooth voices whose pitches step through major
// third/fourth/sixth intervals, each chorus-delayed and panned, the whole thing
// swept by a global Moog ladder.
struct StarlidsEngine : DroneEngine {
    static constexpr int V = 12;
    BlPulse sub; LFTri subWidth;
    struct Voice {
        LFTri o1, o2; BlSaw saw; Biquad lpf; SinOsc cutLfo;
        DelayC delayc; LFNoise1 delayMod; LFNoise0 panN; Lag panLag;
        float o1rate = 0.02f, o2rate = 0.02f, cutRate = 0.05f, delayRate = 7.f;
        void init(uint32_t seed, float sr) {
            Rng rng; rng.seed(seed);
            o1rate  = linlin(rng.uniform(), 0.f, 1.f, 1.f / 100.f, 1.f / 30.f);
            o2rate  = linlin(rng.uniform(), 0.f, 1.f, 1.f / 100.f, 1.f / 30.f);
            cutRate = linlin(rng.uniform(), 0.f, 1.f, 1.f / 30.f, 1.f / 10.f);
            delayRate = 5.f + rng.uniform() * 5.f;
            o1.reset(rng.uniform()); o2.reset(rng.uniform());
            saw.reset(); lpf.reset(); cutLfo.reset(rng.uniform());
            delayc.dl.init(0.05f, sr); delayMod.reset(seed + 3u);
            panN.reset(seed + 4u); panLag.reset();
        }
        void process(float note, float st, float sr, float& L, float& R) {
            float os1 = rack::clamp(std::floor(linlin(o1.process(o1rate, st), -1.f, 1.f, 0.f, 2.f)), 0.f, 1.f);
            float os2 = rack::clamp(std::floor(linlin(o2.process(o2rate, st), -1.f, 1.f, 0.f, 2.f)), 0.f, 1.f);
            float s = saw.process(midicps(note + 4.f * os1 + 5.f * os2), st);
            s = lpf.lpf(s, linexp(cutLfo.process(cutRate, st), -1.f, 1.f, 20.f, 12000.f), st);
            float dt = (0.02f + 0.01f * delayMod.process(delayRate, st)) / 15.f;
            s = delayc.process(s, dt * sr);
            float pan = panLag.process(panN.process(1.f / 3.f, st), 3.f, st);
            pan2(s, pan, 1.f / 12.f, L, R);
        }
    };
    Voice voices[V];
    MoogFF moogL, moogR; Lag moogLag; LFNoise0 moogNoise;
    Biquad hpfL, hpfR;
    const char* name() const override { return "starlids"; }
    void init(uint32_t seed, float sr) override {
        sub.reset(); subWidth.reset();
        for (int i = 0; i < V; ++i) voices[i].init(seed + i * 30011u + 1u, sr);
        moogL.reset(); moogR.reset(); moogLag.reset(); moogNoise.reset(seed + 7u);
        hpfL.reset(); hpfR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st, note = cpsmidi(hz);
        float sw = linlin(subWidth.process(0.5f, st), -1.f, 1.f, 0.2f, 0.8f);
        float subS = sub.process(midicps(note - 12.f), sw, st) / 12.f * amp;
        float sL, sR; pan2(subS, 0.f, 1.f, sL, sR);
        for (int i = 0; i < V; ++i) {
            float vl, vr; voices[i].process(note, st, sr, vl, vr);
            sL += vl * amp; sR += vr * amp;
        }
        float cutoff = linexp(moogLag.process(moogNoise.process(1.f / 6.f, st), 6.f, st), -1.f, 1.f, hz * 8.f, hz * 60.f);
        const float makeup = 3.5f;   // normalise toward the other engines' level
        l = hpfL.hpf(moogL.process(std::tanh(sL), cutoff, 1.f, st), 20.f, st) * makeup;
        r = hpfR.hpf(moogR.process(std::tanh(sR), cutoff, 1.f, st), 20.f, st) * makeup;
    }
};

// sample-and-hold random used by the Mt. * engines:
//   Latch(WhiteNoise*mul+add, Dust(freq)).lag(lag)
struct SHRand {
    WhiteNoise wn; Dust dust; Latch latch; Lag lag;
    void reset(uint32_t seed) { wn.reset(seed); dust.reset(seed * 2u + 1u); latch.reset(); lag.reset(); }
    float process(float freq, float mul, float add, float lagt, float st) {
        float d = dust.process(freq, st);
        return lag.process(latch.process(wn.process() * mul + add, d), lagt, st);
    }
};

// ── Mt. Lion — @license. "Roars through a twisting canyon." ──────────────────
// 9 comb-resonated pulse voices, everything (pitch, width, delay, decay, pan,
// level) driven by slow sample-and-held noise.
struct MtLionEngine : DroneEngine {
    static constexpr int V = 9;
    struct Voice {
        SHRand rFreq, rWidth, rDelayNote, rDelayMul, rDecay, rPan, rLevel;
        LFPulse pulse; CombN comb;
        void init(uint32_t seed, float sr) {
            uint32_t s = seed;
            rFreq.reset(s += 7u); rWidth.reset(s += 7u); rDelayNote.reset(s += 7u);
            rDelayMul.reset(s += 7u); rDecay.reset(s += 7u); rPan.reset(s += 7u); rLevel.reset(s += 7u);
            pulse.reset(); comb.dl.init(1.0f, sr);
        }
        void process(float baseNote, float noteDetune, float maxAmp, int index, float st, float sr, float& L, float& R) {
            float freq = midicps(rFreq.process(0.2f, noteDetune, baseNote, 2.f, st)) * index;
            float width = rack::clamp(rWidth.process(0.5f, 0.5f, 0.5f, 0.5f, st), 0.f, 1.f);
            float p = pulse.process(freq, width, st);
            float delayNote = midicps(rDelayNote.process(0.3f, noteDetune, baseNote, 5.f, st));
            float mul = std::round(rDelayMul.process(0.2f, 3.f, 4.f, 5.f, st));
            float dt = rack::clamp((1.f / std::max(delayNote, 1.f)) * mul, 0.001f, 1.f);
            float decay = rDecay.process(0.2f, 10.f, 0.f, 3.f, st);
            float s = std::tanh(comb.process(p, dt * sr, combFeedback(dt, decay)));
            pan2(s, rPan.process(0.4f, 1.f, 0.f, 2.f, st), rLevel.process(0.1f, maxAmp, 0.f, 0.5f, st), L, R);
        }
    };
    Voice voices[V];
    LeakDC dcL, dcR;
    const char* name() const override { return "mt. lion"; }
    void init(uint32_t seed, float sr) override {
        for (int i = 0; i < V; ++i) voices[i].init(seed + i * 50021u + 1u, sr);
        dcL.reset(); dcR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float baseNote = std::round(cpsmidi(hz));
        float noteDetune = std::fabs(baseNote - cpsmidi(hz));
        float maxAmp = amp / V;
        float sL = 0.f, sR = 0.f;
        for (int i = 0; i < V; ++i) {
            float vl, vr; voices[i].process(baseNote, noteDetune, maxAmp, i + 1, st, sr, vl, vr);
            sL += vl; sR += vr;
        }
        const float makeup = 3.f;
        l = dcL.process(sL) * makeup; r = dcR.process(sR) * makeup;
    }
};

// ── Apparatus — Josue Arias (after Zé Craum / Ruviaro / Mitchell). ───────────
// "Drone simulating old sinusoidal generators": clipped triangle oscillators
// with vibrato + mains hum, plus a crackle/dust interference bed.
struct ApparatusEngine : DroneEngine {
    PinkNoise pink1;
    LFPar mainsPar, hzPar;
    LFNoise2 vib, vib2;
    SinOsc lfo1, lfo2;
    LFTri t1, t2, t3, t4;
    Dust2 dust2; Crackle crackle; PinkNoise pinkMod; SinOsc dustSine;
    Biquad hpf, bpf;
    LeakDC dc;
    static constexpr float noiseAmp = 0.08f, mainsDepth = 0.5f, vrate = 0.19f, vrate2 = 0.67f,
                           vdepth = 0.007f, vdepth2 = 0.01f, sineClip = 0.825f, interference = 1.4f;
    const char* name() const override { return "apparatus"; }
    void init(uint32_t seed, float sr) override {
        pink1.reset(seed + 1u);
        mainsPar.reset(); hzPar.reset();
        vib.reset(seed + 3u); vib2.reset(seed + 4u);
        lfo1.reset(); lfo2.reset();
        t1.reset(); t2.reset(); t3.reset(); t4.reset();
        dust2.reset(seed + 5u); crackle.reset(); pinkMod.reset(seed + 6u); dustSine.reset();
        hpf.reset(); bpf.reset(); dc.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float noise = pink1.process() * (noiseAmp * linlin(mainsPar.process(hz * 2.f, st), -1.f, 1.f, 1.f - mainsDepth, 1.f));
        noise += hzPar.process(hz, st) * (noiseAmp / 8.f);
        float vibrato  = hz * linlin(vib.process(vrate, st),  -1.f, 1.f, 1.f / (1.f + vdepth),  1.f + vdepth);
        float vibrato2 = hz * 3.f * linlin(vib2.process(vrate2, st), -1.f, 1.f, 1.f / (1.f + vdepth2), 1.f + vdepth2);
        float l1 = lfo1.process(0.009f, st);
        float l2 = lfo2.process(0.011f, st);
        float snd  = softclip(rack::clamp(t1.process(vibrato, st),  -sineClip, sineClip));
        snd = (snd + noise) * amp;
        float snd2 = softclip(rack::clamp(t2.process(vibrato2, st), -sineClip, 0.5f)) * (amp / 7.f);
        float snd3 = softclip(rack::clamp(t3.process(hz * 5.f - l2, st), -sineClip, 0.65f)) * l1 * (amp / 10.f);
        float snd4 = softclip(rack::clamp(t4.process(hz * 2.f - l1, st), -sineClip, 0.70f)) * l2 * (amp / 6.f);
        float sndall = snd + snd2 + snd3 + snd4;
        float dustSig = dust2.process(10.f, st) + crackle.process(1.95f) * 0.2f
                      + dustSine.process(pinkMod.process() * 3750.f + 40.f, st) * 0.011f;
        dustSig = hpf.hpf(dustSig, 25.f, st);
        dustSig = bpf.bpf(dustSig, hz * 8.f, 1.f, st);
        dustSig = dustSig * (interference + noise) * amp;
        l = r = dc.process(sndall + dustSig) * 0.5f;   // normalise toward roster level
    }
};

// ── Eliane — @sixolet. "Feedback, slow beatings, highs and lows." ────────────
// Seven sine partials phase-modulating each other in a crosslinked feedback
// ring (an homage to Éliane Radigue). SC's LocalIn/LocalOut(14) is a one-sample
// feedback bus — here just the previous frame's per-partial stereo output.
struct ElianeEngine : DroneEngine {
    static constexpr int N = 7;
    float bus[2 * N] = {};
    float intervals[N] = {};
    SinOsc mainL[N], mainR[N], subL[N], subR[N];
    LFNoise2 detL[N], detR[N], subDetL[N], subDetR[N], modExp[N], rotN[N];
    LFNoise2 gRateFb[N], gRateSn[N], gNoiseFb[N], gNoiseSn[N];
    Lag gLagFb[N], gLagSn[N];
    LFNoise2 nice, combRate;
    CombN combL, combR;
    const char* name() const override { return "eliane"; }
    void init(uint32_t seed, float sr) override {
        static const int steps[N] = {0, 7, 14, 20, 27, 34, 41};
        for (int x = 0; x < N; ++x) intervals[x] = std::pow(2.f, steps[x] / 12.f);
        for (int i = 0; i < 2 * N; ++i) bus[i] = 0.f;
        uint32_t s = seed;
        for (int x = 0; x < N; ++x) {
            mainL[x].reset(); mainR[x].reset(); subL[x].reset(); subR[x].reset();
            detL[x].reset(s += 7u); detR[x].reset(s += 7u);
            subDetL[x].reset(s += 7u); subDetR[x].reset(s += 7u);
            modExp[x].reset(s += 7u); rotN[x].reset(s += 7u);
            gRateFb[x].reset(s += 7u); gRateSn[x].reset(s += 7u);
            gNoiseFb[x].reset(s += 7u); gNoiseSn[x].reset(s += 7u);
            gLagFb[x].reset(); gLagSn[x].reset();
        }
        nice.reset(s += 7u); combRate.reset(s += 7u);
        combL.dl.init(0.35f, sr); combR.dl.init(0.35f, sr);
    }
    // LFDNoise3(rate, add:0.2).clip(0,1).lag2(13), rate from LFNoise2(1/300)
    float gate(LFNoise2& rate, LFNoise2& noise, Lag& lag, float st) {
        float rHz = linlin(rate.process(1.f / 300.f, st), -1.f, 1.f, 1.f / 120.f, 1.f / 30.f);
        float v = rack::clamp(noise.process(rHz, st) + 0.2f, 0.f, 1.f);
        return lag.process(v, 13.f, st);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float expo = linlin(nice.process(1.f / 200.f, st), -1.f, 1.f, 0.9f, 1.7f);
        float newbus[2 * N];
        float sumL = 0.f, sumR = 0.f;
        for (int x = 0; x < N; ++x) {
            int nx = (x + 1) % N, nx2 = (x + 2) % N;
            float modL = (x + 1) * bus[2 * nx]     + bus[2 * nx2];
            float modR = (x + 1) * bus[2 * nx + 1] + bus[2 * nx2 + 1];
            float base = modExp[x].process(1.f / 66.f, st) * 0.5f + 0.5f;   // unipolar
            float coeff = (float)M_PI * std::pow(rack::clamp(base, 0.f, 1.f), expo);
            modL *= coeff; modR *= coeff;
            float pitch = hz * intervals[x];
            float sL = mainL[x].process(pitch + (x * 0.5f) * detL[x].process(1.f / 15.f, st), st, modL);
            float sR = mainR[x].process(pitch + (x * 0.5f) * detR[x].process(1.f / 15.f, st), st, modR);
            if (x == 6) {
                float bL = subL[x].process(hz * 0.5f + subDetL[x].process(1.f / 15.f, st), st, modL);
                float bR = subR[x].process(hz * 0.5f + subDetR[x].process(1.f / 15.f, st), st, modR);
                float sel = hz / (150.f + hz);
                sL = selectx(sel, sL / (1.5f * x + 1.f), bL);
                sR = selectx(sel, sR / (1.5f * x + 1.f), bR);
            } else if (x == 5) {
                float bL = subL[x].process(hz / 3.f + subDetL[x].process(1.f / 15.f, st), st, modL) * 0.5f;
                float bR = subR[x].process(hz / 3.f + subDetR[x].process(1.f / 15.f, st), st, modR) * 0.5f;
                float sel = hz / (300.f + hz);
                sL = selectx(sel, sL / (1.3f * x + 1.f), bL);
                sR = selectx(sel, sR / (1.3f * x + 1.f), bR);
            } else {
                sL /= (x + 1); sR /= (x + 1);
            }
            float rL, rR;
            rotate2(sL, sR, rotN[x].process(1.f / 25.f, st) * 0.5f, rL, rR);
            float gFb = gate(gRateFb[x], gNoiseFb[x], gLagFb[x], st);   // feedback gate
            float gSn = gate(gRateSn[x], gNoiseSn[x], gLagSn[x], st);   // output gate
            newbus[2 * x]     = rL * gFb;
            newbus[2 * x + 1] = rR * gFb;
            sumL += 0.5f * rL * gSn * amp;
            sumR += 0.5f * rR * gSn * amp;
        }
        for (int i = 0; i < 2 * N; ++i) bus[i] = newbus[i];             // LocalOut
        float cr = linexp(combRate.process(0.02f, st), -1.f, 1.f, 0.3f, 3.0f);
        l = std::tanh(combL.process(sumL, 0.3f * sr, combFeedback(0.3f, cr)));
        r = std::tanh(combR.process(sumR, 0.3f * sr, combFeedback(0.3f, cr)));
    }
};

// ── UNRELACC — @zebra. Six Hénon-map chaotic oscillators in intervals. ───────
struct UnrelaccEngine : DroneEngine {
    static constexpr int V = 6;
    Lag hzLag;
    LFTri aLfo[V], bLfo[V];
    HenonC henon[V];
    LFSaw fmSaw[V];
    SinOsc panLfo[V];
    LeakDC dcL, dcR;
    AttackEnv linen;
    const float ratios[V] = {0.5f, 1.f, 4.f / 3.f, 7.f / 4.f, 2.f, 12.f / 5.f};
    const float amps[V]   = {1.f, 1.f, 1.f, 0.56234f, 0.63096f, 0.44668f};   // [0,0,0,-5,-4,-7].dbamp
    const char* name() const override { return "unrelacc"; }
    void init(uint32_t, float) override {
        hzLag.reset(); dcL.reset(); dcR.reset(); linen.reset(6.66f);
        for (int i = 0; i < V; ++i) {
            aLfo[i].reset(); bLfo[i].reset(); henon[i].reset(); fmSaw[i].reset(0.04f);
            panLfo[i].reset(std::fmod((float)(i * 14), (float)M_PI) / kTwoPi);
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float h = hzLag.process(hz * 2.f, 4.f, st);
        float sumL = 0.f, sumR = 0.f;
        for (int i = 0; i < V; ++i) {
            float a = linlin(aLfo[i].process(1.f / (ratios[i] * 9.f), st), -1.f, 1.f, 1.01f, 1.2f);
            float b = linlin(bLfo[i].process(1.f / (ratios[i] * 8.f), st), -1.f, 1.f, 0.11f, 0.214f);
            float freq = h * 2.f * ratios[i] + fmSaw[i].process((i + 1) * (i + 2) / 14.f * (i * 3.f), st);
            float x = henon[i].process(freq, a, b, st);
            float pan = panLfo[i].process((i + 1) / 31.f, st) * 0.77f;
            float pL, pR; pan2(x, pan, amps[i], pL, pR);
            sumL += pL; sumR += pR;
        }
        float e = linen.process(st);
        l = dcL.process(sumL) * amp * 0.5f * e;
        r = dcR.process(sumR) * amp * 0.5f * e;
    }
};

// ── Dreamcrusher — @infinitedigits. No-input-mixer feedback drone. ───────────
// A gated pulse feeds a feedback loop (one-poles, rotation, a modulated delay,
// soft-clip and lowpass) whose gain exceeds unity — held in check by the clip.
struct DreamcrusherEngine : DroneEngine {
    LFNoise0 nFreq, nWidth, nBal, nDelay, nLpf, nFbGain, nBalOut;
    SinOsc sFreq, sWidth, sBal, sBalOut;
    Lag freqLag, delayLag, lpfLag, fbGainLag;
    BlPulse pulse;
    Amplitude ampFollow;
    OnePole op1L, op1R, op2L, op2R;
    DelayC delayL, delayR;
    LeakDC dcL, dcR;
    Biquad lpfL, lpfR;
    float fbL = 0.f, fbR = 0.f;
    const char* name() const override { return "dreamcrusher"; }
    void init(uint32_t seed, float sr) override {
        uint32_t s = seed;
        nFreq.reset(s += 7u); nWidth.reset(s += 7u); nBal.reset(s += 7u); nDelay.reset(s += 7u);
        nLpf.reset(s += 7u); nFbGain.reset(s += 7u); nBalOut.reset(s += 7u);
        sFreq.reset(); sWidth.reset(); sBal.reset(); sBalOut.reset();
        freqLag.reset(); delayLag.reset(); lpfLag.reset(); fbGainLag.reset();
        pulse.reset(); ampFollow.reset();
        op1L.reset(); op1R.reset(); op2L.reset(); op2R.reset();
        delayL.dl.init(0.4f, sr); delayR.dl.init(0.4f, sr);
        dcL.reset(); dcR.reset(); lpfL.reset(); lpfR.reset();
        fbL = fbR = 0.f;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float fmod_ = linlin(sFreq.process(nFreq.process(1.f, st) * 0.5f, st), -1.f, 1.f, 0.99f, 1.01f);
        float pfreq = freqLag.process(hz * fmod_, 1.f, st);
        float pwidth = linlin(sWidth.process(nWidth.process(1.f, st), st), -1.f, 1.f, 0.45f, 0.55f);
        float pin = pulse.process(pfreq, pwidth, st) * 0.70710678f;      // Splay(mono) → centre
        float balPos = sBal.process(linlin(nBal.process(0.1f, st), -1.f, 1.f, 0.05f, 0.2f), st) * 0.1f;
        float inL, inR; balance2(pin, pin, balPos, 1.f, inL, inR);
        float gate = (ampFollow.process(inL + inR, 0.01f, 0.01f, st) > 0.02f) ? 1.f : 0.f;
        inL *= gate; inR *= gate;
        // feedback loop (local = previous LocalOut)
        float lL = op1L.process(fbL, 0.4f),  lR = op1R.process(fbR, 0.4f);
        lL = op2L.process(lL, -0.08f);       lR = op2R.process(lR, -0.08f);
        float rL, rR; rotate2(lL, lR, 0.2f, rL, rR);
        float dtime = delayLag.process(linlin(nDelay.process(0.1f, st), -1.f, 1.f, 0.15f, 0.3f), 10.f, st);
        float dL = dcL.process(delayL.process(rL, dtime * sr));
        float dR = dcR.process(delayR.process(rR, dtime * sr));
        float mL = softclip((dL + inL) * 1.25f), mR = softclip((dR + inR) * 1.25f);
        float cut = lpfLag.process(linlin(nLpf.process(0.3f, st), -1.f, 1.f, std::min(hz, 80.f), 16000.f), 3.333f, st);
        mL = lpfL.lpf(mL, cut, st); mR = lpfR.lpf(mR, cut, st);
        float fbGain = fbGainLag.process(linlin(nFbGain.process(2.f, st), -1.f, 1.f, 1.01f, 1.5f), 0.5f, st);
        fbL = mL * fbGain; fbR = mR * fbGain;
        float outPos = sBalOut.process(linlin(nBalOut.process(0.1f, st), -1.f, 1.f, 0.05f, 0.2f), st) * 0.1f;
        balance2(mL * 0.2f, mR * 0.2f, outPos, amp * 3.f, l, r);   // makeup to roster level
    }
};

// ── Rehberg — @infinitedigits. "Dense, distorted, overwhelming." ─────────────
// A detuned tape-warble pulse pair, folded and DFM1-filtered, with an FM sine
// and resonant band, poured into Freeverb. Random dips/bumps modulate the pitch.
struct RehbergEngine : DroneEngine {
    SinOsc wobbleAmp, flutterAmp, wobbleOsc, flutterOsc;
    LFNoise2 flutterVar;
    LFNoise0 d1atk, d1rel, d1rate; Dust d1; BPEnv env1;
    LFNoise0 d2atk, d2rel, d2rate; Dust d2; BPEnv env2;
    LFNoise0 eiatk, eirel; Changed changed; BPEnv envinv;
    LFTri pwmL, pwmR; BlPulse pulseL, pulseR;
    WhiteNoise wn; SinOsc noiseAmp; LFNoise0 noiseCut; Lag noiseCutLag; Biquad noiseLpf;
    Biquad dfmLpL, dfmLpR, dfmHpL, dfmHpR, bpfL, bpfR;
    SinOsc fmOsc, fmModOsc, fmAmp, bpfMul;
    FreeVerbMono fvL, fvR; SinOsc roomOsc; LFNoise0 mixN;
    float rWobA = 0.015f, rFluA = 0.015f, rNoiseA = 0.015f, rFmA = 0.015f, rBpfMul = 0.015f;
    const char* name() const override { return "rehberg"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        rWobA = linlin(rng.uniform(), 0.f, 1.f, 0.01f, 0.02f);
        rFluA = linlin(rng.uniform(), 0.f, 1.f, 0.01f, 0.02f);
        rNoiseA = linlin(rng.uniform(), 0.f, 1.f, 0.005f, 0.01f);
        rFmA = linlin(rng.uniform(), 0.f, 1.f, 0.01f, 0.02f);
        rBpfMul = linlin(rng.uniform(), 0.f, 1.f, 0.01f, 0.02f);
        wobbleAmp.reset(); flutterAmp.reset(); wobbleOsc.reset(); flutterOsc.reset(); flutterVar.reset(seed + 1u);
        uint32_t s = seed;
        d1atk.reset(s += 7u); d1rel.reset(s += 7u); d1rate.reset(s += 7u); d1.reset(s += 7u); env1.reset();
        d2atk.reset(s += 7u); d2rel.reset(s += 7u); d2rate.reset(s += 7u); d2.reset(s += 7u); env2.reset();
        eiatk.reset(s += 7u); eirel.reset(s += 7u); changed.reset(); envinv.reset();
        pwmL.reset(); pwmR.reset(); pulseL.reset(); pulseR.reset();
        wn.reset(s += 7u); noiseAmp.reset(); noiseCut.reset(s += 7u); noiseCutLag.reset(); noiseLpf.reset();
        dfmLpL.reset(); dfmLpR.reset(); dfmHpL.reset(); dfmHpR.reset(); bpfL.reset(); bpfR.reset();
        fmOsc.reset(); fmModOsc.reset(); fmAmp.reset(); bpfMul.reset();
        fvL.init(sr); fvR.init(sr); roomOsc.reset(); mixN.reset(s += 7u);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float wobA = linlin(wobbleAmp.process(rWobA, st), -1.f, 1.f, 0.1f, 0.5f);
        float fluA = linlin(flutterAmp.process(rFluA, st), -1.f, 1.f, 0.1f, 0.5f);
        float wow = std::max(wobA * std::pow(wobbleOsc.process(33.f / 60.f, st), 39.f), 0.f);
        float flutter = fluA * flutterOsc.process(6.f + flutterVar.process(2.f, st), st);
        float combined = 1.f + wow + flutter;
        float lv[3] = {0.f, 1.f, 0.f};
        float freq = hz;
        float tmA[2] = {linlin(d1atk.process(0.1f, st), -1.f, 1.f, 0.f, 1.f), linlin(d1rel.process(0.1f, st), -1.f, 1.f, 0.f, 1.f)};
        freq *= 1.f - env1.process(d1.process(linlin(d1rate.process(0.1f, st), -1.f, 1.f, 0.001f, 0.1f), st), lv, tmA, 2, false, st);
        float tmB[2] = {linlin(d2atk.process(0.1f, st), -1.f, 1.f, 0.f, 1.f), linlin(d2rel.process(0.1f, st), -1.f, 1.f, 0.f, 1.f)};
        freq *= 1.f + env2.process(d2.process(linlin(d2rate.process(0.1f, st), -1.f, 1.f, 0.001f, 0.1f), st), lv, tmB, 2, false, st);
        float tmC[2] = {linlin(eiatk.process(0.1f, st), -1.f, 1.f, 0.f, 0.2f), linlin(eirel.process(0.1f, st), -1.f, 1.f, 0.f, 0.2f)};
        float envinvV = 1.f - envinv.process(changed.process(freq), lv, tmC, 2, false, st);
        // detuned PWM pulse pair (stereo)
        float sL = pulseL.process(freq,        linlin(pwmL.process(0.5f / 3.f, st),  -1.f, 1.f, 0.2f, 0.8f), st);
        float sR = pulseR.process(freq + 0.1f, linlin(pwmR.process(0.51f / 3.f, st), -1.f, 1.f, 0.2f, 0.8f), st);
        float nz = noiseLpf.lpf(wn.process() * linlin(noiseAmp.process(rNoiseA, st), -1.f, 1.f, 0.005f, 0.01f),
                                noiseCutLag.process(linlin(noiseCut.process(freq, st), -1.f, 1.f, 20.f, 20000.f), 0.1f, st), st);
        sL += nz; sR += nz;
        sL = foldOver(sL, -0.2f, 0.2f); sR = foldOver(sR, -0.2f, 0.2f);
        sL = dfmLpL.dfm1(sL, freq * 24.f, 0.3f, 0, st); sR = dfmLpR.dfm1(sR, freq * 24.f, 0.3f, 0, st);
        sL = dfmHpL.dfm1(sL, 90.f, 0.1f, 1, st);         sR = dfmHpR.dfm1(sR, 90.f, 0.1f, 1, st);
        float fm = fmOsc.process(freq * 1.5f, st, fmModOsc.process(freq, st) * 2.f) * linlin(fmAmp.process(rFmA, st), -1.f, 1.f, 0.01f, 0.2f);
        sL += fm; sR += fm;
        float bpfMulV = linlin(bpfMul.process(rBpfMul, st), -1.f, 1.f, 0.01f, 0.5f);
        sL += bpfL.bpf(sL, freq * combined, 2.f, st) * bpfMulV;
        sR += bpfR.bpf(sR, freq * combined, 2.f, st) * bpfMulV;
        float room = linlin(roomOsc.process(0.1f, st), -1.f, 1.f, 0.3f, 0.6f);
        float mixv = linlin(mixN.process(0.2f, st), -1.f, 1.f, 0.1f, 0.6f);
        sL = fvL.process(sL, mixv, room, 0.5f); sR = fvR.process(sR, mixv, room, 0.5f);
        float g = amp * envinvV * 0.35f * 3.f;   // last factor is makeup to roster level
        l = sL * g; r = sR * g;
    }
};

// ── Toshiya — @infinitedigits. "Object-bound resonate space." ───────────────
// Twelve sine voices whose pitches jump through intervals, chorus-delayed and
// swept by a Moog ladder, into the shared reverb, with a pink-noise-excited
// Klank resonator bank ringing underneath.
struct ToshiyaEngine : DroneEngine {
    static constexpr int V = 12;
    SinOsc sub, subPan; LFTri subPhase;
    struct Voice {
        SinOsc osc; TChoose interval; Impulse imp; float impRate = 0.1f;
        Biquad lpf; SinOsc cutLfo; float cutRate = 0.05f;
        DelayC delayc; LFNoise1 delayMod; float delayRate = 7.f, delayDiv = 15.f;
        LFNoise0 panN; Lag panLag;
        void init(uint32_t seed, float sr) {
            Rng rng; rng.seed(seed);
            osc.reset(); interval.reset(seed + 1u); imp.reset();
            impRate = linlin(rng.uniform(), 0.f, 1.f, 1.f / 30.f, 1.f / 5.f);
            lpf.reset(); cutLfo.reset(rng.uniform());
            cutRate = linlin(rng.uniform(), 0.f, 1.f, 1.f / 30.f, 1.f / 10.f);
            delayc.dl.init(0.05f, sr); delayMod.reset(seed + 2u);
            delayRate = 5.f + rng.uniform() * 5.f;
            delayDiv = 10.f + rng.uniform() * 10.f;      // NRand(10,20,3)
            panN.reset(seed + 3u); panLag.reset();
        }
        void process(float note, float st, float sr, float& L, float& R) {
            static const float intervals[7] = {0, 9, 4, 14, 5, 2, 17};
            float iv = interval.process(imp.process(impRate, st), intervals, 7);
            float s = osc.process(midicps(note + iv), st);
            s = lpf.lpf(s, linexp(cutLfo.process(cutRate, st), -1.f, 1.f, 20.f, 12000.f), st) * 2.f;
            float dt = (0.02f + 0.01f * delayMod.process(delayRate, st)) / delayDiv;
            s = delayc.process(s, dt * sr);
            pan2(s, panLag.process(panN.process(1.f / 3.f, st), 3.f, st), 1.f / 12.f, L, R);
        }
    };
    Voice voices[V];
    MoogFF moogL, moogR; Lag moogLag; LFNoise0 moogNoise;
    SchroederReverb reverb; Lag reverbGainLag; LFNoise0 reverbGainN;
    Amplitude ampL, ampR; Lag klankGainLag; LFNoise0 klankGainN;
    PinkNoise pinkL, pinkR; Ringz ringL[3], ringR[3];
    Biquad finalLpL, finalLpR, finalHpL, finalHpR;
    const char* name() const override { return "toshiya"; }
    void init(uint32_t seed, float sr) override {
        sub.reset(); subPan.reset(); subPhase.reset();
        for (int i = 0; i < V; ++i) voices[i].init(seed + i * 30011u + 1u, sr);
        moogL.reset(); moogR.reset(); moogLag.reset(); moogNoise.reset(seed + 1u);
        reverb.init(seed + 7u, sr); reverbGainLag.reset(); reverbGainN.reset(seed + 2u);
        ampL.reset(); ampR.reset(); klankGainLag.reset(); klankGainN.reset(seed + 3u);
        pinkL.reset(seed + 4u); pinkR.reset(seed + 5u);
        for (int i = 0; i < 3; ++i) { ringL[i].reset(); ringR[i].reset(); }
        finalLpL.reset(); finalLpR.reset(); finalHpL.reset(); finalHpR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st, note = cpsmidi(hz);
        float subPh = linlin(subPhase.process(0.5f, st), -1.f, 1.f, 0.2f, 0.8f);
        float subS = sub.process(midicps(note - 12.f), st, subPh) / 12.f * amp;
        float sL, sR; pan2(subS, subPan.process(0.1f, st) * 0.2f, 1.f, sL, sR);
        for (int i = 0; i < V; ++i) { float vl, vr; voices[i].process(note, st, sr, vl, vr); sL += vl * amp; sR += vr * amp; }
        float mcut = linexp(moogLag.process(moogNoise.process(1.f / 6.f, st), 6.f, st), -1.f, 1.f, hz * 2.f, hz * 10.f);
        sL = moogL.process(std::tanh(sL), mcut, 1.f, st);
        sR = moogR.process(std::tanh(sR), mcut, 1.f, st);
        float rvL, rvR; reverb.process(sL, sR, st, sr, rvL, rvR);
        float rg = linlin(reverbGainLag.process(reverbGainN.process(1.f / 10.f, st), 10.f, st), -1.f, 1.f, 0.01f, 0.06f);
        sL += rg * rvL; sR += rg * rvR;
        float kg = linlin(klankGainLag.process(klankGainN.process(1.f, st), 1.f, st), -1.f, 1.f, 0.f, 0.5f);
        float kfreqs[3] = {hz, hz * 2.f + 23.f, hz * 4.f + 53.f};
        float pnL = pinkL.process() * 0.007f, pnR = pinkR.process() * 0.007f, kL = 0.f, kR = 0.f;
        for (int i = 0; i < 3; ++i) { kL += ringL[i].process(pnL, kfreqs[i], 1.f, st); kR += ringR[i].process(pnR, kfreqs[i], 1.f, st); }
        sL += ampL.process(sL, 0.01f, 0.01f, st) * kg * kL;
        sR += ampR.process(sR, 0.01f, 0.01f, st) * kg * kR;
        sL = finalLpL.lpf(sL, 15000.f, st); sR = finalLpR.lpf(sR, 15000.f, st);
        const float makeup = 3.f;   // normalise toward roster level
        l = finalHpL.hpf(std::tanh(sL) * 0.5f, 20.f, st) * makeup;
        r = finalHpR.hpf(std::tanh(sR) * 0.5f, 20.f, st) * makeup;
    }
};

// ── Magicicada — @sixolet. "Unsettling, organic, chaotic." ──────────────────
// A no-input-mixing drone: two parallel crossfading delay selectors (3 and 4
// delays) inside a feedback loop, filtered/warped and fed back with a touch of
// noise. The audible output is the filtered feedback signal itself.
struct MagicicadaEngine : DroneEngine {
    float fbL = 0.f, fbR = 0.f;
    PinkNoise pink; BrownNoise brown;
    LFNoise2 nBeat, nDist, nHighRes, nLowRes, nBp[3], nOutHpf, nFbGain, nRot, nFinalLpf;
    SinOsc oDelaySel1, oFilterSel1, oDelaySel2, oFilterSel2;
    LeakDC dcL, dcR;
    BAllPass apL[3], apR[3];
    DelayC d1a_L, d1a_R, d1b_L, d1b_R, d1c_L, d1c_R;
    DelayC d2a_L, d2a_R, d2b_L, d2b_R, d2c_L, d2c_R, d2d_L, d2d_R;
    Biquad rlpf1L, rlpf1R, rlpf2L, rlpf2R;
    SVF svfL, svfR;
    Biquad outHpfL, outHpfR, finalHpL, finalHpR, finalLpL, finalLpR;
    const char* name() const override { return "magicicada"; }
    void init(uint32_t seed, float sr) override {
        uint32_t s = seed;
        pink.reset(s += 7u); brown.reset(s += 7u);
        nBeat.reset(s += 7u); nDist.reset(s += 7u); nHighRes.reset(s += 7u); nLowRes.reset(s += 7u);
        for (int i = 0; i < 3; ++i) nBp[i].reset(s += 7u);
        nOutHpf.reset(s += 7u); nFbGain.reset(s += 7u); nRot.reset(s += 7u); nFinalLpf.reset(s += 7u);
        Rng rng; rng.seed(s += 7u);
        oDelaySel1.reset(rng.uniform()); oFilterSel1.reset(rng.uniform());
        oDelaySel2.reset(rng.uniform()); oFilterSel2.reset(rng.uniform());
        dcL.reset(); dcR.reset();
        for (int i = 0; i < 3; ++i) { apL[i].reset(); apR[i].reset(); }
        d1a_L.dl.init(3.0f, sr); d1a_R.dl.init(3.0f, sr);
        d1b_L.dl.init(2.1f, sr); d1b_R.dl.init(2.1f, sr);
        d1c_L.dl.init(1.6f, sr); d1c_R.dl.init(1.6f, sr);
        d2a_L.dl.init(0.8f, sr); d2a_R.dl.init(0.8f, sr);
        d2b_L.dl.init(2.6f, sr); d2b_R.dl.init(2.6f, sr);
        d2c_L.dl.init(1.6f, sr); d2c_R.dl.init(1.6f, sr);
        d2d_L.dl.init(0.35f, sr); d2d_R.dl.init(0.35f, sr);
        rlpf1L.reset(); rlpf1R.reset(); rlpf2L.reset(); rlpf2R.reset();
        svfL.reset(); svfR.reset();
        outHpfL.reset(); outHpfR.reset(); finalHpL.reset(); finalHpR.reset(); finalLpL.reset(); finalLpR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float beat = linexp(nBeat.process(1.f / 600.f, st), -1.f, 1.f, 0.5f, 2.8f);
        float distGain = linlin(nDist.process(1.f / 60.f, st), -1.f, 1.f, 1.f, 2.5f);
        float highRes = linlin(nHighRes.process(1.f / 30.f, st), -1.f, 1.f, 0.05f, 0.2f);
        float lowRes = linlin(nLowRes.process(1.f / 30.f, st), -1.f, 1.f, 0.05f, 0.2f);
        float delaySel1 = linlin(oDelaySel1.process(1.f / 166.6f, st), -1.f, 1.f, 0.f, 3.f);
        float filterSel1 = rack::clamp(linlin(oFilterSel1.process(1.f / 82.f, st), -1.f, 1.f, -0.3f, 1.1f), 0.f, 1.f);
        float delaySel2 = linlin(oDelaySel2.process(1.f / (float)(M_PI * 60.0), st), -1.f, 1.f, 0.f, 4.f);
        float filterSel2 = linlin(oFilterSel2.process(1.f / 123.4f, st), -1.f, 1.f, 0.f, 3.f);
        float mid = pink.process(), side = 0.1f * brown.process();
        float nL = (mid + side) * 0.01f, nR = (mid - side) * 0.01f;
        float sigL = dcL.process(fbL), sigR = dcR.process(fbR);
        float phL = sigL, phR = sigR;
        for (int i = 0; i < 3; ++i) {
            float bf = linlin(nBp[i].process(1.f / 30.f, st), -1.f, 1.f, 20.f, 2000.f);
            phL = apL[i].process(phL, bf, 0.7f, st); phR = apR[i].process(phR, bf, 0.7f, st);
        }
        sigL = (sigL + phL) * 0.5f; sigR = (sigR + phR) * 0.5f;
        float diL = sigL + nL, diR = sigR + nR;
        float a1L[3] = {d1a_L.process(diL, beat * sr), d1b_L.process(diL, (2.f * beat / 3.f) * sr), d1c_L.process(diL, (0.5f * beat) * sr)};
        float a1R[3] = {d1a_R.process(diR, beat * sr), d1b_R.process(diR, (2.f * beat / 3.f) * sr), d1c_R.process(diR, (0.5f * beat) * sr)};
        float del1L = selectxN(delaySel1, a1L, 3, true), del1R = selectxN(delaySel1, a1R, 3, true);
        float f1L[2] = {rlpf1L.rlpf(del1L, hz, lowRes, st), del1L};
        float f1R[2] = {rlpf1R.rlpf(del1R, hz, lowRes, st), del1R};
        del1L = selectxN(filterSel1, f1L, 2, false); del1R = selectxN(filterSel1, f1R, 2, false);
        float muL = diL + foldOver(diL, -0.1f, 0.1f), muR = diR + foldOver(diR, -0.1f, 0.1f);
        float a2L[4] = {d2a_L.process(muL, (beat / 4.f) * sr), d2b_L.process(muL, (0.75f * beat) * sr), d2c_L.process(diL, (0.5f * beat) * sr), d2d_L.process(diL, (0.1f * beat) * sr)};
        float a2R[4] = {d2a_R.process(muR, (beat / 4.f) * sr), d2b_R.process(muR, (0.75f * beat) * sr), d2c_R.process(diR, (0.5f * beat) * sr), d2d_R.process(diR, (0.1f * beat) * sr)};
        float del2L = selectxN(delaySel2, a2L, 4, true), del2R = selectxN(delaySel2, a2R, 4, true);
        float f2L[3] = {rlpf2L.rlpf(del2L, (12.f / 5.f) * hz, highRes, st), svfL.process(del2L, (9.f / 2.f) * hz, highRes, 0.f, 1.f, 0.f, st), del2L};
        float f2R[3] = {rlpf2R.rlpf(del2R, (12.f / 5.f) * hz, highRes, st), svfR.process(del2R, (9.f / 2.f) * hz, highRes, 0.f, 1.f, 0.f, st), del2R};
        del2L = selectxN(filterSel2, f2L, 3, true); del2R = selectxN(filterSel2, f2R, 3, true);
        float outHpf = linlin(nOutHpf.process(1.f / 30.f, st), -1.f, 1.f, 20.f, std::max(hz, 25.f));
        float dL = outHpfL.hpf(std::sin(distGain * (del1L + del2L)), outHpf, st);
        float dR = outHpfR.hpf(std::sin(distGain * (del1R + del2R)), outHpf, st);
        float fbGain = linlin(nFbGain.process(1.f / 20.f, st), -1.f, 1.f, 0.48f, 0.75f);
        fbL = fbGain * dL; fbR = fbGain * dR;
        float roL, roR; rotate2(sigL, sigR, nRot.process(1.f / 30.f, st), roL, roR);
        float finalCut = linexp(nFinalLpf.process(1.f / 43.f, st), -1.f, 1.f, 3000.f, 20000.f);
        l = std::tanh(finalLpL.lpf(finalHpL.hpf(roL, 20.f, st), finalCut, st) * amp);
        r = std::tanh(finalLpR.lpf(finalHpR.hpf(roR, 20.f, st), finalCut, st) * amp);
    }
};

// ── mt. zion — @license. "Thee rusted satellites gather + sing." ──────────────
// Five pulse-wave harmonics where everything — pitch wander, pulse width, pan,
// level — is Latch(WhiteNoise, Dust).lag, i.e. slow lagged sample-and-hold
// randomness. The original's pulse width wanders in [1, 2]; SC's Pulse treats
// width modulo 1, so a voice thins to silence as its width passes an integer —
// kept faithfully by wrapping the width here too.
struct MtZionEngine : DroneEngine {
    static constexpr int N = 5;
    struct SH {                       // Latch.ar(WhiteNoise.ar, Dust.ar(f)).lag(t)
        Rng rng; Dust dust; float held = 0.f; Lag lag;
        void init(uint32_t s) {
            rng.seed(s); dust.reset(s ^ 0x9e3779b9u); held = rng.bipolar(); lag.reset();
        }
        float process(float density, float lagTime, float st) {
            if (dust.process(density, st) > 0.f) held = rng.bipolar();
            return lag.process(held, lagTime, st);
        }
    };
    struct Voice { SH shNote, shWidth, shPan, shLevel; BlPulse pulse; };
    Voice v[N];
    const char* name() const override { return "mt. zion"; }
    void init(uint32_t seed, float) override {
        for (int i = 0; i < N; ++i) {
            v[i].shNote.init(seed + i * 197u + 1u);
            v[i].shWidth.init(seed + i * 197u + 2u);
            v[i].shPan.init(seed + i * 197u + 3u);
            v[i].shLevel.init(seed + i * 197u + 4u);
            v[i].pulse.reset();
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float midi = cpsmidi(hz);
        float baseNote = std::round(midi);
        float detune = std::fabs(baseNote - midi);
        float maxAmp = amp / N;
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            float note = baseNote + v[i].shNote.process(0.2f, 2.f, st) * detune;
            float w = v[i].shWidth.process(0.5f, 0.5f, st) * 0.5f + 1.5f;   // [1, 2]
            w -= std::floor(w);                                            // SC width wrap
            float s = v[i].pulse.process(midicps(note) * (i + 1), w, st);
            float pan = v[i].shPan.process(0.3f, 0.5f, st);
            float lev = v[i].shLevel.process(0.1f, 0.5f, st) * maxAmp;      // bipolar level
            float vl, vr; pan2(s, pan, lev, vl, vr);
            l += vl; r += vr;
        }
        const float makeup = 1.2f;
        l *= makeup; r *= makeup;
    }
};

// ── Mika — @infinitedigits. "Hum and beeps." ──────────────────────────────────
// A chord-walking sine ping fed through a randomly re-timed allpass (after
// Batuhan Bozkurt's sc140 tweet — the delay-time jumps *are* the beeps), over a
// pulse+noise bass. The PMOsc in the original has mul:0 (silent) and the
// Compander uses identity slopes, so both are omitted.
struct MikaEngine : DroneEngine {
    // chords: Dseq of semitone-offset sets, stepped every 2 bars at 86 bpm
    static constexpr int NCH = 4;
    const float* chordAt(int i, int& len) const {
        static const float c0[] = {-3, -3, -3, 4, 4, 2};
        static const float c1[] = {-7, -7, -7, 0, 0, -3};
        static const float c2[] = {0, 0, 0, 2, 4, 7, 7};
        static const float c3[] = {-8, -8, 7, -1, -8, -1, 1};
        static const float* cs[NCH] = {c0, c1, c2, c3};
        static const int ln[NCH] = {6, 6, 7, 7};
        len = ln[i]; return cs[i];
    }
    Rng rng;
    Dust dMult; float mult = 1.f;                    // hz * TChoose([1,1,1,2])
    Impulse impChord; Dseq seqChord; int chordIdx = 0;
    Impulse impFreq; float freq = 220.f;
    SinOsc osc;
    Dust dRate; float apRate = 8.f;                  // allpass retime rate
    Impulse impAp; TExpRand apRand;
    AllpassC apL, apR;
    float lpfCut = 650.f;                            // effectively-static LFNoise0 cutoff
    Biquad lpMain[2], hpMain[2];
    // bass
    SinOsc bassWidthLfo, noiseAmpLfo, bassLpfLfo, bassGainLfo;
    LFTri bassPanLfo;
    float noiseT = 3.5f, noiseA = 3.5f;              // rrand(3,4) pair
    BlPulse bassOsc; WhiteNoise wn; Biquad bassNoiseLp, bassHp[2], bassLp[2];
    LeakDC dc[2];
    const char* name() const override { return "mika"; }
    void init(uint32_t seed, float sr) override {
        rng.seed(seed);
        dMult.reset(seed + 1u); mult = 1.f;
        impChord.reset(); seqChord.reset(); chordIdx = 0;
        impFreq.reset(); freq = 220.f;
        osc.reset();
        dRate.reset(seed + 2u); apRate = 8.f;
        impAp.reset(); apRand.reset(seed + 3u);
        apL.dl.init(0.45f, sr); apR.dl.init(0.45f, sr);
        lpfCut = linlin(rng.bipolar(), -1.f, 1.f, 300.f, 1000.f);
        for (int c = 0; c < 2; ++c) { lpMain[c].reset(); hpMain[c].reset(); bassHp[c].reset(); bassLp[c].reset(); dc[c].reset(); }
        bassWidthLfo.reset(); noiseAmpLfo.reset(); bassLpfLfo.reset(); bassGainLfo.reset();
        bassPanLfo.reset();
        noiseT = 3.f + rng.uniform(); noiseA = 3.f + rng.uniform();
        bassOsc.reset(); wn.reset(seed + 4u); bassNoiseLp.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        // hz octave hops + chord walk
        static const float mults[4] = {1.f, 1.f, 1.f, 2.f};
        if (dMult.process(0.05f, st) > 0.f) mult = mults[std::min((int)(rng.uniform() * 4), 3)];
        float hzEff = hz * mult;
        if (impChord.process(86.f / 60.f / 8.f, st) > 0.f) chordIdx = seqChord.next(NCH);
        int clen; const float* chord = chordAt(chordIdx, clen);
        if (impFreq.process(1.4333f, st) > 0.f) {
            int j = std::min((int)(rng.uniform() * clen), clen - 1);
            freq = midicps(chord[j] + cpsmidi(hzEff));
        }
        float ping = std::tanh(osc.process(freq, st));
        // allpass with randomly re-timed delay, rounded to 2 ms (L) / 4 ms (R)
        static const float rates[9] = {8, 8, 8, 8, 8, 4, 4, 2, 1};
        if (dRate.process(0.1f, st) > 0.f) apRate = rates[std::min((int)(rng.uniform() * 9), 8)];
        float v = apRand.process(impAp.process(apRate, st), 2e-4f, 0.4f);
        float dL = std::max(std::round(v / 2e-3f) * 2e-3f, 2e-3f);
        float dR = std::max(std::round(v / 4e-3f) * 4e-3f, 4e-3f);
        float ch[2];
        ch[0] = apL.process(ping, dL * sr, combFeedback(dL, 2.f));
        ch[1] = apR.process(ping, dR * sr, combFeedback(dR, 2.f));
        for (int c = 0; c < 2; ++c) {
            ch[c] = lpMain[c].lpf(ch[c] * 0.5f, lpfCut, st);
            ch[c] = hpMain[c].hpf(ch[c], 70.f, st);
        }
        // bass: min chord tone, octaved below 90 Hz
        float basshz = midicps(chord[0] + cpsmidi(hzEff));
        for (int j = 1; j < clen; ++j) basshz = std::min(basshz, midicps(chord[j] + cpsmidi(hzEff)));
        for (int k = 0; k < 3; ++k) if (basshz > 90.f) basshz *= 0.5f;
        float width = linlin(bassWidthLfo.process(1.f / 3.f, st), -1.f, 1.f, 0.2f, 0.4f);
        float bass = bassOsc.process(basshz, width, st);
        float nAmp = linlin(noiseAmpLfo.process(1.f / noiseT, st), -1.f, 1.f, 1.f, noiseA);
        bass += bassNoiseLp.lpf(wn.process() * nAmp, 2.f * basshz, st);
        float bl, br;
        pan2(bass, linlin(bassPanLfo.process(1.f / 6.12f, st), -1.f, 1.f, -0.2f, 0.2f), 1.f, bl, br);
        float bassCut = linlin(bassLpfLfo.process(0.1f, st), -1.f, 1.f, 2.f, 3.f) * basshz;
        bl = bassLp[0].lpf(bassHp[0].hpf(bl, 20.f, st), bassCut, st);
        br = bassLp[1].lpf(bassHp[1].hpf(br, 20.f, st), bassCut, st);
        float bGain = linlin(bassGainLfo.process(0.123f, st), -1.f, 1.f, 1.5f, 2.5f);
        const float makeup = 3.f;
        l = std::tanh(dc[0].process(ch[0] + bGain * bl)) * 0.1f * amp * makeup;
        r = std::tanh(dc[1].process(ch[1] + bGain * br)) * 0.1f * amp * makeup;
    }
};

// ── Fieldsteel — after Eli Fieldsteel's Tutorial 15 ("Composing a Piece"). ────
// A drone of three band-passed saws whose notes are demand-picked from a
// four-note set, blended with a "marimba": three high-resonance SVF bandpasses
// rung by slow LFSaw ramps at demand-picked rhythms and pitches.
struct FieldsteelEngine : DroneEngine {
    struct DroneVoice {
        Impulse imp; float phaseOff = 0.f;
        Dxrand notes; Dbrown gain;
        BPEnv env; LFNoise1 vib; BlSaw saw; Biquad bpf;
        float freq = 220.f, sawGain = 0.7f;
        void init(uint32_t s, float phase) {
            imp.reset(); imp.phase = phase; imp.first = (phase == 0.f);
            phaseOff = phase;
            notes.reset(s + 1u); gain.reset(s + 2u);
            env.reset(); vib.reset(s + 3u); saw.reset(); bpf.reset();
            freq = 220.f; sawGain = 0.7f;
        }
    };
    static constexpr int ND = 3;
    DroneVoice dv[ND];
    LFNoise1 cfLfo[2], rqLfo[2];
    // marimba: shared demand streams, polled by the three layers in order
    Impulse mTrig;
    Dxrand mRhythms; Drand mPitches; Dbrown mGain;
    struct MarimbaLayer {
        LFSaw saw; SVF svf; LFNoise1 resLfo;
        float rate = 0.25f, pitch = 220.f, gain = 0.6f;
    };
    static constexpr int NM = 3;
    MarimbaLayer ml[NM];
    const char* name() const override { return "fieldsteel"; }
    void init(uint32_t seed, float) override {
        static const float phases[ND] = {0.f, 1.f / 3.f, 3.f / 5.f};
        for (int i = 0; i < ND; ++i) dv[i].init(seed + i * 1009u, phases[i]);
        for (int c = 0; c < 2; ++c) { cfLfo[c].reset(seed + 7000u + c); rqLfo[c].reset(seed + 7100u + c); }
        mTrig.reset();
        mRhythms.reset(seed + 8001u); mPitches.reset(seed + 8002u); mGain.reset(seed + 8003u);
        for (int k = 0; k < NM; ++k) {
            ml[k].saw.reset(); ml[k].svf.reset(); ml[k].resLfo.reset(seed + 8100u + k);
            ml[k].rate = 0.25f; ml[k].pitch = 220.f; ml[k].gain = 0.6f;
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float noteOffs[4] = {20.f, 13.f, 0.f, 5.f};
        float midi = cpsmidi(hz);
        // drone: two shared BPF-modulation LFO pairs, voices use them cyclically
        float cf[2], rq[2];
        for (int c = 0; c < 2; ++c) {
            cf[c] = linexp(cfLfo[c].process(0.2f, st), -1.f, 1.f, hz * 0.5f, 3.f * hz);
            rq[c] = linexp(rqLfo[c].process(0.1f, st), -1.f, 1.f, 0.1f, 0.2f);
        }
        float dch[ND];
        for (int i = 0; i < ND; ++i) {
            DroneVoice& v = dv[i];
            float trig = v.imp.process(1.f / 8.f, st);
            if (trig > 0.f) {
                v.freq = midicps(midi - noteOffs[v.notes.next(4)]);
                v.sawGain = v.gain.next(0.6f, 0.8f, 0.125f);
            }
            static const float lv[4] = {0.f, 1.f, 1.f, 0.f};
            static const float tm[3] = {2.f, 5.f, 3.f};
            float e = v.env.process(trig, lv, tm, 3, false, st);
            float f = v.freq * midiratio(v.vib.process(0.1f, st) * 0.1f);
            float s = softclip(v.saw.process(f, st) * v.sawGain) * 0.75f;
            s = v.bpf.bpf(s, cf[i % 2], rq[i % 2], st);
            dch[i] = softclip(s * e * 1.5f);
        }
        float droneL, droneR;
        splay(dch, ND, 0.85f, 0.f, droneL, droneR);
        // marimba: one shared demand stream trio, polled by each layer in order
        float mtrig = mTrig.process(0.25f, st);
        static const float rhythmMul[8] = {0.5f, 1.5f, 2.25f, 11.f / 17.f, 1.f, 2.f, 4.f, 5.f};
        static const float pitchMul[5] = {0.5f, 1.f, 2.f, 1.25f, 4.f};
        float mch[NM];
        for (int k = 0; k < NM; ++k) {
            MarimbaLayer& m = ml[k];
            if (mtrig > 0.f) {
                m.rate = 0.25f * rhythmMul[mRhythms.next(8)];
                m.pitch = hz * pitchMul[mPitches.next(5)];
                m.gain = mGain.next(0.6f, 1.f, 0.125f);
            }
            float res = 1.f - linexp(m.resLfo.process(0.1f, st), -1.f, 1.f, 0.002f, 0.01f);
            float s = m.svf.process(m.saw.process(m.rate, st) * m.gain,
                                    m.pitch, res, 0.f, 1.f, 0.f, st);
            mch[k] = softclip(s * 0.1f);
        }
        float marL, marR;
        splay(mch, NM, 0.7f, 0.f, marL, marR);
        // bal = -0.35 → drone 0.675, marimba 0.325
        const float makeup = 4.f;
        l = (0.675f * droneL + 0.325f * marL) * amp * makeup;
        r = (0.675f * droneR + 0.325f * marR) * amp * makeup;
    }
};

// ── Malone — @infinitedigits. "Thick, organ, stepped." ────────────────────────
// Eight organ voices (pulse stack + sub triangle), each picking a random tone
// of a demand-sequenced chord at its own Dust rate, resonant-lowpassed and
// tremolo'd by an LFPar burst after each chord change, into a Moog ladder and
// a whisper of the shared Schroeder reverb. The original's second RLPF channel
// lands on buses 3/4 and is inaudible, so only the first is ported.
struct MaloneEngine : DroneEngine {
    static constexpr int NCH = 13, NV = 8;
    const float* chordAt(int i, int& len) const {
        static const float c0[] = {0, 12};
        static const float c1[] = {0, 4, 7, 12};
        static const float c2[] = {-1, 4, 7, 12};
        static const float c3[] = {0, 0, 7, 12, 12};
        static const float c4[] = {0, 0, 7, 12, 12};
        static const float c5[] = {0, 4, 7, 12};
        static const float c6[] = {-3, 4, 7, 12};
        static const float c7[] = {-3, 4, 7, 11};
        static const float c8[] = {0, 12};
        static const float c9[] = {0, 4, 7, 12};
        static const float c10[] = {0, 4, 7, 11};
        static const float c11[] = {-3, 4, 7, 9};
        static const float c12[] = {0, 0, 7, 12, 12};
        static const float* cs[NCH] = {c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12};
        static const int ln[NCH] = {2, 4, 4, 5, 5, 4, 4, 4, 2, 4, 4, 4, 5};
        len = ln[i]; return cs[i];
    }
    Impulse impChord; Dust dChord; Dseq seqChord; int chordIdx = 0;
    struct Voice {
        Rng rng; Dust dPick; float offset = 0.f;
        SinOsc vibLfo; float vibRate = 0.3f;
        BlPulse p1, p2, p3; LFTri sub;
        Biquad rlpf;
        Trig tremTrig; float tremDur = 4.f; LFPar trem;
        LFNoise0 nPan; Lag panLag;
        void init(uint32_t s) {
            rng.seed(s);
            dPick.reset(s + 1u); offset = 0.f;
            vibRate = 0.1f + rng.uniform() * 0.4f;                 // Rand(0.1,0.5)
            vibLfo.reset(rng.uniform() * 0.5f);                    // Rand(0,pi) phase
            p1.reset(); p2.reset(); p3.reset(); sub.reset(); rlpf.reset();
            tremTrig.reset(); tremDur = 1.f + rng.uniform() * 7.f; // Rand(1,8)
            trem.reset(); nPan.reset(s + 2u); panLag.reset();
        }
    };
    Voice v[NV];
    LFTri rqLfo;
    MoogFF ladder[2];
    SchroederReverb reverb;
    LFNoise0 nRevMix; Lag revMixLag;
    Biquad hp20[2];
    const char* name() const override { return "malone"; }
    void init(uint32_t seed, float sr) override {
        impChord.reset(); dChord.reset(seed + 11u); seqChord.reset(); chordIdx = 0;
        for (int i = 0; i < NV; ++i) v[i].init(seed + i * 3001u + 100u);
        rqLfo.reset();
        ladder[0].reset(); ladder[1].reset();
        reverb.init(seed + 77777u, sr);
        nRevMix.reset(seed + 5u); revMixLag.reset();
        hp20[0].reset(); hp20[1].reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float chordchange = impChord.process(0.06f, st) + dChord.process(0.005f, st);
        if (chordchange > 0.f) chordIdx = seqChord.next(NCH);
        int clen; const float* chord = chordAt(chordIdx, clen);
        float midi = cpsmidi(hz);
        float rq = linlin(rqLfo.process(0.5f, st), -1.f, 1.f, 0.3f, 1.f);
        float sumL = 0.f, sumR = 0.f;
        for (int i = 0; i < NV; ++i) {
            Voice& w = v[i];
            if (w.dPick.process(0.1f, st) > 0.f)
                w.offset = chord[std::min((int)(w.rng.uniform() * clen), clen - 1)];
            float vib = w.vibLfo.process(w.vibRate, st) * 0.04f;
            float vhz = midicps(midi + w.offset + vib);
            float s = w.p1.process(vhz, 0.17f, st)
                    + w.p2.process(vhz * 0.5f, 0.17f, st)
                    + w.p3.process(vhz * 2.f, 0.17f, st)
                    + w.sub.process(vhz * 0.25f, st);
            s = w.rlpf.rlpf(s, vhz * 6.f, rq, st);
            float tf = w.tremTrig.process(chordchange, w.tremDur, st) * 3.5f;
            s *= w.trem.process(tf, st) * 0.5f + 0.5f;
            float pan = w.panLag.process(w.nPan.process(1.f / 3.f, st), 3.f, st);
            float vl, vr; pan2(s, pan, 1.f / 40.f, vl, vr);
            sumL += vl; sumR += vr;
        }
        float cut = hz * 40.f;
        sumL = ladder[0].process(sumL, cut, 0.f, st);
        sumR = ladder[1].process(sumR, cut, 0.f, st);
        float rvL, rvR; reverb.process(sumL, sumR, st, sr, rvL, rvR);
        float rmix = linlin(revMixLag.process(nRevMix.process(0.1f, st), 10.f, st), -1.f, 1.f, 0.01f, 0.03f);
        const float makeup = 6.f;
        l = hp20[0].hpf(sumL + rmix * rvL, 20.f, st) * 0.5f * amp * makeup;
        r = hp20[1].hpf(sumR + rmix * rvR, 20.f, st) * 0.5f * amp * makeup;
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
    v.emplace_back(new CoilEngine());
    v.emplace_back(new SachikoEngine());
    v.emplace_back(new StarlidsEngine());
    v.emplace_back(new MtLionEngine());
    v.emplace_back(new ApparatusEngine());
    v.emplace_back(new ElianeEngine());
    v.emplace_back(new UnrelaccEngine());
    v.emplace_back(new DreamcrusherEngine());
    v.emplace_back(new RehbergEngine());
    v.emplace_back(new ToshiyaEngine());
    v.emplace_back(new MagicicadaEngine());
    v.emplace_back(new MtZionEngine());
    v.emplace_back(new MikaEngine());
    v.emplace_back(new FieldsteelEngine());
    v.emplace_back(new MaloneEngine());
    return v;
}

}  // namespace draen
