// scrupea_dsp.hpp -- the eight-channel chaotic engine behind scrupea.
//
// Eight voices, each holding two identical "levers". A lever is an oscillator
// into a tuned feedback comb with a normalizer in it, and -- in two of the
// three topologies -- a resonant filter either in front of the summer or
// inside the loop. The two levers of a voice modulate each other, and each
// reads its modulator from whichever of the eight voices its `fm` and `am`
// bars point at, so the bank is coupled all-to-all.
//
// Everything here is read off Skrewell's own ensemble file: the signal flow
// from the resolved connection graph, the mapping laws from the macros that
// implement them, and the numeric ranges from the knob records themselves
// (a Reaktor knob keeps Min and Max as float32 in its KSModul payload). The
// constants marked SET_ below are the one thing the file does not give up:
// where inside those ranges the factory snapshots left each knob is packed
// or compressed in the snapshot blocks and is not recoverable. Those, and
// only those, are scrupea's choice.
//
// Header-only and free of Rack types so the offline harnesses in test/ can
// drive it directly.
#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scrupea_dsp {

static const int NCH = 8;

// Each voice is two complete levers, after the structure of the ensemble
// itself: every tone generator in Skrewell holds exactly two LEVER macros
// with a `crossvoice` between them, and a LEVER is not an oscillator, it is a
// whole channel -- oscillator, filter, normalizer, delay and feedback. Both
// levers of a voice are fed the *same* eight bars; what separates them is
// that each one's modulator comes from the other's output.
static const int LPC = 2;
static const int NLEV = NCH * LPC;

// Lever `k` of voice `c`.
inline int lev(int c, int k) { return c * LPC + k; }

// Three topologies, the ensemble's three tone generators. They differ in the
// oscillator, in what the filter is, and in where it sits.
enum Topology {
    TOPO_LOOP = 0,   // pulse osc; 2-pole HP into 2-pole LP, inside the loop
    TOPO_PRE  = 1,   // pulse osc; one multimode 2-pole, in front of the summer
    TOPO_BARE = 2,   // parabolic osc; no filter at all
};

// ------------------------------------------------------- ensemble ranges ---
//
// Min and Max of every knob that matters, straight out of the .ens. Names are
// the ensemble's own.

//   'min' / 'max' -- the two ends of the pitch mapping, in MIDI pitch
static const float K_PITCH_LO_MIN = -90.f, K_PITCH_LO_MAX =  37.f;
static const float K_PITCH_HI_MIN =   9.f, K_PITCH_HI_MAX = 136.f;
//   'CUTmin'/'CUTmax', 'LPmin'/'LPmax', 'HPmin'/'HPmax' -- all the same span
static const float K_CUT_LO_MIN = -90.f, K_CUT_LO_MAX =  37.f;
static const float K_CUT_HI_MIN =   9.f, K_CUT_HI_MAX = 136.f;
//   'short' / 'long' -- the delay is set as a pitch too, and inverted
static const float K_DEL_SHORT_MIN = 9.f,   K_DEL_SHORT_MAX = 136.f;
static const float K_DEL_LONG_MIN  = -90.f, K_DEL_LONG_MAX  =  37.f;
//   the five knob pairs that `flow` crossfades
static const float K_FM_MIN  = 1.f,  K_FM_MAX  = 16.f;
static const float K_AM_MIN  = 0.f,  K_AM_MAX  = 5.f;
static const float K_RES_MIN = 0.f,  K_RES_MAX = 1.f;
static const float K_SMT_MIN = 1.f,  K_SMT_MAX = 20.f;
static const float K_NRM_MIN = 1.f,  K_NRM_MAX = 0.01f;
//   the output fader, in dB
static const float K_OUT_MIN_DB = -36.f, K_OUT_MAX_DB = 18.f;

// ------------------------------------------------------- knob settings ---
//
// Where inside those ranges the knobs sit: the **median of the 48 factory
// snapshots**, which decoded on 2026-08-10. Not guesses any more.
//
// A first attempt at this produced a sub-bass rumble, because the controls
// were being matched to the snapshot's values by *param index* order. The
// file writes them in **module order**, and the two differ -- by index the
// bandpass generator's knobs come before the multimode one's, in the file it
// is the other way round. Matched in module order, 42 knobs line up against
// 42 values in every one of the 48 presets and the numbers come out musical.
//
// One deviation, forced: the factory median for `long` is pitch -50, a delay
// of 2.2 seconds, and sixteen lines that long want seven megabytes. Capped
// here at pitch -46, 1.75 s.
//
// The flow pairs are the ensemble's channel 0 and channel 1, read off the
// `flow` macro's child order, and they say the same thing the manual does:
// modulation rises to the right (FM 2.1 to 12.2, AM 2.0 to 4.05) while
// resonance falls slightly (0.73 to 0.42), which is the bifurcation the Max
// porter found and could not explain. Where three tone generators disagree
// the median of the three is used.
static const float SET_PITCH_LO   = -60.f;    // 13.8 Hz
static const float SET_PITCH_HI   = 112.f;    // 6.6 kHz
static const float SET_CUT_LO     = -40.f;    // 0.81 Hz
static const float SET_CUT_HI     = 136.f;    // 21 kHz, the knob's own top
static const float SET_DEL_SHORT  = 136.f;    // 0.047 ms
static const float SET_DEL_LONG   = -46.f;    // 1.75 s (capped, see below)
static const float SET_FM_LO  = 2.1f,   SET_FM_HI  = 12.2f;
static const float SET_AM_LO  = 2.0f,   SET_AM_HI  = 4.05f;
static const float SET_RES_LO = 0.73f,  SET_RES_HI = 0.42f;
static const float SET_SMT_LO = 1.0f,   SET_SMT_HI = 20.0f;
static const float SET_NRM_LO = 1.0f,   SET_NRM_HI = 0.50f;

// Reaktor's Expon.(P) and Log.(F), which the ensemble uses everywhere a
// frequency is set: pitch in, hertz out, and back.
inline float pitchToHz(float p) {
    return 440.f * std::exp2((p - 69.f) * (1.f / 12.f));
}
inline float hzToPitch(float f) {
    return 69.f + 12.f * std::log2(f / 440.f);
}

// The `shaper` macro, which is what a master knob actually does to a bar.
// Three curves -- v^4, v, the fourth root of v -- with the knob blending
// between them: hard left crushes the bank low and only the tallest bars
// survive, centre passes the bars through, hard right lifts them all.
inline float shape(float v, float pos) {
    if (v <= 0.f) return 0.f;
    if (v >= 1.f) return 1.f;
    const float v4 = v * v * v * v;
    const float vq = std::sqrt(std::sqrt(v));
    const float p = pos * 2.f;
    if (p <= 1.f) return v4 + (v - v4) * p;
    return v + (vq - v) * (p - 1.f);
}

// 2^x to about six digits, which is far more than a modulation ratio needs
// and a good deal cheaper than the libm call this does sixteen times a sample.
inline float fastExp2(float x) {
    if (x < -60.f) x = -60.f;
    if (x >  60.f) x =  60.f;
    const float xi = std::floor(x);
    const float f = x - xi;
    const float p = 1.f + f * (0.6931472f + f * (0.2402265f +
                    f * (0.0555041f + f * 0.0096181f)));
    union { float f; int32_t i; } u;
    u.i = ((int32_t)xi + 127) << 23;
    return p * u.f;
}

inline float fastTanh(float x) {
    if (x < -3.f) return -1.f;
    if (x >  3.f) return  1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// One polyBLEP correction at a discontinuity.
inline float polyBlep(float t, float dt) {
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

// ---------------------------------------------------------------- simd ---
//
// The sixteen levers only read each other through `yPrev`, which is frozen for
// the whole sample, so within a sample they are sixteen independent chains.
// Measured, the engine is throughput-bound rather than latency-bound -- two
// engines cost 2.07x one, and swapping a divide for a polynomial made it
// *slower*, because a divide is one uop and a polynomial is four. The only
// thing that helps is doing fewer, wider operations, so the whole inner loop
// runs four levers at a time.
//
// Four lanes is one group of two voices: group g holds levers 4g..4g+3, which
// is voice 2g lever 0, voice 2g lever 1, voice 2g+1 lever 0, voice 2g+1 lever
// 1. So lanes 0 and 2 are always the left output and lanes 1 and 3 the right,
// and a voice's control values land in a lane pair.
//
// GCC vector extensions rather than intrinsics: gcc, clang and the MinGW and
// arm64 builds all take them, and there is no second scalar code path to keep
// in step (which for a chaotic engine would mean two different sounds).
typedef float  f4 __attribute__((vector_size(16)));
typedef int32_t i4 __attribute__((vector_size(16)));
// the same thing, but allowed to alias the float arrays it is loaded from
typedef float  f4a __attribute__((vector_size(16), may_alias));

static const int NGRP = NLEV / 4;

inline f4 f4set(float a) { return (f4){a, a, a, a}; }

// lane-wise "m ? a : b", with m the all-ones/all-zeros mask a comparison gives
inline f4 f4sel(i4 m, f4 a, f4 b) {
    return (f4)((m & (i4)a) | (~m & (i4)b));
}
inline f4 f4min(f4 a, f4 b) { return f4sel(a < b, a, b); }
inline f4 f4max(f4 a, f4 b) { return f4sel(a > b, a, b); }
inline f4 f4abs(f4 a) { return f4max(a, -a); }
inline f4 f4clamp(f4 x, float lo, float hi) {
    return f4min(f4max(x, f4set(lo)), f4set(hi));
}

// truncation towards zero, then a step down for the negatives that needed it
inline f4 f4floor(f4 x) {
    const f4 t = __builtin_convertvector(__builtin_convertvector(x, i4), f4);
    return f4sel(t > x, t - f4set(1.f), t);
}

// 2^x, four at a time, to the same polynomial as the scalar one above.
inline f4 f4exp2(f4 x) {
    x = f4clamp(x, -60.f, 60.f);
    const f4 xi = f4floor(x);
    const f4 f = x - xi;
    const f4 p = f4set(1.f) + f * (f4set(0.6931472f) + f * (f4set(0.2402265f) +
                 f * (f4set(0.0555041f) + f * f4set(0.0096181f))));
    const i4 e = (__builtin_convertvector(xi, i4) + 127) << 23;
    return p * (f4)e;
}

inline f4 f4tanh(f4 x) {
    const f4 c = f4clamp(x, -3.f, 3.f);
    const f4 x2 = c * c;
    return c * (f4set(27.f) + x2) / (f4set(27.f) + f4set(9.f) * x2);
}

// One polyBLEP correction at a discontinuity, branchless so it vectorises.
inline f4 f4polyBlep(f4 t, f4 dt) {
    const i4 lo = t < dt;
    const i4 hi = t > (f4set(1.f) - dt);
    const f4 a = t / dt;
    const f4 b = (t - f4set(1.f)) / dt;
    const f4 va = a + a - a * a - f4set(1.f);
    const f4 vb = b * b + b + b + f4set(1.f);
    return f4sel(lo, va, f4sel(hi, vb, f4set(0.f)));
}

// Stand-in for REAKTOR's Multi 2-Pole, whose insides are closed: a
// topology-preserving state variable filter with the integrator states
// soft-limited. Three simultaneous outputs, cutoff as a pitch, resonance
// 0..1 with 1 at self-oscillation, and -- as the reference specifies for the
// original -- unity pass-band gain. The saturation is scrupea's, and it is the
// point: a linear filter in this loop just rings, a saturating one folds the
// loop's trajectory back on itself and the bank goes chaotic.
struct SatSVF {
    f4 ic1 = {}, ic2 = {};
    f4 lp = {}, bp = {}, hp = {};

    void reset() { ic1 = ic2 = lp = bp = hp = f4set(0.f); }

    // The three coefficients are functions of g = tan(pi*fc/sr) and k = 1/Q,
    // both of which only move at control rate, so they are worked out once per
    // control tick in Coefs rather than once per sample here.
    struct Coefs {
        f4 a1 = {}, a2 = {}, a3 = {}, k = {};
        // one voice's g into a lane pair, since a group is two voices
        void set(float gA, float gB, float kk) {
            const f4 g = (f4){gA, gA, gB, gB};
            a1 = f4set(1.f) / (f4set(1.f) + g * (g + f4set(kk)));
            a2 = g * a1;
            a3 = g * a2;
            k = f4set(kk);
        }
    };

    void process(f4 v0, const Coefs& c) {
        const f4 v3 = v0 - ic2;
        bp = c.a1 * ic1 + c.a2 * v3;
        lp = ic2 + c.a2 * ic1 + c.a3 * v3;
        hp = v0 - c.k * bp - lp;
        ic1 = f4tanh(bp + bp - ic1);
        ic2 = f4tanh(lp + lp - ic2);
    }

    // The `lbh` bar, doubled, is the Pos of a Selector over the filter's
    // three outputs in the order LP, BP, HP -- which is what the letters of
    // its name stand for.
    f4 morph(f4 typ) const {
        const f4 one = f4set(1.f);
        return f4sel(typ <= one, lp + (bp - lp) * typ,
                                 bp + (hp - bp) * (typ - one));
    }
};

// The normalizer inside each loop, module for module as the ensemble wires
// it: a Peak Detector, a Clipper holding the envelope between `nrm` and a
// constant 300, a one-pole smoother, and the signal divided by the result.
// A real normalizer -- divide by your own envelope -- and the Clipper's floor
// is what keeps it from flattening the bank: an envelope below it is not
// tracked, so a quiet loop is scaled rather than dragged up to full. It also
// bounds the loop: whatever goes in, what comes out cannot exceed 1, which is
// why a feedback bar at the top of its travel is safe and needs no limiter.
struct Normalizer {
    f4 env = {}, sm = {};

    void reset() { env = f4set(0.f); sm = f4set(1.f); }

    // `relCoef` and `smCoef` are not constants here. In the ensemble both
    // times come out of the `smooth` macro as the channel's own delay time
    // multiplied by `smt`, so a long loop gets a slow normalizer and a short
    // one a fast one.
    f4 process(f4 x, float nrm, float relCoef, float smCoef) {
        const f4 a = f4abs(x);
        env = f4max(a, env * f4set(relCoef));   // Peak Detector, zero attack
        f4 e = f4clamp(env, nrm, 300.f);        // Clipper: Max, Min, In
        sm += (e - sm) * f4set(smCoef);         // HP/LP 1-Pole
        sm = f4max(sm, f4set(1e-6f));
        return x / sm;                          // Divide
    }
};

// One lever's delay line. Linear interpolation on the read: the delay time
// is swept constantly here and anything higher-order crackles when it moves.
struct DelayLine {
    std::vector<float> buf;
    int mask = 0, w = 0;

    void setSize(int n) {
        int size = 1;
        while (size < n) size <<= 1;
        buf.assign(size, 0.f);
        mask = size - 1;
        w = 0;
    }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; }

    float read(float d) const {
        if (d < 1.f) d = 1.f;
        if (d > (float)mask - 1.f) d = (float)mask - 1.f;
        const int i = (int)d;
        const float f = d - (float)i;
        const float a = buf[(w - i) & mask];
        const float b = buf[(w - i - 1) & mask];
        return a + (b - a) * f;
    }
    void write(float x) {
        w = (w + 1) & mask;
        buf[w] = x;
    }
};

// A one-pole smoother whose cutoff moves. The ensemble smooths F and DEL this
// way before either reaches a lever, and clamps the delay one hard.
struct Glide {
    float y = 0.f;
    void reset(float v) { y = v; }
    void step(float target, float coef) { y += (target - y) * coef; }
};

// Per-voice control-rate targets, in the ensemble's own units.
struct Targets {
    float hz[NCH];     // oscillator frequency, from `osc F`
    float amp[NCH];    // oscillator amplitude, the `A` bar, 0..1
    float cutHz[NCH];  // multimode cutoff / the HP corner of the bandpass
    float lpHz[NCH];   // the LP corner of the bandpass
    float typ[NCH];    // `lbh` doubled: 0 low, 1 band, 2 high
    float delMs[NCH];  // delay time in milliseconds, from `- P - Ms -`
    float fbk[NCH];    // `FB`, 0..1
    float fmPos[NCH];  // which voice modulates this one, 0..NCH
    float amPos[NCH];
    // the five values `flow` crossfades, one set for the whole bank
    float fmDepth;     // 1..16, a frequency *ratio*
    float amDepth;     // 0..5
    float res;         // 0..1
    float smt;         // 1..20
    float nrm;         // 1..0.01
};

struct Engine {
    float sr = 48000.f;
    int topology = TOPO_LOOP;

    // Smoothed running values. These are per **voice**, not per lever: the
    // ensemble feeds both levers of a voice from the same bars through the
    // same smoother, so the two can never hold different values and keeping
    // sixteen copies of eight numbers only cost cache.
    float hz[NCH], delSmp[NCH], fbk[NCH], typ[NCH], fmP[NCH], amP[NCH],
          amp[NCH];
    // filter coefficients, worked out once per control tick, one set per
    // four-lever group (which is two voices)
    SatSVF::Coefs cutC[NGRP], lpC[NGRP];
    float cutG[NCH], lpG[NCH];
    float fmLog = 0.f, amD = 0.f, filtK = 2.f;
    float relCoef = 0.999f, smCoef = 0.01f;
    // `nrm`, the Clipper's Min inside every normalizer, set by flow.
    float curNrm = 0.5f;
    alignas(16) float phase[NLEV];

    alignas(16) float y[NLEV];      // each lever's output, this sample
    alignas(16) float yPrev[NLEV];  // ...and the previous, what couplings read
    SatSVF fa[NGRP], fb2[NGRP];
    Normalizer norm[NGRP];
    DelayLine line[NLEV];

    float dcxL = 0.f, dcyL = 0.f, dcxR = 0.f, dcyR = 0.f;
    float cvLp = 0.f;

    // The Lissajous taps, which are not L and R and are not the mix at all.
    // Every tone generator carries `X` and `Y` outputs beside `L` and `R`,
    // each a Selector fed by one lever and scaled by `scX` / `scY` from the
    // panel's XY pad -- whose own tooltip in the file says it "scales the
    // Lissajous display". And they are **poly**: Reaktor hands the display a
    // per-voice signal and it plots one point per voice, eight figures rather
    // than one. That is why the display's own record gives its Min and Max as
    // -1.2 and +1.2, which is one lever's range and nowhere near the range of
    // an eight-voice sum: each normalizer bounds its lever to 1, so a calm
    // voice traces a curve inside the frame and a chaotic one fills it out to
    // the edges and squares off against them.
    float scX = 1.f, scY = 1.f;
    float scopeVX[NCH], scopeVY[NCH];

    // The frame the ensemble's display declares.
    static constexpr float SCOPE_RANGE = 1.2f;

    Engine() {
        setSampleRate(48000.f);
        reset();
    }

    void setSampleRate(float rate) {
        sr = rate;
        // `long` at the top of its travel is a 22 second delay; the setting
        // this module uses is 1.75 s.
        const int n = (int)(1.8f * sr) + 8;
        for (int i = 0; i < NLEV; i++)
            line[i].setSize(n);
    }

    void reset() {
        for (int c = 0; c < NCH; c++) {
            hz[c] = 110.f; cutG[c] = 0.1f; lpG[c] = 0.3f; delSmp[c] = 1000.f;
            fbk[c] = 0.f; typ[c] = 0.f; amp[c] = 0.f;
            fmP[c] = (float)c; amP[c] = (float)c;
        }
        for (int g = 0; g < NGRP; g++) {
            cutC[g].set(cutG[2*g], cutG[2*g+1], 2.f);
            lpC[g].set(lpG[2*g], lpG[2*g+1], 2.f);
            fa[g].reset();
            fb2[g].reset();
            norm[g].reset();
        }
        for (int i = 0; i < NLEV; i++) {
            // Start the phases spread out. Identical phases are a fixed point
            // of the coupled bank and it would take a while to leave them.
            phase[i] = (float)i / (float)NLEV;
            y[i] = yPrev[i] = 0.f;
            line[i].reset();
        }
        dcxL = dcyL = dcxR = dcyR = 0.f;
        cvLp = 0.f;
        for (int c = 0; c < NCH; c++) scopeVX[c] = scopeVY[c] = 0.f;
    }

    static float onePoleCoef(float fc, float sr) {
        if (fc < 0.001f) fc = 0.001f;
        if (fc > 0.45f * sr) fc = 0.45f * sr;
        return 1.f - std::exp(-6.2831853f * fc / sr);
    }

    // Glide the running values towards the control-rate targets. The
    // coefficients are the ensemble's: pitch is smoothed at its own frequency
    // over `smt`, delay time at 500 * smt / delay, clamped by an Event
    // Clipper to the pitch range -80..0, which is 0.081 Hz to 8.18 Hz.
    void glide(const Targets& t, int frames) {
        const float step = (float)frames / sr;

        fmLog = std::log2(std::max(t.fmDepth, 1e-3f));
        amD = t.amDepth;
        filtK = std::max(2.f * (1.f - t.res), 0.02f);
        curNrm = std::max(t.nrm, 1e-4f);

        // Both normalizer times are the lever's delay multiplied by `smt`.
        // They are per-lever in the ensemble; one delay time dominates the
        // bank, so they are computed here from the mean.
        float meanDel = 0.f;
        for (int c = 0; c < NCH; c++) meanDel += t.delMs[c];
        meanDel = std::max(meanDel / (float)NCH, 0.01f);
        const float tau = meanDel * t.smt;                 // milliseconds
        relCoef = std::exp(-2.302585093f / (tau * 0.001f * sr));  // ln 10 per tau
        smCoef  = onePoleCoef(1000.f / tau, sr);

        for (int ch = 0; ch < NCH; ch++) {
            // the two smoothers, from the `smooth` macro
            const float fCoef = 1.f - std::exp(
                -6.2831853f * std::max(t.hz[ch] / t.smt, 0.01f) * step);
            float dPitch = hzToPitch(500.f * t.smt / std::max(t.delMs[ch], 0.01f));
            if (dPitch < -80.f) dPitch = -80.f;
            if (dPitch > 0.f) dPitch = 0.f;
            const float dCoef = 1.f - std::exp(
                -6.2831853f * pitchToHz(dPitch) * step);
            // everything else has no smoother of its own in the ensemble;
            // give it the delay's, which is the slow one
            const float c = dCoef;

            const float gCut = std::tan(3.14159265f *
                std::min(t.cutHz[ch], 0.45f * sr) / sr);
            const float gLp = std::tan(3.14159265f *
                std::min(t.lpHz[ch], 0.45f * sr) / sr);

            hz[ch]     += (t.hz[ch]     - hz[ch])     * fCoef;
            delSmp[ch] += (t.delMs[ch] * 0.001f * sr - delSmp[ch]) * dCoef;
            cutG[ch]   += (gCut         - cutG[ch])   * c;
            lpG[ch]    += (gLp          - lpG[ch])    * c;
            fbk[ch]    += (t.fbk[ch]    - fbk[ch])    * c;
            amp[ch]    += (t.amp[ch]    - amp[ch])    * c;
            typ[ch]    += (t.typ[ch]    - typ[ch])    * c;
            fmP[ch]    += (t.fmPos[ch]  - fmP[ch])    * c;
            amP[ch]    += (t.amPos[ch]  - amP[ch])    * c;

        }
        for (int g = 0; g < NGRP; g++) {
            cutC[g].set(cutG[2*g], cutG[2*g+1], filtK);
            lpC[g].set(lpG[2*g], lpG[2*g+1], filtK);
        }
    }

    void process(float in, float* outL, float* outR, float* cv) {
        std::memcpy(yPrev, y, sizeof(yPrev));

        const float sT = 1.f / sr;
        const f4 vIn = f4set(in);
        // lanes 0,1 are voice 2g and lanes 2,3 voice 2g+1, so the alternating
        // sign the chaos CV wants is the same constant in every group
        static const f4 cvSign = {1.f, 1.f, -1.f, -1.f};
        f4 accL = {}, accR = {}, accY = {};

        for (int g = 0; g < NGRP; g++) {
            const int c0 = 2 * g, c1 = c0 + 1, base = 4 * g;

            // a voice's control value fills its lane pair
            const f4 vHz  = {hz[c0], hz[c0], hz[c1], hz[c1]};
            const f4 vAmp = {amp[c0], amp[c0], amp[c1], amp[c1]};
            const f4 vFbk = {fbk[c0], fbk[c0], fbk[c1], fbk[c1]};
            const f4 vTyp = {typ[c0], typ[c0], typ[c1], typ[c1]};

            // crossvoice: the Selector's index arithmetic belongs to the
            // voice, and the two levers of a pair differ only in which slot of
            // the chosen voice they take -- lane 0 and 2 read slot 1, lanes 1
            // and 3 read slot 0.
            float mfv[4], mav[4];
            for (int q = 0; q < 2; q++) {
                const int ch = c0 + q;
                float pf = fmP[ch];
                if (pf < 0.f) pf = 0.f;
                if (pf > (float)(NCH - 1)) pf = (float)(NCH - 1);
                const int f0 = (int)pf, f1 = f0 < NCH - 1 ? f0 + 1 : f0;
                const float ff = pf - (float)f0;
                float pa = amP[ch];
                if (pa < 0.f) pa = 0.f;
                if (pa > (float)(NCH - 1)) pa = (float)(NCH - 1);
                const int a0 = (int)pa, a1 = a0 < NCH - 1 ? a0 + 1 : a0;
                const float af = pa - (float)a0;
                for (int k = 0; k < LPC; k++) {
                    const int slot = 1 - k;
                    const float fA = yPrev[lev(f0, slot)], fB = yPrev[lev(f1, slot)];
                    const float aA = yPrev[lev(a0, slot)], aB = yPrev[lev(a1, slot)];
                    mfv[2 * q + k] = fA + (fB - fA) * ff;
                    mav[2 * q + k] = aA + (aB - aA) * af;
                }
            }
            const f4 mf = {mfv[0], mfv[1], mfv[2], mfv[3]};
            const f4 ma = {mav[0], mav[1], mav[2], mav[3]};

            // FM as the ensemble does it, which is neither of the two obvious
            // things. The depth is a frequency *ratio* between 1 and 16, and
            // the modulator picks a point on a geometric interpolation between
            // its reciprocal and itself, so the frequency is multiplied by
            // ratio^m with m running -1 to 1. At depth 1 there is no
            // modulation at all, at 16 it is four octaves either way. In the
            // file this is a Reciprocal and two Log modules into a Selector
            // whose Pos is the modulator halved and offset, then an Exp; the
            // whole thing multiplies the oscillator's linear F input while P
            // sits pinned at -300.
            const f4 freq = vHz * f4exp2(f4set(fmLog) * mf);
            const f4 inc = f4clamp(freq * f4set(sT), -0.45f, 0.45f);

            // the increment is bounded, so the wrap is a pair of selects and
            // never needs a floor
            f4 ph = (f4)(*(const f4a*)&phase[base]) + inc;
            ph = f4sel(ph < f4set(0.f), ph + f4set(1.f), ph);
            ph = f4sel(ph >= f4set(1.f), ph - f4set(1.f), ph);
            *(f4a*)&phase[base] = (f4a)ph;
            const f4 dt = f4abs(inc) + f4set(1e-9f);

            f4 osc;
            if (topology == TOPO_BARE) {
                // Parabolic wave: smooth, and much fatter at the bottom than
                // the pulse, which is what makes this the calm one.
                const f4 x = ph + ph - f4set(1.f);
                osc = f4set(-4.f) * x * (f4abs(x) - f4set(1.f));
            }
            else {
                f4 p2 = ph + f4set(0.5f);
                p2 = f4sel(p2 >= f4set(1.f), p2 - f4set(1.f), p2);
                // Band-limited, as REAKTOR's own oscillators are: the module
                // reference notes that waveforms with strong transients "have
                // anti-aliasing in REAKTOR".
                osc = f4sel(ph < f4set(0.5f), f4set(1.f), f4set(-1.f));
                osc += f4polyBlep(ph, dt);
                osc -= f4polyBlep(p2, dt);
            }

            // Amplitude is A * (1 + am * AM): a Mult/Add taking the AM signal,
            // the A bar times the am depth, and the A bar. With am up past 1
            // the product goes negative and it is ring modulation, which is
            // where a lot of the harshness lives.
            osc *= vAmp * (f4set(1.f) + f4set(amD) * ma);

            // The loop, in the order the ensemble wires it. The delay comes
            // *before* the normalizer, and the normalizer's output is both
            // what the lever puts out and what feeds back, so the oscillator
            // is never heard directly. Where the filter sits is what separates
            // the three tone generators. The lines are the one part that stays
            // scalar: four buffers, two read offsets, no way to widen it.
            const float dA = delSmp[c0], dB = delSmp[c1];
            const f4 d = {line[base + 0].read(dA), line[base + 1].read(dA),
                          line[base + 2].read(dB), line[base + 3].read(dB)};
            f4 r = norm[g].process(d, curNrm, relCoef, smCoef);

            const f4 sIn = osc + vIn;
            f4 sum;
            if (topology == TOPO_PRE) {
                // osc -> multimode filter -> summer -> delay
                fa[g].process(sIn, cutC[g]);
                sum = fa[g].morph(vTyp) + r * vFbk;
            }
            else if (topology == TOPO_BARE) {
                sum = sIn + r * vFbk;
            }
            else {
                // summer -> 2-pole HP -> 2-pole LP -> delay, the pair inside
                // the loop and both taking the same resonance
                fa[g].process(sIn + r * vFbk, cutC[g]);
                fb2[g].process(fa[g].hp, lpC[g]);
                sum = fb2[g].lp;
            }

            // A lane that has gone non-finite takes its whole group's state
            // down with it, which is the cheap thing to do and happens about
            // never.
            const f4 chk = sum + r;
            if (!std::isfinite(chk[0] + chk[1] + chk[2] + chk[3])) {
                sum = r = f4set(0.f);
                fa[g].reset();
                fb2[g].reset();
                norm[g].reset();
                for (int q = 0; q < 4; q++) line[base + q].reset();
            }

            for (int q = 0; q < 4; q++) line[base + q].write(sum[q]);
            *(f4a*)&y[base] = (f4a)r;

            // The tone generator's L output is its first lever and R its
            // second; Reaktor sums the eight voices into each. Lanes 0 and 2
            // are firsts, lanes 1 and 3 seconds.
            accL += r;
            accR += r;
            accY += r * cvSign;
        }

        const float sumL = accL[0] + accL[2];
        const float sumR = accR[1] + accR[3];
        const float sumY = accY[0] + accY[1] + accY[2] + accY[3];

        // DC blockers. Not in the ensemble -- neither is a patch cable, and
        // REAKTOR's audio interface does not have to keep a Rack rail clean.
        const float rc = 1.f - 20.f / sr;
        dcyL = sumL - dcxL + rc * dcyL; dcxL = sumL;
        dcyR = sumR - dcxR + rc * dcyR; dcxR = sumR;

        *outL = dcyL;
        *outR = dcyR;

        for (int c = 0; c < NCH; c++) {
            scopeVX[c] = y[lev(c, 0)] * scX;
            scopeVY[c] = y[lev(c, 1)] * scY;
        }

        // A slow read of the bank's own wandering, for patching out. The
        // levers are summed with alternating sign so the common motion
        // cancels and what is left is how unevenly the eight are behaving.
        cvLp += (sumY - cvLp) * (25.f * 6.2831853f / sr);
        *cv = cvLp;
    }
};

} // namespace scrupea_dsp
