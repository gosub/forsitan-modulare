// turba_dsp.hpp -- the eight-channel chaotic engine behind turba.
//
// Eight independent channels, each an oscillator feeding a feedback delay
// with a normalizer (and, in two of the three topologies, a resonant filter)
// inside or in front of the loop. The channels are not independent for long:
// every channel's oscillator is frequency-modulated by its right-hand
// neighbour's loop signal and amplitude-modulated by its left-hand one, so
// the eight loops form a ring that couples the whole bank together. That ring
// plus the saturating filter is where the chaos comes from.
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

// Each channel is two complete "levers", after the structure of the ensemble
// itself: every tone generator in Skrewell holds exactly two LEVER macros
// with a `crossvoice` between them, and a LEVER is not an oscillator, it is a
// whole channel -- oscillator, filter, resonance, normalizer, delay and
// feedback. So eight channels are sixteen loops, coupled in pairs.
static const int LPC = 2;
static const int NLEV = NCH * LPC;

// Lever `k` of channel `c`.
inline int lev(int c, int k) { return c * LPC + k; }

// Three topologies, after Skrewell's three operation modes. The difference
// is only where the filter sits, and what the oscillator is.
enum Topology {
    TOPO_LOOP = 0,   // pulse osc; filter *inside* the feedback loop
    TOPO_PRE  = 1,   // pulse osc; filter in front of the delay, outside it
    TOPO_BARE = 2,   // parabolic osc; no filter at all
};

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

// Topology-preserving 2-pole state variable filter, with the integrator
// states soft-limited. The saturation is the point: a linear filter in this
// loop just rings, a saturating one folds the loop's trajectory back on
// itself and the bank goes chaotic.
struct SatSVF {
    float ic1 = 0.f, ic2 = 0.f;

    void reset() { ic1 = ic2 = 0.f; }

    // g = tan(pi * fc / sr), k = 1/Q, type 0 = lowpass, 0.5 = band, 1 = high.
    // The morph is the ensemble's `lbh` parameter: in Skrewell the eighth bar
    // of the multimode tone generator sets the filter *type* per channel, not
    // its resonance, so eight channels can sit on eight different slopes.
    float process(float v0, float g, float k, float type) {
        const float a1 = 1.f / (1.f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;   // bandpass
        const float v2 = ic2 + a2 * ic1 + a3 * v3;   // lowpass
        ic1 = fastTanh(2.f * v1 - ic1);
        ic2 = fastTanh(2.f * v2 - ic2);
        if (type <= 0.5f) {
            const float t = type * 2.f;
            return v2 + (v1 - v2) * t;
        }
        const float hp = v0 - k * v1 - v2;
        const float t = (type - 0.5f) * 2.f;
        return v1 + (hp - v1) * t;
    }
};

// The normalizer inside each loop. It only ever turns the loop *down*: a
// channel whose feedback is over unity is held at the ceiling instead of
// exploding, and one under unity is left alone to be as quiet as it wants.
// An earlier version pushed quiet loops back up as well, which is what the
// name suggests, and it was a mistake: with every channel forced to the same
// level the macro knobs stopped making an audible difference. What sustains
// this bank is not the normalizer, it is that the oscillators never stop.
struct Normalizer {
    float env = 0.f, sm = 1.f;
    float relCoef = 0.999f, smCoef = 0.01f;
    // Reaktor's Peak Detector: rectify, "the attack time of peak detection is
    // zero", release quoted as the time for a peak to fall to a tenth of its
    // value -- Rel 0 is 2.3 ms, 20 is 23 ms, 40 is 230 ms, a decade per
    // twenty. So the attack is instantaneous, which is audible: a transient
    // moves the gain on the sample it arrives on.
    float relT = 0.230f;        // Rel = 40
    float smT = 0.020f;         // the one-pole after the clipper

    void reset() { env = 0.f; sm = 1.f; }

    void setSampleRate(float sr) {
        relCoef = std::exp(-2.302585093f / (relT * sr));   // ln(10) per relT
        smCoef  = 1.f - std::exp(-1.f / (smT * sr));
    }

    // The whole macro, in the order the ensemble wires it: peak detector,
    // then a Clipper holding the envelope between `floor` and a constant 300,
    // then a one-pole, and finally the signal divided by the result. It is a
    // real normalizer -- divide by your own envelope -- and not the limiter
    // this module used to have. The floor is what keeps it from being one:
    // an envelope below it is not tracked, so a quiet loop is scaled rather
    // than dragged up to full, and how quiet a loop may stay is exactly what
    // the `nrm` control sets. Flow drives it, as flow drives the rest.
    float process(float x, float floorLevel) {
        const float a = std::fabs(x);
        env = a > env ? a : env * relCoef;          // zero attack
        float e = env;                               // Clipper: Min, Max, In
        if (e < floorLevel) e = floorLevel;
        if (e > 300.f) e = 300.f;
        sm += (e - sm) * smCoef;                     // HP/LP 1-Pole
        if (sm < 1e-6f) sm = 1e-6f;
        return x / sm;                               // Divide
    }
};

// One channel's delay line. Linear interpolation on the read: the delay time
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

// How the second lever of a pair differs from the first. Both are driven by
// the same eight bars -- there is one bar per channel, not one per lever --
// so without an offset the pair would be one loop played twice.
static const float LEVER_B_RATIO = 1.4783f;   // pitch, near a tritone up
static const float LEVER_B_DELAY = 0.734f;    // and a shorter loop

// Per-channel control-rate targets. Everything here is already mapped: the
// module hands over final values, the engine only glides towards them.
struct Targets {
    float oct[NCH];    // pitch as octaves above 8 Hz
    float g[NCH];      // filter coefficient tan(pi*fc/sr)
    float k[NCH];      // filter 1/Q, from flow rather than from a bar
    float typ[NCH];    // filter type, 0 low / 0.5 band / 1 high
    float dly[NCH];    // delay time in samples
    float fbk[NCH];    // loop gain
    float fmPos[NCH];  // which voice modulates this one, 0..NCH-1, fractional
    float amPos[NCH];  // the same for AM
    float fmDepth;     // how hard, from flow -- one value for the whole bank
    float amDepth;
    float lvl[NCH];    // channel level 0..1
};

struct Engine {
    float sr = 48000.f;
    int topology = TOPO_LOOP;

    // smoothed running values, one per channel
    float oct[NLEV], gc[NLEV], kc[NLEV], dly[NLEV], fbk[NLEV], fm[NLEV],
          tp[NLEV], fmP[NLEV], amP[NLEV];
    float fmD = 0.f, amD = 0.f;
    float lvl[NCH];
    float phase[NLEV];

    float y[NLEV];       // each lever's loop output, this sample
    float yPrev[NLEV];   // ...and the previous one, what the ring reads
    SatSVF filt[NLEV];
    Normalizer norm[NLEV];
    DelayLine line[NLEV];
    float panL[NCH], panR[NCH];

    // How quiet a loop is allowed to stay: the Clipper's Min inside the
    // normalizer, driven by flow.
    float normFloor = 0.25f;

    float smooth = 0.01f;   // one-pole coefficient, set from the inertia time
    float dcxL = 0.f, dcyL = 0.f, dcxR = 0.f, dcyR = 0.f;
    float cvLp = 0.f;

    Engine() {
        setSampleRate(48000.f);
        reset();
    }

    void setSampleRate(float rate) {
        sr = rate;
        // 350 ms is the longest delay the module offers, plus a guard.
        const int n = (int)(0.4f * sr) + 8;
        for (int i = 0; i < NLEV; i++) {
            line[i].setSize(n);
            norm[i].setSampleRate(sr);
        }
        setInertia(0.05f);
    }

    void setInertia(float seconds) {
        if (seconds < 1e-4f) seconds = 1e-4f;
        smooth = 1.f - std::exp(-1.f / (seconds * sr));
    }

    void reset() {
        for (int i = 0; i < NLEV; i++) {
            oct[i] = 4.f; gc[i] = 0.1f; kc[i] = 1.f; dly[i] = 1000.f;
            fbk[i] = 0.f; tp[i] = 0.f;
            fmP[i] = (float)(i / LPC); amP[i] = (float)(i / LPC);
            // Start the phases spread out. Identical phases are a fixed point
            // of the coupled ring and the bank would take a while to leave it.
            phase[i] = (float)i / (float)NLEV;
            y[i] = yPrev[i] = 0.f;
            filt[i].reset();
            norm[i].reset();
            line[i].reset();
        }
        for (int c = 0; c < NCH; c++) {
            lvl[c] = 0.f;
            // Equal-power spread across the field, channel 1 hard left.
            const float p = (float)c / (float)(NCH - 1);
            panL[c] = std::cos(p * 1.5707963f);
            panR[c] = std::sin(p * 1.5707963f);
        }
        dcxL = dcyL = dcxR = dcyR = 0.f;
        cvLp = 0.f;
    }

    // Glide the running values towards the control-rate targets. Called once
    // per control tick; the coefficient is per-sample, so a long inertia
    // setting still reaches its target, just later.
    void glide(const Targets& t, int frames) {
        const float c = 1.f - std::pow(1.f - smooth, (float)frames);
        fmD += (t.fmDepth - fmD) * c;
        amD += (t.amDepth - amD) * c;
        for (int ch = 0; ch < NCH; ch++) {
            lvl[ch] += (t.lvl[ch] - lvl[ch]) * c;
            for (int k = 0; k < LPC; k++) {
                const int i = lev(ch, k);
                const float octT = t.oct[ch] +
                    (k ? 0.5637f : 0.f);          // log2(LEVER_B_RATIO)
                const float dlyT = t.dly[ch] * (k ? LEVER_B_DELAY : 1.f);
                oct[i] += (octT      - oct[i]) * c;
                gc[i]  += (t.g[ch]   - gc[i])  * c;
                kc[i]  += (t.k[ch]   - kc[i])  * c;
                dly[i] += (dlyT      - dly[i]) * c;
                fbk[i] += (t.fbk[ch] - fbk[i]) * c;
                fmP[i] += (t.fmPos[ch] - fmP[i]) * c;
                amP[i] += (t.amPos[ch] - amP[i]) * c;
                tp[i]  += (t.typ[ch]   - tp[i])  * c;
            }
        }
    }

    // Reaktor's Selector: an integer Pos forwards that channel, a fractional
    // one blends the two either side of it.
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
        const int levers = LPC;

        for (int ch = 0; ch < NCH; ch++) {
            float voice = 0.f;

            for (int k = 0; k < levers; k++) {
                const int i = lev(ch, k);

                // crossvoice, as the ensemble calls it. The partner is the
                // other lever of this pair and it is the main modulator; the
                // ring on to the next channel is the weaker, second one.
                // crossvoice, read properly. The macro holds eight From
                // Voice modules wired to the *channel* inputs of a Selector,
                // and the fm and am bars drive its Pos. So a bar does not set
                // how hard this lever is modulated, it picks **which channel
                // modulates it**, blending between two neighbours when it
                // sits between them. Depth comes from flow, one value for the
                // whole bank. The source is the partner lever of the chosen
                // channel: crossed within the pair, selected across channels.
                const int slot = 1 - k;
                const float mf = selectVoice(fmP[i], slot);
                const float ma = selectVoice(amP[i], slot);

                // Linear, through-zero FM, which is what the ensemble does
                // and is not a detail. Its oscillators are the FM variants of
                // Reaktor's primary set, whose F input the manual calls
                // "linear frequency control, which is added to the frequency
                // of the P input" -- and P is pinned at -300, a MIDI pitch so
                // low the oscillator would sit at a fraction of a hertz. So
                // every bit of the frequency arrives through F, in hertz, and
                // it can go negative: the oscillator runs backwards through
                // zero rather than bottoming out. Exponential FM, which this
                // module used to do, cannot cross zero and is a far smoother
                // thing.
                const float base = 8.f * std::exp2(oct[i]);
                const float freq = base * (1.f + fmD * mf);
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

                // AM from the partner, never all the way to silence.
                const float amp = 1.f - amD * 0.5f * (1.f - ma);
                float s = osc * amp * 0.5f + in;

                // The loop, in the order the ensemble wires it: the delay
                // comes *before* the normalizer, and the normalizer's output
                // is both what the lever puts out and what feeds back. So the
                // oscillator is never heard directly -- everything reaches
                // the output through the delay line. Where the filter sits is
                // the only difference between the three topologies, and it is
                // the difference the manual describes.
                const float d = line[i].read(dly[i]);
                float r = norm[i].process(d, normFloor);

                float sum;
                if (topology == TOPO_PRE) {
                    // osc -> filter -> summer -> delay
                    s = filt[i].process(s, gc[i], kc[i], tp[i]);
                    sum = s + r * fbk[i];
                }
                else if (topology == TOPO_BARE) {
                    sum = s + r * fbk[i];
                }
                else {
                    // summer -> filter -> delay, the filter inside the loop
                    sum = filt[i].process(s + r * fbk[i], gc[i], kc[i], tp[i]);
                }

                if (!std::isfinite(sum) || !std::isfinite(r)) {
                    sum = r = 0.f;
                    filt[i].reset();
                    norm[i].reset();
                    line[i].reset();
                }

                line[i].write(sum);
                y[i] = r;
                voice += r;
            }
            voice *= 0.5f;

            sumL += voice * lvl[ch] * panL[ch];
            sumR += voice * lvl[ch] * panR[ch];
            sumY += (ch & 1) ? -voice * lvl[ch] : voice * lvl[ch];
        }

        // DC blockers: sixteen saturating loops leave plenty of offset behind.
        const float r = 1.f - 20.f / sr;
        dcyL = sumL - dcxL + r * dcyL; dcxL = sumL;
        dcyR = sumR - dcxR + r * dcyR; dcxR = sumR;

        *outL = fastTanh(dcyL * 0.5f);
        *outR = fastTanh(dcyR * 0.5f);

        // A slow read of the bank's own wandering, for patching out. The
        // channels are summed with alternating sign so the common motion
        // cancels and what is left is how unevenly the eight are behaving.
        cvLp += (sumY - cvLp) * (25.f * 6.2831853f / sr);
        *cv = cvLp * 2.5f;
    }
};

} // namespace turba_dsp
