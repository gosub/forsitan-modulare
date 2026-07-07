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

namespace draen {

constexpr float kTwoPi = 6.28318530718f;

// ── tiny deterministic PRNG (xorshift32) ─────────────────────────────────────
// Local RNG keeps noise off Rack's shared random:: state (audio-thread safe).
struct Rng {
    uint32_t s = 0x2545F491u;
    void seed(uint32_t v) { s = v ? v : 0x2545F491u; }
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
