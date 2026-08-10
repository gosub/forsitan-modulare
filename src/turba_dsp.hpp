// turba_dsp.hpp -- the eight-channel chaotic engine behind turba.
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
// only those, are turba's choice.
//
// Header-only and free of Rack types so the offline harnesses in test/ can
// drive it directly.
#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

namespace turba_dsp {

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
// Where inside those ranges this module leaves each knob. Not recoverable
// from the file. The pitch, cutoff and delay endpoints are the ones turba
// measured its way to before the ensemble was read, expressed in the
// ensemble's units; the flow endpoints are the knob extremes except for RES,
// which is set to reproduce a measured bifurcation (see doc/turba.md).
static const float SET_PITCH_LO   =   0.f;    // 8.18 Hz
static const float SET_PITCH_HI   = 132.f;    // 16.6 kHz
static const float SET_CUT_LO     =  15.f;    // 20 Hz
static const float SET_CUT_HI     = 135.f;    // 20 kHz
static const float SET_DEL_SHORT  = 116.f;    // 0.15 ms
static const float SET_DEL_LONG   = -16.f;    // 312 ms
static const float SET_FM_LO  = K_FM_MIN,  SET_FM_HI  = K_FM_MAX;
static const float SET_AM_LO  = K_AM_MIN,  SET_AM_HI  = K_AM_MAX;
static const float SET_RES_LO = 0.95f,     SET_RES_HI = 0.30f;
static const float SET_SMT_LO = K_SMT_MAX, SET_SMT_HI = K_SMT_MIN;
static const float SET_NRM_LO = K_NRM_MIN, SET_NRM_HI = K_NRM_MAX;

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

// Stand-in for REAKTOR's Multi 2-Pole, whose insides are closed: a
// topology-preserving state variable filter with the integrator states
// soft-limited. Three simultaneous outputs, cutoff as a pitch, resonance
// 0..1 with 1 at self-oscillation, and -- as the reference specifies for the
// original -- unity pass-band gain. The saturation is turba's, and it is the
// point: a linear filter in this loop just rings, a saturating one folds the
// loop's trajectory back on itself and the bank goes chaotic.
struct SatSVF {
    float ic1 = 0.f, ic2 = 0.f;
    float lp = 0.f, bp = 0.f, hp = 0.f;

    void reset() { ic1 = ic2 = lp = bp = hp = 0.f; }

    // g = tan(pi * fc / sr), k = 1/Q.
    void process(float v0, float g, float k) {
        const float a1 = 1.f / (1.f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = v0 - ic2;
        bp = a1 * ic1 + a2 * v3;
        lp = ic2 + a2 * ic1 + a3 * v3;
        hp = v0 - k * bp - lp;
        ic1 = fastTanh(2.f * bp - ic1);
        ic2 = fastTanh(2.f * lp - ic2);
    }

    // The `lbh` bar, doubled, is the Pos of a Selector over the filter's
    // three outputs in the order LP, BP, HP -- which is what the letters of
    // its name stand for.
    float morph(float typ) const {
        if (typ <= 1.f) return lp + (bp - lp) * typ;
        return bp + (hp - bp) * (typ - 1.f);
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
    float env = 0.f, sm = 1.f;

    void reset() { env = 0.f; sm = 1.f; }

    // `relCoef` and `smCoef` are not constants here. In the ensemble both
    // times come out of the `smooth` macro as the channel's own delay time
    // multiplied by `smt`, so a long loop gets a slow normalizer and a short
    // one a fast one.
    float process(float x, float nrm, float relCoef, float smCoef) {
        const float a = std::fabs(x);
        env = a > env ? a : env * relCoef;          // Peak Detector, zero attack
        float e = env;                               // Clipper: Max, Min, In
        if (e < nrm) e = nrm;
        if (e > 300.f) e = 300.f;
        sm += (e - sm) * smCoef;                     // HP/LP 1-Pole
        if (sm < 1e-6f) sm = 1e-6f;
        return x / sm;                               // Divide
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

    // smoothed running values, one per lever
    float hz[NLEV], cutG[NLEV], lpG[NLEV], delSmp[NLEV], fbk[NLEV],
          typ[NLEV], fmP[NLEV], amP[NLEV], amp[NLEV];
    float fmLog = 0.f, amD = 0.f, filtK = 2.f;
    float relCoef = 0.999f, smCoef = 0.01f;
    // `nrm`, the Clipper's Min inside every normalizer, set by flow.
    float curNrm = 0.5f;
    float phase[NLEV];

    float y[NLEV];       // each lever's output, this sample
    float yPrev[NLEV];   // ...and the previous one, what the couplings read
    SatSVF fa[NLEV], fb2[NLEV];
    Normalizer norm[NLEV];
    DelayLine line[NLEV];

    float dcxL = 0.f, dcyL = 0.f, dcxR = 0.f, dcyR = 0.f;
    float cvLp = 0.f;

    Engine() {
        setSampleRate(48000.f);
        reset();
    }

    void setSampleRate(float rate) {
        sr = rate;
        // `long` at the top of its travel is a 22 second delay; the setting
        // this module uses is 312 ms, and half a second of line covers it.
        const int n = (int)(0.5f * sr) + 8;
        for (int i = 0; i < NLEV; i++)
            line[i].setSize(n);
    }

    void reset() {
        for (int i = 0; i < NLEV; i++) {
            hz[i] = 110.f; cutG[i] = 0.1f; lpG[i] = 0.3f; delSmp[i] = 1000.f;
            fbk[i] = 0.f; typ[i] = 0.f; amp[i] = 0.f;
            fmP[i] = (float)(i / LPC); amP[i] = (float)(i / LPC);
            // Start the phases spread out. Identical phases are a fixed point
            // of the coupled bank and it would take a while to leave them.
            phase[i] = (float)i / (float)NLEV;
            y[i] = yPrev[i] = 0.f;
            fa[i].reset();
            fb2[i].reset();
            norm[i].reset();
            line[i].reset();
        }
        dcxL = dcyL = dcxR = dcyR = 0.f;
        cvLp = 0.f;
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

            for (int k = 0; k < LPC; k++) {
                const int i = lev(ch, k);
                hz[i]     += (t.hz[ch]     - hz[i])     * fCoef;
                delSmp[i] += (t.delMs[ch] * 0.001f * sr - delSmp[i]) * dCoef;
                cutG[i]   += (gCut         - cutG[i])   * c;
                lpG[i]    += (gLp          - lpG[i])    * c;
                fbk[i]    += (t.fbk[ch]    - fbk[i])    * c;
                amp[i]    += (t.amp[ch]    - amp[i])    * c;
                typ[i]    += (t.typ[ch]    - typ[i])    * c;
                fmP[i]    += (t.fmPos[ch]  - fmP[i])    * c;
                amP[i]    += (t.amPos[ch]  - amP[i])    * c;
            }
        }
    }

    // Reaktor's Selector: an integer Pos forwards that channel, a fractional
    // one blends the two either side of it. `crossvoice` multiplies the bar
    // by a constant 8 before it gets here, so the top eighth of a bar's
    // travel all lands on the last voice.
    inline float selectVoice(float pos, int slot) const {
        if (pos < 0.f) pos = 0.f;
        if (pos > (float)(NCH - 1)) pos = (float)(NCH - 1);
        const int i0 = (int)pos;
        const int i1 = i0 < NCH - 1 ? i0 + 1 : i0;
        const float f = pos - (float)i0;
        const float a = yPrev[lev(i0, slot)];
        const float b = yPrev[lev(i1, slot)];
        return a + (b - a) * f;
    }

    void process(float in, float* outL, float* outR, float* cv) {
        std::memcpy(yPrev, y, sizeof(yPrev));

        float sumL = 0.f, sumR = 0.f, sumY = 0.f;
        const float sT = 1.f / sr;

        for (int ch = 0; ch < NCH; ch++) {
            for (int k = 0; k < LPC; k++) {
                const int i = lev(ch, k);

                // crossvoice: eight From Voice modules into a Selector whose
                // Pos is the fm (or am) bar. The bar does not set how hard
                // this lever is modulated, it picks **which voice modulates
                // it**; depth comes from flow, one value for the whole bank.
                // The source is the partner lever of the chosen voice:
                // crossed within the pair, selected across voices.
                const int slot = 1 - k;
                const float mf = selectVoice(fmP[i], slot);
                const float ma = selectVoice(amP[i], slot);

                // FM as the ensemble does it, which is neither of the two
                // obvious things. The fm depth is a frequency *ratio* between
                // 1 and 16, and the modulator picks a point on a geometric
                // interpolation between its reciprocal and itself: the
                // oscillator's frequency is multiplied by ratio^m, m running
                // -1 to 1. At depth 1 there is no modulation at all, at 16
                // it is four octaves either way. In the file this is a
                // Reciprocal and two Log modules into a Selector whose Pos is
                // the modulator halved and offset, then an Exp; the whole
                // thing multiplies the oscillator's linear F input while P
                // sits pinned at -300.
                const float freq = hz[i] * std::exp2(fmLog * mf);
                float inc = freq * sT;
                if (inc >  0.45f) inc =  0.45f;
                if (inc < -0.45f) inc = -0.45f;

                phase[i] += inc;
                phase[i] -= std::floor(phase[i]);   // correct for inc < 0
                const float dt = std::fabs(inc) + 1e-9f;

                float osc;
                if (topology == TOPO_BARE) {
                    // Parabolic wave: smooth, and much fatter at the bottom
                    // than the pulse, which is what makes this the calm one.
                    const float x = 2.f * phase[i] - 1.f;
                    osc = -4.f * x * (std::fabs(x) - 1.f);
                }
                else {
                    float p2 = phase[i] + 0.5f;
                    if (p2 >= 1.f) p2 -= 1.f;
                    // Band-limited, as REAKTOR's own oscillators are: the
                    // module reference notes that waveforms with strong
                    // transients "have anti-aliasing in REAKTOR".
                    osc = phase[i] < 0.5f ? 1.f : -1.f;
                    osc += polyBlep(phase[i], dt);
                    osc -= polyBlep(p2, dt);
                }

                // Amplitude is A * (1 + am * AM): a Mult/Add taking the AM
                // signal, the A bar times the am depth, and the A bar. With
                // am up past 1 the product goes negative and it is ring
                // modulation, which is where a lot of the harshness lives.
                osc *= amp[i] * (1.f + amD * ma);

                // The loop, in the order the ensemble wires it. The delay
                // comes *before* the normalizer, and the normalizer's output
                // is both what the lever puts out and what feeds back, so the
                // oscillator is never heard directly. Where the filter sits
                // is what separates the three tone generators.
                const float d = line[i].read(delSmp[i]);
                float r = norm[i].process(d, curNrm, relCoef, smCoef);

                float s = osc + in;
                float sum;
                if (topology == TOPO_PRE) {
                    // osc -> multimode filter -> summer -> delay
                    fa[i].process(s, cutG[i], filtK);
                    sum = fa[i].morph(typ[i]) + r * fbk[i];
                }
                else if (topology == TOPO_BARE) {
                    sum = s + r * fbk[i];
                }
                else {
                    // summer -> 2-pole HP -> 2-pole LP -> delay, the pair
                    // inside the loop and both taking the same resonance
                    const float m = s + r * fbk[i];
                    fa[i].process(m, cutG[i], filtK);
                    fb2[i].process(fa[i].hp, lpG[i], filtK);
                    sum = fb2[i].lp;
                }

                if (!std::isfinite(sum) || !std::isfinite(r)) {
                    sum = r = 0.f;
                    fa[i].reset();
                    fb2[i].reset();
                    norm[i].reset();
                    line[i].reset();
                }

                line[i].write(sum);
                y[i] = r;

                // The tone generator's L output is its first lever and R is
                // its second; Reaktor sums the eight voices into each.
                if (k == 0) sumL += r; else sumR += r;
                sumY += (ch & 1) ? -r : r;
            }
        }

        // DC blockers. Not in the ensemble -- neither is a patch cable, and
        // REAKTOR's audio interface does not have to keep a Rack rail clean.
        const float rc = 1.f - 20.f / sr;
        dcyL = sumL - dcxL + rc * dcyL; dcxL = sumL;
        dcyR = sumR - dcxR + rc * dcyR; dcxR = sumR;

        *outL = dcyL;
        *outR = dcyR;

        // A slow read of the bank's own wandering, for patching out. The
        // levers are summed with alternating sign so the common motion
        // cancels and what is left is how unevenly the eight are behaving.
        cvLp += (sumY - cvLp) * (25.f * 6.2831853f / sr);
        *cv = cvLp;
    }
};

} // namespace turba_dsp
