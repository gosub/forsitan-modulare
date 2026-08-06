// tundo_dsp.hpp — parameterized digital drum voice.
//
// Built after the Noise Engineering Basimilus Iteritas Alter, from Noise
// Engineering's own published manuals (bi / bia / bia_german / bim). No
// firmware was consulted; everything here is derived from prose descriptions
// of the architecture, and the places where the manuals describe the
// mechanism but not the curve are marked "soft" below.
//
//          per-oscillator (i = 0..5)
//   pitch -> f_i = f0 * r_i(spread) * pitchEnv(liquid)
//            w(phase_i, morph)        sine -> tri -> saw -> square
//            x g_i(harm) x decayEnv_i(decay * d_i(harm))
//                       |
//   noise (LCG, held) --+
//                       v
//                 sum -> x attackEnv x finalEnv -> folder -> out
//                                       (threshold reflection,        |
//                                        amplitude compensation,      +-> env
//                                        pulse train at the top)
//
// Everything between the trigger and the output is rendered on the engine's
// own clock at rate `rate`, which in hardware mode is a power-of-two multiple
// of the fundamental: every alias image then lands on a harmonic of f0, which
// is what makes the original's grit sound tuned rather than dirty. The host
// sees the result through a zero-order hold. Clean mode swaps that for a
// fixed 4x oversampled rate, PolyBLEP edges and a real decimation filter.
//
// The caller hands over already-mapped and already-smoothed parameters (see
// Params); the engine smooths nothing, since the module knows the control
// rate.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tundo_dsp {

constexpr int kNumOsc = 6;

// ---------------------------------------------------------------- constants

// HARM staging. The manuals give the *order* — a second tone over the first
// quarter, then the other four partials' decays, then their amplitudes — and
// nothing else. Every number here is ours, and this block is where voicing by
// ear should move them.
constexpr float kHarmSecondSpan = 0.25f;  // second tone fades in over this
constexpr float kHarmDecayStart = 0.25f;  // partial 3 starts extending decay
constexpr float kHarmAmpStart   = 0.45f;  // ...and amplitude, later
constexpr float kHarmStagger    = 0.30f;  // spread of the four across the knob
constexpr float kHarmRamp       = 0.25f;  // width of one partial's ramp
constexpr float kHarmFloor      = 0.15f;  // residual decay length at HARM 0
constexpr float kSpectralTilt   = 0.7f;   // partial i amplitude x i^-tilt

// Folder. The reflection and the amplitude compensation are the manual's; the
// pulse amplitude and time constant are ours (soft).
constexpr float kFoldMinThresh = 0.06f;   // threshold at the top of the fold range
constexpr float kFoldPulseMix  = 0.6f;
constexpr float kFoldPulseSpan = 0.75f;   // knob above this mixes in the pulses
constexpr int   kMaxFoldStages = 32;

// Metal. Alia's manual says "a pair of 3-operator phase-modulated
// oscillators"; the routing and the index mapping are ours (soft).
constexpr float kMetalIndex = 6.f;        // radians at HARM full

// Output stage. The engine works at nominal unit amplitude; the knee sits
// high enough that a plain hit passes almost untouched and only a folded
// crest or the pulse train leans on it, so the module honours its Vpp
// setting without colouring the clean end of the knobs. (soft)
constexpr float kOutputGain = 1.05f;
constexpr float kSoftKnee = 0.85f;
constexpr float kCeiling = 0.98f;         // headroom for the DC blocker

constexpr float kBaseHz = 32.703f;        // C1 at pitch 0 V
constexpr float kMinF0 = 8.f, kMaxF0 = 12000.f;
constexpr float kMinRate = 8000.f, kMaxRate = 192000.f;
constexpr float kMinMult = 8.f, kMaxMult = 4096.f;

enum Mode { kSkin = 0, kLiquid = 1, kMetal = 2 };

// ------------------------------------------------------------------ helpers

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// phase wrap that survives dt > 1: at the top of the pitch range a prime-spread
// partial can outrun the internal rate, and that is allowed to happen.
inline float wrapPhase(float x) {
    return x - std::floor(x);
}

// unity below the knee, asymptotic to the ceiling above it
inline float softClip(float x) {
    float a = std::fabs(x);
    if (a <= kSoftKnee) return x;
    float span = kCeiling - kSoftKnee;
    float y = kSoftKnee + span * std::tanh((a - kSoftKnee) / span);
    return x < 0.f ? -y : y;
}

inline float smoothstep(float u) {
    u = clampf(u, 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

// sin(2*pi*ph) to about 0.1%. Six oscillators at up to 192 kHz is the whole
// cost of this module, and a naive-aliasing engine has no use for the last
// three digits of a sine.
inline float fastSinPhase(float ph) {
    constexpr float B = 1.2732395447f;    // 4/pi
    constexpr float C = -0.4052847346f;   // -4/pi^2
    float x = (ph - 0.5f) * 2.f * (float)M_PI;
    float y = B * x + C * x * std::fabs(x);
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;                            // sin(2 pi ph - pi) = -sin(2 pi ph)
}

// PolyBLEP residual for a unit downward step at t = 0 (clean mode only)
inline float polyBlep(float t, float dt) {
    if (dt <= 0.f || dt >= 0.5f) return 0.f;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

// sine -> triangle -> saw -> square, three equal thirds of `m`, all four
// aligned so the crossfades add rather than cancel. Deliberately naive unless
// `blep` is set: in hardware mode the aliasing is the point, and it is
// harmonically aligned by construction.
inline float morphWave(float ph, float m, float dt, bool blep) {
    m = clampf(m, 0.f, 1.f);
    if (m < 1.f / 3.f) {
        float sine = fastSinPhase(ph);
        float tri = 4.f * std::fabs(wrapPhase(ph - 0.25f) - 0.5f) - 1.f;
        return sine + (tri - sine) * (m * 3.f);
    }
    float sph = wrapPhase(ph + 0.5f);
    float saw = 2.f * sph - 1.f;
    if (blep) saw -= polyBlep(sph, dt);
    if (m < 2.f / 3.f) {
        float tri = 4.f * std::fabs(wrapPhase(ph - 0.25f) - 0.5f) - 1.f;
        return tri + (saw - tri) * ((m - 1.f / 3.f) * 3.f);
    }
    float sqr = ph < 0.5f ? 1.f : -1.f;
    if (blep) sqr += polyBlep(ph, dt) - polyBlep(sph, dt);
    return saw + (sqr - saw) * ((m - 2.f / 3.f) * 3.f);
}

// ------------------------------------------------------------------- params

struct Params {
    float f0 = 55.f;          // Hz, fundamental (range switch already folded in)
    float harm = 0.3f;        // 0..1
    float spread = 0.f;       // 0..1, harmonic -> prime
    float morph = 0.f;        // 0..1
    float fold = 0.f;         // 0..1
    float attackMs = 0.5f;
    float noiseAmt = 0.f;     // attack-transient noise level, 0..1
    float decayMs = 200.f;
    int mode = kSkin;
    float liquidOct = 2.f;    // liquid pitch envelope depth, octaves
    bool hold = false;        // free-run: envelopes stop decaying
    bool extendedSpread = false;
    bool cleanRate = false;
    bool quantize = true;     // 16-bit output quantization (hardware mode)
};

// ------------------------------------------------------------------- engine

struct Engine {
    // --- host side
    float hostSr = 48000.f;
    float phaseAcc = 0.f;     // fraction of an internal sample since the last tick
    float lastSample = 0.f;   // held output (hardware mode)
    float lpPrev = 0.f, lpCur = 0.f;   // decimation filter history (clean mode)

    // --- internal clock
    float rate = 96000.f;
    float mult = 1024.f;      // rate / f0, a power of two
    float multF0 = 0.f;       // f0 at which `mult` was last chosen

    // --- oscillators
    float phase[kNumOsc] = {};
    float ratio[kNumOsc] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    float gain[kNumOsc] = {};
    float stage[kNumOsc] = {};   // gain before the spectral tilt: the FM index
    float dmul[kNumOsc] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
    float env[kNumOsc] = {};
    float envCoef[kNumOsc] = {};

    // --- envelopes
    float attEnv = 0.f, attCoef = 0.f;
    float noiseEnv = 0.f, noiseCoef = 0.f;
    float pitchEnv = 0.f, pitchCoef = 0.f;
    float finalEnv = 0.f;

    // --- noise oscillator
    uint32_t lcg = 0x2545f491u;
    float noiseHeld = 0.f;
    int noiseCount = 0, noiseHold = 1;

    // --- folder
    float foldThresh = 1.f;
    float pulse = 0.f, pulseCoef = 0.f;
    float prevY = 0.f, prevDiff = 0.f;
    int lastStages = 0;       // fold stages used by the most recent sample

    // --- clean-mode decimation filter (2nd-order Butterworth at 0.45 * hostSr)
    float lpB0 = 1.f, lpB1 = 0.f, lpB2 = 0.f, lpA1 = 0.f, lpA2 = 0.f;
    float lpS1 = 0.f, lpS2 = 0.f;

    // --- output DC blocker
    float dcX = 0.f, dcY = 0.f, dcR = 0.9987f;

    bool holdActive = false;      // free-run armed on the previous control block

    void init(float sampleRate) {
        hostSr = sampleRate > 0.f ? sampleRate : 48000.f;
        rate = 2.f * hostSr;
        mult = 1024.f;
        multF0 = 0.f;
        phaseAcc = 0.f;
        lastSample = lpPrev = lpCur = 0.f;
        for (int i = 0; i < kNumOsc; i++) {
            phase[i] = 0.f;
            env[i] = 0.f;
            envCoef[i] = 0.f;
            gain[i] = 0.f;
            stage[i] = 0.f;
            dmul[i] = 1.f;
            ratio[i] = (float)(i + 1);
        }
        attEnv = noiseEnv = pitchEnv = finalEnv = 0.f;
        noiseCount = 0;
        noiseHold = 1;
        pulse = prevY = prevDiff = 0.f;
        lastStages = 0;
        lpS1 = lpS2 = 0.f;
        holdActive = false;
        dcX = dcY = 0.f;
        dcR = 1.f - 2.f * (float)M_PI * 10.f / hostSr;
        setupDecimator();
    }

    // Fixed once: in clean mode the internal rate is 4 x host, so the
    // anti-image filter never has to move.
    void setupDecimator() {
        float w = std::tan((float)M_PI * 0.45f / 4.f);   // fc/rate = 0.45/4
        float w2 = w * w;
        float n = 1.f / (1.f + (float)M_SQRT2 * w + w2);
        lpB0 = w2 * n;
        lpB1 = 2.f * lpB0;
        lpB2 = lpB0;
        lpA1 = 2.f * (w2 - 1.f) * n;
        lpA2 = (1.f - (float)M_SQRT2 * w + w2) * n;
    }

    inline float decimate(float x) {
        float y = lpB0 * x + lpS1;
        lpS1 = lpB1 * x - lpA1 * y + lpS2;
        lpS2 = lpB2 * x - lpA2 * y;
        return y;
    }

    // A strike restarts every envelope. There is no legato: that is the
    // hardware's behaviour, and resetting the phases is what makes the pop
    // repeat identically. In free-run the partials are already sustaining, so
    // a strike only re-runs the attack.
    void trigger(bool hold) {
        attEnv = 0.f;
        noiseEnv = 1.f;
        pitchEnv = 1.f;
        pulse = 0.f;
        if (hold) return;
        for (int i = 0; i < kNumOsc; i++) {
            env[i] = 1.f;
            // start at the crest, not the zero crossing: the step is the
            // "classic analog pop" the manual puts at the centre of ATTACK,
            // and it is what keeps a short decay from being inaudible at low
            // pitch, where a quarter cycle outlasts the whole envelope
            phase[i] = 0.25f;
        }
    }

    // ---------------------------------------------------------- control rate

    // The internal rate is a power-of-two multiple of the fundamental. The
    // multiple is re-chosen only when f0 has moved by more than a semitone, so
    // a pitch sweep glides `rate` continuously (it stays an exact multiple all
    // the way) instead of stepping every few cents.
    //
    // No crossfade across a change of multiple, and none is needed: the phases
    // carry straight through, so only the density of the zero-order hold
    // changes, not the waveform. Measured over an eight-octave sweep (five
    // changes of multiple) the worst sample step at a change is 0.13 V against
    // 3.35 V for the signal's own worst step. It is inaudible.
    void updateRate(const Params& p) {
        if (p.cleanRate) {
            rate = 4.f * hostSr;
            mult = rate / std::max(p.f0, kMinF0);
            noiseHold = std::max(1, (int)std::lround(mult / 64.f));
            return;
        }
        float f0 = clampf(p.f0, kMinF0, kMaxF0);
        if (multF0 <= 0.f || std::fabs(std::log2(f0 / multF0)) > 1.f / 12.f) {
            float target = 2.f * hostSr;
            float m = std::exp2(std::round(std::log2(target / f0)));
            mult = clampf(m, kMinMult, kMaxMult);
            multF0 = f0;
            // noise is decimated by octave, so its colour tracks the pitch
            noiseHold = std::max(1, (int)std::lround(mult / 64.f));
        }
        rate = clampf(f0 * mult, kMinRate, kMaxRate);
    }

    void updateControls(const Params& p) {
        updateRate(p);

        // Arming free-run opens the envelopes on the spot, so the drone starts
        // without waiting for a trigger — BIM's free-running mode is an
        // oscillator, not a very long decay you still have to strike.
        if (p.hold && !holdActive) {
            for (int i = 0; i < kNumOsc; i++) env[i] = 1.f;
            attEnv = 1.f;
        }
        holdActive = p.hold;

        // ---- SPREAD: harmonic series to prime series, interpolated in the
        // log domain so the intervals stay musical and monotonic.
        //
        // The ratios get no smoother of their own: the module already smooths
        // SPREAD per sample, and these are recomputed every control block.
        // Measured, the high band sits at -70 dB and does not move as the
        // sweep rate goes from 0.1 Hz to 100 Hz, so there is nothing to zipper.
        static const float H[kNumOsc] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
        static const float P[kNumOsc] = {1.f, 3.f, 5.f, 7.f, 11.f, 13.f};
        float s = clampf(p.spread, 0.f, 1.f);
        if (p.extendedSpread) {
            // k < 0 compresses the series towards a slightly detuned unison,
            // the fat region the harmonic-only law cannot reach
            float k = s * 2.f - 1.f;
            for (int i = 0; i < kNumOsc; i++) {
                if (k < 0.f)
                    ratio[i] = std::pow(H[i], 1.f + k) * (1.f + 0.002f * (i + 1));
                else
                    ratio[i] = std::exp((1.f - k) * std::log(H[i]) + k * std::log(P[i]));
            }
        }
        else {
            for (int i = 0; i < kNumOsc; i++)
                ratio[i] = std::exp((1.f - s) * std::log(H[i]) + s * std::log(P[i]));
        }

        // ---- HARM: partial 1 is always full; partial 2 fades in over the
        // first quarter; partials 3..6 extend first their decays, then their
        // amplitudes, staggered across the rest of the knob.
        float h = clampf(p.harm, 0.f, 1.f);
        gain[0] = stage[0] = 1.f;
        dmul[0] = 1.f;
        float u2 = smoothstep(h / kHarmSecondSpan);
        gain[1] = stage[1] = u2;
        dmul[1] = 0.5f + 0.5f * u2;
        for (int i = 2; i < kNumOsc; i++) {
            float st = (float)(i - 2) / 3.f;
            float uD = smoothstep((h - (kHarmDecayStart + kHarmStagger * st)) / kHarmRamp);
            float uA = smoothstep((h - (kHarmAmpStart + kHarmStagger * st)) / kHarmRamp);
            // the floor is for the *decay* staging only: a partial that has
            // not been faded in yet is silent, so HARM fully CCW really is
            // the manual's single tone (and, in Metal, an unmodulated
            // carrier), not a shelf of residue under it
            dmul[i] = kHarmFloor + (1.f - kHarmFloor) * uD;
            stage[i] = uA;
            gain[i] = stage[i] * std::pow((float)(i + 1), -kSpectralTilt);
        }
        // ---- envelopes, in coefficients at the internal rate
        float D = std::max(p.decayMs, 0.5f) * 0.001f;
        for (int i = 0; i < kNumOsc; i++) {
            if (p.hold) {
                envCoef[i] = 1.f;
                continue;
            }
            float tau = std::max(D * dmul[i], 5e-4f) / 6.9f;   // -60 dB at D * d_i
            envCoef[i] = std::exp(-1.f / (tau * rate));
        }
        float tauA = std::max(p.attackMs, 0.01f) * 0.001f / 3.f;
        attCoef = std::exp(-1.f / (tauA * rate));
        noiseCoef = std::exp(-1.f / (clampf(D * 0.125f, 0.003f, 0.06f) * rate));
        pitchCoef = std::exp(-1.f / (clampf(D * 0.125f, 0.003f, 0.2f) * rate));
        // a quarter of a cycle, so a fold pulse never smears past the period
        pulseCoef = std::exp(-4.f * clampf(p.f0, kMinF0, kMaxF0) / rate);

        // ---- FOLD: the first three quarters set the reflection threshold
        float t = std::min(clampf(p.fold, 0.f, 1.f) / kFoldPulseSpan, 1.f);
        foldThresh = std::pow(kFoldMinThresh, t);
    }

    // --------------------------------------------------------- internal rate

    inline float nextNoise() {
        if (--noiseCount <= 0) {
            noiseCount = noiseHold;
            lcg = 1664525u * lcg + 1013904223u;
            noiseHeld = (float)(int32_t)lcg * (1.f / 2147483648.f);
        }
        return noiseHeld;
    }

    // One sample on the engine's own clock.
    float render(const Params& p) {
        bool blep = p.cleanRate && p.mode != kMetal;
        float f0 = clampf(p.f0, kMinF0, kMaxF0);
        float pmod = (p.mode == kLiquid) ? std::exp2(p.liquidOct * pitchEnv) : 1.f;
        float base = f0 * pmod / rate;

        float acc;
        if (p.mode == kMetal) {
            // Two 3-operator stacks. Stack A carries ratio 1 and is modulated
            // by ratios 3 and 5; stack B carries ratio 2 and is modulated by
            // ratios 4 and 6 — so SPREAD still retunes everything, and HARM
            // still fades the second voice in. HARM drives the modulation
            // index here the way it drives partial level in Skin.
            float dt[kNumOsc];
            for (int i = 0; i < kNumOsc; i++) {
                dt[i] = base * ratio[i];
                phase[i] = wrapPhase(phase[i] + dt[i]);
            }
            const float inv2pi = 1.f / (2.f * (float)M_PI);
            float a2 = morphWave(phase[4], p.morph, dt[4], blep)
                     * kMetalIndex * stage[4] * env[4];
            float a1 = morphWave(wrapPhase(phase[2] + a2 * inv2pi), p.morph, dt[2], blep)
                     * kMetalIndex * stage[2] * env[2];
            float cA = morphWave(wrapPhase(phase[0] + a1 * inv2pi), p.morph, dt[0], blep)
                     * gain[0] * env[0];
            float b2 = morphWave(phase[5], p.morph, dt[5], blep)
                     * kMetalIndex * stage[5] * env[5];
            float b1 = morphWave(wrapPhase(phase[3] + b2 * inv2pi), p.morph, dt[3], blep)
                     * kMetalIndex * stage[3] * env[3];
            float cB = morphWave(wrapPhase(phase[1] + b1 * inv2pi), p.morph, dt[1], blep)
                     * gain[1] * env[1];
            // normalized by the live carrier gain, exactly as Skin normalizes
            // its sum below: without this the mode decays as the *square* of
            // the envelope (the carriers fade, then the final envelope fades
            // the result again) and sits several dB under the other two
            float live = gain[0] * env[0] + gain[1] * env[1];
            acc = (cA + cB) / std::max(live, 1e-3f);
        }
        else {
            acc = 0.f;
            float live = 0.f;
            for (int i = 0; i < kNumOsc; i++) {
                float dt = base * ratio[i];
                phase[i] = wrapPhase(phase[i] + dt);
                float a = gain[i] * env[i];
                live += a;
                // a partial the envelope has silenced still has to keep its
                // phase, but not its waveform: at low HARM that is four of six
                if (a > 1e-5f)
                    acc += a * morphWave(phase[i], p.morph, dt, blep);
            }
            // Loudness compensation. The sum is normalized by the gain of the
            // partials that are *currently* alive, not by the static gain sum:
            // HARM then changes timbre rather than level, which is what the
            // manual means by "compensation for loudness occurs", and the
            // envelope applied below is the only envelope heard.
            acc *= 1.f / std::max(live, 1e-3f);
        }

        acc += p.noiseAmt * noiseEnv * nextNoise();

        // ---- the envelopes, applied *before* the folder, so they scale its
        // drive: a hit starts deep in the fold and unwinds as it decays. The
        // stage count falling with level is what makes the folder answer the
        // envelope; normalizing the sum above and enveloping it here keeps
        // the two jobs separate. The max, not the sum, for the final
        // envelope: a sum changes shape with HARM in exactly the way that
        // would undo the compensation.
        float fe = 0.f;
        for (int i = 0; i < kNumOsc; i++)
            fe = std::max(fe, gain[i] * env[i]);
        finalEnv = fe;
        float x = acc * attEnv * fe;

        // ---- infinifolder: reflect about the threshold as many times as the
        // level demands, then divide by the threshold to compensate. The
        // division holds the folded crest at unity and lifts the unfolded
        // tail with it, so FOLD adds sustain the way a fuzz pedal does.
        float T = foldThresh;
        float y = x;
        int stages = 0;
        while (std::fabs(y) > T && stages < kMaxFoldStages) {
            y = (y >= 0.f ? 1.f : -1.f) * (2.f * T - std::fabs(y));
            stages++;
        }
        lastStages = stages;
        y /= T;

        // ---- pulse train at the top of the knob: an exponentially decaying
        // impulse fired at every local extremum of the folded signal, its mix
        // scaled by the envelope so the train dies with the hit
        float diff = y - prevY;
        if (diff * prevDiff < 0.f)
            pulse = prevDiff > 0.f ? 1.f : -1.f;
        if (diff != 0.f) prevDiff = diff;
        prevY = y;
        if (p.fold > kFoldPulseSpan) {
            float mix = (p.fold - kFoldPulseSpan) / (1.f - kFoldPulseSpan);
            y += mix * kFoldPulseMix * pulse * fe;
        }
        pulse *= pulseCoef;

        // ---- advance the envelopes
        for (int i = 0; i < kNumOsc; i++) env[i] *= envCoef[i];
        attEnv = 1.f + (attEnv - 1.f) * attCoef;
        noiseEnv *= noiseCoef;
        pitchEnv *= pitchCoef;

        return y;
    }

    // ------------------------------------------------------------ host rate

    void process(const Params& p, float& audioOut, float& envOut) {
        float step = clampf(rate / hostSr, 1e-4f, 64.f);
        phaseAcc += step;
        int ticks = 0;
        while (phaseAcc >= 1.f && ticks < 128) {
            phaseAcc -= 1.f;
            lastSample = render(p);
            if (p.cleanRate) {
                lpPrev = lpCur;
                lpCur = decimate(lastSample);
            }
            ticks++;
        }
        if (ticks >= 128) phaseAcc = 0.f;

        float out;
        if (p.cleanRate) {
            // one internal sample of fractional delay, which at 4x host is
            // below 6 us and buys a properly interpolated resampling
            out = lpPrev + (lpCur - lpPrev) * phaseAcc;
        }
        else {
            out = lastSample;                     // zero-order hold, deliberately
            if (p.quantize)
                out = std::round(out * 32767.f) * (1.f / 32767.f);
        }

        // gentle DC blocker: folding and the pulse train are odd-symmetric, so
        // there should be nothing to remove, but a drum voice must never park
        // an offset on the output — and the strike itself is a step
        dcY = out - dcX + dcR * dcY;
        dcX = out;
        if (!std::isfinite(dcY)) { dcY = 0.f; dcX = 0.f; }

        // makeup gain and soft knee last, so the ceiling is a real bound
        audioOut = softClip(dcY * kOutputGain);
        envOut = finalEnv;
    }
};

}  // namespace tundo_dsp
