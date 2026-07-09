#pragma once
// draen_alt_engines.hpp — the "hyf" bank: 37 original drone engines.
//
// dræn is Old English for "bee"; hyf is Old English for "hive". Where the
// first bank ports the dronecaster SynthDefs faithfully, this bank is a set
// of original instruments built directly on the same UGEN layer — deliberately
// covering ground the dronecaster set doesn't: binaural beating, Shepard
// tones, phase distortion, wavefolding, formant/vocal drones, shimmer
// feedback, and environmental textures. Same DroneEngine contract; selected
// from the module's right-click "Engine bank" menu.

#include "draen_ugens.hpp"
#include "draen_engines.hpp"

namespace draen {

// ── hz-dependent makeup gain ─────────────────────────────────────────────────
// Some engines' loudness varies strongly with the fundamental: fixed formants
// drift in and out of the harmonic stack, pitch-tracking filters change
// bandwidth, pluck/comb loops store more energy at longer delays. For those,
// the makeup gain is a table over the sweep octaves 27.5·2^k Hz (k = 0..7,
// the grid used by test/draen_sweep), interpolated linearly in log2(hz),
// calibrated so AC RMS at amp 1 stays near the bank target across the range.
inline float octaveGain(float hz, const float (&g)[8]) {
    float x = rack::clamp(std::log2(std::max(hz, 1.f) * (1.f / 27.5f)), 0.f, 7.f);
    int i = std::min((int)x, 6);
    float t = x - (float)i;
    return g[i] + (g[i + 1] - g[i]) * t;
}

// ── beam — binaural beating: two pure sines a few Hz apart, plus a sub ───────
struct BeamEngine : DroneEngine {
    SinOsc oscL, oscR, sub; LFNoise2 deltaN;
    const char* name() const override { return "beam"; }
    void init(uint32_t seed, float) override {
        oscL.reset(); oscR.reset(0.5f); sub.reset(); deltaN.reset(seed + 1u);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float delta = 0.4f + 3.f * std::fabs(deltaN.process(0.02f, st));
        float s = sub.process(hz * 0.5f, st) * 0.3f;
        l = (oscL.process(hz, st) * 0.6f + s) * amp * 0.45f;
        r = (oscR.process(hz + delta, st) * 0.6f + s) * amp * 0.45f;
    }
};

// ── wall — eleven detuned saws into a ladder filter: a monolithic wall ───────
struct WallEngine : DroneEngine {
    static constexpr int N = 11;
    BlSaw saw[N]; float det[N] = {};
    MoogFF ladL, ladR;
    const char* name() const override { return "wall"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            saw[i].reset(rng.uniform());
            det[i] = 1.f + ((i - (N - 1) * 0.5f) / (N - 1)) * 0.016f
                   + rng.bipolar() * 0.0008f;
        }
        ladL.reset(); ladR.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sl = 0.f, sr_ = 0.f;
        for (int i = 0; i < N; ++i) {
            float s = saw[i].process(hz * det[i], st);
            if (i & 1) sr_ += s; else sl += s;
        }
        l = ladL.process(sl / 6.f, hz * 6.f, 0.4f, st) * amp * 1.5f;
        r = ladR.process(sr_ / 5.f, hz * 6.f, 0.4f, st) * amp * 1.5f;
    }
};

// ── choir — vowel drone: a saw pair through five morphing formant bands ──────
struct ChoirEngine : DroneEngine {
    BlSaw saw1, saw2; SinOsc vib; LFNoise2 vowelN;
    Biquad form[2][3];
    const char* name() const override { return "choir"; }
    void init(uint32_t seed, float) override {
        saw1.reset(); saw2.reset(0.37f);
        vib.reset(); vowelN.reset(seed + 1u);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 3; ++k) form[c][k].reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        // vowel formant tables: A, O, U (first three formants)
        static const float FA[3] = {800, 1150, 2900};
        static const float FO[3] = {450, 800, 2830};
        static const float FU[3] = {325, 700, 2700};
        static const float G[3] = {1.f, 0.5f, 0.18f};
        float v = vowelN.process(0.06f, st) * 0.5f + 0.5f;       // 0..1 vowel walk
        float f[3];
        for (int k = 0; k < 3; ++k) {
            float ao = FA[k] + (FO[k] - FA[k]) * rack::clamp(v * 2.f, 0.f, 1.f);
            f[k] = ao + (FU[k] - FO[k]) * rack::clamp(v * 2.f - 1.f, 0.f, 1.f);
        }
        float vr = 1.f + 0.006f * vib.process(4.7f, st);
        float sL = saw1.process(hz * vr, st);
        float sR = saw2.process(hz * 1.003f * vr, st);
        float ol = 0.f, orr = 0.f;
        for (int k = 0; k < 3; ++k) {
            ol += form[0][k].bpf(sL, f[k], 0.12f, st) * G[k];
            orr += form[1][k].bpf(sR, f[k] * 1.01f, 0.12f, st) * G[k];
        }
        // fixed formants drift through the saw's harmonic stack as hz moves
        static const float MG[8] = {2.09f, 1.53f, 1.18f, 0.77f, 0.41f, 0.95f, 3.93f, 3.20f};
        float g = 2.2f * octaveGain(hz, MG);
        l = ol * amp * g; r = orr * amp * g;
    }
};

// ── breath — whispered vowels: pink noise through the same formant space ─────
struct BreathEngine : DroneEngine {
    PinkNoise pk[2]; LFNoise2 vowelN, swellN; Biquad form[2][3], hp[2];
    SinOsc tone;
    const char* name() const override { return "breath"; }
    void init(uint32_t seed, float) override {
        pk[0].reset(seed + 1u); pk[1].reset(seed + 2u);
        vowelN.reset(seed + 3u); swellN.reset(seed + 4u);
        for (int c = 0; c < 2; ++c) { hp[c].reset(); for (int k = 0; k < 3; ++k) form[c][k].reset(); }
        tone.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float FE[3] = {400, 1700, 2600};
        static const float FO[3] = {450, 800, 2830};
        static const float G[3] = {1.f, 0.6f, 0.25f};
        float v = vowelN.process(0.045f, st) * 0.5f + 0.5f;
        float swell = 0.55f + 0.45f * swellN.process(0.07f, st);
        float f[3];
        for (int k = 0; k < 3; ++k) f[k] = FE[k] + (FO[k] - FE[k]) * v;
        float t = tone.process(hz, st) * 0.05f;                  // faint pitch centre
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float n = pk[c].process() * swell;
            float s = 0.f;
            for (int k = 0; k < 3; ++k) s += form[c][k].bpf(n, f[k] * (c ? 1.02f : 1.f), 0.08f, st) * G[k];
            out[c] = hp[c].hpf(s + t, 90.f, st);
        }
        static const float MG[8] = {2.27f, 1.91f, 1.21f, 1.06f, 1.04f, 1.05f, 1.05f, 1.05f};
        float g = 6.f * octaveGain(hz, MG);
        l = out[0] * amp * g; r = out[1] * amp * g;
    }
};

// ── glass — stretched-partial additive: nine partials at hz·n^1.13 ───────────
struct GlassEngine : DroneEngine {
    static constexpr int N = 9;
    SinOsc part[N], am[N]; float amRate[N] = {}, pos[N] = {};
    LeakDC dc[2];
    const char* name() const override { return "glass"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            part[i].reset(rng.uniform());
            am[i].reset(rng.uniform());
            amRate[i] = 0.03f + rng.uniform() * 0.12f;
            pos[i] = rng.bipolar() * 0.8f;
        }
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            int n = i + 1;
            float f = hz * std::pow((float)n, 1.13f);
            if (f > 0.45f / st) continue;
            float a = (am[i].process(amRate[i], st) * 0.5f + 0.5f) / std::pow((float)n, 0.7f);
            float s = part[i].process(f, st) * a;
            float vl, vr; pan2(s, pos[i], 1.f, vl, vr);
            l += vl; r += vr;
        }
        l = dc[0].process(l) * amp * 0.55f;
        r = dc[1].process(r) * amp * 0.55f;
    }
};

// ── gong — inharmonic resonator bank struck softly every few seconds ─────────
struct GongEngine : DroneEngine {
    static constexpr int N = 6;
    Ringz res[2][N]; WhiteNoise wn; Dust dHit; PercEnv strike; Dust dTickle;
    float hitTimer = 0.f; bool first = true;
    const char* name() const override { return "gong"; }
    void init(uint32_t seed, float) override {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i) res[c][i].reset();
        wn.reset(seed + 1u); dHit.reset(seed + 2u); dTickle.reset(seed + 3u);
        strike.reset(); first = true;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float ratio[N] = {1.f, 1.41f, 2.13f, 2.92f, 3.76f, 5.11f};
        float trig = dHit.process(1.f / 7.f, st) + (first ? 1.f : 0.f);
        first = false;
        float ex = wn.process() * (strike.process(trig, 0.003f, 0.05f, -4.f, st) * 0.4f
                                   + dTickle.process(30.f, st) * 0.06f);
        float ringScale = 400.f / std::max(hz, 20.f);
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            float decay = rack::clamp(ringScale / ratio[i], 0.5f, 20.f);
            l += res[0][i].process(ex, hz * ratio[i], decay, st) / (1.f + i * 0.5f);
            r += res[1][i].process(ex, hz * ratio[i] * 1.004f, decay, st) / (1.f + i * 0.5f);
        }
        static const float MG[8] = {4.00f, 4.00f, 3.89f, 2.56f, 2.43f, 1.92f, 2.29f, 1.86f};
        float g = 90.f * octaveGain(hz, MG);
        l = softclip(l * g) * amp; r = softclip(r * g) * amp;
    }
};

// ── swarm — sixteen band-passed saws gliding between harmonics: bees ─────────
struct SwarmEngine : DroneEngine {
    static constexpr int N = 16;
    struct Voice {
        BlSaw saw; Biquad bpf; Dust dMove; Rng rng;
        Lag glide; LFNoise1 jitter;
        float targetH = 1.f, pos = 0.f;
    };
    Voice v[N];
    const char* name() const override { return "swarm"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            v[i].saw.reset(rng.uniform());
            v[i].bpf.reset();
            v[i].dMove.reset(seed + i * 61u + 1u);
            v[i].rng.seed(seed + i * 61u + 2u);
            v[i].glide.reset();
            v[i].jitter.reset(seed + i * 61u + 3u);
            v[i].targetH = 1.f + (float)(int)(rng.uniform() * 8.f);
            v[i].pos = rng.bipolar() * 0.9f;
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            Voice& w = v[i];
            if (w.dMove.process(0.15f, st) > 0.f)
                w.targetH = 1.f + (float)(int)(w.rng.uniform() * 8.f);
            float f = w.glide.process(hz * w.targetH, 1.2f, st)
                    * (1.f + 0.004f * w.jitter.process(6.f, st));
            float s = w.bpf.bpf(w.saw.process(f, st), f, 0.3f, st);
            float vl, vr; pan2(s, w.pos, 1.f, vl, vr);
            l += vl; r += vr;
        }
        l = softclip(l * 0.18f) * amp; r = softclip(r * 0.18f) * amp;
    }
};

// ── organelle — drawbar organ with tremulant and a small chorus ──────────────
struct OrganelleEngine : DroneEngine {
    static constexpr int N = 6;
    SinOsc bar[N]; SinOsc trem; DelayC chorL, chorR; SinOsc chorLfo;
    const char* name() const override { return "organelle"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) bar[i].reset(rng.uniform());
        trem.reset();
        chorL.dl.init(0.03f, sr); chorR.dl.init(0.03f, sr);
        chorLfo.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float mult[N] = {1, 2, 3, 4, 6, 8};
        static const float lvl[N] = {0.8f, 0.6f, 0.4f, 0.5f, 0.3f, 0.35f};
        float s = 0.f;
        for (int i = 0; i < N; ++i) {
            float f = hz * mult[i];
            if (f < 0.45f / st) s += bar[i].process(f, st) * lvl[i];
        }
        s *= 1.f + 0.15f * trem.process(5.7f, st);
        float sr = 1.f / st;
        float m = chorLfo.process(0.7f, st);
        l = (s + chorL.process(s, (0.008f + 0.002f * m) * sr)) * 0.5f * amp * 0.42f;
        r = (s + chorR.process(s, (0.008f - 0.002f * m) * sr)) * 0.5f * amp * 0.42f;
    }
};

// ── fold — West Coast: a sine through a slowly deepening wavefolder ──────────
struct FoldEngine : DroneEngine {
    SinOsc osc, osc2; LFNoise2 depthN; SinOsc depthLfo; Biquad lp[2]; LeakDC dc[2];
    const char* name() const override { return "fold"; }
    void init(uint32_t seed, float) override {
        osc.reset(); osc2.reset(0.31f);
        depthN.reset(seed + 1u); depthLfo.reset();
        for (int c = 0; c < 2; ++c) { lp[c].reset(); dc[c].reset(0.999f); }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float depth = 1.2f + 2.4f * (depthN.process(0.05f, st) * 0.5f + 0.5f)
                    + 0.6f * depthLfo.process(0.11f, st);
        float a = foldOver(osc.process(hz, st) * depth, -1.f, 1.f);
        float b = foldOver(osc2.process(hz * 1.002f, st) * depth * 0.95f, -1.f, 1.f);
        l = dc[0].process(lp[0].lpf(a, hz * 12.f, st)) * amp * 0.45f;
        r = dc[1].process(lp[1].lpf(b, hz * 12.f, st)) * amp * 0.45f;
    }
};

// ── phase — CZ-style phase distortion, knee swept slowly ─────────────────────
struct PhaseEngine : DroneEngine {
    float p1 = 0.f, p2 = 0.f; LFNoise2 kneeN; SinOsc kneeLfo; LeakDC dc[2];
    const char* name() const override { return "phase"; }
    void init(uint32_t seed, float) override {
        p1 = 0.f; p2 = 0.33f;
        kneeN.reset(seed + 1u); kneeLfo.reset();
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    static float pd(float p, float d) {
        // piecewise-linear phase warp: knee at d, then read a cosine
        float w = (p < d) ? p * 0.5f / d : 0.5f + (p - d) * 0.5f / (1.f - d);
        return -std::cos(kTwoPi * w);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float d = rack::clamp(0.5f - 0.42f * kneeLfo.process(0.043f, st)
                              - 0.25f * kneeN.process(0.09f, st), 0.03f, 0.97f);
        p1 += hz * st; p1 -= std::floor(p1);
        p2 += hz * 1.0035f * st; p2 -= std::floor(p2);
        l = dc[0].process(pd(p1, d)) * amp * 0.4f;
        r = dc[1].process(pd(p2, 1.f - d)) * amp * 0.4f;
    }
};

// ── tide — ocean: swelling band-swept brown noise over a deep sub ────────────
struct TideEngine : DroneEngine {
    BrownNoise bn[2]; SVF band[2]; LFNoise2 gustN[2], swellN; SinOsc sub;
    const char* name() const override { return "tide"; }
    void init(uint32_t seed, float) override {
        bn[0].reset(seed + 1u); bn[1].reset(seed + 2u);
        band[0].reset(); band[1].reset();
        gustN[0].reset(seed + 3u); gustN[1].reset(seed + 4u);
        swellN.reset(seed + 5u);
        sub.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float swell = rack::clamp(swellN.process(0.08f, st) * 0.7f + 0.5f, 0.05f, 1.f);
        float s = sub.process(hz * 0.25f, st) * 0.35f * swell;
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float cf = linexp(gustN[c].process(0.06f, st), -1.f, 1.f, hz, hz * 14.f);
            out[c] = band[c].process(bn[c].process(), cf, 0.55f, 0.f, 1.f, 0.f, st) * swell * 2.8f;
        }
        // the gust sweep hz..14·hz loses band energy as it nears Nyquist
        static const float MG[8] = {0.96f, 1.17f, 1.47f, 1.85f, 2.27f, 2.68f, 3.01f, 3.29f};
        float g = octaveGain(hz, MG);
        l = (out[0] + s) * amp * g; r = (out[1] + s) * amp * g;
    }
};

// ── ember — fire: crackle, flickering roar, sub rumble, occasional pops ──────
struct EmberEngine : DroneEngine {
    Crackle crk; BrownNoise bn; Biquad roarLp; LFNoise1 flick1, flick2;
    SinOsc sub; Dust2 dPop; Ringz pop; LeakDC dc[2];
    const char* name() const override { return "ember"; }
    void init(uint32_t seed, float) override {
        crk.reset(); bn.reset(seed + 1u); roarLp.reset();
        flick1.reset(seed + 2u); flick2.reset(seed + 3u);
        sub.reset(); dPop.reset(seed + 4u); pop.reset();
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float f1 = flick1.process(7.f, st) * 0.5f + 0.5f;
        float f2 = flick2.process(2.3f, st) * 0.5f + 0.5f;
        float roar = roarLp.lpf(bn.process(), hz * 3.f, st) * (0.4f + 0.6f * f2) * 1.6f;
        float cr = (crk.process(1.9f) - 0.4f) * 0.25f * (0.3f + 0.7f * f1);
        float s = sub.process(hz * 0.5f, st) * 0.3f * (0.6f + 0.4f * f2);
        float pp = pop.process(dPop.process(0.5f, st), hz * 1.5f, 0.2f, st) * 2.f;
        float m = roar + cr + s + pp;
        l = dc[0].process(std::tanh(m * 1.2f)) * amp * 0.45f;
        r = dc[1].process(std::tanh((roar * 0.9f + cr * 1.1f + s + pp * 0.8f) * 1.2f)) * amp * 0.45f;
    }
};

// ── pipe — a hollow tube: negative-feedback comb sung by breath noise ────────
struct PipeEngine : DroneEngine {
    BlPulse sq; WhiteNoise wn; Biquad breathBp; CombL combL_, combR_;
    LFNoise2 breathN;
    const char* name() const override { return "pipe"; }
    void init(uint32_t seed, float sr) override {
        sq.reset(); wn.reset(seed + 1u); breathBp.reset();
        combL_.dl.init(0.6f, sr); combR_.dl.init(0.6f, sr);
        breathN.reset(seed + 2u);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float breath = 0.5f + 0.5f * (breathN.process(0.15f, st) * 0.5f + 0.5f);
        float ex = sq.process(hz * 0.5f, 0.5f, st) * 0.08f
                 + breathBp.bpf(wn.process(), hz * 2.f, 1.5f, st) * breath * 0.3f;
        float d = rack::clamp(sr / std::max(hz, 8.f), 4.f, 0.55f * sr);
        // negative feedback → odd harmonics, hollow clarinet-like bore
        l = softclip(combL_.process(ex, d, -0.93f)) * amp * 0.45f;
        r = softclip(combR_.process(ex * 0.98f, d * 1.002f, -0.93f)) * amp * 0.45f;
    }
};

// ── bowl — singing bowl: pure long resonances continuously stroked ───────────
struct BowlEngine : DroneEngine {
    static constexpr int N = 4;
    Ringz res[2][N]; PinkNoise pk[2]; Biquad strokeBp[2]; LFNoise2 strokeN;
    const char* name() const override { return "bowl"; }
    void init(uint32_t seed, float) override {
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < N; ++i) res[c][i].reset();
            pk[c].reset(seed + c + 1u); strokeBp[c].reset();
        }
        strokeN.reset(seed + 5u);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float ratio[N] = {1.f, 2.71f, 4.95f, 7.87f};
        float stroke = 0.5f + 0.5f * (strokeN.process(0.1f, st) * 0.5f + 0.5f);
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float ex = strokeBp[c].bpf(pk[c].process(), hz * 2.f, 2.f, st) * stroke * 0.5f;
            float s = 0.f;
            float detune = c ? 1.002f : 1.f;   // beating pair between channels
            for (int i = 0; i < N; ++i)
                s += res[c][i].process(ex, hz * ratio[i] * detune, 12.f / (1.f + i), st) / (1.f + i);
            out[c] = s;
        }
        // Ringz stores less energy per strike as the resonances rise
        static const float MG[8] = {0.84f, 1.32f, 1.41f, 1.82f, 3.01f, 3.85f, 4.00f, 4.00f};
        float g = 80.f * octaveGain(hz, MG);
        l = out[0] * amp * g; r = out[1] * amp * g;
    }
};

// ── drift — a six-voice cluster forever re-tuning itself ─────────────────────
struct DriftEngine : DroneEngine {
    static constexpr int N = 6;
    SinOsc osc[N]; Dust dStep[N]; Dbrown walk[N]; Lag glide[N];
    float target[N] = {}, base[N] = {}, pos[N] = {};
    const char* name() const override { return "drift"; }
    void init(uint32_t seed, float) override {
        static const float b[N] = {1.f, 1.f, 1.5f, 1.5f, 2.f, 2.f};
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            osc[i].reset(rng.uniform());
            dStep[i].reset(seed + i * 17u + 1u);
            walk[i].reset(seed + i * 17u + 2u);
            glide[i].reset();
            base[i] = b[i];
            target[i] = b[i];
            pos[i] = rng.bipolar() * 0.7f;
        }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            if (dStep[i].process(0.3f, st) > 0.f)
                target[i] = base[i] * (1.f + walk[i].next(-0.035f, 0.035f, 0.012f));
            float f = glide[i].process(hz * target[i], 4.f, st);
            float s = osc[i].process(f, st);
            float vl, vr; pan2(s, pos[i], 1.f, vl, vr);
            l += vl; r += vr;
        }
        l = std::tanh(l * 0.5f) * amp * 0.55f;
        r = std::tanh(r * 0.5f) * amp * 0.55f;
    }
};

// ── shepard — the ever-rising barberpole drone ────────────────────────────────
struct ShepardEngine : DroneEngine {
    static constexpr int N = 8;
    SinOsc part[N];
    float master = 0.f;
    const char* name() const override { return "shepard"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) part[i].reset(rng.uniform());
        master = 0.f;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        master += st / 50.f;                    // one octave per 50 s, forever
        master -= std::floor(master);
        float base = hz / 8.f;
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            float p = i + master;               // octave position 0..8
            float f = base * std::exp2(p);
            if (f > 0.4f / st) continue;
            float a = 0.5f - 0.5f * std::cos(kTwoPi * p / N);   // spectral window
            float s = part[i].process(f, st) * a;
            if (i & 1) { l += s * 0.8f; r += s; } else { l += s; r += s * 0.8f; }
        }
        l *= amp * 0.28f; r *= amp * 0.28f;
    }
};

// ── rain — water: plucked droplets in a wet cave over a soft pad ─────────────
struct RainEngine : DroneEngine {
    static constexpr int N = 8;
    Pluck pl[N]; Dust2 dDrop; Rng rng; int next = 0;
    float pos[N] = {}, freq[N] = {};
    LFTri pad1, pad2; Biquad padLp;
    SchroederReverb rev; LeakDC dc[2];
    const char* name() const override { return "rain"; }
    void init(uint32_t seed, float sr) override {
        rng.seed(seed);
        for (int i = 0; i < N; ++i) { pl[i].init(0.3f, sr); pos[i] = 0.f; freq[i] = 200.f; }
        dDrop.reset(seed + 1u); next = 0;
        pad1.reset(); pad2.reset(0.4f); padLp.reset();
        rev.init(seed + 2u, sr);
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        static const float penta[6] = {1.f, 9.f / 8.f, 5.f / 4.f, 3.f / 2.f, 5.f / 3.f, 2.f};
        float trigIn = dDrop.process(3.f, st);
        float dl = 0.f, dr = 0.f;
        for (int i = 0; i < N; ++i) {
            float trig = 0.f;
            if (trigIn != 0.f && i == next) {
                trig = 1.f;
                freq[i] = hz * penta[std::min((int)(rng.uniform() * 6), 5)]
                        * (rng.uniform() < 0.3f ? 2.f : 1.f);
                pos[i] = rng.bipolar() * 0.9f;
                next = (next + 1) % N;
            }
            float ex = trig * rng.bipolar();
            float s = pl[i].process(ex * 1.5f, trig, sr / rack::clamp(freq[i], 30.f, 6000.f), 1.5f, 0.45f, st);
            float vl, vr; pan2(s, pos[i], 1.f, vl, vr);
            dl += vl; dr += vr;
        }
        float pad = padLp.lpf(pad1.process(hz * 0.5f, st) + pad2.process(hz * 0.501f, st), hz * 2.f, st) * 0.18f;
        float wl, wr; rev.process(dl, dr, st, sr, wl, wr);
        static const float MG[8] = {1.30f, 0.67f, 0.85f, 0.77f, 0.84f, 1.10f, 1.53f, 1.97f};
        float g = 1.5f * octaveGain(hz, MG);
        // the Pluck loops recirculate excitation DC: block after the clip
        l = dc[0].process(softclip((dl * 0.6f + wl * 0.12f + pad) * g)) * amp;
        r = dc[1].process(softclip((dr * 0.6f + wr * 0.12f + pad) * g)) * amp;
    }
};

// ── wire — a bowed string: Karplus loop continuously excited by "bow" noise ──
struct WireEngine : DroneEngine {
    DelayLine loop[2]; float lp[2] = {}; WhiteNoise wn; Biquad bowLp;
    LFNoise2 pressureN; Dust dFlip; Lag flipLag; float mult = 1.f;
    Rng rng; LeakDC dc[2];
    const char* name() const override { return "wire"; }
    void init(uint32_t seed, float sr) override {
        loop[0].init(0.6f, sr); loop[1].init(0.6f, sr);
        lp[0] = lp[1] = 0.f;
        wn.reset(seed + 1u); bowLp.reset();
        pressureN.reset(seed + 2u);
        dFlip.reset(seed + 3u); flipLag.reset(); mult = 1.f;
        rng.seed(seed + 4u);
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        if (dFlip.process(0.08f, st) > 0.f) {
            static const float m[4] = {1.f, 1.f, 2.f, 3.f};
            mult = m[std::min((int)(rng.uniform() * 4), 3)];
        }
        float mm = flipLag.process(mult, 0.8f, st);
        float press = 0.4f + 0.6f * (pressureN.process(0.09f, st) * 0.5f + 0.5f);
        float bow = bowLp.lpf(wn.process(), 2500.f, st) * press * 0.25f;
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float d = rack::clamp(sr / std::max(hz * mm * (c ? 1.003f : 1.f), 12.f), 4.f, 0.55f * sr);
            float y = loop[c].tapL(d);
            lp[c] += (y - lp[c]) * 0.45f;                 // loop damping
            loop[c].write(softclip(lp[c] * 0.996f + bow));
            out[c] = y;
        }
        // the 0.996 loop recirculates any bow-noise DC ~250x: block it
        l = dc[0].process(out[0]) * amp * 1.4f; r = dc[1].process(out[1]) * amp * 1.4f;
    }
};

// ── pulsework — meshing tick trains through tuned combs ──────────────────────
struct PulseworkEngine : DroneEngine {
    Impulse tick[3]; PercEnv env[3]; WhiteNoise wn;
    CombC comb[3]; float pos[3] = {-0.7f, 0.f, 0.7f};
    LFTri pad; Biquad padLp; LeakDC dc[2];
    const char* name() const override { return "pulsework"; }
    void init(uint32_t seed, float sr) override {
        for (int k = 0; k < 3; ++k) {
            tick[k].reset(); env[k].reset();
            comb[k].dl.init(0.3f, sr);
        }
        wn.reset(seed + 1u);
        pad.reset(); padLp.reset();
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        static const float div[3] = {64.f, 48.f, 32.f};
        static const float tune[3] = {1.f, 1.5f, 2.f};
        l = r = 0.f;
        for (int k = 0; k < 3; ++k) {
            float tk = tick[k].process(rack::clamp(hz / div[k], 0.3f, 4.f), st);
            float ex = env[k].process(tk, 0.001f, 0.04f, -4.f, st) * wn.process();
            float d = rack::clamp(sr / (hz * tune[k]), 3.f, 0.28f * sr);
            float s = comb[k].process(ex, d, combFeedback(d / sr, 1.2f));
            float vl, vr; pan2(s, pos[k], 1.f, vl, vr);
            l += vl; r += vr;
        }
        float p = padLp.lpf(pad.process(hz * 0.5f, st), hz * 1.5f, st) * 0.15f;
        static const float MG[8] = {2.67f, 1.87f, 1.11f, 0.80f, 0.72f, 0.65f, 0.60f, 0.63f};
        float g = octaveGain(hz, MG);
        // the combs recirculate tick DC: block after the clip
        l = dc[0].process(softclip((l * 0.4f + p) * g)) * amp;
        r = dc[1].process(softclip((r * 0.4f + p) * g)) * amp;
    }
};

// ── cavern — sparse harmonic blips lost in a vast reverb over a sub ──────────
struct CavernEngine : DroneEngine {
    SinOsc blip, sub; PercEnv benv; Dust dBlip; Rng rng;
    float blipHz = 220.f; bool first = true;
    SchroederReverb rev;
    const char* name() const override { return "cavern"; }
    void init(uint32_t seed, float sr) override {
        blip.reset(); sub.reset(); benv.reset();
        dBlip.reset(seed + 1u); rng.seed(seed + 2u);
        blipHz = 220.f; first = true;
        rev.init(seed + 3u, sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float trig = dBlip.process(1.f / 6.f, st) + (first ? 1.f : 0.f);
        first = false;
        if (trig > 0.f) {
            static const float h[5] = {2.f, 3.f, 4.f, 6.f, 8.f};
            blipHz = hz * h[std::min((int)(rng.uniform() * 5), 4)];
        }
        float b = blip.process(blipHz, st) * benv.process(trig, 0.6f, 3.5f, -3.f, st) * 0.5f;
        float s = sub.process(hz * 0.25f, st) * 0.28f;
        float wl, wr; rev.process(b, b * 0.8f, st, sr, wl, wr);
        l = softclip(s + b * 0.12f + wl * 0.1f) * amp;
        r = softclip(s + b * 0.12f + wr * 0.1f) * amp;
    }
};

// ── corona — odd harmonics through a breathing wavefolder, bright halo ───────
struct CoronaEngine : DroneEngine {
    static constexpr int N = 8;
    SinOsc part[N], shim[N]; float shimRate[N] = {};
    LFNoise2 foldN; Biquad hp[2]; LeakDC dc[2];
    const char* name() const override { return "corona"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            part[i].reset(rng.uniform());
            shim[i].reset(rng.uniform());
            shimRate[i] = 0.06f + rng.uniform() * 0.14f;
        }
        foldN.reset(seed + 99u);
        for (int c = 0; c < 2; ++c) { hp[c].reset(); dc[c].reset(0.999f); }
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float g = 1.4f + 1.2f * (foldN.process(0.07f, st) * 0.5f + 0.5f);
        float sl = 0.f, sr_ = 0.f;
        for (int i = 0; i < N; ++i) {
            int n = 2 * i + 1;                        // odd harmonics
            float f = hz * n;
            if (f > 0.4f / st) continue;
            float a = (shim[i].process(shimRate[i], st) * 0.5f + 0.5f) / n;
            float s = part[i].process(f, st) * a;
            if (i & 1) sr_ += s; else sl += s;
        }
        l = dc[0].process(hp[0].hpf(foldOver(sl * g, -1.f, 1.f), hz, st)) * amp * 0.8f;
        r = dc[1].process(hp[1].hpf(foldOver(sr_ * g, -1.f, 1.f), hz, st)) * amp * 0.8f;
    }
};

// ── loam — dark triangle pad through a slow four-stage phaser ────────────────
struct LoamEngine : DroneEngine {
    LFTri tri[3]; BAllPass ap[2][4]; SinOsc phLfo; Biquad lp[2];
    const char* name() const override { return "loam"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int k = 0; k < 3; ++k) tri[k].reset(rng.uniform());
        for (int c = 0; c < 2; ++c) {
            lp[c].reset();
            for (int k = 0; k < 4; ++k) ap[c][k].reset();
        }
        phLfo.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float s = tri[0].process(hz * 0.5f, st)
                + tri[1].process(hz * 0.503f, st)
                + tri[2].process(hz * 0.497f, st);
        s *= 0.33f;
        float m = phLfo.process(0.045f, st);
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float y = s;
            float cf = linexp(c ? -m : m, -1.f, 1.f, hz, hz * 10.f);
            for (int k = 0; k < 4; ++k) y = ap[c][k].process(y, cf * (1.f + 0.4f * k), 1.f, st);
            out[c] = lp[c].lpf(s * 0.6f + y * 0.6f, hz * 4.f, st);
        }
        l = out[0] * amp * 0.75f; r = out[1] * amp * 0.75f;
    }
};

// ── lattice — golden-ratio sine pairs diode-ring-modulated, slowly rotating ──
struct LatticeEngine : DroneEngine {
    SinOsc a[3], b[3]; SinOsc rot; LeakDC dc[2];
    const char* name() const override { return "lattice"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int k = 0; k < 3; ++k) { a[k].reset(rng.uniform()); b[k].reset(rng.uniform()); }
        rot.reset(rng.uniform());
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float PHI = 1.618034f;
        float s = 0.f;
        for (int k = 0; k < 3; ++k) {
            float fa = hz * std::pow(PHI, (float)k - 1.f);
            float fb = hz * std::pow(PHI, (float)k);
            s += diodeRingMod(a[k].process(fa, st), b[k].process(fb, st)) / (1.f + k);
        }
        float pos = rot.process(0.017f, st) * 0.7f;
        float vl, vr; rotate2(dc[0].process(s), dc[1].process(s * 0.85f), pos * 0.5f, vl, vr);
        l = vl * amp * 0.5f; r = vr * amp * 0.5f;
    }
};

// ── aster — sustained two-operator FM with a wandering index ─────────────────
struct AsterEngine : DroneEngine {
    SinOsc car1, mod1, car2, mod2; LFNoise2 idxN; Lag idxLag; SinOsc idxLfo; LeakDC dc;
    const char* name() const override { return "aster"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        car1.reset(rng.uniform()); mod1.reset(rng.uniform());
        car2.reset(rng.uniform()); mod2.reset(rng.uniform());
        idxN.reset(seed + 1u); idxLag.reset(); idxLfo.reset();
        dc.reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float idx = idxLag.process(2.f * (idxN.process(0.05f, st) * 0.5f + 0.5f), 1.f, st)
                  + 0.6f + 0.4f * idxLfo.process(0.13f, st);
        float m1 = mod1.process(hz * 1.001f, st) * idx;
        // mod sits at hz·1.001, so PM drops a sideband at 0.001·hz — near-DC
        // at drone pitches; block it at the carrier
        float v1 = dc.process(car1.process(hz, st, m1));
        float m2 = mod2.process(hz * 3.f, st) * idx * 0.5f;
        float v2 = car2.process(hz * 2.f, st, m2) * 0.22f;
        l = (v1 * 0.6f + v2) * amp * 0.55f;
        r = (v1 * 0.6f - v2) * amp * 0.55f;
    }
};

// ── veldt — insects at dusk: sparse high chirps over a warm low drone ────────
struct VeldtEngine : DroneEngine {
    static constexpr int N = 5;
    struct Chirp {
        Dust d; SinOscFB osc; PercEnv env; Rng rng;
        float f = 3000.f, pos = 0.f, rate = 12.f;
    };
    Chirp ch[N];
    LFTri warm1, warm2; Biquad warmLp;
    const char* name() const override { return "veldt"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            ch[i].d.reset(seed + i * 13u + 1u);
            ch[i].osc.reset(rng.uniform());
            ch[i].env.reset();
            ch[i].rng.seed(seed + i * 13u + 2u);
            ch[i].pos = rng.bipolar() * 0.9f;
            ch[i].rate = 8.f + rng.uniform() * 20.f;
        }
        warm1.reset(); warm2.reset(0.35f); warmLp.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float warm = warmLp.lpf(warm1.process(hz * 0.5f, st) + warm2.process(hz * 0.505f, st),
                                hz * 1.8f, st) * 0.3f;
        l = warm * amp; r = warm * amp;
        for (int i = 0; i < N; ++i) {
            Chirp& c = ch[i];
            float trig = c.d.process(0.8f, st);
            if (trig > 0.f) {
                c.f = hz * (18.f + c.rng.uniform() * 22.f);
                c.pos = c.rng.bipolar() * 0.9f;
            }
            float e = c.env.process(trig, 0.004f, 0.05f + 0.04f * (i & 1), -4.f, st);
            if (e > 0.f) {
                float s = c.osc.process(std::min(c.f, 0.35f / st), 0.9f, st) * e
                        * (0.5f + 0.5f * std::sin(kTwoPi * c.rate * e));   // trill
                float vl, vr; pan2(s * 0.12f, c.pos, 1.f, vl, vr);
                l += vl * amp; r += vr * amp;
            }
        }
    }
};

// ── mirror — tuned comb-space: noise sustained inside two long combs ─────────
struct MirrorEngine : DroneEngine {
    CombC c1, c2; WhiteNoise wn; PinkNoise pk; Biquad exLp; LeakDC dc[2];
    const char* name() const override { return "mirror"; }
    void init(uint32_t seed, float sr) override {
        c1.dl.init(0.6f, sr); c2.dl.init(0.6f, sr);
        wn.reset(seed + 1u); pk.reset(seed + 2u); exLp.reset();
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float ex = exLp.lpf(wn.process() * 0.03f + pk.process() * 0.05f, hz * 8.f, st);
        float d1 = rack::clamp(sr / std::max(hz, 8.f), 4.f, 0.55f * sr);
        float d2 = rack::clamp(sr / std::max(hz * 1.5f, 8.f), 4.f, 0.55f * sr);
        float s1 = c1.process(ex, d1, 0.985f);
        float s2 = c2.process(ex, d2, 0.982f);
        // the combs hold more modes (louder) as their delay shortens
        static const float MG[8] = {3.32f, 3.08f, 2.71f, 2.22f, 1.67f, 1.23f, 0.92f, 0.74f};
        float g = 2.2f * octaveGain(hz, MG);
        // comb feedback 0.985 gives ~66x gain at DC: block after the clip
        l = dc[0].process(softclip((s1 + s2 * 0.6f) * g)) * amp * 0.9f;
        r = dc[1].process(softclip((s2 + s1 * 0.6f) * g)) * amp * 0.9f;
    }
};

// ── halo — regenerative octave-up shimmer around a quiet sine ────────────────
struct HaloEngine : DroneEngine {
    SinOsc seedOsc; PitchShift shift; SchroederReverb rev;
    float fbL = 0.f, fbR = 0.f;
    Biquad fbHp;
    const char* name() const override { return "halo"; }
    void init(uint32_t seed, float sr) override {
        seedOsc.reset();
        shift.init(0.15f, sr, seed + 1u);
        rev.init(seed + 2u, sr);
        fbL = fbR = 0.f; fbHp.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float src = seedOsc.process(hz, st) * 0.22f;
        float in = src + fbHp.hpf(softclip((fbL + fbR) * 0.5f), hz * 0.8f, st) * 0.4f;
        float up = shift.process(in, 0.12f, 2.f, 0.01f, st);
        float wl, wr; rev.process(up, up * 0.9f, st, sr, wl, wr);
        fbL = softclip(wl * 0.22f); fbR = softclip(wr * 0.22f);
        l = softclip(src * 0.9f + wl * 0.2f) * amp * 0.6f;
        r = softclip(src * 0.9f + wr * 0.2f) * amp * 0.6f;
    }
};

// ── turbine — machine-room hum: sub square and a sweeping high whine ─────────
struct TurbineEngine : DroneEngine {
    BlPulse sq; SinOsc whine, whineLfo; Biquad peak[2]; BrownNoise bn; Biquad rumbleLp;
    LeakDC dc;
    const char* name() const override { return "turbine"; }
    void init(uint32_t seed, float) override {
        sq.reset(); whine.reset(); whineLfo.reset();
        peak[0].reset(); peak[1].reset();
        bn.reset(seed + 1u); rumbleLp.reset();
        dc.reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float s = sq.process(hz * 0.5f, 0.5f, st) * 0.4f;
        float wf = hz * 8.f * (1.f + 0.03f * whineLfo.process(0.09f, st));
        float w = whine.process(std::min(wf, 0.35f / st), st) * 0.09f;
        // brown noise wanders around a nonzero mean; the lpf keeps it
        float rum = rumbleLp.lpf(bn.process(), hz, st) * 0.5f;
        float m = dc.process(s + rum);
        l = (peak[0].peakeq(m, hz * 2.f, 0.7f, 6.f, st) + w) * amp * 0.5f;
        r = (peak[1].peakeq(m, hz * 3.f, 0.7f, 6.f, st) - w) * amp * 0.5f;
    }
};

// ── frost — high crystalline partial strikes over a near-silent root ─────────
struct FrostEngine : DroneEngine {
    static constexpr int N = 6;
    struct Ice { Dust d; SinOsc osc; PercEnv env; Rng rng; float f = 2000.f, pos = 0.f; };
    Ice ice[N];
    SinOsc root; CombC air;
    const char* name() const override { return "frost"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            ice[i].d.reset(seed + i * 29u + 1u);
            ice[i].osc.reset(rng.uniform());
            ice[i].env.reset();
            ice[i].rng.seed(seed + i * 29u + 2u);
            ice[i].pos = rng.bipolar() * 0.9f;
        }
        root.reset();
        air.dl.init(0.05f, sr);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        l = r = root.process(hz, st) * 0.07f;
        float sum = 0.f;
        for (int i = 0; i < N; ++i) {
            Ice& c = ice[i];
            float trig = c.d.process(0.4f, st);
            if (trig > 0.f) {
                int h = 8 + (int)(c.rng.uniform() * 9.f);
                c.f = hz * h;
                c.pos = c.rng.bipolar() * 0.9f;
            }
            float e = c.env.process(trig, 0.005f, 2.f + (i & 3) * 0.7f, -5.f, st);
            if (e > 0.f && c.f < 0.4f / st) {
                float s = c.osc.process(c.f, st) * e * 0.35f;
                sum += s;
                float vl, vr; pan2(s, c.pos, 1.f, vl, vr);
                l += vl; r += vr;
            }
        }
        float sheen = air.process(sum, 0.031f * sr, 0.75f) * 0.4f;
        // top octave left alone: the ice partials sit beyond Nyquist there
        static const float MG[8] = {0.85f, 0.90f, 1.14f, 1.13f, 0.80f, 0.94f, 1.74f, 1.00f};
        float g = octaveGain(hz, MG);
        l = softclip((l + sheen) * g) * amp * 0.65f;
        r = softclip((r + sheen * 0.9f) * g) * amp * 0.65f;
    }
};

// ── root — sub-octave breathing meditation drone ─────────────────────────────
struct RootEngine : DroneEngine {
    SinOsc s1, s2, fifth; SinOsc breathLfo; LeakDC dc;
    const char* name() const override { return "root"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        s1.reset(); s2.reset(rng.uniform()); fifth.reset(rng.uniform());
        breathLfo.reset(rng.uniform());
        dc.reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float br = 0.7f + 0.3f * breathLfo.process(0.05f, st);
        float s = s1.process(hz * 0.5f, st) * 0.5f
                + s2.process(hz * 0.25f, st) * 0.4f
                + fifth.process(hz * 0.75f, st) * 0.08f;
        s = dc.process(std::tanh(s * 1.3f)) * br;
        l = s * amp * 0.62f; r = s * amp * 0.62f;
    }
};

// ── sputter — granular haze: dust-triggered filtered saw grains ──────────────
struct SputterEngine : DroneEngine {
    static constexpr int N = 12;
    struct Grain { bool on = false; float t = 0.f, dur = 0.05f, ph = 0.f, f = 220.f, pos = 0.f; };
    Grain g[N]; int slot = 0;
    Dust dG; Rng rng; Biquad bp[2]; LeakDC dc[2];
    const char* name() const override { return "sputter"; }
    void init(uint32_t seed, float) override {
        for (int i = 0; i < N; ++i) g[i].on = false;
        slot = 0;
        dG.reset(seed + 1u); rng.seed(seed + 2u);
        bp[0].reset(); bp[1].reset();
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        if (dG.process(14.f, st) > 0.f) {
            Grain& n = g[slot]; slot = (slot + 1) % N;
            static const float oct[4] = {0.5f, 1.f, 1.f, 2.f};
            n.on = true; n.t = 0.f;
            n.dur = 0.03f + rng.uniform() * 0.06f;
            n.f = hz * oct[std::min((int)(rng.uniform() * 4), 3)] * (1.f + rng.bipolar() * 0.01f);
            n.pos = rng.bipolar() * 0.9f;
            n.ph = rng.uniform();
        }
        float sl = 0.f, sr_ = 0.f;
        for (int i = 0; i < N; ++i) {
            Grain& n = g[i];
            if (!n.on) continue;
            n.t += st;
            if (n.t >= n.dur) { n.on = false; continue; }
            float u = n.t / n.dur;
            float win = 0.5f - 0.5f * std::cos(kTwoPi * u);
            n.ph += n.f * st; n.ph -= std::floor(n.ph);
            float s = (2.f * n.ph - 1.f) * win;
            float vl, vr; pan2(s, n.pos, 1.f, vl, vr);
            sl += vl; sr_ += vr;
        }
        l = dc[0].process(softclip(bp[0].bpf(sl, hz * 2.f, 1.8f, st) * 1.3f)) * amp;
        r = dc[1].process(softclip(bp[1].bpf(sr_, hz * 2.f, 1.8f, st) * 1.3f)) * amp;
    }
};

// ── anthem — swelling brass ensemble: saws into a self-opening ladder ────────
struct AnthemEngine : DroneEngine {
    BlSaw saw[4]; MoogFF lad[2]; SinOsc swellLfo; DelayC chor[2]; SinOsc chorLfo;
    const char* name() const override { return "anthem"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        for (int k = 0; k < 4; ++k) saw[k].reset(rng.uniform());
        lad[0].reset(); lad[1].reset();
        swellLfo.reset(rng.uniform());
        chor[0].dl.init(0.03f, sr); chor[1].dl.init(0.03f, sr);
        chorLfo.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float s = saw[0].process(hz * 0.996f, st) + saw[1].process(hz * 1.004f, st)
                + saw[2].process(hz, st) + saw[3].process(hz * 1.5f, st) * 0.35f;
        s *= 0.3f;
        float sw = swellLfo.process(0.07f, st) * 0.5f + 0.5f;
        float cut = hz * (1.5f + 7.f * sw * sw);
        float m = chorLfo.process(0.6f, st);
        float a = lad[0].process(s, cut, 0.25f, st);
        float b = lad[1].process(s, cut * 1.05f, 0.25f, st);
        l = (a + chor[0].process(a, (0.011f + 0.003f * m) * sr)) * amp * 0.9f;
        r = (b + chor[1].process(b, (0.011f - 0.003f * m) * sr)) * amp * 0.9f;
    }
};

// ── naiad — water-modulated FM burble ────────────────────────────────────────
struct NaiadEngine : DroneEngine {
    SinOsc car[2], mod[2]; WhiteNoise wn; CombL burble; Lag idxLag; Biquad exLp;
    const char* name() const override { return "naiad"; }
    void init(uint32_t seed, float sr) override {
        Rng rng; rng.seed(seed);
        for (int c = 0; c < 2; ++c) { car[c].reset(rng.uniform()); mod[c].reset(rng.uniform()); }
        wn.reset(seed + 1u);
        burble.dl.init(0.05f, sr);
        idxLag.reset(); exLp.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        float n = exLp.lpf(wn.process(), 300.f, st);
        float bub = burble.process(n, 0.021f * sr, 0.88f);
        float idx = idxLag.process(0.4f + 5.f * std::fabs(bub), 0.06f, st);
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float m = mod[c].process(hz * (c ? 2.003f : 2.001f), st) * idx;
            out[c] = car[c].process(hz * (c ? 1.001f : 1.f), st, m);
        }
        l = out[0] * amp * 0.36f; r = out[1] * amp * 0.36f;
    }
};

// ── eclipse — dark vowel of brown noise over a deep sub ──────────────────────
struct EclipseEngine : DroneEngine {
    BrownNoise bn[2]; Biquad form[2][3], lp[2]; LFNoise2 morphN; SinOsc sub;
    const char* name() const override { return "eclipse"; }
    void init(uint32_t seed, float) override {
        for (int c = 0; c < 2; ++c) {
            bn[c].reset(seed + c + 1u);
            lp[c].reset();
            for (int k = 0; k < 3; ++k) form[c][k].reset();
        }
        morphN.reset(seed + 5u); sub.reset();
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float FO[3] = {450, 800, 2830};
        static const float FU[3] = {325, 700, 2700};
        static const float G[3] = {1.f, 0.45f, 0.08f};
        float v = morphN.process(0.03f, st) * 0.5f + 0.5f;
        float s = sub.process(hz * 0.25f, st) * 0.35f;
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float n = bn[c].process();
            float y = 0.f;
            for (int k = 0; k < 3; ++k) {
                float f = FO[k] + (FU[k] - FO[k]) * v;
                y += form[c][k].bpf(n, f * (c ? 0.98f : 1.f), 0.1f, st) * G[k];
            }
            out[c] = lp[c].lpf(y, 900.f, st) * 2.2f + s;
        }
        l = out[0] * amp; r = out[1] * amp;
    }
};

// ── quill — slow harp: long-sustain plucks arpeggiating a harmonic set ───────
struct QuillEngine : DroneEngine {
    static constexpr int N = 4;
    Pluck pl[N]; int next = 0, step = 0;
    float freq[N] = {}, pos[N] = {};
    float timer = 0.f, interval = 3.f; bool first = true;
    Rng rng; SchroederReverb rev; LeakDC dc[2];
    const char* name() const override { return "quill"; }
    void init(uint32_t seed, float sr) override {
        rng.seed(seed);
        for (int i = 0; i < N; ++i) { pl[i].init(0.4f, sr); freq[i] = 220.f; pos[i] = 0.f; }
        next = 0; step = 0; timer = 0.f; first = true;
        interval = 2.5f + rng.uniform() * 1.5f;
        rev.init(seed + 1u, sr);
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        static const float harm[6] = {1.f, 1.5f, 2.f, 8.f / 3.f, 3.f, 4.f};
        timer -= st;
        float dl = 0.f, dr = 0.f;
        for (int i = 0; i < N; ++i) {
            float trig = 0.f, ex = 0.f;
            if ((timer <= 0.f || first) && i == next) {
                trig = 1.f; first = false;
                timer = interval * (0.8f + rng.uniform() * 0.4f);
                freq[i] = hz * harm[step % 6];
                step += (rng.uniform() < 0.75f) ? 1 : 2;
                pos[i] = rng.bipolar() * 0.7f;
                next = (next + 1) % N;
                ex = rng.bipolar() * 1.2f;
            }
            float s = pl[i].process(ex, trig, sr / rack::clamp(freq[i], 25.f, 5000.f), 8.f, 0.3f, st);
            float vl, vr; pan2(s, pos[i], 1.f, vl, vr);
            dl += vl; dr += vr;
        }
        float wl, wr; rev.process(dl, dr, st, sr, wl, wr);
        // the Pluck loops recirculate excitation DC: block after the clip.
        // 9.0: the original 4.5 was calibrated against that drift, which
        // inflated the measured level; the actual plucks sat far too low.
        l = dc[0].process(softclip((dl * 0.7f + wl * 0.1f) * 9.f)) * amp;
        r = dc[1].process(softclip((dr * 0.7f + wr * 0.1f) * 9.f)) * amp;
    }
};

// ── saros — an endless cadence: four gliding sines cycling a chord table ─────
struct SarosEngine : DroneEngine {
    static constexpr int NV = 4, NCH = 8;
    SinOsc osc[NV], det[NV]; Lag glide[NV];
    float target[NV] = {}; int chordIdx = 0; float timer = 0.f;
    const char* name() const override { return "saros"; }
    void init(uint32_t seed, float) override {
        Rng rng; rng.seed(seed);
        for (int i = 0; i < NV; ++i) {
            osc[i].reset(rng.uniform()); det[i].reset(rng.uniform());
            glide[i].reset(); target[i] = 0.f;
        }
        chordIdx = (int)(rng.uniform() * NCH); timer = 0.f;
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        static const float chords[NCH][NV] = {
            {0, 4, 7, 11}, {0, 3, 7, 10}, {-1, 2, 7, 9}, {0, 5, 7, 12},
            {-3, 0, 4, 7}, {-1, 4, 6, 11}, {0, 2, 7, 9}, {0, 4, 9, 14},
        };
        timer -= st;
        if (timer <= 0.f) {
            timer += 16.f;
            chordIdx = (chordIdx + 1) % NCH;
            for (int i = 0; i < NV; ++i) target[i] = chords[chordIdx][i];
        }
        float midi = cpsmidi(hz);
        l = r = 0.f;
        for (int i = 0; i < NV; ++i) {
            float f = midicps(glide[i].process(midi + target[i], 6.f, st));
            float s = osc[i].process(f, st) + det[i].process(f * 1.0015f, st);
            if (i & 1) { l += s * 0.4f; r += s * 0.6f; } else { l += s * 0.6f; r += s * 0.4f; }
        }
        l *= amp * 0.24f; r *= amp * 0.24f;
    }
};

// ── hive — the queen: feedback-sine swarm combed at the fundamental ──────────
struct HiveEngine : DroneEngine {
    static constexpr int N = 12;
    SinOscFB osc[N]; LFNoise2 fbN[N]; float harm[N] = {}, det[N] = {}, pos[N] = {};
    CombC comb[2]; LeakDC dc[2];
    const char* name() const override { return "hive"; }
    void init(uint32_t seed, float sr) override {
        static const float h[N] = {1, 1, 1.5f, 2, 2, 2.5f, 3, 3, 4, 5, 6, 8};
        Rng rng; rng.seed(seed);
        for (int i = 0; i < N; ++i) {
            osc[i].reset(rng.uniform());
            fbN[i].reset(seed + i * 43u + 1u);
            harm[i] = h[i];
            det[i] = 1.f + rng.bipolar() * 0.004f;
            pos[i] = rng.bipolar() * 0.85f;
        }
        comb[0].dl.init(0.3f, sr); comb[1].dl.init(0.3f, sr);
        dc[0].reset(0.999f); dc[1].reset(0.999f);
    }
    void process(float hz, float amp, float st, float& l, float& r) override {
        float sr = 1.f / st;
        l = r = 0.f;
        for (int i = 0; i < N; ++i) {
            float fb = 0.25f + 0.55f * (fbN[i].process(0.08f, st) * 0.5f + 0.5f);
            float f = hz * harm[i] * det[i];
            if (f > 0.4f / st) continue;
            float s = osc[i].process(f, fb, st) / (1.f + harm[i] * 0.4f);
            float vl, vr; pan2(s, pos[i], 1.f, vl, vr);
            l += vl; r += vr;
        }
        float d = rack::clamp(sr / std::max(hz, 12.f), 4.f, 0.28f * sr);
        // feedback sines carry intrinsic DC (mean of sin(φ + fb·sin) ≠ 0)
        l = dc[0].process(std::tanh((l + comb[0].process(l, d, 0.4f)) * 0.5f)) * amp * 0.42f;
        r = dc[1].process(std::tanh((r + comb[1].process(r, d * 1.001f, 0.4f)) * 0.5f)) * amp * 0.42f;
    }
};

// ── registry: the hyf bank ────────────────────────────────────────────────────
inline std::vector<std::unique_ptr<DroneEngine>> makeAltEngines() {
    std::vector<std::unique_ptr<DroneEngine>> v;
    v.emplace_back(new BeamEngine());
    v.emplace_back(new RootEngine());
    v.emplace_back(new WallEngine());
    v.emplace_back(new OrganelleEngine());
    v.emplace_back(new ChoirEngine());
    v.emplace_back(new BreathEngine());
    v.emplace_back(new GlassEngine());
    v.emplace_back(new BowlEngine());
    v.emplace_back(new GongEngine());
    v.emplace_back(new SwarmEngine());
    v.emplace_back(new HiveEngine());
    v.emplace_back(new FoldEngine());
    v.emplace_back(new PhaseEngine());
    v.emplace_back(new AsterEngine());
    v.emplace_back(new NaiadEngine());
    v.emplace_back(new LatticeEngine());
    v.emplace_back(new CoronaEngine());
    v.emplace_back(new ShepardEngine());
    v.emplace_back(new DriftEngine());
    v.emplace_back(new SarosEngine());
    v.emplace_back(new MirrorEngine());
    v.emplace_back(new WireEngine());
    v.emplace_back(new QuillEngine());
    v.emplace_back(new RainEngine());
    v.emplace_back(new PulseworkEngine());
    v.emplace_back(new HaloEngine());
    v.emplace_back(new CavernEngine());
    v.emplace_back(new LoamEngine());
    v.emplace_back(new AnthemEngine());
    v.emplace_back(new PipeEngine());
    v.emplace_back(new TideEngine());
    v.emplace_back(new EmberEngine());
    v.emplace_back(new VeldtEngine());
    v.emplace_back(new FrostEngine());
    v.emplace_back(new SputterEngine());
    v.emplace_back(new TurbineEngine());
    v.emplace_back(new EclipseEngine());
    return v;
}

}  // namespace draen
