#pragma once
// inedia_engine.hpp — starved-clock DSP core (Rack-free, header-only)
//
// Simulates a digital effect running off a dying battery, after Nathan Ho,
// "Low battery audio effects" (nathan.ho.name/posts/low-battery-audio-effects).
//
// A dead battery has high internal resistance, so its terminal voltage sags
// under load. In a cheap digital device the master clock is an RC or ring
// oscillator whose frequency tracks Vcc, so the sag *detunes the clock*. The
// audio itself is the load (speaker current), which closes a feedback loop:
//
//   loud output -> more current -> more sag -> slower clock -> pitch/time droop
//
// The whole point is that this is one mechanism, not a per-effect hack: the
// inner DSP runs on a variable clock and every algorithm behind it inherits
// the behaviour. Hence the split below:
//
//   StarvedClock  drives an arbitrary inner engine at a rail-dependent rate
//   Sag           the load model that computes the rail from the output
//   InnerDelay    the first inner engine we hang off it (a chip delay)
//
// Three details that are not cosmetic:
//
// 1. DC BLOCK ON THE LOAD. Without it a sustained loud input pins the rail at
//    its floor and the clock parks there: the effect "sticks" instead of
//    drooping and recovering. The highpass is what makes sag transient, and
//    the droop-and-bloom motion is the entire musical payload.
//
// 2. THE OUTPUT IS ZERO-ORDER HELD, and the input is sampled only on inner
//    ticks. That is what the hardware does, and the resulting aliasing and
//    imaging are part of the sound. Do not "fix" it with a tracking
//    anti-imaging filter. The real device has a FIXED reconstruction filter
//    (kOutputCutoff), which is why a starved chip gets gritty rather than
//    just dull.
//
// 3. DELAY TIME IS FIXED IN INNER SAMPLES, not in seconds. A chip delay is
//    N clock cycles of a bucket brigade or a RAM pointer, so slowing the clock
//    lengthens the delay and drops the pitch of what is already in it. That
//    coupling is why this reads as "tape" rather than "vibrato".

#include <algorithm>
#include <cmath>
#include <vector>

namespace inedia {

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

// The clock never stops and never overclocks: below the floor the inner engine
// starves so hard it stops producing output (and the sag loop, sensing silence,
// would stall); above 1 we would be inventing headroom the battery cannot give.
constexpr float kRatioFloor = 0.25f;
constexpr float kRatioCeil  = 1.0f;

// Fixed output reconstruction filter, in Hz at the HOST rate. Stands in for the
// device's analog output stage, which knows nothing about the clock sagging. A
// cheap toy's filter is lazy: one gentle pole, so the ZERO-ORDER-HOLD imaging
// (the staircase's spectral copies at multiples of the inner rate) leaks through
// as grit rather than being scrubbed clean. That leakage is most of why a
// starved chip sounds broken instead of merely detuned.
constexpr float kOutputCutoff = 7500.f;

// The chip's word length on a FRESH battery. It is a cheap converter, so this is
// already lossy; starving drops the DAC reference and sheds a few more bits.
constexpr float kFreshBits = 12.f;
constexpr float kStarvedBits = 8.f;

// ---------------------------------------------------------------------------
// Sag — load model: output amplitude in, supply rail out.
//
// rectify -> RC lowpass (the reservoir cap) -> DC block -> scaled by starve.
// ---------------------------------------------------------------------------
struct Sag {
    float rc = 0.f;      // smoothed rectified load
    float dc = 0.f;      // DC-blocker state
    float kRC = 0.01f;   // reservoir time constant
    float kDC = 0.0002f; // DC-block corner

    void setRates(float sampleRate, float rcMs, float dcHz) {
        kRC = clampf(1.f - std::exp(-1.f / (0.001f * rcMs * sampleRate)), 0.f, 1.f);
        kDC = clampf(1.f - std::exp(-2.f * M_PI * dcHz / sampleRate), 0.f, 1.f);
    }

    void reset() { rc = dc = 0.f; }

    // Returns the transient (zero-mean) component of the load.
    float process(float out) {
        rc += (std::fabs(out) - rc) * kRC;
        dc += (rc - dc) * kDC;
        return rc - dc;
    }
};

// ---------------------------------------------------------------------------
// InnerDelay — the effect behind the clock. Runs entirely in inner-clock time
// and has no idea the clock is moving.
// ---------------------------------------------------------------------------
struct InnerDelay {
    std::vector<float> buf;
    int   writeIdx = 0;
    float delaySamples = 4800.f; // in INNER samples
    float feedback = 0.5f;
    float headroom = 1.f;        // shrinks as the rail drops (analog starve)
    float bias = 0.f;            // clip asymmetry, grows as the rail drops
    float quantStep = 0.f;       // DAC step; 0 = off. Set from the rail.
    float damp = 0.f;            // one-pole in the feedback path
    float dampState = 0.f;

    void init(int maxSamples) {
        buf.assign((size_t)std::max(maxSamples, 2), 0.f);
        writeIdx = 0;
        dampState = 0.f;
    }

    void reset() {
        std::fill(buf.begin(), buf.end(), 0.f);
        writeIdx = 0;
        dampState = 0.f;
    }

    // Cubic (Catmull-Rom) read, so slewing the delay time does not sound like
    // linear interpolation's lowpass wobble.
    float read(float d) const {
        int n = (int)buf.size();
        d = clampf(d, 1.f, (float)(n - 3));
        float pos = (float)writeIdx - d;
        while (pos < 0.f) pos += (float)n;
        int i1 = (int)pos;
        float f = pos - (float)i1;
        int i0 = (i1 + n - 1) % n, i2 = (i1 + 1) % n, i3 = (i1 + 2) % n;
        float y0 = buf[i0], y1 = buf[i1], y2 = buf[i2], y3 = buf[i3];
        float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        float b = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
        float c = -0.5f * y0 + 0.5f * y2;
        return ((a * f + b) * f + c) * f + y1;
    }

    // One inner clock cycle.
    float tick(float in) {
        float r = read(delaySamples);
        dampState += (r - dampState) * (1.f - damp);
        float w = in + feedback * dampState;
        // Starved supply = collapsed headroom: the rail itself is the clipper,
        // and its two halves sag unequally, so the clip goes asymmetric (bias).
        float h = std::max(headroom, 0.05f);
        w = h * std::tanh((w + bias) / h) - h * std::tanh(bias / h);
        // Cheap converter: quantize what actually lands in memory. The step
        // widens as the rail drops, so the noise floor rises with the load.
        if (quantStep > 0.f) w = std::round(w / quantStep) * quantStep;
        buf[(size_t)writeIdx] = w;
        writeIdx = (writeIdx + 1) % (int)buf.size();
        return r;
    }
};

// ---------------------------------------------------------------------------
// StarvedClock — ties it together. One call per HOST sample.
// ---------------------------------------------------------------------------
struct StarvedClock {
    InnerDelay inner;
    Sag sag;

    float phase = 0.f;   // fractional inner-clock accumulator
    float held = 0.f;    // zero-order-hold output register
    float lp = 0.f;      // fixed one-pole reconstruction filter
    float kOut = 0.3f;
    float ratio = 1.f;   // current clock rate, relative to the FRESH-battery rate

    // The chip's nominal clock relative to the host rate, on a fresh battery.
    // Below 1 the device already aliases and images before any starving: it is a
    // cheap toy, not a studio converter. This is the knob that stops the effect
    // from sounding like a clean modulated delay.
    float baseRatio = 0.5f;

    // Macro controls
    float droop  = 0.f;  // static rate loss from a flat battery   (0..1)
    float starve = 0.f;  // depth of the load -> rate feedback
    float sampleRate = 48000.f;

    void init(float sr, int maxInnerSamples) {
        sampleRate = sr;
        inner.init(maxInnerSamples);
        sag.setRates(sr, 12.f, 3.f);
        kOut = clampf(1.f - std::exp(-2.f * M_PI * kOutputCutoff / sr), 0.f, 1.f);
        phase = 0.f;
        held = lp = 0.f;
        ratio = 1.f;
    }

    void reset() {
        inner.reset();
        sag.reset();
        phase = 0.f;
        held = lp = 0.f;
        ratio = 1.f;
    }

    float process(float in) {
        // The rail follows the PREVIOUS output: the loop needs a delay, and the
        // hardware has one too (the cap cannot respond instantly).
        float load = sag.process(held);
        ratio = clampf(1.f - droop - starve * load, kRatioFloor, kRatioCeil);

        // The rail also squeezes the analog stage: loud passages slow down AND
        // crush AND lose bits. sag01 is 0 on a fresh full clock, 1 fully starved.
        float sag01 = clampf((1.f - ratio) / (1.f - kRatioFloor), 0.f, 1.f);
        inner.headroom = clampf(1.f - 0.75f * sag01, 0.05f, 1.f);
        inner.bias = 0.25f * sag01;
        float bits = kFreshBits - (kFreshBits - kStarvedBits) * sag01;
        inner.quantStep = 2.f / std::pow(2.f, bits);

        phase += baseRatio * ratio;
        if (phase >= 1.f) {
            phase -= 1.f;
            held = inner.tick(in); // input is sampled ONLY here: aliasing is real
        }

        // Lazy 1-pole reconstruction, wet path only: lets the ZOH imaging leak.
        lp += (held - lp) * kOut;
        return lp;
    }
};

} // namespace inedia
