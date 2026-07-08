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
    return v;
}

}  // namespace draen
