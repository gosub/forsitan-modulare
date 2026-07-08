#pragma once
// draen_ugens.hpp — SuperCollider-flavoured UGEN primitives (header-only).
//
// dræn ports the drone SynthDefs from northern-information/dronecaster, whose
// engines are little SuperCollider graphs of the form { |hz, amp| ... }. This
// header is the reusable DSP layer those ports are built on: one small C++ class
// per SC UGEN, named after its SC counterpart, so future SC→C++ ports can lean
// on the same vocabulary. Where Rack already ships a good primitive we reuse it
// (MinBlepGenerator for band-limited edges, TBiquadFilter for the SOS filters)
// rather than reinventing it.
//
// Conventions:
//   - audio/control signals are bipolar floats, nominally [-1, 1] like SC.
//   - `st` is the sample time (1/sampleRate); pass it through from process().
//   - oscillators take their frequency in Hz each sample (SC-style .ar args).

#include <rack.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace draen {

constexpr float kTwoPi = 6.28318530718f;

// ── tiny deterministic PRNG (xorshift32) ─────────────────────────────────────
// Local RNG keeps noise off Rack's shared random:: state (audio-thread safe).
struct Rng {
    uint32_t s = 0x2545F491u;
    // avalanche-hash the seed so nearby seeds (e.g. s, s+7, s+14 for per-voice
    // seeding) decorrelate — xorshift alone gives correlated first outputs
    void seed(uint32_t v) {
        v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
        s = v ? v : 0x2545F491u;
    }
    float uniform() {  // [0, 1)
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.f / 16777216.f);
    }
    float bipolar() { return uniform() * 2.f - 1.f; }  // [-1, 1)
};

// ── SinOsc.ar / .kr — plain sine, phase accumulator ──────────────────────────
struct SinOsc {
    float phase = 0.f;
    void reset(float ph = 0.f) { phase = ph; }
    // freq in Hz; returns sin over [-1, 1]. `phaseOff` is a phase offset in
    // radians (SC's second SinOsc arg).
    float process(float freq, float st, float phaseOff = 0.f) {
        phase += freq * st;
        phase -= std::floor(phase);            // wrap to [0, 1)
        return std::sin(kTwoPi * phase + phaseOff);
    }
};

// ── LFTri.ar — naive (non-bandlimited) triangle, faithful to SC's LFTri ───────
struct LFTri {
    float phase = 0.f;
    void reset(float ph = 0.f) { phase = ph; }
    float process(float freq, float st) {
        phase += freq * st;
        phase -= std::floor(phase);
        // 0..1 -> triangle in [-1, 1], SC LFTri starts at 0 rising
        float p = phase;
        return (p < 0.5f) ? (4.f * p - 1.f) : (3.f - 4.f * p);
    }
};

// ── Saw.ar — band-limited sawtooth via MinBLEP ───────────────────────────────
struct BlSaw {
    float phase = 0.f;
    rack::dsp::MinBlepGenerator<16, 16> blep;
    void reset(float ph = 0.5f) { phase = ph; }
    float process(float freq, float st) {
        float dp = rack::clamp(freq * st, -0.35f, 0.35f);
        phase += dp;
        if (phase >= 1.f) {
            phase -= 1.f;
            blep.insertDiscontinuity(phase / dp - 1.f, -2.f);
        } else if (phase < 0.f) {
            phase += 1.f;
            blep.insertDiscontinuity(phase / dp, 2.f);
        }
        return 2.f * phase - 1.f + blep.process();
    }
};

// ── Pulse.ar — band-limited pulse/square with pulse width, via MinBLEP ────────
struct BlPulse {
    float phase = 0.f;
    rack::dsp::MinBlepGenerator<16, 16> blep;
    void reset(float ph = 0.f) { phase = ph; }
    // width in (0,1); returns a ±1 pulse whose high portion is `width` long.
    float process(float freq, float width, float st) {
        float dp = rack::clamp(freq * st, 1e-6f, 0.35f);
        width = rack::clamp(width, 0.001f, 0.999f);
        float adv = phase + dp;               // pre-wrap advanced phase
        // falling edge when the phase passes `width` (high -> low, jump -2)
        if (phase < width && adv >= width)
            blep.insertDiscontinuity((adv - width) / dp - 1.f, -2.f);
        // rising edge at the period wrap (low -> high, jump +2)
        if (adv >= 1.f)
            blep.insertDiscontinuity((adv - 1.f) / dp - 1.f, 2.f);
        phase = adv - std::floor(adv);
        float naive = (phase < width) ? 1.f : -1.f;
        return naive + blep.process();
    }
};

// ── LFNoise0.kr — stepped random, held between updates at `freq` Hz ───────────
struct LFNoise0 {
    Rng rng;
    float value = 0.f;
    float phase = 1.f;                         // force an update on first call
    void reset(uint32_t seed) { rng.seed(seed); value = rng.bipolar(); phase = 1.f; }
    float process(float freq, float st) {
        phase += freq * st;
        if (phase >= 1.f) { phase -= std::floor(phase); value = rng.bipolar(); }
        return value;
    }
};

// ── LFNoise1.kr — linearly-interpolated random ramps at `freq` Hz ─────────────
struct LFNoise1 {
    Rng rng;
    float cur = 0.f, target = 0.f;
    float phase = 1.f;
    void reset(uint32_t seed) { rng.seed(seed); cur = target = rng.bipolar(); phase = 1.f; }
    float process(float freq, float st) {
        phase += freq * st;
        if (phase >= 1.f) {
            phase -= std::floor(phase);
            cur = target;
            target = rng.bipolar();
        }
        return cur + (target - cur) * phase;
    }
};

// ── LeakDC.ar — one-pole DC blocker (SC default coef 0.995) ───────────────────
struct LeakDC {
    float x1 = 0.f, y1 = 0.f;
    void reset() { x1 = 0.f; y1 = 0.f; }
    float process(float x) { float y = x - x1 + 0.995f * y1; x1 = x; y1 = y; return y; }
};

// ── LFNoise2.kr — quadratically-interpolated random at `freq` Hz ──────────────
struct LFNoise2 {
    Rng rng;
    float y0 = 0.f, y1 = 0.f, y2 = 0.f;
    float phase = 1.f;
    void reset(uint32_t seed) { rng.seed(seed); y0 = y1 = y2 = rng.bipolar(); phase = 1.f; }
    float process(float freq, float st) {
        phase += freq * st;
        if (phase >= 1.f) { phase -= std::floor(phase); y0 = y1; y1 = y2; y2 = rng.bipolar(); }
        // Lagrange quadratic through (-1,y0), (0,y1), (1,y2)
        float t = phase;
        float a = (y0 - 2.f * y1 + y2) * 0.5f;
        float b = (y2 - y0) * 0.5f;
        return a * t * t + b * t + y1;
    }
};

// ── WhiteNoise.ar ─────────────────────────────────────────────────────────────
struct WhiteNoise {
    Rng rng;
    void reset(uint32_t seed) { rng.seed(seed); }
    float process() { return rng.bipolar(); }
};

// ── PinkNoise.ar — Paul Kellet's refined economy filter, ~[-1, 1] ─────────────
struct PinkNoise {
    Rng rng;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    void reset(uint32_t seed) { rng.seed(seed); b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.f; }
    float process() {
        float w = rng.bipolar();
        b0 = 0.99886f * b0 + w * 0.0555179f;
        b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f;
        b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f;
        b5 = -0.7616f * b5 - w * 0.0168980f;
        float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
        return pink * 0.11f;
    }
};

// ── Dust.ar — random positive impulses at an average density (Hz) ─────────────
struct Dust {
    Rng rng;
    void reset(uint32_t seed) { rng.seed(seed); }
    float process(float density, float st) {
        float thresh = density * st;
        return (rng.uniform() < thresh) ? 1.f : 0.f;
    }
};

// ── Latch.ar — sample & hold: capture `in` when `trig` crosses > 0 ────────────
struct Latch {
    float held = 0.f;
    float prevTrig = 0.f;
    void reset() { held = 0.f; prevTrig = 0.f; }
    float process(float in, float trig) {
        if (trig > 0.f && prevTrig <= 0.f) held = in;
        prevTrig = trig;
        return held;
    }
};

// ── Lag.kr / VarLag — one-pole smoother toward the input over `time` seconds ──
struct Lag {
    float y = 0.f;
    bool primed = false;
    void reset() { y = 0.f; primed = false; }
    float process(float in, float time, float st) {
        if (!primed) { y = in; primed = true; }
        float b1 = (time > 1e-6f) ? std::exp(-st / time) : 0.f;
        y = in + b1 * (y - in);
        return y;
    }
};

// ── BPF / RLPF / LPF / HPF — SC's second-order filters over Rack's biquad ─────
// SC parameterises resonance by `rq` (reciprocal Q, i.e. bandwidth); Rack's
// biquad takes Q. These thin wrappers keep the SC call shape (freq, rq).
struct Biquad {
    rack::dsp::TBiquadFilter<float> f;
    void reset() { f.reset(); }
    float bpf(float in, float freqHz, float rq, float st) {
        f.setParameters(rack::dsp::TBiquadFilter<float>::BANDPASS,
                        clampFreq(freqHz, st), 1.f / std::max(rq, 1e-3f), 1.f);
        return f.process(in);
    }
    float rlpf(float in, float freqHz, float rq, float st) {
        f.setParameters(rack::dsp::TBiquadFilter<float>::LOWPASS,
                        clampFreq(freqHz, st), 1.f / std::max(rq, 1e-3f), 1.f);
        return f.process(in);
    }
    float lpf(float in, float freqHz, float st) {
        f.setParameters(rack::dsp::TBiquadFilter<float>::LOWPASS,
                        clampFreq(freqHz, st), M_SQRT1_2, 1.f);
        return f.process(in);
    }
    // SC BLowPass(in, freq, rq): resonant RBJ lowpass parameterised by rq = 1/Q
    float blowpass(float in, float freqHz, float rq, float st) {
        f.setParameters(rack::dsp::TBiquadFilter<float>::LOWPASS,
                        clampFreq(freqHz, st), 1.f / std::max(rq, 1e-3f), 1.f);
        return f.process(in);
    }
    // DFM1 approximation: resonant low- (type 0) or high-pass (type 1) biquad
    float dfm1(float in, float freqHz, float res, int type, float st) {
        auto t = (type == 1) ? rack::dsp::TBiquadFilter<float>::HIGHPASS
                             : rack::dsp::TBiquadFilter<float>::LOWPASS;
        f.setParameters(t, clampFreq(freqHz, st), 0.5f + res * 8.f, 1.f);
        return f.process(in);
    }
    float hpf(float in, float freqHz, float st) {
        f.setParameters(rack::dsp::TBiquadFilter<float>::HIGHPASS,
                        clampFreq(freqHz, st), M_SQRT1_2, 1.f);
        return f.process(in);
    }
private:
    static float clampFreq(float freqHz, float st) {
        // normalised cutoff must stay below Nyquist for a stable biquad
        return rack::clamp(freqHz * st, 1e-5f, 0.49f);
    }
};

// ── scalar helpers mirroring SC's math messages ──────────────────────────────
inline float linlin(float x, float inMin, float inMax, float outMin, float outMax) {
    return (x - inMin) / (inMax - inMin) * (outMax - outMin) + outMin;
}
inline float linexp(float x, float inMin, float inMax, float outMin, float outMax) {
    x = rack::clamp(x, std::min(inMin, inMax), std::max(inMin, inMax));
    return std::pow(outMax / outMin, (x - inMin) / (inMax - inMin)) * outMin;
}
inline float midicps(float note) { return 440.f * std::pow(2.f, (note - 69.f) / 12.f); }
inline float cpsmidi(float hz)   { return 69.f + 12.f * std::log2(std::max(hz, 1e-6f) / 440.f); }

// ── Pan2.ar — equal-power stereo pan; pos in [-1, 1], scaled by `level` ───────
inline void pan2(float in, float pos, float level, float& outL, float& outR) {
    float a = (rack::clamp(pos, -1.f, 1.f) * 0.5f + 0.5f) * (float)M_PI_2;
    outL = in * std::cos(a) * level;
    outR = in * std::sin(a) * level;
}

// ── SelectX.ar (two-element) — equal-power crossfade, frac in [0, 1] ──────────
inline float selectx(float frac, float a, float b) {
    float ang = rack::clamp(frac, 0.f, 1.f) * (float)M_PI_2;
    return a * std::cos(ang) + b * std::sin(ang);
}

// ── Impulse — single impulse at start (freq 0), else at `freq` Hz ─────────────
struct Impulse {
    float phase = 0.f;
    bool first = true;
    void reset() { phase = 0.f; first = true; }
    float process(float freq, float st) {
        if (first) { first = false; return 1.f; }
        if (freq <= 0.f) return 0.f;
        phase += freq * st;
        if (phase >= 1.f) { phase -= std::floor(phase); return 1.f; }
        return 0.f;
    }
};

// ── Trig.kr — output 1 for `dur` seconds after `in` crosses > 0 ───────────────
struct Trig {
    float timer = 0.f, prev = 0.f;
    void reset() { timer = 0.f; prev = 0.f; }
    float process(float in, float dur, float st) {
        if (in > 0.f && prev <= 0.f) timer = dur;
        prev = in;
        if (timer > 0.f) { timer -= st; return 1.f; }
        return 0.f;
    }
};

// ── TChoose.kr — pick a random array element on each trigger ──────────────────
struct TChoose {
    Rng rng; float val = 0.f, prev = 0.f;
    void reset(uint32_t seed) { rng.seed(seed); val = 0.f; prev = 0.f; }
    float process(float trig, const float* arr, int n) {
        if (trig > 0.f && prev <= 0.f) {
            int i = (int)(rng.uniform() * n); if (i >= n) i = n - 1;
            val = arr[i];
        }
        prev = trig;
        return val;
    }
};

// ── SinOscFB.ar — sine with phase feedback (a one-oscillator FM growl) ────────
struct SinOscFB {
    float phase = 0.f, last = 0.f;
    void reset(float ph = 0.f) { phase = ph; last = 0.f; }
    float process(float freq, float fb, float st) {
        float y = std::sin(kTwoPi * phase + fb * last);
        last = y;
        phase += freq * st;
        phase -= std::floor(phase);
        return y;
    }
};

// ── EnvGen — breakpoint envelope, retriggered on gate's rising edge ───────────
// Levels/times are latched at the trigger, so callers can pass live-modulated
// values (as SC does with Env.new([...],[...]) built from UGens).
struct BPEnv {
    enum { MAX = 3 };
    float lv[MAX + 1] = {}; float tm[MAX] = {}; int nseg = 0; bool sine = false;
    int seg = 999; float phase = 0.f, prevGate = 0.f, out = 0.f;
    void reset() { seg = 999; phase = 0.f; prevGate = 0.f; out = 0.f; }
    float process(float gate, const float* levels, const float* times, int segs,
                  bool sineCurve, float st) {
        if (gate > 0.f && prevGate <= 0.f) {
            segs = std::min(segs, (int)MAX);
            for (int i = 0; i <= segs; ++i) lv[i] = levels[i];
            for (int i = 0; i < segs; ++i) tm[i] = times[i];
            nseg = segs; sine = sineCurve; seg = 0; phase = 0.f;
        }
        prevGate = gate;
        if (seg >= nseg) { out = (nseg > 0) ? lv[nseg] : 0.f; return out; }
        float T = tm[seg];
        if (T <= 1e-6f) { out = lv[seg + 1]; ++seg; return out; }
        phase += st / T;
        float t = std::min(phase, 1.f);
        float shaped = sine ? (0.5f - 0.5f * std::cos((float)M_PI * t)) : t;
        out = lv[seg] + (lv[seg + 1] - lv[seg]) * shaped;
        if (phase >= 1.f) { phase -= 1.f; ++seg; }
        return out;
    }
};

// ── Env.asr with gate held high — a one-shot attack ramp to 1, then hold ──────
struct AttackEnv {
    float v = 0.f, atk = 1.f;
    void reset(float atkSec) { v = 0.f; atk = atkSec; }
    float process(float st) {
        v = std::min(v + ((atk > 1e-6f) ? st / atk : 1.f), 1.f);
        return v;
    }
};

// ── MoogFF.ar — 4-pole Moog-style ladder (cascaded one-poles + saturated fb) ──
struct MoogFF {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    void reset() { s0 = s1 = s2 = s3 = 0.f; }
    float process(float in, float cutoffHz, float res, float st) {
        float fc = rack::clamp(cutoffHz * st, 0.f, 0.45f);
        float g = 1.f - std::exp(-kTwoPi * fc);
        float x = std::tanh(in - res * s3);       // global resonant feedback
        s0 += g * (x  - s0);
        s1 += g * (s0 - s1);
        s2 += g * (s1 - s2);
        s3 += g * (s2 - s3);
        return s3;
    }
};

// ── delay line (power-of-two circular buffer; read-before-write) ──────────────
struct DelayLine {
    std::vector<float> buf; int mask = 0, w = 0;
    void init(float maxSec, float sr) {
        int need = (int)std::ceil(maxSec * sr) + 4;
        int n = 1; while (n < need) n <<= 1;
        buf.assign(n, 0.f); mask = n - 1; w = 0;
    }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; }
    void write(float x) { buf[w] = x; w = (w + 1) & mask; }
    float tapN(int d) { return buf[(w - d) & mask]; }                 // d >= 1
    float tapL(float ds) {
        int d = std::max((int)ds, 1); float f = ds - d;
        float a = buf[(w - d) & mask], b = buf[(w - d - 1) & mask];
        return a + f * (b - a);
    }
    float tapC(float ds) {
        int d = std::max((int)ds, 2); float f = ds - d;
        float ym1 = buf[(w - d + 1) & mask], y0 = buf[(w - d) & mask];
        float y1  = buf[(w - d - 1) & mask], y2 = buf[(w - d - 2) & mask];
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
};

// feedback coefficient for a comb/allpass to decay 60 dB over `decaySec`;
// a negative decaytime gives negative feedback of the same magnitude (SC)
inline float combFeedback(float delaySec, float decaySec) {
    if (decaySec == 0.f) return 0.f;
    float g = std::exp(-6.907755f * delaySec / std::fabs(decaySec));  // ln(0.001)
    return (decaySec < 0.f) ? -g : g;
}

// ── CombL / CombC — feedback comb, linear / cubic interpolated tap ────────────
struct CombL {
    DelayLine dl;
    float process(float x, float delaySamp, float g) {
        float d = dl.tapL(delaySamp); dl.write(x + g * d); return d;
    }
};
struct CombC {
    DelayLine dl;
    float process(float x, float delaySamp, float g) {
        float d = dl.tapC(delaySamp); dl.write(x + g * d); return d;
    }
};

// ── AllpassN — Schroeder allpass, non-interpolated tap ────────────────────────
struct AllpassN {
    DelayLine dl;
    float process(float x, int delaySamp, float g) {
        float d = dl.tapN(delaySamp);
        float w = x + g * d;
        dl.write(w);
        return d - g * w;
    }
};

// ── DelayN / DelayC — pure delay, non-interp / cubic (feed-forward) ───────────
struct DelayNode {
    DelayLine dl;
    float process(float x, int delaySamp) { float o = dl.tapN(delaySamp); dl.write(x); return o; }
};
struct DelayC {
    DelayLine dl;
    float process(float x, float delaySamp) { float o = dl.tapC(delaySamp); dl.write(x); return o; }
};

// ── SchroederReverb — the block shared by several dronecaster engines ─────────
//   DelayN(0.048) pre-delay → 7 slowly-modulated parallel CombL → 4 series
//   AllpassN. Built per channel with independent random taps for a wide image.
struct SchroederReverb {
    struct Chan {
        DelayNode pre;
        CombL comb[7]; LFNoise1 mod[7]; float rate[7] = {};
        AllpassN ap[4]; float apSec[4] = {};
        void init(uint32_t seed, float sr) {
            Rng rng; rng.seed(seed);
            pre.dl.init(0.06f, sr);
            for (int i = 0; i < 7; ++i) {
                comb[i].dl.init(0.12f, sr);
                rate[i] = rng.uniform() * 0.1f;
                mod[i].reset(seed + i * 131u + 7u);
            }
            for (int i = 0; i < 4; ++i) {
                ap[i].dl.init(0.06f, sr);
                apSec[i] = rng.uniform() * 0.05f + 0.001f;
            }
        }
        void reset() {
            pre.dl.reset();
            for (int i = 0; i < 7; ++i) comb[i].dl.reset();
            for (int i = 0; i < 4; ++i) ap[i].dl.reset();
        }
        float process(float x, float st, float sr) {
            float z = pre.process(x, (int)(0.048f * sr));
            float y = 0.f;
            for (int i = 0; i < 7; ++i) {
                float dt = 0.05f + 0.04f * mod[i].process(rate[i], st);
                y += comb[i].process(z, dt * sr, combFeedback(dt, 15.f));
            }
            for (int i = 0; i < 4; ++i)
                y = ap[i].process(y, (int)(apSec[i] * sr), combFeedback(apSec[i], 1.f));
            return y;
        }
    };
    Chan L, R;
    void init(uint32_t seed, float sr) { L.init(seed, sr); R.init(seed * 2246822519u + 1u, sr); }
    void reset() { L.reset(); R.reset(); }
    void process(float inL, float inR, float st, float sr, float& outL, float& outR) {
        outL = L.process(inL, st, sr); outR = R.process(inR, st, sr);
    }
};

// ── CombN — feedback comb, non-interpolated tap ──────────────────────────────
struct CombN {
    DelayLine dl;
    float process(float x, int delaySamp, float g) {
        float d = dl.tapN(delaySamp); dl.write(x + g * d); return d;
    }
};

// ── LFPulse.ar — non-bandlimited unipolar (0/1) pulse ────────────────────────
struct LFPulse {
    float phase = 0.f;
    void reset(float ph = 0.f) { phase = ph; }
    float process(float freq, float width, float st) {
        phase += freq * st; phase -= std::floor(phase);
        return (phase < width) ? 1.f : 0.f;
    }
};

// ── LFPar.ar — parabolic oscillator (cosine-like, made of parabola arcs) ─────
inline float lfparWave(float phase) {
    phase -= std::floor(phase);
    float x = 4.f * phase;
    if (x < 1.f) return 1.f - x * x;
    if (x < 3.f) { float y = x - 2.f; return y * y - 1.f; }
    float y = x - 4.f; return 1.f - y * y;
}
struct LFPar {
    float phase = 0.f;
    void reset(float ph = 0.f) { phase = ph; }
    float process(float freq, float st) {
        phase += freq * st; phase -= std::floor(phase);
        return lfparWave(phase);
    }
};

// ── Dust2.ar — random bipolar impulses at an average density (Hz) ────────────
struct Dust2 {
    Rng rng;
    void reset(uint32_t seed) { rng.seed(seed); }
    float process(float density, float st) {
        return (rng.uniform() < density * st) ? rng.bipolar() : 0.f;
    }
};

// ── Crackle.ar — chaotic "crackling" generator (2nd-order chaotic map) ───────
struct Crackle {
    float y1 = 0.3f, y2 = 0.f;
    void reset() { y1 = 0.3f; y2 = 0.f; }
    float process(float param) {
        float y0 = std::fabs(y1 * param - y2 - 0.05f);
        y2 = y1; y1 = y0; return y0;
    }
};

// ── softclip / Rotate2 helpers ───────────────────────────────────────────────
inline float softclip(float x) {
    float a = std::fabs(x);
    if (a <= 0.5f) return x;
    return (x < 0.f ? -1.f : 1.f) * (1.f - 0.25f / a);
}
inline void rotate2(float x, float y, float pos, float& outL, float& outR) {
    float a = pos * (float)M_PI, c = std::cos(a), s = std::sin(a);
    outL = x * c - y * s; outR = x * s + y * c;
}

// ── fold — reflect x back into [lo, hi] (wavefolder) ─────────────────────────
inline float foldOver(float x, float lo, float hi) {
    if (hi <= lo) return lo;
    float range = hi - lo, twice = 2.f * range;
    float y = x - lo;
    y -= twice * std::floor(y / twice);
    if (y > range) y = twice - y;
    return y + lo;
}

// ── Changed.kr — 1 when the input changes by more than `thresh`, else 0 ──────
struct Changed {
    float x1 = 0.f; bool first = true;
    void reset() { x1 = 0.f; first = true; }
    float process(float x, float thresh = 0.f) {
        float d = (first || std::fabs(x - x1) > thresh) ? 1.f : 0.f;
        x1 = x; first = false; return d;
    }
};

// ── LFSaw.ar — non-bandlimited bipolar ramp [-1, 1] ──────────────────────────
struct LFSaw {
    float phase = 0.f;
    void reset(float ph = 0.f) { phase = ph; }
    float process(float freq, float st) {
        phase += freq * st; phase -= std::floor(phase);
        return 2.f * phase - 1.f;
    }
};

// ── HenonC.ar — Hénon-map chaotic oscillator, interpolated at `freq` ─────────
struct HenonC {
    float phase = 0.f, x1 = 0.3f, x2 = 0.3f;
    void reset() { phase = 0.f; x1 = 0.3f; x2 = 0.3f; }
    float process(float freq, float a, float b, float st) {
        phase += std::fabs(freq) * st;
        while (phase >= 1.f) {
            phase -= 1.f;
            float x0 = 1.f - a * x1 * x1 + b * x2;
            x0 = rack::clamp(x0, -2.f, 2.f);
            x2 = x1; x1 = x0;
        }
        return x2 + (x1 - x2) * phase;      // linear interp between iterations
    }
};

// ── Amplitude.kr — envelope follower (attack/release smoothing of |x|) ───────
struct Amplitude {
    float env = 0.f;
    void reset() { env = 0.f; }
    float process(float x, float atkT, float relT, float st) {
        float a = std::fabs(x);
        float coef = (a > env) ? ((atkT > 0.f) ? std::exp(-st / atkT) : 0.f)
                               : ((relT > 0.f) ? std::exp(-st / relT) : 0.f);
        env = a + coef * (env - a);
        return env;
    }
};

// ── OnePole.ar — one-pole filter, y = (1-|c|)*x + c*y1 (c may be negative) ────
struct OnePole {
    float y1 = 0.f;
    void reset() { y1 = 0.f; }
    float process(float x, float coef) {
        y1 = (1.f - std::fabs(coef)) * x + coef * y1;
        return y1;
    }
};

// ── Balance2.ar — equal-power stereo balance of a stereo signal ──────────────
inline void balance2(float l, float r, float pos, float level, float& outL, float& outR) {
    float a = (rack::clamp(pos, -1.f, 1.f) * 0.5f + 0.5f) * (float)M_PI_2;
    outL = l * std::cos(a) * level;
    outR = r * std::sin(a) * level;
}

// ── Ringz — a resonator (ringing 2-pole BPF); the building block of Klank ─────
// freq = resonant frequency, decay = -60 dB ring time. Input is scaled by
// (1 - R^2) so the resonant gain stays ~unity regardless of decay.
struct Ringz {
    float y1 = 0.f, y2 = 0.f;
    void reset() { y1 = 0.f; y2 = 0.f; }
    float process(float in, float freq, float decay, float st) {
        float w = kTwoPi * rack::clamp(freq * st, 0.f, 0.49f);
        float R = std::exp(-6.907755f * st / std::max(decay, 1e-4f));
        float y0 = in * (1.f - R * R) + 2.f * R * std::cos(w) * y1 - R * R * y2;
        float out = y0 - y2;
        y2 = y1; y1 = y0;
        return out;
    }
};

// ── FreeVerb — Jezar's public-domain Freeverb (mono in → mono out) ───────────
// Faithful port of the classic algorithm: 8 parallel damped combs → 4 series
// allpasses. Comb/allpass lengths are the original 44.1 kHz tunings, scaled to
// the running sample rate. `mix` is the wet/dry balance (0 dry … 1 wet).
struct FreeVerbMono {
    static const int NC = 8, NA = 4;
    std::vector<float> cbuf[NC], abuf[NA];
    int cidx[NC] = {}, aidx[NA] = {}; float cfilt[NC] = {};
    void init(float sr) {
        static const int ctun[NC] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int atun[NA] = {556, 441, 341, 225};
        float scale = sr / 44100.f;
        for (int i = 0; i < NC; ++i) { cbuf[i].assign(std::max(1, (int)(ctun[i] * scale)), 0.f); cidx[i] = 0; cfilt[i] = 0.f; }
        for (int i = 0; i < NA; ++i) { abuf[i].assign(std::max(1, (int)(atun[i] * scale)), 0.f); aidx[i] = 0; }
    }
    void reset() {
        for (int i = 0; i < NC; ++i) { std::fill(cbuf[i].begin(), cbuf[i].end(), 0.f); cidx[i] = 0; cfilt[i] = 0.f; }
        for (int i = 0; i < NA; ++i) { std::fill(abuf[i].begin(), abuf[i].end(), 0.f); aidx[i] = 0; }
    }
    float process(float in, float mix, float room, float damp) {
        float feedback = room * 0.28f + 0.7f;
        float damp1 = damp * 0.4f, damp2 = 1.f - damp1;
        float input = in * 0.015f;              // fixedgain
        float out = 0.f;
        for (int i = 0; i < NC; ++i) {
            float o = cbuf[i][cidx[i]];
            cfilt[i] = o * damp2 + cfilt[i] * damp1;
            cbuf[i][cidx[i]] = input + cfilt[i] * feedback;
            if (++cidx[i] >= (int)cbuf[i].size()) cidx[i] = 0;
            out += o;
        }
        for (int i = 0; i < NA; ++i) {
            float bufout = abuf[i][aidx[i]];
            float o = -out + bufout;
            abuf[i][aidx[i]] = out + bufout * 0.5f;
            if (++aidx[i] >= (int)abuf[i].size()) aidx[i] = 0;
            out = o;
        }
        return in * (1.f - mix) + out * 3.f * mix;    // scalewet = 3
    }
};

// ── BrownNoise.ar — Brownian noise (integrated white, reflected at ±1) ───────
struct BrownNoise {
    Rng rng; float y = 0.f;
    void reset(uint32_t s) { rng.seed(s); y = 0.f; }
    float process() {
        y += rng.bipolar() * 0.125f;
        if (y > 1.f) y = 2.f - y; else if (y < -1.f) y = -2.f - y;
        return y;
    }
};

// ── BAllPass.ar — second-order RBJ allpass; rq = 1/Q ─────────────────────────
struct BAllPass {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void reset() { x1 = x2 = y1 = y2 = 0.f; }
    float process(float x, float freqHz, float rq, float st) {
        float w = kTwoPi * rack::clamp(freqHz * st, 1e-5f, 0.49f);
        float cw = std::cos(w), sw = std::sin(w), alpha = sw * rq * 0.5f;
        float a0 = 1.f + alpha;
        float b0 = (1.f - alpha) / a0, b1 = -2.f * cw / a0, b2 = (1.f + alpha) / a0;
        float a1 = -2.f * cw / a0, a2 = (1.f - alpha) / a0;
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// ── SVF.ar — TPT state-variable filter; returns a mix of lp/bp/hp outputs ────
struct SVF {
    float ic1 = 0.f, ic2 = 0.f;
    void reset() { ic1 = ic2 = 0.f; }
    float process(float v0, float cutoffHz, float res, float lpMix, float bpMix, float hpMix, float st) {
        float g = std::tan((float)M_PI * rack::clamp(cutoffHz * st, 1e-5f, 0.49f));
        float k = 2.f - 1.98f * rack::clamp(res, 0.f, 1.f);
        float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
        return lpMix * v2 + bpMix * v1 + hpMix * (v0 - k * v1 - v2);
    }
};

// ── SelectX.ar (N-element) — equal-power crossfade across an array ────────────
inline float selectxN(float sel, const float* arr, int n, bool wrap) {
    if (n <= 1) return (n == 1) ? arr[0] : 0.f;
    int i0 = (int)std::floor(sel), i1;
    float f = sel - i0;
    if (wrap) { i0 = ((i0 % n) + n) % n; i1 = (i0 + 1) % n; }
    else { i0 = (int)rack::clamp((float)i0, 0.f, (float)(n - 1)); i1 = std::min(i0 + 1, n - 1); }
    float ang = rack::clamp(f, 0.f, 1.f) * (float)M_PI_2;
    return arr[i0] * std::cos(ang) + arr[i1] * std::sin(ang);
}

// ── Splay.ar — spread N channels across the stereo field (equal power) ────────
// Matches SC Splay(array, spread, level, center, levelComp): channels are laid
// out evenly across [-1, 1], panned equal-power, summed, and (by default)
// amplitude-compensated by 1/sqrt(n).
inline void splay(const float* chans, int n, float spread, float center,
                  float& outL, float& outR, bool levelComp = true) {
    outL = 0.f; outR = 0.f;
    if (n <= 0) return;
    for (int k = 0; k < n; ++k) {
        float pos = (n == 1) ? center
                             : ((float)k / (n - 1) * 2.f - 1.f) * spread + center;
        pos = rack::clamp(pos, -1.f, 1.f);
        float angle = (pos * 0.5f + 0.5f) * (float)M_PI_2;   // 0..pi/2
        outL += chans[k] * std::cos(angle);
        outR += chans[k] * std::sin(angle);
    }
    if (levelComp) {
        float g = 1.f / std::sqrt((float)n);
        outL *= g; outR *= g;
    }
}

}  // namespace draen
