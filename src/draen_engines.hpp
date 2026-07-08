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

// ── Gristle — @infinitedigits. "A primal sawtooth." ───────────────────────────
// Three octave-stacked VarSaws (default width 0.5), each detuned by a sine
// whose rate is itself noise, band-passed by a slowly wandering filter, and
// heard entirely through Greyhole (wet-only).
struct GristleEngine : DroneEngine {
    VarSaw saw[3]; LFNoise0 detN[3]; SinOsc detLfo[3];
    LFNoise0 bpfN; SinOsc bpfLfo; Biquad bpf;
    Greyhole hole;
    const char* name() const override { return "gristle"; }
    void init(uint32_t seed, float sr) override {
        for (int i = 0; i < 3; ++i) { saw[i].reset(); detN[i].reset(seed + i * 17u + 1u); detLfo[i].reset(); }
        bpfN.reset(seed + 91u); bpfLfo.reset(); bpf.reset();
        hole.init(seed + 301u, 0.6f, sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sig = 0.f;
        for (int i = 0; i < 3; ++i) {
            float det = linlin(detLfo[i].process(detN[i].process(1.f, st), st), -1.f, 1.f, 0.99f, 1.01f);
            sig += saw[i].process(hz * (float)(1 << i) * det, 0.5f, st) * amp / (float)(1 << i);
        }
        sig = bpf.bpf(sig, linlin(bpfLfo.process(bpfN.process(1.f, st) * 0.1f, st), -1.f, 1.f, 30.f, 2000.f), 1.f, st);
        hole.process(sig, sig, 0.1f, 0.f, 1.f, 0.707f, 0.9f, st, l, r);
        const float makeup = 0.55f;
        l *= makeup; r *= makeup;
    }
};

// ── Grove — @sixolet. "The orchestra is preparing to play among the arching
// roots. There is no conductor." ──────────────────────────────────────────────
// Five pulsar-synthesis voices: a slow sine "pulse" fires formant-period grain
// envelopes at its zero crossings, self-gated by an audio-rate guard window fed
// back LocalIn-style; ratios are demand-picked when each voice's amplitude LFO
// turns positive. Washed in and out of Greyhole.
struct GroveEngine : DroneEngine {
    static constexpr int C = 5;
    float tempo = 1.5f; int backup = 0;
    struct Chan {
        Rng rng;
        SinOsc formantLfo, ampLfo, envLfo, intervalLfo, widthLfo;
        float formantRate = 0.01f, ampRate = 0.005f, envRate = 0.003f,
              intervalRate = 0.01f, widthRate = 0.01f;
        Drand dNum, dNum2, dBasis, dBasis2;
        float ratio = 1.f, ratio2 = 1.f, ctlRatio = 2.f, ctlRatio2 = 1.f;
        float prevAmp = -1.f;
        LFNoise1 phaseNoise; SinOsc osc1, osc2; float ph1 = 0.f, ph2 = 0.f;
        Impulse impA, impB; SetResetFF ff; Trig1 trigC, trigA;
        float guardTimer = -1.f, guardCtlTimer = -1.f;
        float prevGuard = 0.f, prevGuardCtl = 0.f;      // one-sample feedback
        bool firstGuard = true, firstGuardCtl = true;
        PercEnv grain, slow; float grainCurve = -2.f, slowCurve = -2.f;
        float air0 = 2.f;
        LeakDC dc; OnePole onep; Lag ampLag;
        void init(uint32_t s) {
            rng.seed(s);
            formantRate = 1.f / (40.f + rng.uniform() * 360.f);
            ampRate = 1.f / (100.f + rng.uniform() * 300.f);
            envRate = 1.f / (200.f + rng.uniform() * 600.f);
            intervalRate = 1.f / (10.f + rng.uniform() * 390.f);
            widthRate = 1.f / (10.f + rng.uniform() * 390.f);
            formantLfo.reset(rng.uniform()); ampLfo.reset(rng.uniform());
            envLfo.reset(rng.uniform()); intervalLfo.reset(rng.uniform());
            widthLfo.reset(rng.uniform());
            dNum.reset(s + 11u); dNum2.reset(s + 12u); dBasis.reset(s + 13u); dBasis2.reset(s + 14u);
            ratio = 1.f; ratio2 = 1.f; ctlRatio = 2.f; ctlRatio2 = 1.f;
            prevAmp = -1.f;
            phaseNoise.reset(s + 15u);
            ph1 = rng.uniform() * kTwoPi; ph2 = rng.uniform() * kTwoPi;
            osc1.reset(); osc2.reset();
            impA.reset(); impB.reset(); ff.reset(); trigC.reset(); trigA.reset();
            guardTimer = guardCtlTimer = -1.f;
            prevGuard = prevGuardCtl = 0.f;
            firstGuard = firstGuardCtl = true;
            grain.reset(); slow.reset();
            grainCurve = rack::clamp(-4.f + rng.uniform() * 6.f, -4.f, 0.f);   // Rand(-4,2).clip(-4,0)
            slowCurve = -4.f + rng.uniform() * 4.f;                            // Rand(-4,0)
            air0 = rng.uniform() * 4.f;                                        // Rand(0,4)
            dc.reset(); onep.reset(); ampLag.reset();
        }
    };
    Chan ch[C];
    SinOsc rotLfo; float rotPhase = 0.f;
    LeakDC dcL, dcR;
    Greyhole hole;
    SinOsc xfLfo; float xfRate = 0.003f, xfPhase = 0.f;
    const char* name() const override { return "grove"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        tempo = 1.f + rng.uniform() * 1.5f;
        backup = std::min((int)(rng.uniform() * C), C - 1);
        for (int i = 0; i < C; ++i) ch[i].init(seed + i * 6151u + 1u);
        rotLfo.reset(); rotPhase = rng.uniform() * kTwoPi - (float)M_PI;
        dcL.reset(); dcR.reset();
        hole.init(seed + 40961u, 4.f, sr);
        xfRate = 1.f / (100.f + rng.uniform() * 700.f);
        xfPhase = rng.uniform() * kTwoPi;
        xfLfo.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float nums[9] = {1, 1, 2, 3, 4, 6, 7, 8, 9};
        static const float basis[2] = {2, 4};
        static const float basis2[13] = {2.f/4, 3.f/4, 4.f/4, 5.f/4, 6.f/4, 7.f/4,
                                         8.f/4, 9.f/4, 10.f/4, 11.f/4, 12.f/4, 13.f/4, 14.f/4};
        float amps[C], sig[C];
        // first pass: amplitude LFOs (they also clock the demand streams)
        for (int i = 0; i < C; ++i) {
            Chan& c = ch[i];
            float a = c.ampLfo.process(c.ampRate, st);
            if (a > 0.f && c.prevAmp <= 0.f) {
                c.ratio = nums[c.dNum.next(9)];
                c.ratio2 = nums[c.dNum2.next(9)];
                c.ctlRatio = basis[c.dBasis.next(2)];
                c.ctlRatio2 = basis2[c.dBasis2.next(13)];
            }
            c.prevAmp = a;
            amps[i] = c.ampLag.process(rack::clamp(a, 0.f, 1.f), 4.f, st);
        }
        float ampSum = 0.f;
        for (int i = 0; i < C; ++i) ampSum += amps[i];
        amps[backup] = std::max(amps[backup], 0.4f - ampSum);
        for (int i = 0; i < C; ++i) {
            Chan& c = ch[i];
            float fl = c.formantLfo.process(c.formantRate, st);
            float formantHz = linexp(fl, -1.f, 1.f, std::max(55.f, hz * c.ratio / 8.f), 1000.f);
            float e = c.envLfo.process(c.envRate, st);
            float envLen = linexp(e, -1.f, 1.f, 0.5f * tempo, 8.f * tempo);
            float widthCtl = linexp(e, -1.f, 1.f, 0.005f, 0.5f);
            float interval = c.intervalLfo.process(c.intervalRate, st) * 0.5f + 0.5f;
            float pm = 0.02f * c.phaseNoise.process(hz * c.ratio, st);
            float pulse = (1.f - interval) * c.osc1.process(hz * c.ratio, st, pm + c.ph1)
                        + interval * c.osc2.process(hz * c.ratio2, st, c.ph2);
            float pulseCtl = c.trigC.process(
                c.ff.process(c.impA.process(tempo * c.ctlRatio, st),
                             c.impB.process(tempo * c.ctlRatio2, st)), 0.01f, st);
            float width = linlin(c.widthLfo.process(c.widthRate, st), -1.f, 1.f, 0.1f, 0.9f);
            // audio-rate retrigger, gated by the fed-back guard window
            float rt = c.trigA.process(pulse, 1.f / 4000.f, st) * (1.f - c.prevGuard);
            float rtc = pulseCtl * (1.f - c.prevGuardCtl);
            if (rt > 0.5f || c.firstGuard) { c.guardTimer = width / formantHz; c.firstGuard = false; }
            if (rtc > 0.5f || c.firstGuardCtl) { c.guardCtlTimer = widthCtl * envLen; c.firstGuardCtl = false; }
            float guard = (c.guardTimer > 0.f) ? 1.f : 0.f;
            float guardCtl = (c.guardCtlTimer > 0.f) ? 1.f : 0.f;
            c.guardTimer -= st; c.guardCtlTimer -= st;
            float sound = c.grain.process(rt, width / formantHz, (1.f - width) / formantHz, c.grainCurve, st);
            float slowEnv = c.slow.process(rtc, widthCtl * envLen, (1.f - widthCtl) * envLen, c.slowCurve, st);
            float air = (1.f + c.air0 * amps[i]) * slowEnv;
            sound = c.dc.process(sound);
            sound = c.onep.process(std::tanh(sound * air), 1.f - slowEnv) * amps[i];
            sound /= linexp(fl, -1.f, 1.f, 1.f, 7.f);
            sig[i] = sound;
            c.prevGuard = guard; c.prevGuardCtl = guardCtl;
        }
        float sL, sR;
        splay(sig, C, 0.9f, 0.f, sL, sR);
        float roL, roR;
        rotate2(sL, sR, rotLfo.process(0.001f, st, rotPhase), roL, roR);
        roL = softclip(dcL.process(roL)) * amp;
        roR = softclip(dcR.process(roR)) * amp;
        float wL, wR;
        hole.process(roL, roR, 1.5f / tempo, 0.2f, 2.f, 0.5f, 0.8f, st, wL, wR);
        // XFade2(dry, wet, pan in [-1, 0]): -1 = dry only, 0 = equal mix
        float pos = linlin(xfLfo.process(xfRate, st, xfPhase), -1.f, 1.f, -1.f, 0.f);
        float a1 = std::cos((pos + 1.f) * (float)M_PI_4), a2 = std::sin((pos + 1.f) * (float)M_PI_4);
        const float makeup = 2.5f;
        l = (roL * a1 + wL * a2) * makeup;
        r = (roR * a1 + wR * a2) * makeup;
    }
};

// ── Shields — @infinitedigits. "Bendy, Bloody, Loud." ─────────────────────────
// Six detuned saw pairs, each double-combed, splayed to stereo; the mix is
// recorded onto an 8 s tape loop and read back slower (a slipping repitch),
// combed once more, swept by a Moog ladder, and widened with a big 32-comb
// reverb. Limited, with a 10 s fade-in.
struct ShieldsEngine : DroneEngine {
    static constexpr int V = 6;
    struct Voice {
        Rng rng;
        float det1 = 1.f, det2 = 1.f, lagT = 1.f, mult = 1.f;
        Dust dMult; bool first = true;
        Lag hzLag1, hzLag2;
        BlSaw saw1, saw2;
        struct CombStage {
            CombC cA, cB; LFNoise0 n; Lag lag;
            float maxD = 0.5f, minD = 0.3f, decay = 0.7f;
        } s1, s2;
        void init(uint32_t s, float sr) {
            rng.seed(s);
            det1 = 0.995f + rng.uniform() * 0.005f;
            det2 = 1.f + rng.uniform() * 0.005f;
            lagT = 0.5f + rng.uniform() * 1.5f;
            mult = 1.f; first = true;
            dMult.reset(s + 1u);
            hzLag1.reset(); hzLag2.reset();
            saw1.reset(); saw2.reset();
            s1.maxD = 0.3f + rng.uniform() * 0.3f;
            s1.minD = s1.maxD * (0.5f + rng.uniform() * 0.4f);
            s1.decay = 0.5f + rng.uniform() * 0.5f;
            s2.maxD = 0.2f + rng.uniform() * 0.2f;
            s2.minD = s2.maxD * (0.5f + rng.uniform() * 0.4f);
            s2.decay = 0.5f + rng.uniform() * 0.5f;
            s1.cA.dl.init(0.65f, sr); s1.cB.dl.init(0.65f, sr);
            s2.cA.dl.init(0.45f, sr); s2.cB.dl.init(0.45f, sr);
            s1.n.reset(s + 2u); s2.n.reset(s + 3u);
            s1.lag.reset(); s2.lag.reset();
        }
    };
    Voice v[V];
    Biquad hp80L, hp80R;
    std::vector<float> tapeL, tapeR; int tapeN = 0;
    double mph = 0.0, rph = 0.0;
    LFNoise0 nRate, nRateLag; Lag rateLag;
    CombC c3L, c3R; LFNoise0 n3; Lag l3; float dec3 = 0.7f;
    MoogFF ladL, ladR;
    LFNoise0 nSweepRate, nCutLow; Lag lSweepRate, lCutLow; SinOsc sweepOsc;
    CombVerb<32, 5> verb; LeakDC vdcL, vdcR;
    LFNoise0 nVerbMix; Lag lVerbMix;
    Limiter limL, limR;
    AttackEnv intro;
    const char* name() const override { return "shields"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < V; ++i) v[i].init(seed + i * 7013u + 1u, sr);
        hp80L.reset(); hp80R.reset();
        tapeN = (int)(8.f * sr);
        tapeL.assign(tapeN, 0.f); tapeR.assign(tapeN, 0.f);
        mph = 0.0; rph = 0.0;
        nRate.reset(seed + 101u); nRateLag.reset(seed + 102u); rateLag.reset();
        c3L.dl.init(0.55f, sr); c3R.dl.init(0.55f, sr);
        n3.reset(seed + 103u); l3.reset(); dec3 = 0.5f + rng.uniform() * 0.5f;
        ladL.reset(); ladR.reset();
        nSweepRate.reset(seed + 104u); nCutLow.reset(seed + 105u);
        lSweepRate.reset(); lCutLow.reset(); sweepOsc.reset();
        verb.init(seed + 106u, sr);
        vdcL.reset(); vdcR.reset();
        nVerbMix.reset(seed + 107u); lVerbMix.reset();
        limL.reset(); limR.reset();
        intro.reset(10.f);
    }
    static float tapeRead(const std::vector<float>& buf, int n, double pos) {
        int i0 = (int)pos; float f = (float)(pos - i0);
        int im1 = (i0 - 1 + n) % n, i1 = (i0 + 1) % n, i2 = (i0 + 2) % n;
        float ym1 = buf[im1], y0 = buf[i0 % n], y1 = buf[i1], y2 = buf[i2];
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        static const float mults[12] = {0.5f, 0.5f, 1, 1, 1, 1, 2, 2, 3, 4, 0.5f, 0.25f};
        float chans[V * 2];
        for (int i = 0; i < V; ++i) {
            Voice& w = v[i];
            if (w.first || w.dMult.process(1.f / 30.f, st) > 0.f) {
                w.mult = mults[std::min((int)(w.rng.uniform() * 12), 11)];
                w.first = false;
            }
            float f1 = w.hzLag1.process(hz * w.det1 * w.mult, w.lagT, st);
            float f2 = w.hzLag2.process(hz * w.det2 * w.mult, w.lagT, st);
            float a = w.saw1.process(f1, st), b = w.saw2.process(f2, st);
            // stage 1 comb (per sub-channel), then stage 2 on the sum
            float d1 = linlin(w.s1.lag.process(w.s1.n.process(0.2f, st), 5.f, st), -1.f, 1.f, w.s1.minD, w.s1.maxD);
            float g1 = combFeedback(d1, w.s1.decay);
            a += w.s1.cA.process(a, d1 * sr, g1);
            b += w.s1.cB.process(b, d1 * sr, g1);
            float d2 = linlin(w.s2.lag.process(w.s2.n.process(0.2f, st), 5.f, st), -1.f, 1.f, w.s2.minD, w.s2.maxD);
            float g2 = combFeedback(d2, w.s2.decay);
            a += w.s2.cA.process(a, d2 * sr, g2);
            b += w.s2.cB.process(b, d2 * sr, g2);
            chans[i * 2] = a; chans[i * 2 + 1] = b;
        }
        float sL, sR;
        splay(chans, V * 2, 1.f, 0.f, sL, sR);
        sL = hp80L.hpf(sL, 80.f, st);
        sR = hp80R.hpf(sR, 80.f, st);
        // tape loop: write 0.5 s behind the master head, read at a slipping rate
        float lagT = linlin(nRateLag.process(0.1f, st), -1.f, 1.f, 0.1f, 10.f);
        float rate = linlin(rateLag.process(nRate.process(0.1f, st), lagT, st), -1.f, 1.f, 0.5f, 1.f);
        int wpos = ((int)mph - (int)(0.5f * sr) % tapeN + tapeN) % tapeN;
        tapeL[wpos] = sL; tapeR[wpos] = sR;
        float tL = tapeRead(tapeL, tapeN, rph), tR = tapeRead(tapeR, tapeN, rph);
        mph += 1.0; if (mph >= tapeN) mph -= tapeN;
        rph += rate; if (rph >= tapeN) rph -= tapeN;
        // one more comb
        float d3 = linlin(l3.process(n3.process(0.2f, st), 5.f, st), -1.f, 1.f, 0.3f, 0.5f);
        float g3 = combFeedback(d3, dec3);
        tL += c3L.process(tL, d3 * sr, g3);
        tR += c3R.process(tR, d3 * sr, g3);
        // oscillating Moog ladder
        float swRate = linexp(lSweepRate.process(nSweepRate.process(0.2f, st), 5.f, st), -1.f, 1.f, 0.001f, 20.f);
        float cutLow = linlin(lCutLow.process(nCutLow.process(0.1f, st), 0.1f, st), -1.f, 1.f, 600.f, 6000.f);
        float cut = linexp(sweepOsc.process(swRate, st), -1.f, 1.f, cutLow, 9000.f);
        tL = ladL.process(tL, cut, 0.f, st);
        tR = ladR.process(tR, cut, 0.f, st);
        // reverb
        float wetL, wetR;
        verb.process(tL, tR, st, sr, wetL, wetR);
        wetL = vdcL.process(wetL); wetR = vdcR.process(wetR);
        float vm = linlin(lVerbMix.process(nVerbMix.process(0.2f, st), 5.f, st), -1.f, 1.f, 0.1f, 0.4f);
        tL += vm * wetL; tR += vm * wetR;
        tL = limL.process(tL, 0.95f, 0.1f, st);
        tR = limR.process(tR, 0.95f, 0.1f, st);
        float env = intro.process(st);
        const float makeup = 1.f;
        l = tL * env * amp * 0.5f * makeup;
        r = tR * env * amp * 0.5f * makeup;
    }
};

// ── Eno — @infinitedigits. "Music for airports." ──────────────────────────────
// Two low sines under a plane of eight chorused saws voicing a slowly changing
// chord, a Klank bank ringing the chord tones, and a "piano" that walks the
// actual Music-for-Airports note sequences — a Karplus comb-string crossfaded
// with a PolyPerc pulse — all through Freeverb.
struct EnoEngine : DroneEngine {
    // note sequences from the original (semitone offsets)
    static const float* airportAt(int i, int& len) {
        static const float a0[] = {5, 7, 4, 2, 0, 12, 7, 5, 7, 4, 2, 0};
        static const float a1[] = {5, 7, 4, 2, 0, 12, 4, 7, 5, 0};
        static const float a2[] = {-5, 2, 0, 4, 7, 12, 5, 2, 7, 4, 0, 7, 2, 5, 5, 2, 4, 0};
        static const float a3[] = {7, 7, 2, 4, 4, 4, 2, 0, 7, 0, 0};
        static const float* as[4] = {a0, a1, a2, a3};
        static const int ln[4] = {12, 10, 18, 11};
        len = ln[i]; return as[i];
    }
    static constexpr int NPL = 4;
    const float* planeAt(int i) const {
        static const float p0[] = {0, 4, 7, 12};
        static const float p1[] = {4, 7, 11, 16};
        static const float p2[] = {-3, 0, 4, 7};
        static const float p3[] = {-3, 0, 5, 9};
        static const float* ps[NPL] = {p0, p1, p2, p3};
        return ps[i];
    }
    Dust dPlane; bool firstPlane = true;
    PercEnv planeEnv; TDelay planeDelay; Dxrand planePick; int planeIdx = 0;
    Dust dRate; bool firstRate = true; TChoose chRate; float rateMul = 1.f, noterate = 0.5f;
    Impulse impNote;
    Dust dSeq; bool firstSeq = true; Dxrand seqPick; int airportIdx = 0; int seqPos = 0;
    float seqnote = 0.f;
    SinOsc lowSine1, lowSine2, lowAm1, lowAm2; float amRate1 = 0.005f, amRate2 = 0.005f;
    struct SawVoice {
        BlSaw saw; SinOsc cutLfo; float cutRate = 0.05f, cutPhase = 0.f;
        DelayC chor; LFNoise1 chorN; float chorRate = 7.f, chorBase = 0.02f;
        LFNoise0 nPan; Lag panLag; Biquad lp;
    };
    static constexpr int NV = 8;
    SawVoice sv[NV];
    LFNoise0 nLadder; Lag ladderLag; MoogFF ladL, ladR;
    Ringz klank[2][4]; PinkNoise pinkL, pinkR;
    // piano
    LFNoise0 nNoiseHz; Lag noiseHzLag; LFNoise2 pnoise; Decay2 pDecay; Impulse impNoise;
    CombL string1, string2;
    Biquad pRlp, pHp;
    PercEnv polyEnv; BlPulse polyPulse; MoogFF polyFF;
    LFNoise0 nMixRate; SinOsc mixLfo;
    Biquad outLp[2], outHp[2];
    FreeVerbMono fvL, fvR;
    float introT = 0.f;
    const char* name() const override { return "eno"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        dPlane.reset(seed + 1u); firstPlane = true;
        planeEnv.reset(); planeDelay.reset(); planePick.reset(seed + 2u); planeIdx = 0;
        dRate.reset(seed + 3u); firstRate = true; chRate.reset(seed + 4u);
        rateMul = 0.78f + rng.uniform() * 0.54f;      // Rand(0.78,1.32)
        noterate = 0.5f * rateMul;
        impNote.reset();
        dSeq.reset(seed + 5u); firstSeq = true; seqPick.reset(seed + 6u);
        airportIdx = 0; seqPos = 0; seqnote = 0.f;
        lowSine1.reset(); lowSine2.reset(); lowAm1.reset(); lowAm2.reset();
        amRate1 = 0.001f + rng.uniform() * 0.009f;
        amRate2 = 0.001f + rng.uniform() * 0.009f;
        for (int i = 0; i < NV; ++i) {
            sv[i].saw.reset();
            sv[i].cutRate = linlin(rng.uniform(), 0.f, 1.f, 1.f / 30.f, 1.f / 10.f);
            sv[i].cutPhase = rng.uniform() * kTwoPi;
            sv[i].cutLfo.reset();
            sv[i].chor.dl.init(0.05f, sr);
            sv[i].chorN.reset(seed + 10u + i);
            sv[i].chorRate = 5.f + rng.uniform() * 5.f;
            sv[i].chorBase = 0.01f + rng.uniform() * 0.02f;
            sv[i].nPan.reset(seed + 30u + i); sv[i].panLag.reset();
            sv[i].lp.reset();
        }
        nLadder.reset(seed + 50u); ladderLag.reset(); ladL.reset(); ladR.reset();
        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < 4; ++j) klank[c][j].reset();
        pinkL.reset(seed + 60u); pinkR.reset(seed + 61u);
        nNoiseHz.reset(seed + 70u); noiseHzLag.reset(); pnoise.reset(seed + 71u);
        pDecay.reset(); impNoise.reset();
        string1.dl.init(1.0f, sr); string2.dl.init(1.0f, sr);
        pRlp.reset(); pHp.reset();
        polyEnv.reset(); polyPulse.reset(); polyFF.reset();
        nMixRate.reset(seed + 80u); mixLfo.reset();
        for (int c = 0; c < 2; ++c) { outLp[c].reset(); outHp[c].reset(); }
        fvL.init(sr); fvR.init(sr);
        introT = 0.f;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float note = cpsmidi(hz);
        // slow chord ("planes") selection, with a dip on each change
        float planeTrig = dPlane.process(1.f / 30.f, st) + (firstPlane ? 1.f : 0.f);
        firstPlane = false;
        float planeenv = 1.f - planeEnv.process(planeTrig, 3.f, 10.f, -4.f, st) * 0.9f;
        if (planeDelay.process(planeTrig, 3.f, st) > 0.f) planeIdx = planePick.next(NPL);
        const float* plane = planeAt(planeIdx);
        // note pulse rate: TChoose([0.02,0.05,1,2,0.5,0.25,2]/2) * Rand(0.78,1.32)
        static const float rateVals[7] = {0.01f, 0.025f, 0.5f, 1.f, 0.25f, 0.125f, 1.f};
        float rateTrig = dRate.process(1.f, st) + (firstRate ? 1.f : 0.f);
        firstRate = false;
        noterate = chRate.process(rateTrig, rateVals, 7) * rateMul;
        if (noterate <= 0.f) noterate = 0.25f * rateMul;
        float notepulse = impNote.process(noterate, st);
        // airport sequence walk
        float seqTrig = dSeq.process(0.1f, st) + (firstSeq ? 1.f : 0.f);
        firstSeq = false;
        if (seqTrig > 0.f) airportIdx = seqPick.next(4);
        int alen; const float* airport = airportAt(airportIdx, alen);
        if (notepulse > 0.f) { seqnote = airport[seqPos % alen]; ++seqPos; }
        // low sines
        float snd = lowSine1.process(midicps(note - 24.f), st)
                    * linlin(lowAm1.process(amRate1, st), -1.f, 1.f, 0.05f, 0.15f);
        snd += lowSine2.process(midicps(note - 12.f), st)
               * linlin(lowAm2.process(amRate2, st), -1.f, 1.f, 0.001f, 0.05f);
        // eight chorused saw voices on the plane chord
        float planeL = 0.f, planeR = 0.f;
        for (int i = 0; i < NV; ++i) {
            SawVoice& w = sv[i];
            float off = plane[i % 4] + ((i % 4) == 0 ? -36.f : -24.f);
            float s = w.saw.process(midicps(note + off), st) * 0.9f;
            float cut = linexp(w.cutLfo.process(w.cutRate, st, w.cutPhase), -1.f, 1.f, hz, hz * 5.f);
            s = w.lp.lpf(s, cut, st);
            float dt = (w.chorBase + 0.01f * w.chorN.process(w.chorRate, st)) / 15.f;
            s = w.chor.process(s, rack::clamp(dt * sr, 8.f, 0.045f * sr));
            float pan = w.panLag.process(w.nPan.process(1.f / 3.f, st), 3.f, st);
            float vl, vr; pan2(s, pan, 1.f / 7.f, vl, vr);
            planeL += vl; planeR += vr;
        }
        float sndL = snd + planeenv * planeL;
        float sndR = snd + planeenv * planeR;
        float lcut = linexp(ladderLag.process(nLadder.process(1.f / 6.f, st), 6.f, st), -1.f, 1.f, hz * 2.f, hz * 60.f);
        sndL = ladL.process(std::tanh(sndL), lcut, 0.f, st);
        sndR = ladR.process(std::tanh(sndR), lcut, 0.f, st);
        // Klank ringing the chord tones (per-side pink noise excitation)
        float kl = 0.f, kr_ = 0.f;
        float exL = pinkL.process() * 0.004f, exR = pinkR.process() * 0.004f;
        for (int j = 0; j < 4; ++j) {
            float f = midicps(note + plane[j]);
            kl += klank[0][j].process(exL, f, 1.f, st);
            kr_ += klank[1][j].process(exR, f, 1.f, st);
        }
        sndL += 0.55f * kl; sndR += 0.55f * kr_;
        // piano 1: noise burst into a detuned comb pair (Karplus string)
        float noiseHz = linlin(noiseHzLag.process(nNoiseHz.process(0.1f, st), 10.f, st), -1.f, 1.f, 2000.f, 5000.f);
        float pianohz = std::max(midicps(note + seqnote - 12.f), 2.f);
        float nz = pnoise.process(noiseHz, st) * pDecay.process(impNoise.process(noterate, st), 0.01f, 1.f, st);
        float dt1 = rack::clamp(sr / (pianohz * 1.0005f), 2.f, 0.95f * sr);
        float dt2 = rack::clamp(sr / (pianohz * 0.9996f), 2.f, 0.95f * sr);
        float string = string1.process(nz, dt1, combFeedback(dt1 / sr, 6.f))
                     + string2.process(nz, dt2, combFeedback(dt2 / sr, 6.f));
        float piano = pRlp.rlpf(string, 2.f * pianohz, 4.f, st) * amp;
        piano = pHp.hpf(piano, 40.f, st);
        // piano 2: PolyPerc (perc-enveloped pulse through a Moog)
        float piano2 = polyEnv.process(notepulse, 0.01f, 4.f, -4.f, st)
                     * polyFF.process(polyPulse.process(midicps(note + seqnote), 0.5f, st), hz * 1.5f, 2.f, st);
        float mixFr = linlin(mixLfo.process(linlin(nMixRate.process(0.1f, st), -1.f, 1.f, 0.01f, 0.1f), st), -1.f, 1.f, 0.1f, 0.9f);
        float pmix = selectx(mixFr, piano * 0.3f, piano2);
        sndL += pmix; sndR += pmix;
        float locut = midicps(note + 36.f);
        sndL = outHp[0].hpf(outLp[0].lpf(sndL, locut, st), 120.f, st);
        sndR = outHp[1].hpf(outLp[1].lpf(sndR, locut, st), 120.f, st);
        introT += st;
        float intro = rack::clamp((introT - 0.5f) / 3.f, 0.f, 1.f);
        sndL *= intro; sndR *= intro;
        const float makeup = 1.f;
        l = fvL.process(sndL, 0.45f, 1.f, 0.5f) * amp * makeup;
        r = fvR.process(sndR, 0.45f, 1.f, 0.5f) * amp * makeup;
    }
};

// ── Belong — @infinitedigits. "Thick, enveloping, shimmering." ────────────────
// Ten chorused saws walking scrambled chord tones, enveloped differently per
// side, overdubbing themselves onto a 16-beat tape loop; with a pulse+noise
// bass, a comb-bank reverb, and a spaced-out kick that only appears when the
// amp knob is pushed past 0.7.
struct BelongEngine : DroneEngine {
    float bpm = 90.f;
    float chords[4][3]; float notesAll[12];
    Impulse impPulse, impFive, impFourth, impWin;
    Impulse impEighth, impQuarter, impHalf;
    Dseq seqChord, seqNote, seqOct;
    TExpRand selRand;
    float bassnote = 0.f, noteVal = 0.f, octOff = 0.f;
    SinOsc envSelLfo; float envSelRate = 0.15f, envSelPhase = 0.f;
    TExpRand et1[3], et2[3];
    BPEnv envL, envR;
    Lag noteLag;
    static constexpr int NS = 10;
    BlSaw saws[NS]; DelayC chor[NS]; LFNoise1 chorN[NS];
    float chorRate[NS] = {}, chorBase[NS] = {};
    LFNoise0 nCut; float cutConst = 0.f; Lag cutLag; MoogFF ladder;
    // tape loop
    std::vector<float> tapeL, tapeR; int loopN = 0, tapePos = 0;
    float recT = 0.f; BPEnv winEnv;
    // bass
    Lag bassLag; SinOsc bassWidthLfo, bassNoiseAmpLfo, bassNoiseCutLfo, bassLpLfo, bassTremLfo;
    float noiseT = 3.5f, noiseA = 3.5f, tremRate = 0.2f, tremPhase = 0.f;
    BlPulse bassOsc; WhiteNoise wn; Biquad bassNoiseLp, bassHp[2], bassLp[2];
    LFTri bassPanLfo;
    float bassSwellT = 0.f;
    // kick
    SinOsc kickSelLfo; float kickSelRate = 1.f / 60.f;
    TDelay kickDelay;
    BPEnv kickEnv0, kickEnv1; LFPulse kickPulse; WhiteNoise kickNoise;
    Biquad kickLp, kickEq; SinOsc kickSin; PercEnv kickGate;
    CombVerb<16, 5> kickVerb;
    // reverb + out
    CombVerb<8, 4> verb;
    AttackEnv intro;
    Biquad outHp[2];
    const char* name() const override { return "belong"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        bpm = 60.f + rng.uniform() * 70.f;
        // chords scrambled at build time
        static const float base[4][3] = {{4, 7, 11}, {0, 4, 7}, {7, 11, 14}, {2, 6, 9}};
        int order[4] = {0, 1, 2, 3};
        for (int i = 3; i > 0; --i) { int j = (int)(rng.uniform() * (i + 1)); std::swap(order[i], order[j]); }
        for (int i = 0; i < 4; ++i) {
            float c[3] = {base[order[i]][0], base[order[i]][1], base[order[i]][2]};
            for (int k = 2; k > 0; --k) { int j = (int)(rng.uniform() * (k + 1)); std::swap(c[k], c[j]); }
            for (int k = 0; k < 3; ++k) chords[i][k] = c[k];
        }
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 4; ++i) notesAll[k * 4 + i] = chords[i][k];   // flop.flatten
        impPulse.reset(); impFive.reset(); impFourth.reset(); impWin.reset();
        impEighth.reset(); impQuarter.reset(); impHalf.reset();
        seqChord.reset(); seqNote.reset(); seqOct.reset();
        selRand.reset(seed + 1u);
        bassnote = 0.f; noteVal = 0.f; octOff = 0.f;
        envSelRate = 0.1f + rng.uniform() * 0.1f; envSelPhase = rng.uniform() * 2.f;
        envSelLfo.reset();
        for (int k = 0; k < 3; ++k) { et1[k].reset(seed + 10u + k); et2[k].reset(seed + 20u + k); }
        envL.reset(); envR.reset(); noteLag.reset();
        for (int i = 0; i < NS; ++i) {
            saws[i].reset();
            chor[i].dl.init(0.05f, sr);
            chorN[i].reset(seed + 30u + i);
            chorRate[i] = 5.f + rng.uniform() * 5.f;
            chorBase[i] = 0.01f + rng.uniform() * 0.02f;
        }
        nCut.reset(seed + 50u);
        { Rng r2; r2.seed(seed + 51u); cutConst = r2.bipolar(); }
        cutLag.reset(); ladder.reset();
        loopN = std::max((int)(16.f * 60.f / bpm * sr), 1);
        tapeL.assign(loopN, 0.f); tapeR.assign(loopN, 0.f);
        tapePos = 0; recT = 0.f; winEnv.reset();
        bassLag.reset(); bassWidthLfo.reset(); bassNoiseAmpLfo.reset();
        bassNoiseCutLfo.reset(); bassLpLfo.reset();
        noiseT = 3.f + rng.uniform(); noiseA = 3.f + rng.uniform();
        tremRate = 0.1f + rng.uniform() * 0.2f; tremPhase = rng.uniform() * 2.f;
        bassTremLfo.reset();
        bassOsc.reset(); wn.reset(seed + 60u); bassNoiseLp.reset();
        for (int c = 0; c < 2; ++c) { bassHp[c].reset(); bassLp[c].reset(); outHp[c].reset(); }
        bassPanLfo.reset(); bassSwellT = 0.f;
        kickSelRate = 1.f / (40.f + rng.uniform() * 40.f);
        kickSelLfo.reset(); kickDelay.reset();
        kickEnv0.reset(); kickEnv1.reset(); kickPulse.reset(); kickNoise.reset(seed + 70u);
        kickLp.reset(); kickEq.reset(); kickSin.reset(); kickGate.reset();
        kickVerb.init(seed + 80u, sr);
        verb.init(seed + 90u, sr);
        intro.reset(5.f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float beat = 60.f / bpm;
        float pulse = impPulse.process(1.f / (4.f * beat), st);
        float five = impFive.process(5.f / (4.f * beat), st);
        float fourth = impFourth.process(1.f / (16.f * beat), st);
        float eighth = impEighth.process(1.f / (32.f * beat), st);
        float quarter = impQuarter.process(1.f / (16.f * beat), st);
        float half = impHalf.process(1.f / (8.f * beat), st);
        float hzMidi = cpsmidi(hz);
        if (pulse > 0.f) {
            int ci = seqChord.next(4);
            bassnote = std::min(chords[ci][0], std::min(chords[ci][1], chords[ci][2]));
        }
        float sel = selRand.process(pulse, 0.01f, 6.f);
        float noteTrig = (sel >= 4.f) ? five : pulse;
        if (noteTrig > 0.f) noteVal = notesAll[seqNote.next(12)];
        static const float octs[4] = {12, -12, 0, 24};
        if (fourth > 0.f) octOff = octs[seqOct.next(4)];
        // per-side envelopes, re-randomized every pulse
        float envSel = envSelLfo.process(envSelRate, st, envSelPhase) * 0.5f + 0.5f;
        float t1[3], t2[3], s1 = 0.f, s2 = 0.f;
        t1[0] = et1[0].process(pulse, 0.2f, 1.f); t1[1] = et1[1].process(pulse, 0.01f, 1.f); t1[2] = et1[2].process(pulse, 0.01f, 1.f);
        t2[0] = et2[0].process(pulse, 0.2f, 1.f); t2[1] = et2[1].process(pulse, 0.01f, 1.f); t2[2] = et2[2].process(pulse, 0.01f, 1.f);
        for (int k = 0; k < 3; ++k) { s1 += t1[k]; s2 += t2[k]; }
        for (int k = 0; k < 3; ++k) { t1[k] *= 4.f * beat / s1; t2[k] *= 4.f * beat / s2; }
        static const float elv[4] = {0.f, 1.f, 1.f, 0.f};
        float e1 = envL.process(pulse, elv, t1, 3, true, st);
        float e2 = envR.process(pulse, elv, t2, 3, true, st);
        e1 = selectx(envSel, 1.f, e1);
        e2 = selectx(envSel, 1.f, e2);
        // saw stack (all voices on the same lagged note, chorused apart)
        float noteHz = noteLag.process(midicps(noteVal + octOff + hzMidi), 0.02f, st);
        float stack = 0.f;
        for (int i = 0; i < NS; ++i) {
            float s = saws[i].process(noteHz, st);
            float dt = (chorBase[i] + 0.01f * chorN[i].process(chorRate[i], st)) / 15.f;
            s = chor[i].process(s, rack::clamp(dt * sr, 8.f, 0.045f * sr));
            stack += s * 0.25f;
        }
        stack *= (float)M_SQRT1_2;   // Pan2(x, 0)
        float lcut = linexp(cutLag.process(nCut.process(1.f / 6.f, st) + cutConst, 6.f, st), -1.f, 1.f, hz * 8.f, hz * 15.f);
        stack = ladder.process(std::tanh(stack), lcut, 0.f, st);
        float sndL = stack * e1, sndR = stack * e2;
        // tape loop overdub
        recT += st;
        float recamt = rack::clamp((recT - 16.f * beat) / 0.1f, 0.f, 1.f) * 0.9f;
        float tapePlayL = tapeL[tapePos], tapePlayR = tapeR[tapePos];
        float alevel = std::min(2.f * amp, 1.f);
        sndL = sndL * 0.5f + tapePlayL * recamt * alevel;
        sndR = sndR * 0.5f + tapePlayR * recamt * alevel;
        float wtm[3] = {0.1f, std::max(16.f * beat - 0.2f, 0.1f), 0.1f};
        float win = winEnv.process(impWin.process(1.f / (16.f * beat), st), elv, wtm, 3, false, st);
        tapeL[tapePos] = sndL * win; tapeR[tapePos] = sndR * win;
        if (++tapePos >= loopN) tapePos = 0;
        // bass
        float basshz = midicps(bassnote + hzMidi);
        for (int k = 0; k < 3; ++k) if (basshz > 90.f) basshz *= 0.5f;
        basshz = bassLag.process(basshz, 0.1f, st);
        float width = linlin(bassWidthLfo.process(1.f / 3.f, st), -1.f, 1.f, 0.2f, 0.4f);
        float bass = bassOsc.process(basshz, width, st);
        float nAmp = linlin(bassNoiseAmpLfo.process(1.f / noiseT, st), -1.f, 1.f, 1.f, noiseA);
        float ncut = linlin(bassNoiseCutLfo.process(0.123f, st), -1.f, 1.f, 1.5f, 2.5f) * basshz;
        bass += bassNoiseLp.lpf(wn.process() * nAmp, ncut, st);
        float bl, br;
        pan2(bass, linlin(bassPanLfo.process(1.f / 6.12f, st), -1.f, 1.f, -0.2f, 0.2f), 1.f, bl, br);
        float bcut = linlin(bassLpLfo.process(0.1f, st), -1.f, 1.f, 2.f, 3.f) * basshz;
        bl = bassLp[0].lpf(bassHp[0].hpf(bl, 20.f, st), bcut, st);
        br = bassLp[1].lpf(bassHp[1].hpf(br, 20.f, st), bcut, st);
        bassSwellT += st;
        float swell = rack::clamp((bassSwellT - 6.f) / 6.f, 0.f, 1.f) * 0.2f;
        float trem = linlin(bassTremLfo.process(tremRate, st, tremPhase), -1.f, 1.f, 0.f, 0.2f);
        float bgain = (trem + swell) * 0.1f;   // * -20 dB
        sndL += bgain * bl; sndR += bgain * br;
        // kick (gated: only when the amp knob is past 0.7)
        float kick = 0.f;
        {
            static const int NOPT = 9;
            float pulses[NOPT] = {eighth, eighth, quarter, half, half, pulse, pulse, pulse, pulse};
            int ki = rack::clamp((int)((kickSelLfo.process(kickSelRate, st, 4.712389f) * 0.5f + 0.5f) * 9.f), 0, NOPT - 1);
            float ktrig = kickDelay.process(pulses[ki], 2.f * beat, st);
            static const float k0lv[4] = {0.5f, 1.f, 0.5f, 0.f};
            static const float k0tm[3] = {0.005f, 0.06f, 15.6f};
            float env0 = kickEnv0.process(ktrig, k0lv, k0tm, 3, false, st);
            static const float k1lv[3] = {110.f, 59.f, 29.f};
            static const float k1tm[2] = {0.005f, 0.29f};
            float env1m = midicps(kickEnv1.process(ktrig, k1lv, k1tm, 2, false, st));
            float out = kickPulse.process(env1m, 0.5f, st) - 0.5f;
            out += kickNoise.process() * 60.f;
            out = kickLp.lpf(out, env1m * 1.5f, st) * env0;
            out += kickSin.process(env1m, st, 0.5f) * env0;
            out = rack::clamp(out * 1.2f, -1.f, 1.f);
            out *= kickGate.process(ktrig, 0.01f, 2.f, -4.f, st);
            float kwL, kwR;
            kickVerb.process(out, out, st, sr, kwL, kwR);
            float ksum = out + 0.7f * (kwL + kwR) * 0.5f;
            ksum = kickEq.peakeq(ksum, basshz, 1.f, 12.f, st);
            kick = ksum * 0.0631f;                       // -24 dB
        }
        float kgate = (amp > 0.7f) ? 1.f : 0.f;
        sndL = sndL * 0.6f + kgate * kick;
        sndR = sndR * 0.6f + kgate * kick;
        // reverb
        float wL, wR;
        verb.process(sndL, sndR, st, sr, wL, wR);
        sndL += 0.4f * wL; sndR += 0.4f * wR;
        float env = intro.process(st);
        const float makeup = 1.6f;
        l = outHp[0].hpf(sndL * amp * env * 0.5f, 40.f, st) * makeup;
        r = outHp[1].hpf(sndR * amp * env * 0.5f, 40.f, st) * makeup;
    }
};

// ── Ruins — @rplktr & @sixolet. "A reality darker than fiction." ──────────────
// Metallic 2- and 3-operator FM hits (after James McCartney's "100 FM Synths")
// fired by a self-clocked trigger loop, drowned in a very long reverb whose
// level warbles with tape-style wow and flutter, over a windy noise floor.
// Only the demand-selected instrument is rendered; all envelopes share the
// global trigger, so switching mid-decay lands at the right envelope phase.
struct RuinsEngine : DroneEngine {
    static constexpr int NI = 12, NOPS = 6;
    struct Op {
        float atk = 0.1f, rel = 0.5f, lvl = 0.5f, mult = 1.f, phase = 0.f;
        SinOsc osc[2];
    };
    struct Instr {
        int kind = 0; float det2 = 0.f;
        Op ops[NOPS];
    };
    Instr ins[NI];
    Rng rng;
    float trigElapsed = 1e9f;
    float rate = 1.f; float sinceTrig = 1e9f; bool first = true;
    Drand dRate, dInt1, dInt2, dChord, dVelo, dInstr; CoinGate coin;
    float int1 = 1.f, int2 = 1.f, velo = 1.f; int which = 0;
    LFNoise2 panN[2];
    SinOsc wobbleOsc, flutterOsc; LFNoise2 flutterVar;
    LFNoise0 nNoiseHz; Lag noiseHzLag; SinOsc noiseVol; LFNoise2 wind[2];
    Compander compL, compR;
    GVerbApprox gverb;
    Limiter limL, limR;
    const char* name() const override { return "ruins"; }
    void init(uint32_t seed, float sr) override {
        rng.seed(seed);
        for (int i = 0; i < NI; ++i) {
            Instr& I = ins[i];
            I.kind = std::min((int)(rng.uniform() * 3), 2);
            float d = (rng.uniform() * 2.f - 1.f) * 1.8f;
            I.det2 = d * d;
            for (int o = 0; o < NOPS; ++o) {
                Op& op = I.ops[o];
                op.atk = 0.001f * std::pow(400.f, rng.uniform());     // exprand(0.001, 0.4)
                op.rel = 0.1f * std::pow(20.f, rng.uniform());        // exprand(0.1, 2.0)
                bool carrier = (I.kind == 0) ? (o % 2 == 1)
                             : (I.kind == 1 ? (o % 3 == 2) : (o % 3 != 0));
                float u1 = rng.uniform(), u2 = rng.uniform();
                if (carrier) {
                    op.mult = std::floor(10.f * std::min(u1, u2)) + 1.f;   // linrand(10)+1
                    float m = 0.5f + rng.uniform() * 0.1f;                 // rrand(0.5, 0.6)
                    float x = rng.uniform() * m; op.lvl = x * x;
                } else {
                    op.mult = std::floor(5.f * std::min(u1, u2)) + 1.f;    // linrand(5)+1
                    float x = rng.uniform() * 3.f; op.lvl = x * x;
                }
                float p = rng.uniform() * 1.3f;
                op.phase = (!carrier) ? p * p * p : 0.f;                   // 1.3.rand.cubed
                op.osc[0].reset(rng.uniform()); op.osc[1].reset(rng.uniform());
            }
        }
        trigElapsed = 1e9f; rate = 1.f; sinceTrig = 1e9f; first = true;
        dRate.reset(seed + 1u); dInt1.reset(seed + 2u); dInt2.reset(seed + 3u);
        dChord.reset(seed + 4u); dVelo.reset(seed + 5u); dInstr.reset(seed + 6u);
        coin.reset(seed + 7u);
        int1 = int2 = 1.f; velo = 1.f; which = 0;
        panN[0].reset(seed + 8u); panN[1].reset(seed + 9u);
        wobbleOsc.reset(); flutterOsc.reset(); flutterVar.reset(seed + 10u);
        nNoiseHz.reset(seed + 11u); noiseHzLag.reset(); noiseVol.reset();
        wind[0].reset(seed + 12u); wind[1].reset(seed + 13u);
        compL.reset(); compR.reset();
        gverb.init(seed + 14u, sr);
        limL.reset(); limR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float rates[12] = {1, 1, 1, 1, 2, 2, 2, 4, 4, 8, 8, 16};
        static const float ivs1[17] = {0.25f, 0.891f, 0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1, 1,
                                       1.189f, 1.782f, 2, 2, 2, 4};
        static const float ivs2[9] = {0.25f, 0.891f, 0.5f, 1, 1.189f, 1.498f, 1.782f, 2, 2.378f};
        static const float chordP[7] = {0, 0, 0, 0, 1, 1, 1};
        static const float velos[7] = {1.f, 1.f, 1.f, 0.66f, 0.66f, 0.33f, 0.1f};
        // self-clocked trigger: period = the demand-picked "rate" in seconds
        sinceTrig += st;
        if (first || sinceTrig >= rate) {
            first = false; sinceTrig = 0.f;
            rate = rates[dRate.next(12)];
            int1 = ivs1[dInt1.next(17)];
            int2 = ivs2[dInt2.next(9)];
            if (chordP[dChord.next(7)] < 0.5f) int2 = int1;
            velo = velos[dVelo.next(7)];
            if (rng.uniform() < 0.69f) which = dInstr.next(NI);            // CoinGate(0.69)
            trigElapsed = 0.f;
        }
        trigElapsed += st;
        float t = trigElapsed;
        // render only the selected instrument, on both interval channels
        Instr& I = ins[which];
        float freqs[2] = {hz * int1 + I.det2, hz * int2 + I.det2};
        auto envAt = [&](const Op& op) -> float {
            if (t < op.atk) return envCurve(t / op.atk, 4.f) * op.lvl;
            float u = (t - op.atk) / op.rel;
            if (u >= 1.f) return 0.f;
            return (1.f - envCurve(u, -4.f)) * op.lvl;
        };
        float sL = 0.f, sR = 0.f;
        float pans[2] = {panN[0].process(0.05f, st), panN[1].process(0.05f, st)};
        for (int c = 0; c < 2; ++c) {
            float f = freqs[c], out = 0.f;
            Op* op = I.ops;
            if (I.kind == 0) {
                for (int k = 0; k < 3; ++k) {
                    float m = op[k * 2].osc[c].process(f * op[k * 2].mult, st, op[k * 2].phase) * envAt(op[k * 2]);
                    out += op[k * 2 + 1].osc[c].process(f * op[k * 2 + 1].mult, st, m) * envAt(op[k * 2 + 1]);
                }
            } else if (I.kind == 1) {
                for (int k = 0; k < 2; ++k) {
                    float m = op[k * 3].osc[c].process(f * op[k * 3].mult, st, op[k * 3].phase) * envAt(op[k * 3]);
                    m = op[k * 3 + 1].osc[c].process(f * op[k * 3 + 1].mult, st, m) * envAt(op[k * 3 + 1]);
                    out += op[k * 3 + 2].osc[c].process(f * op[k * 3 + 2].mult, st, m) * envAt(op[k * 3 + 2]);
                }
            } else {
                for (int k = 0; k < 2; ++k) {
                    float m = op[k * 3].osc[c].process(f * op[k * 3].mult, st, op[k * 3].phase) * envAt(op[k * 3]);
                    out += op[k * 3 + 1].osc[c].process(f * op[k * 3 + 1].mult, st, m) * envAt(op[k * 3 + 1]);
                    out += op[k * 3 + 2].osc[c].process(f * op[k * 3 + 2].mult, st, m) * envAt(op[k * 3 + 2]);
                }
            }
            float pl, pr;
            pan2(out, rack::clamp(pans[c], -1.f, 1.f), velo, pl, pr);
            sL += pl; sR += pr;
        }
        float soundL = 0.2f * 0.5f * sL, soundR = 0.2f * 0.5f * sR;
        soundL = compL.process(soundL, soundL, 0.1f, 1.f, 0.1f, 0.01f, 0.1f, st);
        soundR = compR.process(soundR, soundR, 0.1f, 1.f, 0.1f, 0.01f, 0.1f, st);
        // the wind
        float nhz = linlin(noiseHzLag.process(nNoiseHz.process(0.1f, st), 10.f, st), -1.f, 1.f, 2000.f, 5000.f);
        float nv = noiseVol.process(0.1f, st);
        soundL += wind[0].process(nhz, st) * 0.0005f * rack::clamp(nv, 0.f, 1.f);
        soundR += wind[1].process(nhz, st) * 0.0005f * (1.f - rack::clamp(nv, -1.f, 0.f));
        // wow & flutter, modulating the reverb output level
        float s = wobbleOsc.process(33.f / 60.f, st);
        float wob = 0.07f * std::copysign(std::pow(std::fabs(s), 39.f), s);
        float wow = (wob > 0.f) ? 0.f : wob;
        float flut = 0.04f * flutterOsc.process(6.f + flutterVar.process(2.f, st), st);
        float defects = 1.f + wow + flut;
        float gl, gr;
        gverb.process(soundL + soundR, 103.f, 0.43f, st, gl, gr);
        float outL = (soundL * 0.562f + gl * 0.32f) * defects;  // dry -5 dB; tail lifted
        float outR = (soundR * 0.562f + gr * 0.32f) * defects;  // (approx GVerb wash weight)
        const float makeup = 14.f;
        l = limL.process(outL, 1.f, 0.1f, st) * amp * makeup;
        r = limR.process(outR, 1.f, 0.1f, st) * amp * makeup;
    }
};


// ── SUNNO — (uncredited in source; the doom of SUNN O))). ─────────────────────
// Five "guitars", each two Karplus-Strong strings (one with negative-decay
// feedback for the octave-under growl) re-plucked at random, crushed through
// crossover distortion and two cascaded tanh+RLPF gain stages, with a local
// feedback path on two of them; over a pulse-pair bass and the shared reverb.
struct SunnoEngine : DroneEngine {
    LFNoise0 nHz; Lag hzLag;
    struct StringV {
        Rng rng;
        Dust dPick; bool first = true;
        Pluck pluck;
        float odd = 1.f;
        PercEnv exEnv; float exT = 1e9f;
        SinOsc exOsc;
        DelayC chor; LFNoise1 chorN; float chorBase = 0.03f;
        float chorRate = 5.f;
        SinOsc trem; float tremRate = 0.05f, tremPhase = 1.f;
        void init(uint32_t s, float sr, float odd_) {
            rng.seed(s);
            dPick.reset(s + 1u); first = true;
            pluck.init(0.6f, sr);
            odd = odd_;
            exEnv.reset(); exT = 1e9f; exOsc.reset();
            chor.dl.init(0.08f, sr);
            chorN.reset(s + 2u);
            chorBase = 0.01f + rng.uniform() * 0.05f;      // Rand(0.01, 0.06)
            chorRate = 1.f + rng.uniform() * 9.f;
            tremRate = (1.f + rng.uniform() * 99.f) / 1000.f;   // Rand(1,100)/1000
            tremPhase = 1.f + rng.uniform() * 9.f;              // Rand(1,10) rad
            trem.reset();
        }
        float process(float freq, float st, float sr) {
            float pick = (first ? 1.f : 0.f) + dPick.process(0.1f, st);
            first = false;
            if (pick > 0.f) exT = 0.f;
            // exciter: 10 ms chirp 1000→50 Hz under a fast perc envelope
            float ex = 0.f;
            if (exT < 0.02f) {
                float f = std::max(1000.f - 95000.f * exT, 50.f);
                ex = exOsc.process(f, st) * exEnv.process(pick, 0.001f, 0.01f, -4.f, st);
            }
            exT += st;
            float delay = sr / rack::clamp(freq, 2.f, 10000.f);
            float s = pluck.process(ex, pick, delay, 1000.f * odd, 0.1f, st);
            float dt = (chorBase + 0.01f * chorN.process(chorRate, st) + 0.02f) / 15.f;
            s = chor.process(s, rack::clamp(dt * sr, 4.f, 0.075f * sr));
            return s * std::fabs(trem.process(tremRate, st, tremPhase));
        }
    };
    struct Guitar {
        StringV s1, s2;
        float doFeedback = 0.f, freqMod = 1.f;
        float fbPrev = 0.f;
        LFNoise0 nFbAmt; Lag fbLag; Biquad hpFb;
        LFNoise0 nRq1, nRq2; Lag rqLag1, rqLag2;
        Biquad rlpf1, rlpf2, shelf; LeakDC dc;
        DelayC micro; SinOsc microLfo; float microRate = 0.5f;
        LFNoise0 nPan; Lag panLag;
        void init(uint32_t s, float sr, float doFb, float fm) {
            Rng rng; rng.seed(s);
            s1.init(s + 100u, sr, -1.f);
            s2.init(s + 200u, sr, 1.f);
            doFeedback = doFb; freqMod = fm; fbPrev = 0.f;
            nFbAmt.reset(s + 1u); fbLag.reset(); hpFb.reset();
            nRq1.reset(s + 2u); nRq2.reset(s + 3u); rqLag1.reset(); rqLag2.reset();
            rlpf1.reset(); rlpf2.reset(); shelf.reset(); dc.reset();
            micro.dl.init(0.12f, sr);
            microRate = (1.f + rng.uniform() * 99.f) / 100.f;
            microLfo.reset();
            nPan.reset(s + 4u); panLag.reset();
        }
        void process(float hz, float st, float sr, float& outL, float& outR) {
            float snd = s1.process(hz * freqMod, st, sr) + s2.process(hz * 1.5f * freqMod, st, sr);
            float fbAmt = std::pow(10.f, linlin(fbLag.process(nFbAmt.process(1.f / 3.f, st), 3.f, st), -1.f, 1.f, -60.f, 0.f) / 20.f);
            snd += hpFb.hpf(fbPrev, 30.f, st) * fbAmt;
            snd = crossoverDistortion(snd, 0.5f, 0.5f);
            snd = std::tanh(snd * 3.162f);                       // +10 dB
            float rq1 = linexp(rqLag1.process(nRq1.process(1.f / 3.f, st), 3.f, st), -1.f, 1.f, 0.2f, 0.6f);
            snd = rlpf1.rlpf(snd, hz * 4.f, rq1, st);
            snd = std::tanh(snd * 39.8f);                        // +32 dB
            float rq2 = linexp(rqLag2.process(nRq2.process(1.f / 3.f, st), 3.f, st), -1.f, 1.f, 0.1f, 0.5f);
            snd = rlpf2.rlpf(snd, hz * 2.f, rq2, st);
            snd = std::tanh(snd * 39.8f);                        // +32 dB
            snd = shelf.hishelf(snd, hz * 6.f, -2.f, st);
            snd = dc.process(snd);
            fbPrev = snd * doFeedback;
            float mdt = linlin(microLfo.process(microRate, st), -1.f, 1.f, 0.f, 1e-4f);
            snd = micro.process(snd, rack::clamp(mdt * sr + 4.f, 4.f, 0.11f * sr));
            float pan = panLag.process(nPan.process(0.1f, st), 10.f, st);
            pan2(snd, pan, 1.f, outL, outR);
        }
    };
    static constexpr int NG = 5;
    Guitar g[NG]; float gGain[NG] = {1.f, 1.f, 1.f, 1.f, 0.1f};
    BlPulse b1, b2; SinOsc widthLfo, bassCutLfo, bassAmpLfo;
    Biquad bassLp1, bassLp2;
    SchroederReverb rev; LFNoise0 nRevMix; Lag revMixLag;
    const char* name() const override { return "sunno"; }
    void init(uint32_t seed, float sr) override {
        static const float fb[NG] = {1, 0, 0, 0, 1};
        static const float fm[NG] = {1, 1, 1, 0.5f, 2};
        nHz.reset(seed + 1u); hzLag.reset();
        for (int i = 0; i < NG; ++i) g[i].init(seed + i * 9013u + 10u, sr, fb[i], fm[i]);
        b1.reset(); b2.reset(); widthLfo.reset(); bassCutLfo.reset(); bassAmpLfo.reset();
        bassLp1.reset(); bassLp2.reset();
        rev.init(seed + 90001u, sr);
        nRevMix.reset(seed + 2u); revMixLag.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        hz *= linlin(hzLag.process(nHz.process(0.1f, st), 10.f, st), -1.f, 1.f, 0.95f, 1.f);
        float sumL = 0.f, sumR = 0.f;
        for (int i = 0; i < NG; ++i) {
            float gl, gr; g[i].process(hz, st, sr, gl, gr);
            sumL += gl * gGain[i]; sumR += gr * gGain[i];
        }
        sumL /= 30.f; sumR /= 30.f;
        // bass
        float width = linlin(widthLfo.process(1.f / 3.f, st), -1.f, 1.f, 0.2f, 0.4f);
        float cut = hz * linlin(bassCutLfo.process(0.1f, st), -1.f, 1.f, 1.f, 2.f);
        float bass1 = bassLp1.lpf(b1.process(hz * 0.5f, width, st), cut, st);
        float bass2 = bassLp2.lpf(b2.process(hz * 0.75f, width, st), cut, st);
        float bgain = (60.f / std::max(hz, 1.f))
                    * std::pow(10.f, linlin(bassAmpLfo.process(0.123f, st), -1.f, 1.f, -25.f, -16.f) / 20.f);
        sumL += bass1 * bgain; sumR += bass2 * bgain;
        float rvL, rvR; rev.process(sumL, sumR, st, sr, rvL, rvR);
        float rmix = linlin(revMixLag.process(nRevMix.process(0.1f, st), 10.f, st), -1.f, 1.f, 0.025f, 0.06f);
        const float makeup = 0.5f;
        l = (sumL + rmix * rvL) * amp * makeup;
        r = (sumR + rmix * rvR) * amp * makeup;
    }
};

// ── Nautilus — @taubaland. "Dusty waves, chaotic undercurrent." ───────────────
// A Lorenz attractor iterated at the fundamental drives everything: six voices
// of overlapping sine grains (10/s), each with a looping swell envelope, into
// chaos-swept lowpasses with a whisper of noise. Voicing after Supersaw.
struct NautilusEngine : DroneEngine {
    static constexpr int NV = 6, NGRAIN = 8;
    LorenzL lorenz;
    struct Grain { bool on = false; float t = 0.f, dur = 1.f, freq = 100.f, phase = 0.f; };
    struct Voice {
        Grain grains[NGRAIN]; int slot = 0;
        float trigTimer = 0.f;
        SinOsc envLfo1, envLfo2;
        int envSeg = 0; float envT = 0.f, segDur = 1.f;
        Biquad dfm1; WhiteNoise wn; SinOsc noiseLfo; Lag cutSmooth;
        void init(uint32_t s) {
            for (int k = 0; k < NGRAIN; ++k) grains[k].on = false;
            slot = 0; trigTimer = 0.f;
            envLfo1.reset(); envLfo2.reset();
            envSeg = 0; envT = 0.f; segDur = 1.f;
            dfm1.reset(); wn.reset(s); noiseLfo.reset(); cutSmooth.reset();
        }
    };
    Voice v[NV];
    const char* name() const override { return "nautilus"; }
    void init(uint32_t seed, float) override {
        lorenz.reset();
        for (int i = 0; i < NV; ++i) v[i].init(seed + i * 5501u + 3u);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float lz = lorenz.process(hz, 10.f, 28.f, 8.f / 3.f, 0.05f, st);
        float ch[NV];
        for (int i = 0; i < NV; ++i) {
            Voice& w = v[i];
            // Pulse.ar(10) trigger → new grain every 0.1 s (oldest slot stolen)
            w.trigTimer -= st;
            if (w.trigTimer <= 0.f) {
                w.trigTimer += 0.1f;
                Grain& gr = w.grains[w.slot];
                w.slot = (w.slot + 1) % NGRAIN;
                gr.on = true; gr.t = 0.f;
                gr.dur = std::max(2.f * i + lz, 0.02f);
                gr.freq = hz + i * lz;
                gr.phase = 0.f;
            }
            float s = 0.f;
            for (int k = 0; k < NGRAIN; ++k) {
                Grain& gr = w.grains[k];
                if (!gr.on) continue;
                gr.t += st;
                if (gr.t >= gr.dur) { gr.on = false; continue; }
                float u = gr.t / gr.dur;
                float win = 0.5f - 0.5f * std::cos(kTwoPi * u);       // hann
                gr.phase += gr.freq * st; gr.phase -= std::floor(gr.phase);
                s += std::sin(kTwoPi * gr.phase) * win;
            }
            // looping swell envelope 0 → 0.5 → 0, times from slow sines
            float lfo1 = w.envLfo1.process(0.1f * i + lz, st);
            float lfo2 = w.envLfo2.process(0.1f * i + lz, st);
            w.envT += st;
            if (w.envT >= w.segDur) {
                w.envT = 0.f;
                w.envSeg ^= 1;
                w.segDur = std::max((w.envSeg == 0) ? lfo1 : lfo2, 0.05f);
            }
            float u = w.envT / w.segDur;
            float shaped = 0.5f - 0.5f * std::cos((float)M_PI * u);
            float env = (w.envSeg == 0) ? 0.5f * shaped : 0.5f * (1.f - shaped);
            s *= env;
            // the raw chaos would modulate the biquad at audio rate and blow it
            // up; DFM1 proper tolerates that, our biquad needs a smoothed cutoff
            float cutoff = w.cutSmooth.process(hz + (10.f * i * lz) * 0.5f, 0.005f, st);
            float nl = 0.08f + 0.02f * 0.1f * w.noiseLfo.process(lz, st);
            ch[i] = w.dfm1.dfm1(s * 0.5f + w.wn.process() * nl, std::max(cutoff, 20.f), 0.05f, 0, st);
        }
        splay(ch, NV, 1.f, 0.f, l, r);
        const float makeup = 6.f;
        l = softclip(l * amp * makeup); r = softclip(r * amp * makeup);
    }
};

// ── Drumm — @infinitedigits. "Sometimes gentle, the other time intense." ──────
// Two layers crossfaded by slow sines: a bass layer of chaos-width pulse pairs
// and phase-modulated sub sines, and a "kind" layer of ten Moog-swept melodic
// voices stepping three interlocking rows; both DFM1-swept, sine-shaped, and
// drenched in Freeverb whose character pumps with a random stomp envelope.
struct DrummEngine : DroneEngine {
    LFTri osc1Lfo, osc2Lfo; float o1Rate = 0.02f, o1Phase = 0.f, o2Rate = 0.02f, o2Phase = 0.f;
    float mainrate = 0.2f;
    LFNoise0 nSpeed; Lag speedLag; SinOsc pOsc, qOsc;
    float introT = 0.f;
    FBSineN fbs1, fbs2; float fbsRate1 = 22500.f, fbsRate2 = 22500.f;
    Lag width1Lag, width2Lag;
    BlPulse pulseA[2], pulseB[2];
    SinOsc sin2[2], sin4[2], sinBass[2], sinBassPm, sinSub[2], sinSubPm;
    Dust dChord; TChoose chChord;
    LFNoise0 nAmp1, nAmp2; Lag amp1Lag, amp2Lag;
    Impulse impMain;
    struct KindVoice {
        Rng rng;
        int seqPos = 0;
        float noteOff = 0.f;
        SinOsc osc; VarSaw vsaw; SinOsc vsawWidthLfo;
        PinkNoise pink;
        MoogFF ff; SinOsc cutLfo; float cutRate = 0.05f, cutPhase = 0.f;
        DelayC chor; LFNoise1 chorN; float chorBase = 0.02f, chorRate = 7.f;
        PercEnv env;
        LFNoise0 nPan; Lag panLag;
        void init(uint32_t s, float sr) {
            rng.seed(s);
            seqPos = 0; noteOff = 0.f;
            osc.reset(); vsaw.reset(); vsawWidthLfo.reset();
            pink.reset(s + 1u);
            ff.reset();
            cutRate = linlin(rng.uniform(), 0.f, 1.f, 1.f / 30.f, 1.f / 10.f);
            cutPhase = rng.uniform() * kTwoPi;
            cutLfo.reset();
            chor.dl.init(0.05f, sr);
            chorN.reset(s + 2u);
            chorBase = 0.01f + rng.uniform() * 0.02f;
            chorRate = 5.f + rng.uniform() * 5.f;
            env.reset();
            nPan.reset(s + 3u); panLag.reset();
        }
    };
    static constexpr int NK = 10;
    KindVoice kv[NK];
    Biquad dfmA[2], dfmB[2];
    LFNoise0 nDrop; Lag dropLag; SinOsc dropOsc;
    BPEnv stompEnv; Dust dStomp; Lag stompLag;
    LFNoise0 nStompDur; Lag stompDurLag;
    SinOsc introLfo;
    FreeVerbMono fvL, fvR;
    const char* name() const override { return "drumm"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        o1Rate = 1.f / linlin(rng.uniform(), 0.f, 1.f, 30.f, 100.f);
        o1Phase = rng.uniform() * kTwoPi;
        o2Rate = 1.f / linlin(rng.uniform(), 0.f, 1.f, 30.f, 100.f);
        o2Phase = rng.uniform() * kTwoPi;
        osc1Lfo.reset(); osc2Lfo.reset();
        mainrate = 0.15f + rng.uniform() * 0.1f;
        nSpeed.reset(seed + 1u); speedLag.reset(); pOsc.reset(); qOsc.reset();
        introT = 0.f;
        fbs1.reset(); fbs2.reset();
        fbsRate1 = 22000.f + rng.uniform() * 1000.f;
        fbsRate2 = 22000.f + rng.uniform() * 1000.f;
        width1Lag.reset(); width2Lag.reset();
        for (int c = 0; c < 2; ++c) {
            pulseA[c].reset(); pulseB[c].reset();
            sin2[c].reset(); sin4[c].reset(); sinBass[c].reset(); sinSub[c].reset();
            dfmA[c].reset(); dfmB[c].reset();
        }
        sinBassPm.reset(); sinSubPm.reset();
        dChord.reset(seed + 2u); chChord.reset(seed + 3u);
        nAmp1.reset(seed + 4u); nAmp2.reset(seed + 5u); amp1Lag.reset(); amp2Lag.reset();
        impMain.reset();
        for (int i = 0; i < NK; ++i) kv[i].init(seed + i * 6007u + 100u, sr);
        nDrop.reset(seed + 6u); dropLag.reset(); dropOsc.reset();
        stompEnv.reset(); dStomp.reset(seed + 7u); stompLag.reset();
        nStompDur.reset(seed + 8u); stompDurLag.reset();
        introLfo.reset();
        fvL.init(sr); fvR.init(sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        // slow global modulators
        float osc1 = std::floor(rack::clamp(linlin(osc1Lfo.process(o1Rate, st), -1.f, 1.f, 0.f, 2.f), 0.f, 1.999f));
        float osc2 = std::floor(rack::clamp(linlin(osc2Lfo.process(o2Rate, st), -1.f, 1.f, 0.f, 2.f), 0.f, 1.999f));
        (void)o1Phase; (void)o2Phase;
        float speed = linlin(speedLag.process(nSpeed.process(0.1f, st), 10.f, st), -1.f, 1.f, 1.f / 3000.f, 1.f / 30.f);
        float p = pOsc.process(speed, st);
        float q = qOsc.process(speed, st, (float)M_PI_2);
        introT += st;
        float intro = rack::clamp((introT - 4.f / mainrate) / (4.f / mainrate), 0.f, 1.f);
        // bass layer
        float basshz2 = hz;
        for (int k = 0; k < 2; ++k) if (basshz2 >= 200.f) basshz2 *= 0.5f;
        float basshz = hz;
        for (int k = 0; k < 4; ++k) if (basshz >= 70.f) basshz *= 0.5f;
        float w1 = width1Lag.process(linlin(fbs1.process(fbsRate1, 1.f, 0.1f, 1.1f, 0.5f, st), -1.f, 1.f, 0.1f, 0.9f), 0.5f, st);
        float w2 = width2Lag.process(linlin(fbs2.process(fbsRate2, 1.f, 0.1f, 1.1f, 0.5f, st), -1.f, 1.f, 0.12f, 0.9f), 0.9f, st);
        static const float chordOffs[4] = {3, 7, 8, 10};
        float chordOff = chChord.process(dChord.process(0.1f, st), chordOffs, 4);
        float bhz2 = midicps(cpsmidi(basshz2) + chordOff);
        float amp1 = linlin(amp1Lag.process(nAmp1.process(4.f, st), 0.25f, st), -1.f, 1.f, 0.1f, 0.3f);
        float amp2 = linlin(amp2Lag.process(nAmp2.process(4.f, st), 0.25f, st), -1.f, 1.f, 0.1f, 0.3f);
        float pmBass = sinBassPm.process(basshz / 16.f, st);
        float pmSub = sinSubPm.process(basshz / 128.f, st);
        float sndB[2];
        for (int c = 0; c < 2; ++c) {
            float s = pulseA[c].process(basshz2, w1, st) * 0.3f;
            s += sin2[c].process(basshz2 * (c ? 2.01f : 2.f), st) * 0.1f;
            s += sin4[c].process(basshz2 * (c ? 4.01f : 4.f), st) * 0.05f;
            s += pulseB[c].process(bhz2, w2, st) * 0.2f;
            s += sinBass[c].process(basshz * (c ? 1.01f : 1.f), st, pmBass) * amp1;
            s += sinSub[c].process(basshz * 0.5f * (c ? 1.01f : 1.f), st, pmSub) * amp2;
            sndB[c] = s;
        }
        // melodic "kind" layer
        float note = cpsmidi(hz);
        for (int k = 0; k < 4; ++k) if (note <= 50.f) note += 12.f;
        float imp = impMain.process(mainrate, st);
        static const float m0[13] = {0, 0, 3, 3, 0, 3, 3, 2, 0, 0, -2, -2, 0};
        static const float m1[13] = {3, 3, 7, 8, 3, 8, 7, 7, 3, 3, 3, -7, 3};
        static const float m2[13] = {8, 7, 10, 12, 7, 12, 10, 10, 7, 8, 7, 5, 7};
        static const float* mel[3] = {m0, m1, m2};
        float kindL = 0.f, kindR = 0.f;
        for (int i = 0; i < NK; ++i) {
            KindVoice& w = kv[i];
            if (imp > 0.f) { w.noteOff = mel[i % 3][w.seqPos % 13]; ++w.seqPos; }
            float nhz = midicps(note + w.noteOff);
            float s = w.osc.process(nhz, st) * 0.8f;
            float vw = linlin(w.vsawWidthLfo.process(4.f, st), -1.f, 1.f, 0.4f, 0.6f);
            s += w.vsaw.process(nhz * 0.5f, vw, st) * 0.8f;
            s += w.pink.process() * q * 0.05f;
            float cut = linexp(w.cutLfo.process(w.cutRate, st, w.cutPhase), -1.f, 1.f, nhz, 12000.f);
            s = w.ff.process(s, cut, 2.f, st);
            float dt = (w.chorBase + 0.01f * w.chorN.process(w.chorRate, st)) / 15.f;
            s = w.chor.process(s, rack::clamp(dt * sr, 8.f, 0.045f * sr));
            s *= w.env.process(imp, 3.f, 5.f, -4.f, st);
            float pan = w.panLag.process(w.nPan.process(1.f / 3.f, st), 3.f, st);
            float vl, vr; pan2(s, pan, 0.25f, vl, vr);
            kindL += vl; kindR += vr;
        }
        (void)osc1; (void)osc2;
        // filters + shaping
        float cutB = basshz * 2.f * linlin(p, -1.f, 1.f, 1.f, 10.f);
        float cutK = basshz * 3.f * linlin(1.f - p, -1.f, 1.f, 1.f, 10.f);
        for (int c = 0; c < 2; ++c) sndB[c] = dfmA[c].dfm1(sndB[c], cutB, 0.1f, 0, st);
        kindL = dfmB[0].dfm1(kindL, cutK, 0.1f, 0, st);
        kindR = dfmB[1].dfm1(kindR, cutK, 0.1f, 0, st);
        float dropFreq = linlin(dropLag.process(nDrop.process(4.f, st), 0.25f, st), -1.f, 1.f, 0.7f, 1.f);
        float drop = rack::clamp(dropOsc.process(dropFreq, st) + 1.7f, -1.f, 1.f) * 2.f;
        for (int c = 0; c < 2; ++c) sndB[c] = sineShaper(sndB[c], 0.5f) * drop;
        // stomp + intro crossfade between the layers
        float stompDur = linlin(stompDurLag.process(nStompDur.process(0.1f, st), 10.f, st), -1.f, 1.f, 5.f, 12.f);
        static const float slv[4] = {0.f, 1.f, 1.f, 0.f};
        float stm[3] = {0.5f, stompDur, 0.2f};
        float stomp = stompEnv.process(dStomp.process(1.f / 20.f, st), slv, stm, 3, false, st);
        float introp = (1.f - (q * 0.5f + 0.5f)) * intro;
        float xf = 1.f - rack::clamp(introp + linlin(introLfo.process(mainrate / 64.f, st), -1.f, 1.f, 0.f, 0.1f) * intro, 0.f, 1.f);
        float outL = selectx(xf, kindL, sndB[0]);
        float outR = selectx(xf, kindR, sndB[1]);
        // Freeverb whose mix pumps with the stomp
        float mix = linlin(stompLag.process(stomp, 1.f, st), 0.f, 1.f, 0.3f, 0.5f);
        float room = rack::clamp(drop, 0.f, 1.f);
        const float makeup = 0.7f;
        l = fvL.process(outL, mix, room, room) * amp * 0.5f * makeup;
        r = fvR.process(outR, mix, room, room) * amp * 0.5f * makeup;
    }
};


// ── Takita — @sixolet. "Rhythmic." ────────────────────────────────────────────
// A self-clocked drum language: a beat impulse gates a self-suppressing
// "division" window (LocalIn/Out), whose phasors flip tik/tok/tuk flip-flops;
// each fires a filtered percussive click (RHPF→RLPF + BPF bands), with ki/ka
// cross-triggered from tik and tok. Everything — band edges, resonances,
// drumhead bite, trash noise — drifts on immensely slow sines.
struct TakitaEngine : DroneEngine {
    // slow global modulators
    LFTri beatLfo; SinOsc divLfo; float divPhase = 0.f;
    SinOsc mod1234Lfo, modGateLfo;
    LFTri oneLfo, twoLfo;
    static constexpr int NB = 5;                    // bands: ki tik tuk tok ka
    SinOsc ampLfo[NB]; float ampRate[NB] = {}, ampPhase[NB] = {}; Lag ampLag[NB];
    SinOsc resLfo[8]; float resRate[8] = {}, resPhase[8] = {};
    SinOsc dhLfo[2]; float dhRate[2] = {}, dhPhase[2] = {};
    SinOsc trashLfo[4]; float trashRate[4] = {}, trashPhase[4] = {};
    // riddim core
    Impulse takitakImp;
    float tokitokTimer = -1.f; bool firstTok = true; float prevTokitok = 0.f;
    Phasor prong, pring, prang;
    SetResetFF ffTik, ffTok, ffTuk, ffKi, ffKa;
    PercEnv bandEnv[NB], drumheadEnv;
    PinkNoise trashNoise[NB];
    Biquad rhp[NB], rlp[NB], bp[NB];
    // lace: interleaved delayed copies
    DelayC laceDelay[NB]; LFNoise2 laceAmpN[NB]; Lag laceAmpLag[NB]; LFNoise2 laceTimeN[4];
    const char* name() const override { return "takita"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        beatLfo.reset(rng.uniform());
        divPhase = rng.uniform() * kTwoPi; divLfo.reset();
        mod1234Lfo.reset(); modGateLfo.reset();
        oneLfo.reset(rng.uniform()); twoLfo.reset(rng.uniform());
        for (int i = 0; i < NB; ++i) {
            ampRate[i] = 1.f / (100.f + rng.uniform() * 400.f);
            ampPhase[i] = rng.uniform() * kTwoPi;
            ampLfo[i].reset(); ampLag[i].reset();
            bandEnv[i].reset(); trashNoise[i].reset(seed + 40u + i);
            rhp[i].reset(); rlp[i].reset(); bp[i].reset();
            laceDelay[i].dl.init(0.02f, sr);
            laceAmpN[i].reset(seed + 50u + i); laceAmpLag[i].reset();
        }
        for (int i = 0; i < 8; ++i) {
            resRate[i] = 1.f / (200.f + rng.uniform() * 600.f);
            resPhase[i] = rng.uniform() * kTwoPi; resLfo[i].reset();
        }
        for (int i = 0; i < 2; ++i) {
            dhRate[i] = 1.f / (200.f + rng.uniform() * 600.f);
            dhPhase[i] = rng.uniform() * kTwoPi; dhLfo[i].reset();
        }
        for (int i = 0; i < 4; ++i) {
            trashRate[i] = 1.f / (200.f + rng.uniform() * 600.f);
            trashPhase[i] = rng.uniform() * kTwoPi; trashLfo[i].reset();
            laceTimeN[i].reset(seed + 60u + i);
        }
        takitakImp.reset();
        tokitokTimer = -1.f; firstTok = true; prevTokitok = 0.f;
        prong.reset(); pring.reset(); prang.reset();
        ffTik.reset(); ffTok.reset(); ffTuk.reset(); ffKi.reset(); ffKa.reset();
        drumheadEnv.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        // tempo language from very slow LFOs
        float beat = linexp(beatLfo.process(1.f / 4000.f, st), -1.f, 1.f, 0.11f, 0.22f);
        float div = rack::clamp(linlin(divLfo.process(1.f / 2111.f, st, divPhase), -1.f, 1.f, 5.f, 20.f), 7.1f, 15.9f);
        float modGate = (modGateLfo.process(1.f / (2.f * beat * div), st) > 0.f) ? 1.f : 0.f;
        float mod = (mod1234Lfo.process(1.f / 1234.f, st) * 0.5f + 0.5f) * modGate;
        float one = linexp(oneLfo.process(1.f / 1111.f, st), -1.f, 1.f, 0.15f, div * 0.5f) + 1.f + mod;
        float two = linexp(twoLfo.process(1.f / 1323.f, st), -1.f, 1.f, 0.17f, div * 0.5f) + 1.f;
        float amps[NB], asum = 0.f;
        for (int i = 0; i < NB; ++i) {
            float a = ampLfo[i].process(ampRate[i], st, ampPhase[i]) * 0.5f + 0.5f;
            amps[i] = ampLag[i].process(rack::clamp(a - 0.2f, 0.f, 1.f), 10.f, st);
            asum += amps[i];
        }
        amps[0] = std::max(amps[0], 0.4f - asum);
        float res[8];
        for (int i = 0; i < 8; ++i)
            res[i] = linexp(resLfo[i].process(resRate[i], st, resPhase[i]), -1.f, 1.f, 0.06f, 0.8f);
        res[4] *= 0.5f;
        float dh[2];
        for (int i = 0; i < 2; ++i)
            dh[i] = linlin(dhLfo[i].process(dhRate[i], st, dhPhase[i]), -1.f, 1.f, 0.f, 1.4f);
        float trash[4];
        for (int i = 0; i < 4; ++i)
            trash[i] = rack::clamp(trashLfo[i].process(trashRate[i], st, trashPhase[i]), 0.f, 1.f);
        // band edges from hz
        bool isLow = hz < 150.f, isVeryLow = hz < 80.f;
        int lowIdx = (isLow ? 1 : 0) + (isVeryLow ? 1 : 0);
        float tikHp = isLow ? hz : hz * 0.5f;
        float tokHp = hz * (lowIdx == 0 ? 1.f : (lowIdx == 1 ? 2.f : 3.f));
        float kiHp = hz * (lowIdx == 0 ? 6.f : (lowIdx == 1 ? 9.f : 12.f));
        float kaHp = isLow ? hz * 6.f : hz * 3.f;
        float tikLp = isLow ? hz * 8.f : hz * 6.f;
        float tokLp = isVeryLow ? hz * 12.f : hz * 9.f;
        float kiLp = isLow ? hz * 30.f : hz * 20.f;
        float kaLp = isVeryLow ? hz * 12.f : hz * 15.f;
        // ── the riddim core ──
        float takitak = takitakImp.process(1.f / beat, st);
        float divEnvTime = beat * div;
        float syncT1 = beat * one, syncT2 = beat * two;
        float divFeedback = prevTokitok;                     // LocalIn (one sample)
        float tokTrig = takitak * (1.f - divFeedback) + (firstTok ? 1.f : 0.f);
        firstTok = false;
        if (tokTrig > 0.5f && prevTokitok <= 0.f) tokitokTimer = divEnvTime;
        float tokitok = (tokitokTimer > 0.f) ? 1.f : 0.f;
        tokitokTimer -= st;
        float prongV = prong.process(tokitok, 1.f / syncT2, st);
        float pringV = pring.process(tokitok, 1.f / syncT1, st);
        float prangV = prang.process(tokitok, 1.f / (syncT1 + syncT2), st);
        float tik = ffTik.process(takitak, (prongV > 0.1f) ? 1.f : 0.f);
        float tok = ffTok.process(takitak, (pringV > 0.1f) ? 1.f : 0.f);
        float tuk = ffTuk.process(takitak, (prangV > 0.1f) ? 1.f : 0.f);
        float ki = ffKi.process(tik, tok);
        float ka = ffKa.process(tok, tik);
        prevTokitok = tokitok;
        float drumhead = drumheadEnv.process(takitak, 0.01f * beat, 0.24f * beat, -4.f, st);
        // filt(thing, hp, lp, highres, lowres, trash)
        auto filt = [&](int b, float thing, float hp, float lp, float hres, float lres, float tr) {
            float gend = bandEnv[b].process(thing, 0.01f * beat, 0.24f * beat, -4.f, st);
            gend += gend * tr * trashNoise[b].process();
            float band = rlp[b].rlpf(rhp[b].rhpf(gend, hp, hres, st), lp, lres, st)
                       + bp[b].bpf(gend, (hp + lp) * 0.5f, (hres + lres) * 0.5f, st);
            return band;
        };
        float band[NB];
        band[0] = amps[4] * filt(0, ki, kiHp, kiLp, res[4], res[5], trash[2] * 0.5f) * 0.5f;
        band[1] = amps[1] * filt(1, tik, tikHp * (1.f + dh[0] * drumhead), tikLp, res[0], res[1], trash[0]);
        band[2] = amps[2] * filt(2, tuk, std::min(tikLp, tokLp), std::max(tikLp, tokLp), res[0], res[3], 0.f);
        band[3] = amps[3] * filt(3, tok, tokHp * (1.f + dh[1] * drumhead), tokLp, res[2], res[3], trash[1]);
        band[4] = amps[0] * filt(4, ka, kaHp, kaLp, res[6], res[7], trash[3]);
        // lace with slow-faded delayed copies → 10 channels splayed
        float ch[NB * 2];
        for (int i = 0; i < NB; ++i) {
            float la = rack::clamp(laceAmpN[i].process(1.f / 500.f, st), 0.f, 0.5f);
            la = laceAmpLag[i].process(la, 10.f, st);
            float dt = 0.014f * (laceTimeN[i % 4].process(0.1f, st) * 0.5f + 0.5f);
            float d = laceDelay[i].process(band[i], rack::clamp(dt * sr, 4.f, 0.019f * sr));
            ch[i * 2] = band[i];
            ch[i * 2 + 1] = la * d;
        }
        splay(ch, NB * 2, 0.7f, 0.f, l, r);
        const float makeup = 10.f;   // intrinsically sparse; lift toward roster level
        l = std::tanh(l * makeup) * amp;
        r = std::tanh(r * makeup) * amp;
    }
};

// ── Twin Pks — (uncredited). "Retro stylings, timeless horror." ───────────────
// No oscillator bank at all: tape/vinyl noise (dust + crackle + a pink-driven
// sine whistle) is compressed hard, band-passed at the fundamental, warbled
// through a wow/flutter delay, saturated with a second noise layer, lightly
// bit-crushed, and band-passed again.
struct TwinPksEngine : DroneEngine {
    Dust2 dust1, dust2; Crackle crack1, crack2;
    PinkNoise pinkF1, pinkF2; SinOsc whistle1, whistle2;
    Biquad hp25, bpf1, bpf2;
    Compander comp; Limiter lim;
    LFPar depthLfo;
    DelayC wowDelay; SinOsc wowLfo;
    Decimator decim;
    const char* name() const override { return "twin pks"; }
    void init(uint32_t seed, float sr) override {
        dust1.reset(seed + 1u); dust2.reset(seed + 2u);
        crack1.reset(); crack2.reset();
        pinkF1.reset(seed + 3u); pinkF2.reset(seed + 4u);
        whistle1.reset(); whistle2.reset();
        hp25.reset(); bpf1.reset(); bpf2.reset();
        comp.reset(); lim.reset();
        depthLfo.reset();
        wowDelay.dl.init(0.06f, sr);
        wowLfo.reset();
        decim.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        // main tape-noise voice
        float wet = dust1.process(10.f, st)
                  + crack1.process(1.95f) * 0.2f
                  + whistle1.process(pinkF1.process() * 0.5f * 7500.f + 40.f, st) * 0.04f;
        wet = hp25.hpf(wet, 25.f, st);
        // "shitty compression" at drive = 0.75
        const float ratio = 0.0197f;        // linexp(0.75, 0,1, 0.15, 0.01)        // linexp(drive, 0,1, 0.15, 0.01)
        const float threshold = 0.4475f;    // linlin(drive, 0,1, 0.8, 0.33)
        const float gain = 1.f / ((1.f - threshold) * ratio + threshold);
        wet = comp.process(wet, wet, threshold, 1.f, ratio, 0.1f, 1.f, st) * gain;
        wet = lim.process(wet, 1.f, 0.0008f, st);
        wet = bpf1.bpf(wet, hz, 0.4f, st);
        // wow / flutter / warble at wow = 0.6
        const float wowRate = 1.741f;       // linexp(0.6, 0,1, 0.5, 4)
        const float depthBase = 7.696f;     // linexp(0.6, 0,1, 1, 30)
        const float depthLfoAmt = 3.f;      // floor(linlin(0.6, 0,1, 1, 5))
        float depth = depthLfo.process(depthLfoAmt * 0.1f, st) * depthLfoAmt + depthBase;
        float wowMul = (std::exp2(depth / 1200.f) - 1.f) / (4.f * wowRate);
        const float maxDelay = 0.0509f;     // ((2^(35/1200))-1)/(4*0.5) * 2.5
        float dsec = wowLfo.process(wowRate, st, 2.f) * wowMul + wowMul + 1.f / 689.f;
        wet = wowDelay.process(wet, rack::clamp(dsec, 4.f / sr, maxDelay) * sr);
        // second noise layer + saturation (drive gain linexp(0.75) ≈ 1.99)
        float noise2 = dust2.process(10.f, st)
                     + crack2.process(1.95f) * 0.2f
                     + whistle2.process(pinkF2.process() * 0.5f * 7500.f + 40.f, st) * 0.006f;
        wet = std::tanh(wet * 1.987f + noise2);
        // a little bitcrushing
        wet = decim.process(wet, 24000.f, 16.f, st) * 0.33f + wet * 0.67f;
        wet = bpf2.bpf(wet, hz, 0.4f, st);
        wet = softclip(wet * 15.85f * amp) * 0.65f;    // +24 dB, trimmed to roster level
        l = r = wet;
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
    v.emplace_back(new GristleEngine());
    v.emplace_back(new GroveEngine());
    v.emplace_back(new ShieldsEngine());
    v.emplace_back(new EnoEngine());
    v.emplace_back(new BelongEngine());
    v.emplace_back(new RuinsEngine());
    v.emplace_back(new SunnoEngine());
    v.emplace_back(new NautilusEngine());
    v.emplace_back(new DrummEngine());
    v.emplace_back(new TakitaEngine());
    v.emplace_back(new TwinPksEngine());
    return v;
}

}  // namespace draen
