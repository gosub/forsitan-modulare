// materiae_dsp.hpp - percussive voice built from two square waves and the
// relationship between them.
//
// The premise: complexity comes from how two primitive signals relate, not
// from the signals themselves. Both oscillators are naive squares. Everything
// interesting happens between them.
//
//   trig -> phase reset
//                 .---------------------------------.
//                 v                                 |
//   OSC A ----> [ cell A->B ] --FM/AM--> OSC B ------'
//     ^                                    |
//     '------- [ cell B->A ] <-------------'
//
//   A, B -> RELATION (and / sum / or / ring / sr, crossfaded)
//        -> BLEND (A alone <-> operator)
//        -> resonant SVF ---> VCA (env 1) -> DC block -> out
//
//   env 2 -> pitch, relation position, cutoff   (bipolar, per destination)
//
// Two rates. The logic core -- oscillators, cross-modulation, operator --
// runs on its own clock at `gridRate`, which the GRID control sweeps from 4x
// the host rate down to 250 Hz. High, it is plain oversampling and the voice
// is clean; low, the whole relationship is quantized onto a coarse grid, the
// oscillators alias outright, and what is left of the relationship between
// them is the timbre. That knob is the module's digital character made
// tuneable instead of accidental.
//
// The filter and the VCA run at the host rate, downstream of the decimator,
// and stay clean on purpose: the square is the exciter, the resonant filter
// is the body. Squares alone are thin, and this module is supposed to sound
// heavy. Near maximum resonance the filter rings for seconds and a trigger
// pings it, so a kick is the filter's own sine with a square transient on it.
//
// A note on the operator set. For bipolar squares A,B in {-1,+1}:
//     A * B  ==  -(A xor B)          exactly
//     |A - B| == 2 * (A xor B)       up to scale
//     A - B   ==  A + B              same magnitude spectrum, B inverted is
//                                    B shifted half a period
//     A and B == min(A,B),  A or B == max(A,B), and those two are mirror
//                                    images: same magnitude spectrum, opposite
//                                    DC, and the output DC blocker eats the
//                                    only difference
// so ring modulation, XOR, absolute difference and subtraction collapse onto
// two operators, and OR earns nothing that AND does not already give. What is
// left that is genuinely distinct is min, mean, product -- and then only
// operators that are *stateful*, because an instantaneous function of two
// two-valued signals has almost no room left. Hence the last two: a set/reset
// latch, whose duty cycle is literally the phase difference between the
// oscillators, and a shift register for pseudo-noise. Five, ordered sparse ->
// dense -> metallic -> phase -> noise, and RELATION crossfades between
// neighbours so env 2 can sweep the operator itself over the length of a hit.
//
// (`materiae_probe ops` prints the pairwise spectral distance that settled
// this: AND against OR came out at 0.018 where every other pair is above 0.7.)
//
// The caller hands over already-mapped parameters; the engine smooths nothing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace materiae_dsp {

// ---------------------------------------------------------------- constants

constexpr int kNumOps = 5;          // and, sum, ring, flip, noise
constexpr int kNumDiv = 5;          // /1 /2 /4 /8 /16

// GRID's range, chosen from `materiae_probe grid`. The top used to be 8x the
// host rate and the bottom 1500 Hz, which spent knob travel badly at both ends:
// 8x to 4x measured 0.063 of spectral distance across a whole octave of the
// knob, out of about 1.9 for the full sweep, while every octave below 1500 Hz
// was still worth close to 1.0 a step. So the top comes down to 4x -- which
// also halves the worst-case cost of the module -- and the bottom goes to
// 250 Hz, where the grid rate is about to become a pitch of its own.
constexpr float kGridMaxMult = 4.f; // top of the GRID knob, x host rate
constexpr float kGridMin = 250.f;   // bottom, in Hz
constexpr int kMaxGridSteps = 64;   // guard on the inner loop

constexpr float kFMOct = 4.f;       // cross-modulation depth at XMOD full, octaves
constexpr float kMinF0 = 8.f, kMaxF0 = 12000.f;
constexpr float kBaseHz = 32.703f;  // C1 at pitch 0 V

constexpr float kMinCut = 20.f, kMaxCut = 12000.f;
constexpr float kMinK = 0.02f;      // 1/Q at full resonance: rings for seconds
constexpr float kMaxK = 2.f;
constexpr float kStateLimit = 12.f; // integrator clamp, keeps the ring bounded

constexpr float kCurveExp = 2.5f;   // curve knob -> pow exponent, in octaves
// Retrigger fade. A trigger arriving while the voice is still sounding
// restarts the oscillator phases, the latch and the shift register, and jumps
// the envelope back to full -- four discontinuities at once, landing on a tone
// that is already there. That is a retrigger click.
//
// Two earlier attempts at this were wrong and are worth recording. A minimum
// attack time does nothing, because the attack shares the CURVE knob and a
// concave curve is at a quarter of full scale one sample in. Ramping the VCA
// back up from zero does nothing either -- it only fixes the second half of
// the problem, and getting *to* zero instantly is itself a step: measured, the
// output went from -1.40 V to -0.04 V in one sample, which is the click.
//
// So the reset waits. A trigger arriving on a sounding voice fades the VCA
// down over kRetrigFade, and only when it reaches zero does anything actually
// reset; then it fades back up. Both edges are ramps and the discontinuities
// all happen while the output is silent. The cost is that a retrigger sounds
// twice this late, which at 0.4 ms is under a millisecond and not something a
// drum part can hear. A trigger from silence skips all of it.
constexpr float kRetrigFade = 0.0004f;
constexpr float kDCPole = 8.f;      // output DC blocker corner, Hz
// The exciter is scaled so a plain hit peaks near unity, which leaves the
// resonant filter's own boost somewhere to go: everything above 0.8 bends
// into a knee instead of slamming a rail. An operator whose duty cycle is far
// from half carries enough DC that removing it alone puts the peak at 1.6.
constexpr float kExciterGain = 0.6f;
constexpr float kClipKnee = 0.8f;

// The ratio table. The just ratios the design brief asked for, a 1.01 detune
// for slow beating at unison, and four irrationals (sqrt2, e, pi, and the
// golden ratio's octave) whose partials never line up -- with logic operators
// an irrational ratio never repeats, so the pulse pattern keeps evolving.
constexpr int kNumRatios = 19;
const float kRatios[kNumRatios] = {
    0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.75f,
    1.f, 1.01f, 1.25f, 4.f / 3.f, 1.41421356f,
    1.5f, 5.f / 3.f, 1.75f, 2.f, 2.5f,
    2.71828183f, 3.f, 3.14159265f, 4.f
};

// ------------------------------------------------------------------ helpers

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// exp2 good to ~1e-4 over the ranges used here. The cross-modulation path
// calls it twice per grid step, which at 8x oversampling is 768k calls a
// second; libm's is accurate enough to be the module's largest single cost.
inline float fastExp2(float x) {
    x = clampf(x, -30.f, 30.f);
    float xf = std::floor(x);
    float f = x - xf;
    // degree-4 minimax on [0,1)
    float p = 1.f + f * (0.6960656421f + f * (0.2240818177f +
              f * (0.0792043842f + f * 0.0136308485f)));
    union { float f; int32_t i; } u;
    u.i = (int32_t)((int)xf + 127) << 23;
    return p * u.f;
}

// transparent below the knee, asymptotic to +-1 above it. Nothing in the
// signal path is allowed to reach a hard clamp: this module is meant to be
// driven into instability, and a rail turns that into digital clipping.
inline float softClip(float x) {
    float ax = std::fabs(x);
    if (ax <= kClipKnee) return x;
    float s = x < 0.f ? -1.f : 1.f;
    float o = (ax - kClipKnee) / (1.f - kClipKnee);
    return s * (kClipKnee + (1.f - kClipKnee) * (o / (1.f + o)));
}

// Antiderivative of softClip. Even, because softClip is odd.
inline float softClipInt(float x) {
    float u = std::fabs(x);
    if (u <= kClipKnee) return 0.5f * u * u;
    const float a = 1.f - kClipKnee;
    float w = u - kClipKnee;
    return 0.5f * kClipKnee * kClipKnee + kClipKnee * w + a * w
         - a * a * std::log1p(w / a);
}

// softClip with first-order antiderivative antialiasing.
//
// A memoryless nonlinearity run at the host rate makes harmonics above Nyquist
// and folds every one of them back down, inharmonically. Driven hard that is
// not warmth, it is grit and clicks: measured on a 2.35 kHz sine, plain
// softClip put 17.9% of the output energy on non-harmonic bins at the top of
// the GAIN knob, against 0.08% at the bottom, while the peak sample-to-sample
// jump grew nine times over as the level grew two.
//
// Integrating the transfer function across the sample interval instead of
// evaluating it at a point is the cheap standard answer, and it costs one
// log per sample above the knee. The fallback for a flat interval is the
// midpoint, since the difference quotient goes 0/0 there.
struct SoftClipper {
    float x1 = 0.f, F1 = 0.f;

    void reset() { x1 = 0.f; F1 = 0.f; }

    float process(float x) {
        float F = softClipInt(x);
        float d = x - x1;
        float y = (std::fabs(d) > 1e-5f) ? (F - F1) / d
                                         : softClip(0.5f * (x + x1));
        x1 = x;
        F1 = F;
        return y;
    }
};

inline float wrapPhase(float x) {
    x -= std::floor(x);
    return (x >= 0.f && x < 1.f) ? x : 0.f;   // catches inf/nan
}

// ------------------------------------------------------------------- blocks

// Edge divider: a flip-flop chain clocked by the source's rising zero
// crossings. /1 passes the input through and keeps its pulse width; every
// other tap is a square at exactly half duty, because that is what a
// flip-flop does. Defined for any signal, meaningful for pulse-like ones.
struct Divider {
    uint32_t count = 0;
    bool last = false;

    void reset() { count = 0; last = false; }

    float process(float x, int shift) {
        bool s = x > 0.f;
        if (s && !last) count++;
        last = s;
        if (shift <= 0) return s ? 1.f : -1.f;
        return ((count >> (shift - 1)) & 1u) ? 1.f : -1.f;
    }
};

// The signal conditioning cell: attenuvert + bias, divide, band-limit. One
// struct, used in both cross-modulation directions. Deliberately not exposed
// as three panel controls per instance -- the amount and the division are on
// the panel, the bias and the smoothing are derived.
struct Cell {
    Divider div;
    float lp = 0.f;

    void reset() { div.reset(); lp = 0.f; }

    float process(float x, float amt, float bias, int shift, float lpCoef) {
        float d = div.process(x, shift);
        lp += lpCoef * (d - lp);
        return bias + amt * lp;
    }
};

// Two squares into pseudo-noise: an 8-bit shift register clocked by A and fed
// from bit 7 xor B. Deterministic, pitch-related, and the only route to hats
// and snares that does not break the two-sources premise.
struct ShiftReg {
    uint32_t reg = 0xACEu;
    bool last = false;
    float out = 0.f;

    void reset() { reg = 0xACEu; last = false; out = 0.f; }

    float process(float clk, float data) {
        bool s = clk > 0.f;
        if (s && !last) {
            uint32_t fb = ((reg >> 7) & 1u) ^ (data > 0.f ? 1u : 0u);
            reg = ((reg << 1) | fb) & 0xFFu;
            out = (float)(reg & 0xFFu) * (2.f / 255.f) - 1.f;
        }
        last = s;
        return out;
    }
};

// Set/reset latch: A sets it, B clears it, so the output is high for exactly
// the interval by which A leads B. Its duty cycle is the phase difference
// between the two oscillators -- the one operator that turns the relationship
// itself into an amplitude rather than reading the two levels instant by
// instant. At an irrational ratio it never settles.
struct Latch {
    bool state = false, lastS = false, lastR = false;

    void reset() { state = false; lastS = lastR = false; }

    float process(float s, float r) {
        bool ss = s > 0.f, rr = r > 0.f;
        if (ss && !lastS) state = true;
        if (rr && !lastR) state = false;
        lastS = ss; lastR = rr;
        return state ? 1.f : -1.f;
    }
};

struct Osc {
    float phase = 0.f;

    void reset(float ph) { phase = wrapPhase(ph); }

    float step(float dt, float pw) {
        phase = wrapPhase(phase + dt);
        return (phase < pw) ? 1.f : -1.f;
    }
};

// Topology-preserving state variable filter. The integrator states are
// clamped rather than left to run: at kMinK the loop is very nearly lossless
// and a heavy exciter would otherwise walk it out of range over a few seconds.
struct SVF {
    float ic1 = 0.f, ic2 = 0.f;

    void reset() { ic1 = ic2 = 0.f; }

    void process(float v0, float g, float k, float& lp, float& bp, float& hp) {
        float a1 = 1.f / (1.f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = clampf(2.f * v1 - ic1, -kStateLimit, kStateLimit);
        ic2 = clampf(2.f * v2 - ic2, -kStateLimit, kStateLimit);
        hp = v0 - k * v1 - v2;
        bp = v1;
        lp = v2;
    }
};

// AD envelope with a continuous curve. curve < 0 is convex (slow to leave the
// peak, a "log" fall), 0 is linear, > 0 concave (an exponential-looking drop).
// The decay reaches exactly zero, which is what lets a hit end in silence even
// with a ringing filter behind it.
struct Env {
    float time = 1e9f;
    bool running = false;

    void reset() { time = 1e9f; running = false; }
    void trigger() { time = 0.f; running = true; }
    bool isRunning() const { return running; }

    float process(float dt, float atk, float dec, float curve) {
        if (!running) return 0.f;
        time += dt;
        float p = fastExp2(curve * kCurveExp);
        if (time < atk) {
            float x = atk > 1e-6f ? time / atk : 1.f;
            return std::pow(clampf(x, 0.f, 1.f), 1.f / p);
        }
        float x = (time - atk) / (dec > 1e-6f ? dec : 1e-6f);
        if (x >= 1.f) { running = false; return 0.f; }
        return std::pow(1.f - x, p);
    }
};

// --------------------------------------------------------------- parameters

struct Params {
    float f0 = 110.f;       // osc A, Hz (already includes V/oct)
    float ratio = 1.5f;     // osc B = f0 * ratio
    float shape = 0.5f;     // pulse width skew, 0.5 = both square
    float xmod = 0.f;       // cross-modulation depth, 0..1
    float tilt = 0.f;       // -1 = A->B only, 0 = both, +1 = B->A only
    int divShift = 0;       // 0..4, division = 1 << divShift
    int modDest = 0;        // 0 = frequency, 1 = amplitude, 2 = both
    float gridRate = 0.f;   // logic core rate, Hz
    float relation = 1.f;   // 0..kNumOps-1, crossfaded between neighbours
    float blend = 1.f;      // 0 = osc A alone, 1 = operator
    float gain = 1.f;       // drive into the output saturator, 1x .. 16x
    float cutoff = 2000.f;  // Hz
    float reso = 0.f;       // 0..1
    int filterMode = 0;     // 0 = lowpass, 1 = bandpass
    float attack = 0.001f;  // env 1, seconds
    float decay = 0.25f;
    float curve = 0.f;      // -1..1
    float decay2 = 0.1f;    // env 2
    float curve2 = 0.f;
};

// ------------------------------------------------------------------- engine

struct Engine {
    // logic core, at gridRate
    Osc oscA, oscB;
    Cell cellAB, cellBA;
    Divider opDiv;
    Latch latch;
    ShiftReg sr;
    float prevA = 1.f, prevB = 1.f;
    float gridAcc = 0.f;
    float held = 0.f;

    // host rate
    SVF svf;
    SoftClipper drive;
    Env env1, env2;
    float dcx = 0.f, dcy = 0.f;
    float velocity = 1.f;
    float retrigFade = 1.f;
    bool pendingStrike = false;

    float sampleRate = 44100.f;
    bool freeRun = false;       // do not reset phase on trigger
    bool alwaysRun = false;     // keep sounding with env 1 finished (drone out)
    bool env2Free = false;      // env 2 keeps running through a retrigger
    bool trackCutoff = false;
    float phaseOffset = 0.f;    // B's phase at reset, 0..1

    void setSampleRate(float sr) {
        sampleRate = sr;
        svf.reset();
    }

    void reset() {
        oscA.reset(0.f); oscB.reset(0.f);
        cellAB.reset(); cellBA.reset();
        opDiv.reset(); latch.reset(); sr.reset();
        prevA = prevB = 1.f;
        gridAcc = 0.f; held = 0.f;
        svf.reset();
        drive.reset();
        env1.reset(); env2.reset();
        dcx = dcy = 0.f;
        retrigFade = 1.f;
        pendingStrike = false;
    }

    // Is anything actually coming out right now? A retrigger has to be hidden
    // whenever it is, which includes the drone tap, where env 1 is not running
    // but the voice is still sounding.
    bool sounding() const { return env1.isRunning() || alwaysRun; }

    void trigger(const Params& p, float vel) {
        velocity = vel;
        if (sounding()) {
            pendingStrike = true;   // process() strikes at the bottom of the fade
            return;
        }
        strike(p);
    }

    // everything a trigger actually does, once it is safe to do it
    void strike(const Params& p) {
        env1.trigger();
        if (!env2Free || !env2.isRunning()) env2.trigger();
        if (!freeRun) {
            // deterministic hits: with logic operators the output pattern is a
            // function of the phase offset, so free-running oscillators make
            // every strike of the same patch a different sound.
            oscA.reset(0.f);
            oscB.reset(phaseOffset);
            cellAB.reset(); cellBA.reset();
            opDiv.reset(); latch.reset(); sr.reset();
            prevA = prevB = 1.f;
            // the grid accumulator too: left alone it carries a fractional
            // step across the trigger, which lands the core's first step at a
            // different sub-sample offset on every hit. Small, but it is the
            // whole of what "repeatable" means here. `held` is deliberately
            // not zeroed with it: below the host rate a grid step does not
            // land every sample, so forcing the exciter to zero would punch a
            // hole in a voice that is still sounding. A hit from silence has
            // it at zero already, by way of the idle branch.
            gridAcc = 0.f;
        }
        // ping the body. At zero resonance this adds nothing.
        svf.ic1 += 0.6f * p.reso;
    }

    // one operator evaluation. The two stateful operators are advanced by the
    // caller on every step regardless of the crossfade weight, so their state
    // never depends on where the RELATION knob happens to sit.
    inline float ops(int i, float a, float b, float lat, float srOut) const {
        switch (i) {
            case 0: return std::min(a, b);          // and
            case 1: return 0.5f * (a + b);          // sum
            case 2: return a * b;                   // ring, == -(a xor b)
            case 3: return lat;                     // set/reset latch
            default: return srOut;                  // shift register
        }
    }

    // one step of the logic core
    float gridStep(const Params& p, float fA, float fB, float relation,
                   float depthAB, float depthBA, float lpA, float lpB) {
        float pwA = clampf(0.5f + (p.shape - 0.5f) * 0.8f, 0.06f, 0.94f);
        float pwB = clampf(0.5f - (p.shape - 0.5f) * 0.8f, 0.06f, 0.94f);

        // the cells read the previous step's outputs: that one-step delay is
        // what closes the loop and lets A<->B run into feedback
        float modB = cellAB.process(prevA, depthAB, 0.f, p.divShift, lpA);
        float modA = cellBA.process(prevB, depthBA, 0.f, p.divShift, lpB);

        float fm = (p.modDest == 1) ? 0.f : kFMOct;
        float dtA = fA * fastExp2(modA * fm) / p.gridRate;
        float dtB = fB * fastExp2(modB * fm) / p.gridRate;

        // dt is deliberately not held at half a cycle. Clamping it there made
        // both oscillators degenerate to the same alternate-every-step square
        // as soon as the grid rate fell below twice the pitch: A and B became
        // identical, every operator collapsed to a constant, and the DC
        // blocker turned the result into silence. Letting the phase alias
        // instead keeps the two at different aliased frequencies, so the
        // relationship survives -- messy and folded, which is the point of
        // the bottom of the GRID knob.
        float a = oscA.step(clampf(dtA, 0.f, 1024.f), pwA);
        float b = oscB.step(clampf(dtB, 0.f, 1024.f), pwB);
        prevA = a; prevB = b;   // the feedback loop reads the undivided pair

        // DIV divides osc A on the way into the operator, not only inside the
        // cross-modulation cells. Confined to those cells it was a modifier of
        // a modifier: with XMOD at zero -- the default -- it changed nothing at
        // all, which `materiae_probe div` measured as a spectral distance of
        // exactly 0.000 on every operator. Read here it is a subharmonic
        // operand, so the two stateful operators are clocked at A/N against an
        // undivided B and the pulse pattern changes outright.
        //
        // Only A. Dividing both would drop the whole voice an octave, which is
        // what PITCH is for; dividing one is what makes a new relationship.
        // At /1 the chain passes A through untouched, pulse width and all.
        a = opDiv.process(a, p.divShift);

        // The stateful operators read edges, not levels, so they run from the
        // raw squares: gating their inputs would only mask the occasional edge
        // and leave the pattern otherwise untouched. (The first version scaled
        // a and b ahead of every operator, and `materiae_probe xmod` showed the
        // amplitude destination doing literally nothing at RELATION on the
        // latch -- a sign-reading operator cannot hear a gain.) They get gated
        // on the way out instead, which keeps the pattern and loses the level.
        float lat = latch.process(a, b);
        float srOut = sr.process(a, b);

        float gA = 1.f, gB = 1.f;
        if (p.modDest >= 1) {
            float dA = clampf(std::fabs(depthBA), 0.f, 1.f);
            float dB = clampf(std::fabs(depthAB), 0.f, 1.f);
            gA = 1.f - dA * (0.5f - 0.5f * clampf(modA, -1.f, 1.f));
            gB = 1.f - dB * (0.5f - 0.5f * clampf(modB, -1.f, 1.f));
        }
        float ag = a * gA, bg = b * gB;
        float gg = gA * gB;

        int i0 = (int)relation;
        if (i0 > kNumOps - 2) i0 = kNumOps - 2;
        if (i0 < 0) i0 = 0;
        float f = clampf(relation - (float)i0, 0.f, 1.f);
        float op = (1.f - f) * ops(i0, ag, bg, lat * gg, srOut * gg)
                 + f * ops(i0 + 1, ag, bg, lat * gg, srOut * gg);

        return ag + (op - ag) * p.blend;
    }

    // one host sample
    void process(const Params& p, float dt, float& audioOut, float& droneOut,
                 float& env2Out, float e2Pitch, float e2Relation,
                 float e2Cutoff, float voct) {
        // the deferred strike, and the ramp either side of it
        if (pendingStrike) {
            if (!sounding()) {                  // it ended on its own meanwhile
                retrigFade = 1.f;
                pendingStrike = false;
                strike(p);
            } else {
                retrigFade -= dt / kRetrigFade;
                if (retrigFade <= 0.f) {
                    retrigFade = 0.f;
                    pendingStrike = false;
                    strike(p);
                }
            }
        } else if (retrigFade < 1.f) {
            retrigFade = std::min(1.f, retrigFade + dt / kRetrigFade);
        }

        float e1 = env1.process(dt, p.attack, p.decay, p.curve);
        float e2 = env2.process(dt, 0.f, p.decay2, p.curve2);
        env2Out = e2;

        float f0 = clampf(p.f0 * fastExp2(e2Pitch * e2 * 4.f), kMinF0, kMaxF0);
        float fA = f0;
        float fB = clampf(f0 * p.ratio, kMinF0, kMaxF0 * 2.f);

        if (!env1.isRunning() && !alwaysRun) {
            // Idle. Nothing can reach the output, so nothing downstream runs.
            // The filter and the DC blocker are cleared rather than left to
            // coast: the oscillators would otherwise keep driving the filter
            // through the silence and every strike would inherit a different
            // integrator state, which is exactly the non-repeatability that
            // resetting the oscillator phase is there to prevent. It also
            // costs nothing to render a voice that is not sounding, and a
            // drum voice is idle most of the time.
            if (freeRun) {
                // ...except in free-run, where the phase relationship at the
                // next trigger is the whole point, so the two oscillators keep
                // turning even though no one is listening.
                gridAcc += p.gridRate * dt;
                int n = 0;
                while (gridAcc >= 1.f && n < kMaxGridSteps) {
                    gridAcc -= 1.f;
                    oscA.step(clampf(fA / p.gridRate, 0.f, 0.5f), 0.5f);
                    oscB.step(clampf(fB / p.gridRate, 0.f, 0.5f), 0.5f);
                    n++;
                }
                if (n >= kMaxGridSteps) gridAcc = 0.f;
            } else {
                gridAcc = 0.f;
            }
            svf.reset();
            drive.reset();      // it remembers the previous sample, and a hit
                                // has to start from the same state every time
            dcx = dcy = 0.f;
            held = 0.f;
            retrigFade = 1.f;
            audioOut = 0.f;
            droneOut = 0.f;
            return;
        }

        float relation = clampf(p.relation + e2Relation * e2 * 2.f,
                                0.f, (float)(kNumOps - 1));

        float depthAB = p.xmod * clampf(1.f - p.tilt, 0.f, 1.f);
        float depthBA = p.xmod * clampf(1.f + p.tilt, 0.f, 1.f);

        // each cell smooths at a few times its own source's rate, divided:
        // a divided square keeps its edges, an undivided one at the top of
        // the pitch range does not get to alias the modulator as well
        float divN = (float)(1 << p.divShift);
        float lpA = clampf(6.2831853f * (3.f * fA / divN) / p.gridRate, 0.f, 1.f);
        float lpB = clampf(6.2831853f * (3.f * fB / divN) / p.gridRate, 0.f, 1.f);

        // run the logic core and box-average whatever lands inside this sample
        gridAcc += p.gridRate * dt;
        float sum = 0.f;
        int n = 0;
        while (gridAcc >= 1.f && n < kMaxGridSteps) {
            gridAcc -= 1.f;
            sum += gridStep(p, fA, fB, relation, depthAB, depthBA, lpA, lpB);
            n++;
        }
        if (n >= kMaxGridSteps) gridAcc = 0.f;
        if (n > 0) held = sum / (float)n;
        float x = held;

        // body
        float cut = p.cutoff * fastExp2(e2Cutoff * e2 * 5.f);
        if (trackCutoff) cut *= fastExp2(voct);
        cut = clampf(cut, kMinCut, std::min(kMaxCut, sampleRate * 0.45f));
        float g = std::tan(3.14159265f * cut / sampleRate);
        float k = kMaxK + (kMinK - kMaxK) * clampf(p.reso, 0.f, 1.f);
        float lp, bp, hp;
        svf.process(x, g, k, lp, bp, hp);
        float body = (p.filterMode == 1) ? bp * (2.f + 3.f * p.reso) : lp;

        // Drive, ahead of the VCA. Saturating after the envelope would make
        // how hard the voice clips a function of where in the decay you are,
        // so a hit would change character as it fell rather than simply
        // getting quieter. Here the knob sets a property of the patch.
        float sat = drive.process(body * kExciterGain * p.gain);

        // DC block ahead of the VCA, and one blocker for both taps.
        //
        // It used to sit after the VCA, on the argument that the operator's
        // duty-cycle offset is part of the transient and should be shaped by
        // the envelope before being removed. Two things were wrong with that.
        // A blocker fed a cut signal emits the offset it had been removing:
        // when a retrigger drops the VCA to zero the output does not go with
        // it, it steps to minus that offset and decays over the blocker's own
        // twenty milliseconds -- which is a thump on every retrigger, and was
        // measurably 10% of the level the voice had been at. And what the
        // envelope was shaping was DC, which is not something to send through
        // a VCA in the first place.
        //
        // Blocked first, the VCA sees a signal centred on zero, so an envelope
        // at zero means an output at zero, exactly, and the retrigger has
        // nothing left to click with.
        float r = 1.f - 6.2831853f * kDCPole / sampleRate;
        dcy = sat - dcx + r * dcy;
        dcx = sat;
        if (!std::isfinite(dcy)) { dcy = dcx = 0.f; svf.reset(); }

        // the voice before env 1: the same sound, held open. It gets the
        // retrigger fade too -- a trigger resets the oscillators underneath a
        // drone just as abruptly as underneath a hit.
        droneOut = softClip(dcy * retrigFade);
        audioOut = softClip(dcy * e1 * velocity * retrigFade);
    }
};

}  // namespace materiae_dsp
