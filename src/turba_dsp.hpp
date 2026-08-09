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
    float env = 0.f, gain = 1.f;
    float atk = 0.01f, rel = 0.0005f, down = 0.05f, up = 0.0005f;

    void reset() { env = 0.f; gain = 1.f; }

    void setSampleRate(float sr) {
        atk  = 1.f - std::exp(-1.f / (0.002f * sr));
        rel  = 1.f - std::exp(-1.f / (0.100f * sr));
        down = 1.f - std::exp(-1.f / (0.001f * sr));
        up   = 1.f - std::exp(-1.f / (0.250f * sr));
    }

    float process(float x, float ceiling) {
        const float a = std::fabs(x);
        env += (a - env) * (a > env ? atk : rel);
        float want = 1.f;
        if (env > ceiling) {
            want = ceiling / env;
            if (want < 0.02f) want = 0.02f;
        }
        gain += (want - gain) * (want < gain ? down : up);
        return fastTanh(x * gain);
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

// The largest filter coefficient the SVF is ever handed, tan(pi*0.45), i.e.
// a cutoff at 0.45 of the sample rate. The two-state switch multiplies the
// coefficient, so it needs a ceiling of its own.
static const float G_MAX = 6.3138f;

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
    float fm[NCH];     // FM index in octaves
    float am[NCH];     // AM depth 0..1
    float lvl[NCH];    // channel level 0..1
};

struct Engine {
    float sr = 48000.f;
    int topology = TOPO_LOOP;

    // smoothed running values, one per channel
    float oct[NLEV], gc[NLEV], kc[NLEV], dly[NLEV], fbk[NLEV], fm[NLEV],
          am[NLEV], tp[NLEV];
    float lvl[NCH];
    float phase[NLEV];
    // Both levers of every pair run unless this is off, in which case only
    // the first does and the module costs half as much.
    bool pairs = true;
    // How much of a lever's cross-modulation comes from its partner in the
    // pair (the ensemble's `crossvoice`) against the ring on to the next
    // channel. Tuned by measurement.
    float pairWeight = 0.35f;

    // colB, on why Skrewell sounds the way it does: "the kind of sounds you
    // get from Skrewell depend at least in part on being digital with
    // aliasing and quantization". Two separate things, both optional here.
    //
    // bandLimit off drops the polyBLEP correction from the pulses, so every
    // edge folds its harmonics back down the spectrum. In a bank of eight
    // oscillators being exponentially FM'd by each other, that is not a few
    // stray partials, it is a second inharmonic spectrum that moves the wrong
    // way when the pitch does.
    //
    // crushBits quantizes each loop signal on its way into the delay. Zero
    // leaves it alone.
    bool bandLimit = true;
    int crushBits = 0;
    float y[NLEV];       // each lever's loop output, this sample
    float yPrev[NLEV];   // ...and the previous one, what the ring reads
    SatSVF filt[NLEV];
    Normalizer norm[NLEV];
    DelayLine line[NLEV];
    float panL[NCH], panR[NCH];

    // Two-state switching, the mechanism colB found when he took Skrewell
    // apart: "parameters that have two settings that get switched between...
    // components of the sound toggle chaotically between two states". Once
    // per pass of its own delay line, each channel latches a bit from the
    // sign of another channel's loop signal, and that bit picks between two
    // values of a parameter. It is not drift and it is not an LFO: the sound
    // *flips*, and eight channels flipping at eight different rates is what
    // makes the bank evolve with nobody touching it.
    //
    // The parameter switched is the filter cutoff, in octaves. That is the
    // one that works: measured over an untouched minute, switching the cutoff
    // takes the spectral wander from 0.19 to 0.97 octaves, while switching
    // pitch or delay time instead makes it *worse* (0.10-0.14). Where there
    // is no filter to switch, the bare topology, it shortens the delay
    // instead, which is the next best thing there.
    float bifurcate = 0.f;
    int flip[NLEV];
    float flipS[NLEV];       // glided, so a flip is a swoop and not a click
    float flipCount[NLEV];
    float flipGlide = 0.f;

    float smooth = 0.01f;   // one-pole coefficient, set from the inertia time
    float dcxL = 0.f, dcyL = 0.f, dcxR = 0.f, dcyR = 0.f;
    float cvLp = 0.f;
    bool ringCoupling = true;

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
        flipGlide = 1.f - std::exp(-1.f / (0.008f * sr));
    }

    void setInertia(float seconds) {
        if (seconds < 1e-4f) seconds = 1e-4f;
        smooth = 1.f - std::exp(-1.f / (seconds * sr));
    }

    void reset() {
        for (int i = 0; i < NLEV; i++) {
            oct[i] = 4.f; gc[i] = 0.1f; kc[i] = 1.f; dly[i] = 1000.f;
            fbk[i] = 0.f; fm[i] = 0.f; am[i] = 0.f; tp[i] = 0.f;
            // Start the phases spread out. Identical phases are a fixed point
            // of the coupled ring and the bank would take a while to leave it.
            phase[i] = (float)i / (float)NLEV;
            y[i] = yPrev[i] = 0.f;
            flip[i] = 0;
            flipS[i] = 0.f;
            flipCount[i] = 0.f;
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
                fm[i]  += (t.fm[ch]  - fm[i])  * c;
                am[i]  += (t.am[ch]  - am[i])  * c;
                tp[i]  += (t.typ[ch] - tp[i])  * c;
            }
        }
    }

    void process(float in, float* outL, float* outR, float* cv) {
        std::memcpy(yPrev, y, sizeof(yPrev));

        float sumL = 0.f, sumR = 0.f, sumY = 0.f;
        const float sT = 1.f / sr;
        const int levers = pairs ? LPC : 1;

        for (int ch = 0; ch < NCH; ch++) {
            float voice = 0.f;

            for (int k = 0; k < levers; k++) {
                const int i = lev(ch, k);

                // crossvoice, as the ensemble calls it. The partner is the
                // other lever of this pair and it is the main modulator; the
                // ring on to the next channel is the weaker, second one.
                const float ringF = yPrev[lev((ch + 1) & (NCH - 1), k)];
                const float ringA = yPrev[lev((ch + 7) & (NCH - 1), k)];
                float mf, ma;
                if (!ringCoupling) {
                    // no ring: a lever hears only its partner, or itself
                    mf = ma = yPrev[pairs ? lev(ch, 1 - k) : i];
                }
                else if (pairs) {
                    const float p = yPrev[lev(ch, 1 - k)];
                    mf = pairWeight * p + (1.f - pairWeight) * ringF;
                    ma = pairWeight * p + (1.f - pairWeight) * ringA;
                }
                else {
                    mf = ringF;
                    ma = ringA;
                }

                // One tick per pass of this lever's delay line, but never
                // faster than 10 ms: past that the switch stops being a
                // change of state and becomes an audio-rate modulator.
                float period = dly[i];
                if (period < 0.010f * sr) period = 0.010f * sr;
                flipCount[i] += 1.f;
                if (flipCount[i] >= period) {
                    flipCount[i] -= period;
                    flip[i] = yPrev[lev((ch + 5) & (NCH - 1), k)] > 0.f ? 1 : 0;
                }
                flipS[i] += ((float)flip[i] - flipS[i]) * flipGlide;

                // Exponential FM, so the frequency stays positive however
                // hard the modulator swings.
                const float freq = 8.f * std::exp2(oct[i] + fm[i] * mf);
                float inc = freq * sT;
                if (inc > 0.45f) inc = 0.45f;
                if (inc < 1e-7f) inc = 1e-7f;

                phase[i] += inc;
                if (phase[i] >= 1.f) phase[i] -= std::floor(phase[i]);

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
                    osc = phase[i] < 0.5f ? 1.f : -1.f;
                    if (bandLimit) {
                        osc += polyBlep(phase[i], inc);
                        osc -= polyBlep(p2, inc);
                    }
                }

                // AM from the partner, never all the way to silence.
                const float amp = 1.f - am[i] * 0.5f * (1.f - ma);
                float s = osc * amp * 0.5f + in;

                // The switch moves the cutoff where there is a filter, and
                // the delay time where there is not.
                float gEff = gc[i];
                float dEff = dly[i];
                if (bifurcate > 0.f) {
                    if (topology == TOPO_BARE)
                        dEff *= 1.f - std::min(bifurcate * 0.25f, 0.8f) * flipS[i];
                    else {
                        gEff *= std::exp2(bifurcate * flipS[i]);
                        if (gEff > G_MAX) gEff = G_MAX;
                    }
                }

                const float d = line[i].read(dEff);
                float v;
                if (topology == TOPO_PRE) {
                    s = filt[i].process(s, gEff, kc[i], tp[i]);
                    v = norm[i].process(s + d * fbk[i], 1.0f);
                }
                else if (topology == TOPO_BARE) {
                    v = norm[i].process(s + d * fbk[i], 1.0f);
                }
                else {
                    v = filt[i].process(s + d * fbk[i], gEff, kc[i], tp[i]);
                    v = norm[i].process(v, 1.0f);
                }

                if (!std::isfinite(v)) {
                    v = 0.f;
                    filt[i].reset();
                    norm[i].reset();
                    line[i].reset();
                }

                if (crushBits > 0) {
                    const float steps = (float)(1 << (crushBits - 1));
                    v = std::floor(v * steps + 0.5f) / steps;
                }

                line[i].write(v);
                y[i] = v;
                voice += v;
            }
            if (levers > 1) voice *= 0.5f;

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
        *cv = cvLp;
    }
};

} // namespace turba_dsp
