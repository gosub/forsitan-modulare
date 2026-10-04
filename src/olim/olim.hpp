// olim.hpp - the engine of olim: eight read heads over one long buffer.
//
// A clone of Olivia Artz Modular's Time Machine. The behaviour and the
// numbers are the hardware's, taken from its manual and its firmware, which
// the user made the source of truth; the code is written from
// doc/design/olim.md and shares nothing with the firmware's. Per channel,
// per sample:
//
//   buf[w] = in
//   wet    = sum of the eight heads, each times its gain
//   wet    = compress(wet - slowDC(wet), key = in + wet)
//   buf[w] = -limit(in + wet * feedback * norm)      inverted, every pass
//   out    = limit(wet + in * dry)
//
// norm is 1 / max(1, sum of head gains): the loop gain at feedback 1 is at
// most 1 however many sliders are up, while the output is the plain sum.
// Full scale 1.0 is 5 V; the compressor and both limiters act only above it.
//
// A head never slides. It crossfades linearly from one position to the next
// at 5 Hz, and starts the next fade the moment one ends, so a knob move is
// heard as a fade to the new place within 200 ms, never as a pitch bend.
// Its gain rides the same fade. Past feedback 1 each fade's rate is drawn at
// random, 5 +- (feedback - 1) Hz, separately per head and per channel: that
// is the blur, and why the stereo image comes apart as the loop runs hot.
//
// The caller hands over knob positions (0..1), CV in volts, slider values
// and VCA gains; every law from there on (dead zones, the spread curve, the
// quadratic or clocked TIME) is here, so the probe plays the same laws.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace olim {

// ---------------------------------------------------------------- constants

constexpr int kHeads = 8;
constexpr float kFullScale = 5.f;          // volts at 1.0
constexpr float kTimeMax = 8.f;            // seconds at the top of the knob
constexpr float kFadeHz = 5.f;             // head crossfades per second
constexpr float kFeedbackMax = 3.f;        // loop gain, knob plus CV
constexpr float kClockStale = 2.f;         // seconds without an edge = no clock
constexpr int kClockKnobSteps = 12;        // factors of two across the knob
constexpr float kClockCvSteps = 10.f;      // across -5..+5 V, 2 per volt
constexpr float kStepHysteresis = 1.f / 3.f;
constexpr int kControlInterval = 8;        // samples between control updates
// Margin past the longest delay so a TIME clamped to the memory never reads
// the sample being written.
constexpr float kBufferMargin = 0.025f;    // seconds

// Time constants of the smoothing, in seconds.
constexpr float kNormTau = 0.208f;
constexpr float kFeedbackTau = 0.0021f;
constexpr float kDryTau = 0.0208f;
constexpr float kLevelTau = 0.0208f;
constexpr float kDcTau = 2.08f;
constexpr float kLimiterRelease = 16.f;    // per second

// The soft output limiter's look-ahead: the output is late by this, and the
// gain glides down over it instead of jumping on the sample that is over.
constexpr float kLookahead = 0.001f;       // seconds
constexpr int kLookaheadMax = 512;         // samples, 1 ms up to 512 kHz

// The compressor on the wet signal, keyed on input plus wet.
constexpr float kCompRatio = 5.f;
constexpr float kCompAttack = 0.02f;
constexpr float kCompRelease = 0.2f;

// ------------------------------------------------------------------- laws

inline float clamp(float x, float lo, float hi) {
    return std::min(std::max(x, lo), hi);
}

// One pole coefficient for a time constant, per sample.
inline float poleCoef(float tau, float sr) {
    return 1.f - std::exp(-1.f / (tau * sr));
}

// A flat spot at noon: 0..0.45 -> 0..0.5, 0.45..0.55 -> 0.5, 0.55..1 -> 0.5..1.
inline float noonFlat(float x) {
    x = clamp(x, 0.f, 1.f);
    if (x < 0.45f) return x * (0.5f / 0.45f);
    if (x <= 0.55f) return 0.5f;
    return 0.5f + (x - 0.55f) * (0.5f / 0.45f);
}

// The FEEDBACK and SPREAD knobs go through it twice, which widens the flat
// spot to 0.405..0.595 of the travel: the arc on the FEEDBACK knob.
inline float noonFlat2(float x) { return noonFlat(noonFlat(x)); }
constexpr float kArcLow = 0.405f;
constexpr float kArcHigh = 0.595f;

inline float cvUnit(float volts) { return clamp(volts / kFullScale, -1.f, 1.f); }

// Loop gain from the knob and its CV: 0..2 on the knob, exactly 1 on the arc.
inline float feedbackGain(float knob, float cvVolts) {
    return clamp(2.f * noonFlat2(knob) + cvUnit(cvVolts), 0.f, kFeedbackMax);
}

// The knob position that sets loop gain g, the inverse of feedbackGain with
// no CV. Below the arc noonFlat2 is x / 0.81, so the gain is x / kArcLow;
// above it, 1 + (x - kArcHigh) / (1 - kArcHigh).
inline float feedbackKnob(float g) {
    g = clamp(g, 0.f, 2.f);
    if (g < 1.f) return g * kArcLow;
    if (g == 1.f) return 0.5f;
    return kArcHigh + (g - 1.f) * (1.f - kArcHigh);
}

inline float spreadAmount(float knob, float cvVolts) {
    return noonFlat2(knob) + cvUnit(cvVolts);
}

// Where head x = i / 8 sits, as a fraction of TIME. s in 0..1, noon even;
// below noon the heads crowd toward now, above it toward TIME. x = 1 maps
// to 1 whatever s is, so the last head is always TIME.
inline float spreadWeight(float x, float s) {
    s = clamp(s, 0.f, 1.f);
    float k = 1.f + 2.5f * std::fabs(2.f * s - 1.f);
    if (s < 0.5f) return std::pow(x, k);
    if (s > 0.5f) return 1.f - std::pow(1.f - x, k);
    return x;
}

// TIME without a clock: quadratic in the knob, halved per volt.
inline float freeTime(float knob, float cvVolts) {
    knob = clamp(knob, 0.f, 1.f);
    return kTimeMax * knob * knob * std::exp2(-kFullScale * cvUnit(cvVolts));
}

// A VCA input: 0..5 V to 0..1, clamped. Unpatched is 1.
inline float vcaGain(float volts) { return clamp(volts / kFullScale, 0.f, 1.f); }

// Rounds to an integer, but only moves once x is within a third of one: the
// clocked TIME knob steps without chattering at the edges.
struct StepHold {
    float value = 0.f;
    float process(float x) {
        float r = std::round(x);
        if (std::fabs(x - r) < kStepHysteresis) value = r;
        return value;
    }
};

// Clock period from rising edges: the running mean of the last interval and
// the new one. Gone after kClockStale seconds without an edge; the first
// edge after that only starts timing.
struct ClockMeter {
    float sr = 48000.f;
    bool prev = false;
    bool armed = false;
    int64_t since = 0;
    double interval = 0.0;     // samples, 0 = none yet

    void setSampleRate(float s) { sr = s; }
    bool stale() const { return since > (int64_t)(kClockStale * sr); }
    void process(bool high) {
        since++;
        if (high && !prev) {
            if (!armed || stale()) interval = 0.0;
            else if (interval <= 0.0) interval = (double)since;
            else interval = 0.5 * (interval + (double)since);
            armed = true;
            since = 0;
        }
        prev = high;
    }
    // Seconds, or 0 without a running clock.
    float period() const {
        if (!armed || stale() || interval <= 0.0) return 0.f;
        return (float)(interval / sr);
    }
};

// xorshift32, one per channel, for the blur.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x9e3779b9u) : s(seed ? seed : 1u) {}
    float uniform() {           // -1..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s >> 8) * (2.f / 16777216.f) - 1.f;
    }
};

// ------------------------------------------------------------------ heads

struct Head {
    size_t delayA = 0, delayB = 0;   // samples
    float gainA = 0.f, gainB = 0.f;
    float phase = 1.f;               // 0..1 across a fade
    float rate = 0.f;                // phase per sample
    float level = 0.f;               // |output| before the gain, smoothed

    // Reads the buffer and advances the fade. A finished fade starts the next
    // toward (target, targetGain) at once, at fadeBase + fadeSpread * [-1, 1]
    // phase per sample.
    inline float process(const float* buf, size_t size, size_t w,
                         size_t target, float targetGain,
                         float fadeBase, float fadeSpread, Rng& rng,
                         float levelCoef) {
        if (phase >= 1.f) {
            delayA = delayB; gainA = gainB;
            delayB = target; gainB = targetGain;
            phase = 0.f;
            rate = fadeBase;
            if (fadeSpread > 0.f) rate += fadeSpread * rng.uniform();
        }
        size_t ia = w >= delayA ? w - delayA : w + size - delayA;
        size_t ib = w >= delayB ? w - delayB : w + size - delayB;
        float a = buf[ia], b = buf[ib];
        float x = a + (b - a) * phase;
        float g = gainA + (gainB - gainA) * phase;
        phase = std::min(phase + rate, 1.f);
        level += (std::fabs(x) - level) * levelCoef;
        return x * g;
    }
};

// ---------------------------------------------------------- soft limiter

// The output limiter without the hardware's instant attack, which clips a
// peak in the sample it arrives and crackles on anything that crosses full
// scale often (a 5 V sine and its echo drifting in and out of phase). The
// gain each sample needs, 1 / max(1, |x|), is held at its minimum over the
// look-ahead window, released at the hardware's rate, then averaged over
// the same window: the gain is down to what a peak needs by the time the
// peak, delayed by the window, reaches it, and it gets there in a ramp.
struct SoftLimiter {
    int n = 48;                    // window, samples
    int pos = 0;
    float x[kLookaheadMax] = {};   // the signal, delayed n - 1
    float need[kLookaheadMax] = {};
    float held[kLookaheadMax] = {};
    double sum = 0.;               // of held[], for the average
    float env = 1.f;

    void setSampleRate(float sr) {
        n = std::max(1, std::min(kLookaheadMax, (int)std::lround(kLookahead * sr)));
        reset();
    }
    void reset() {
        pos = 0;
        for (int i = 0; i < kLookaheadMax; i++) {
            x[i] = 0.f;
            need[i] = held[i] = 1.f;
        }
        sum = n;
        env = 1.f;
    }
    inline float process(float in, float release) {
        need[pos] = 1.f / std::max(std::fabs(in), 1.f);
        float lo = 1.f;
        for (int i = 0; i < n; i++) lo = std::min(lo, need[i]);
        env = lo < env ? lo : env + (lo - env) * release;
        sum += env - held[pos];
        held[pos] = env;
        // the oldest sample in the window is n - 1 behind
        int oldest = pos + 1 >= n ? 0 : pos + 1;
        float out = x[oldest];
        x[pos] = in;
        out = n > 1 ? out : in;
        pos = oldest;
        float g = (float)(sum / n);
        return out * std::min(g, 1.f);
    }
};

// ---------------------------------------------------------------- channel

struct Channel {
    float* buf = nullptr;
    size_t size = 0;
    size_t w = 0;
    Head heads[kHeads];
    Rng rng;

    float norm = 1.f, fb = 0.f, dry = 0.f;
    float dc = 0.f;
    float compEnv = 0.f, compDb = 0.f, compGain = 1.f;
    float limFb = 1.f, limOut = 1.f;
    SoftLimiter softOut;
    float inLevel = 0.f;

    static inline float limit(float x, float& g, float release) {
        float target = 1.f / std::max(std::fabs(x), 1.f);
        if (target < g) g = target;
        else g += (target - g) * release;
        return x * g;
    }
};

// ----------------------------------------------------------------- engine

// Everything the module reads, already in engine units except the CVs,
// which are volts.
struct Controls {
    float time = 0.5f, spread = 0.5f, feedback = 0.f;   // knobs 0..1
    float timeCv = 0.f, spreadCv = 0.f, feedbackCv = 0.f;
    float dry = 1.f;                                    // slider * VCA
    float gain[kHeads] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f};
};

struct Engine {
    float sr = 48000.f;
    Channel ch[2];
    ClockMeter clock;
    StepHold knobStep, cvStep;
    int tick = 0;
    // The output limiter: soft (look-ahead, 1 ms late) or the hardware's,
    // instant. The loop's limiter is the hardware's either way.
    bool softLimit = true;

    // derived per control tick
    float timeSec = 0.f;
    float fbTarget = 0.f, dryTarget = 1.f, normTarget = 1.f, blur = 0.f;
    size_t target[kHeads] = {};
    float gainTarget[kHeads] = {};

    // per-sample coefficients
    float cNorm = 0.f, cFb = 0.f, cDry = 0.f, cLevel = 0.f, cDc = 0.f;
    float cLim = 0.f, compAtk = 0.f, compRel = 0.f, compAtk2 = 0.f;

    Engine() {
        ch[0].rng = Rng(0x13579bdfu);
        ch[1].rng = Rng(0x2468ace1u);
        setSampleRate(48000.f);
    }

    void setSampleRate(float s) {
        sr = s;
        clock.setSampleRate(s);
        cNorm = poleCoef(kNormTau, s);
        cFb = poleCoef(kFeedbackTau, s);
        cDry = poleCoef(kDryTau, s);
        cLevel = poleCoef(kLevelTau, s);
        cDc = poleCoef(kDcTau, s);
        cLim = std::min(kLimiterRelease / s, 1.f);
        compAtk = std::exp(-1.f / (s * kCompAttack));
        compRel = std::exp(-1.f / (s * kCompRelease));
        compAtk2 = std::exp(-2.f / (s * kCompAttack));
        for (Channel& c : ch) c.softOut.setSampleRate(s);
        tick = 0;
    }

    // Samples a buffer must hold for `seconds` of memory at `rate`.
    static size_t bufferSamples(float seconds, float rate) {
        return (size_t)((seconds + kBufferMargin) * rate) + 1;
    }

    // Hands the engine its two buffers (zeroed, `size` samples each), or
    // none: without buffers it passes the dry signal only.
    void attach(float* left, float* right, size_t size) {
        ch[0].buf = left;
        ch[1].buf = right;
        for (Channel& c : ch) {
            c.size = left && right ? size : 0;
            c.w = 0;
            for (Head& h : c.heads) h = Head();
        }
    }

    bool ready() const { return ch[0].buf && ch[1].buf && ch[0].size > 2; }

    // Longest TIME the buffer holds, in seconds.
    float memorySeconds() const {
        if (!ready()) return 0.f;
        return (float)(ch[0].size - 1) / sr - kBufferMargin;
    }

    bool clocked() const { return clock.period() > 0.f; }

    // Samples the output runs behind the input: the soft limiter's window.
    int outputLatency() const { return softLimit ? ch[0].softOut.n - 1 : 0; }

    // Recomputes TIME, the head targets and the gains.
    void update(const Controls& c) {
        float mem = memorySeconds();
        float period = clock.period();
        float t;
        if (period > 0.f) {
            float steps = knobStep.process((1.f - clamp(c.time, 0.f, 1.f)) * kClockKnobSteps)
                        + cvStep.process(cvUnit(c.timeCv) * kClockCvSteps);
            t = period * std::exp2(0.5f * kClockKnobSteps - steps);
            while (t > mem && t > 0.f && mem > 0.f) t *= 0.5f;
        } else {
            t = freeTime(c.time, c.timeCv);
        }
        timeSec = clamp(t, 0.f, mem);

        float fbk = feedbackGain(c.feedback, c.feedbackCv);
        blur = std::max(0.f, fbk - 1.f);
        fbTarget = fbk;
        dryTarget = std::max(c.dry, 0.f);

        float s = spreadAmount(c.spread, c.spreadCv);
        float sum = 0.f;
        size_t maxDelay = ready() ? ch[0].size - 1 : 0;
        for (int i = 0; i < kHeads; i++) {
            float d = spreadWeight((float)(i + 1) / kHeads, s) * timeSec;
            target[i] = std::min((size_t)(d * sr), maxDelay);
            gainTarget[i] = clamp(c.gain[i], 0.f, 1.f);
            sum += gainTarget[i];
        }
        normTarget = 1.f / std::max(1.f, sum);
    }

    // One sample. Inputs and outputs in volts.
    inline void process(const Controls& c, bool clockHigh,
                        float inL, float inR, float& outL, float& outR) {
        clock.process(clockHigh);
        if (tick-- <= 0) {
            update(c);
            tick = kControlInterval - 1;
        }
        const float in[2] = {inL / kFullScale, inR / kFullScale};
        float out[2];
        const float fadeBase = kFadeHz / sr;
        const float fadeSpread = blur / sr;
        for (int k = 0; k < 2; k++) {
            Channel& h = ch[k];
            h.dry += (dryTarget - h.dry) * cDry;
            h.inLevel += (std::fabs(in[k]) - h.inLevel) * cLevel;
            if (!ready()) {
                out[k] = limitOut(h, in[k] * h.dry);
                continue;
            }
            h.norm += (normTarget - h.norm) * cNorm;
            h.fb += (fbTarget - h.fb) * cFb;

            h.buf[h.w] = in[k];
            float wet = 0.f;
            for (int i = 0; i < kHeads; i++)
                wet += h.heads[i].process(h.buf, h.size, h.w, target[i],
                                          gainTarget[i], fadeBase, fadeSpread,
                                          h.rng, cLevel);
            h.dc += (wet - h.dc) * cDc;
            wet -= h.dc;
            wet *= compress(h, in[k] + wet);

            h.buf[h.w] = -Channel::limit(in[k] + wet * h.fb * h.norm, h.limFb, cLim);
            out[k] = limitOut(h, wet + in[k] * h.dry);
            if (++h.w >= h.size) h.w = 0;
        }
        outL = out[0] * kFullScale;
        outR = out[1] * kFullScale;
    }

    // Both run all the time, so switching between them from the menu meets
    // a limiter already in step with the signal rather than a stale one.
    inline float limitOut(Channel& h, float x) {
        float soft = h.softOut.process(x, cLim);
        float hard = Channel::limit(x, h.limOut, cLim);
        return softLimit ? soft : hard;
    }

    // Gain for the wet signal from the key: a peak follower, then 5:1 above
    // 0 dBFS, the reduction smoothed in dB. Unity until the key passes full
    // scale, which it never does in the sound-on-sound zone.
    inline float compress(Channel& h, float key) {
        float a = std::fabs(key);
        float c = h.compEnv > a ? compRel : compAtk;
        h.compEnv = h.compEnv * c + (1.f - c) * a;
        float over = h.compEnv > 1.f ? 20.f * std::log10(h.compEnv) : 0.f;
        h.compDb = compAtk2 * h.compDb
                 + (1.f - compAtk2) * (1.f / kCompRatio - 1.f) * over;
        if (h.compDb > -1e-5f) return 1.f;
        return std::pow(10.f, 0.05f * h.compDb);
    }

    // Light levels, 0..1 of full scale: the heads before their sliders, and
    // the input for the dry slider. The louder channel of the two.
    float headLevel(int i) const {
        return std::max(ch[0].heads[i].level, ch[1].heads[i].level);
    }
    float inputLevel() const { return std::max(ch[0].inLevel, ch[1].inLevel); }
};

}  // namespace olim
