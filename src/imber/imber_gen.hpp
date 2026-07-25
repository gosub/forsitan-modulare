// imber_gen.hpp — the procedural sample-bank generators. Everything the
// engine plays is rendered from these at (re)seed time, never shipped:
// loop buffers (drones, pads, fragments, bells, plucks, ambients,
// glitches), micro one-shots, and CD-skip material. The magic numbers are
// tuned by ear, all pitches snap to the shared scale table.
//
// Two family sets group the same generators for sylla's selector: v1 (the
// reverse-engineered Haiku taxonomy, shipped in 2.9 and frozen because
// patches reproduce from a seed) and v2 (the default, regrouped into a
// bed-to-point continuum and home to any new material).
#pragma once
#include "imber_dsp.hpp"
#include <atomic>

namespace imber_gen {

using namespace imber_dsp;

// ---------------------------------------------------------- sub-parts ---

// additive partials with independent slow tremolos — the drone skeleton
inline void addPartial(std::vector<float>& b, float sr, float freq,
                       float amp, float lfoHz, float lfoDepth, float phase) {
    if (freq > 0.45f * sr) return;
    int n = (int)b.size();
    float w = kTau * freq / sr;
    float wl = kTau * lfoHz / sr;
    for (int i = 0; i < n; i++) {
        float trem = 1.f - lfoDepth * (0.5f + 0.5f * std::sin(wl * i + phase * 7.f));
        b[i] += amp * trem * std::sin(w * i + phase);
    }
}

// Karplus-Strong pluck mixed in at a start offset
inline void ksPluck(std::vector<float>& b, float sr, int start, float freq,
                    float durS, float damp, float amp, Rng& rng) {
    int len = std::max(2, (int)(sr / clampf(freq, 30.f, 4000.f)));
    std::vector<float> dl(len);
    for (int i = 0; i < len; i++)
        dl[i] = rng.bipolar();
    int n = std::min((int)b.size() - start, (int)(durS * sr));
    float decay = 0.996f - 0.02f * damp;
    int idx = 0;
    float prev = 0.f;
    for (int i = 0; i < n; i++) {
        float out = dl[idx];
        dl[idx] = decay * 0.5f * (out + prev);
        prev = out;
        if (++idx >= len) idx = 0;
        b[start + i] += amp * out * (1.f - (float)i / n);
    }
}

// additive bell strike (ratios + per-partial exponential decay)
inline void bellStrike(std::vector<float>& b, float sr, int start, float f0,
                       const float* ratios, int nr, float decayS, float amp,
                       Rng& rng) {
    int n = std::min((int)b.size() - start, (int)(decayS * 4.f * sr));
    for (int p = 0; p < nr; p++) {
        float f = f0 * ratios[p];
        if (f > 0.45f * sr) continue;
        float w = kTau * f / sr;
        float pa = amp / (1.f + p) * rng.range(0.6f, 1.f);
        float tau = decayS / (1.f + 0.7f * p);
        float k = std::exp(-1.f / (tau * sr));
        float env = 1.f, ph = rng.range(0.f, kTau);
        for (int i = 0; i < n; i++) {
            b[start + i] += pa * env * std::sin(w * i + ph);
            env *= k;
        }
    }
}

// noise-excited resonant body. Where bellStrike is pure additive (sines
// only, so a clean attack), this rings a short noise burst through a bank
// of high-Q bandpasses: the attack keeps its grit and the modes colour the
// decay, which is what a struck object actually does. Q sets most of the
// ring length; the envelope is only a taper on top of it.
inline void struckBody(std::vector<float>& b, float sr, int start, float f0,
                       const float* ratios, int nr, float decayS, float amp,
                       Rng& rng) {
    int n = std::min((int)b.size() - start, (int)(decayS * 4.f * sr));
    if (n <= 0) return;
    int bl = std::max(8, (int)(rng.range(0.002f, 0.008f) * sr));
    std::vector<float> exc(std::min(n, bl * 3), 0.f);
    for (size_t i = 0; i < exc.size(); i++)
        exc[i] = rng.bipolar() * std::exp(-3.f * (float)i / bl);
    int el = (int)exc.size();
    for (int p = 0; p < nr; p++) {
        float f = f0 * ratios[p];
        if (f > 0.45f * sr) continue;
        float q = rng.range(80.f, 400.f) / (1.f + 0.5f * p);
        Biquad bp;
        bp.setBp(f, q, sr);
        float tau = decayS / (1.f + 0.8f * p);
        float k = std::exp(-1.f / (tau * sr));
        float env = 1.f;
        // the constant-skirt bandpass peaks at ~Q, so divide it back out
        float pa = amp / (1.f + p) * rng.range(0.6f, 1.f) * 8.f / q;
        for (int i = 0; i < n; i++) {
            b[start + i] += pa * env * bp.process(i < el ? exc[i] : 0.f);
            env *= k;
        }
    }
}

// a raised-cosine windowed grain of a sine
inline void sineGrainAt(std::vector<float>& b, float sr, int start, float freq,
                        float durS, float amp) {
    int n = std::min((int)b.size() - start, std::max(8, (int)(durS * sr)));
    float w = kTau * freq / sr;
    for (int i = 0; i < n; i++) {
        float win = 0.5f - 0.5f * std::cos(kTau * i / n);
        b[start + i] += amp * win * std::sin(w * i);
    }
}

// ------------------------------------------------------ loop generators ---

typedef void (*GenFn)(Rng&, float, std::vector<float>&);

inline int loopLen(Rng& rng, float sr) {
    return (int)(rng.range(2.5f, 4.f) * sr);
}

inline void genDronePure(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f = pickFreq(rng, -1, 0);
    int nh = rng.irange(3, 6);
    for (int h = 1; h <= nh; h++)
        addPartial(b, sr, f * h, 0.5f / (h * h) * rng.range(0.5f, 1.f),
                   rng.range(0.05f, 0.3f), rng.range(0.2f, 0.6f),
                   rng.range(0.f, kTau));
}

inline void genDroneDetuned(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f = pickFreq(rng, -1, 0);
    int nv = rng.irange(2, 3);
    for (int v = 0; v < nv; v++) {
        float det = 1.f + rng.bipolar() * rng.range(0.002f, 0.012f);
        for (int h = 1; h <= 3; h++)
            addPartial(b, sr, f * det * h, 0.35f / (nv * h),
                       rng.range(0.05f, 0.2f), 0.3f, rng.range(0.f, kTau));
    }
}

inline void genDroneFm(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float fc = pickFreq(rng, -1, 1);
    static const float ratios[5] = {0.5f, 1.f, 1.5f, 2.f, 3.f};
    float fm = fc * ratios[rng.irange(0, 4)];
    float index = rng.range(0.5f, 3.f);
    float driftHz = rng.range(0.03f, 0.15f);
    float wc = kTau * fc / sr, wm = kTau * fm / sr, wd = kTau * driftHz / sr;
    int n = (int)b.size();
    for (int i = 0; i < n; i++) {
        float idx = index * (0.6f + 0.4f * std::sin(wd * i));
        b[i] = 0.6f * std::sin(wc * i + idx * std::sin(wm * i));
    }
}

inline void genDroneFiltNoise(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float fc = pickFreq(rng, 0, 2);
    Biquad bq;
    float sweepHz = rng.range(0.05f, 0.25f);
    float sweep = rng.range(0.1f, 0.5f);
    float q = rng.range(6.f, 20.f);
    int n = (int)b.size();
    for (int i = 0; i < n; i++) {
        if ((i & 255) == 0)
            bq.setBp(fc * (1.f + sweep * std::sin(kTau * sweepHz * i / sr)), q, sr);
        b[i] = bq.process(rng.bipolar()) * 2.f;
    }
}

inline void genDroneSub(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f = pickFreq(rng, -2, -1);
    addPartial(b, sr, f, 0.7f, rng.range(0.05f, 0.15f), 0.3f, 0.f);
    addPartial(b, sr, f * 1.5f, 0.12f, rng.range(0.05f, 0.2f), 0.5f, 1.f);
    for (size_t i = 0; i < b.size(); i++)
        b[i] = std::tanh(b[i] * 1.6f);
}

inline void genDroneComb(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f = pickFreq(rng, -1, 1);
    int len = std::max(2, (int)(sr / f));
    std::vector<float> dl(len, 0.f);
    OnePoleLp lp;
    lp.setTau(rng.range(1500.f, 5000.f), sr);
    float fb = rng.range(0.96f, 0.995f);
    int idx = 0, n = (int)b.size();
    for (int i = 0; i < n; i++) {
        float x = rng.bipolar() * 0.25f + dl[idx] * fb;
        x = lp.process(x);
        dl[idx] = x;
        if (++idx >= len) idx = 0;
        b[i] = x * 1.5f;
    }
}

inline void genPadSlow(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int oct = rng.irange(-1, 0);
    int nv = rng.irange(3, 4);
    for (int v = 0; v < nv; v++) {
        float f = chordFreq(rng, v, oct + (v == 3 ? 1 : 0));
        for (int d = 0; d < 2; d++) {
            float det = 1.f + rng.bipolar() * 0.004f;
            addPartial(b, sr, f * det, 0.22f / nv,
                       rng.range(0.04f, 0.15f), rng.range(0.3f, 0.7f),
                       rng.range(0.f, kTau));
        }
    }
}

inline void genPadCluster(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int nv = rng.irange(5, 8);
    float base = pickFreq(rng, 0, 1);
    for (int v = 0; v < nv; v++) {
        float f = base * rng.range(0.94f, 1.06f);
        addPartial(b, sr, f, 0.35f / nv, rng.range(0.05f, 0.3f),
                   rng.range(0.4f, 0.8f), rng.range(0.f, kTau));
    }
}

inline void genBellClassic(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    static const float ratios[4] = {1.f, 2.f, 3.01f, 4.16f};
    int hits = rng.irange(1, 3);
    for (int h = 0; h < hits; h++) {
        int start = (int)(rng.uniform() * 0.6f * b.size());
        bellStrike(b, sr, start, pickFreq(rng, 0, 2), ratios, 4,
                   rng.range(0.4f, 1.5f), 0.5f, rng);
    }
}

inline void genBellInharmonic(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    static const float ratios[4] = {1.f, 2.76f, 5.40f, 8.93f};
    int hits = rng.irange(1, 2);
    for (int h = 0; h < hits; h++) {
        int start = (int)(rng.uniform() * 0.5f * b.size());
        bellStrike(b, sr, start, pickFreq(rng, 0, 1), ratios, 4,
                   rng.range(0.6f, 2.f), 0.45f, rng);
    }
}

inline void genKarplusPluck(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int hits = rng.irange(2, 5);
    for (int h = 0; h < hits; h++) {
        int start = (int)(rng.uniform() * 0.8f * b.size());
        ksPluck(b, sr, start, pickFreq(rng, -1, 1), rng.range(0.5f, 1.5f),
                rng.range(0.f, 0.5f), 0.6f, rng);
    }
}

inline void genKarplusRun(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int notes = rng.irange(6, 12);
    float step = (float)b.size() / (notes + 2);
    for (int k = 0; k < notes; k++) {
        int start = (int)(k * step + rng.uniform() * step * 0.3f);
        ksPluck(b, sr, start, pickFreq(rng, 0, 1), rng.range(0.2f, 0.5f),
                rng.range(0.2f, 0.7f), 0.5f, rng);
    }
}

inline void genFragPluckDirty(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int hits = rng.irange(2, 4);
    for (int h = 0; h < hits; h++) {
        int start = (int)(rng.uniform() * 0.8f * b.size());
        ksPluck(b, sr, start, pickFreq(rng, 0, 2), rng.range(0.15f, 0.5f),
                rng.range(0.3f, 0.9f), 0.7f, rng);
    }
    dirtify(b, rng, sr, rng.range(0.5f, 1.f));
}

inline void genFragStutter(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f = pickFreq(rng, 0, 1);
    Biquad lp;
    lp.setLp(f * rng.range(2.f, 6.f), 1.2f, sr);
    float w = kTau * f / sr;
    int gateLen = (int)(rng.range(0.03f, 0.08f) * sr);
    int n = (int)b.size();
    bool on = true;
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (++c >= gateLen) {
            c = 0;
            on = rng.chance(0.6f);
            if (rng.chance(0.2f))
                gateLen = (int)(rng.range(0.03f, 0.08f) * sr);
        }
        float saw = 2.f * (std::fmod(w * i, kTau) / kTau) - 1.f;
        b[i] = on ? lp.process(saw) * 0.6f : 0.f;
    }
}

inline void genFragGranular(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int grains = rng.irange(20, 60);
    for (int g = 0; g < grains; g++) {
        int start = (int)(rng.uniform() * 0.95f * b.size());
        sineGrainAt(b, sr, start, pickFreq(rng, 0, 3),
                    rng.range(0.01f, 0.08f), rng.range(0.1f, 0.4f));
    }
}

inline void genFragNoiseBurst(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int bursts = rng.irange(3, 8);
    for (int k = 0; k < bursts; k++) {
        int start = (int)(rng.uniform() * 0.9f * b.size());
        int len = (int)(rng.range(0.02f, 0.15f) * sr);
        Biquad bp;
        bp.setBp(rng.range(300.f, 6000.f), rng.range(2.f, 8.f), sr);
        float tau = len / 3.f;
        for (int i = 0; i < len && start + i < (int)b.size(); i++)
            b[start + i] += bp.process(rng.bipolar()) * 2.f * std::exp(-i / tau);
    }
    if (rng.chance(0.5f)) {
        int hold = rng.irange(2, 6);
        float held = 0.f;
        int hc = 0;
        for (size_t i = 0; i < b.size(); i++) {
            if (hc == 0) held = b[i];
            if (++hc >= hold) hc = 0;
            b[i] = held;
        }
    }
}

inline void genFragChordStab(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int hits = rng.irange(1, 3);
    for (int h = 0; h < hits; h++) {
        int start = (int)(rng.uniform() * 0.7f * b.size());
        int len = (int)(rng.range(0.1f, 0.4f) * sr);
        int oct = rng.irange(-1, 0);
        Biquad lp;
        lp.setLp(rng.range(800.f, 3000.f), 1.f, sr);
        float tau = len / 3.f;
        float w[3], ph[3];
        for (int v = 0; v < 3; v++) {
            w[v] = kTau * chordFreq(rng, v, oct) / sr;
            ph[v] = rng.range(0.f, kTau);
        }
        for (int i = 0; i < len && start + i < (int)b.size(); i++) {
            float s = 0.f;
            for (int v = 0; v < 3; v++) {
                float p = std::fmod(w[v] * i + ph[v], kTau) / kTau;
                s += 2.f * p - 1.f;
            }
            b[start + i] += lp.process(s * 0.3f) * std::exp(-i / tau);
        }
    }
}

inline void genFragMelodic(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int notes = rng.irange(4, 8);
    float step = (float)b.size() / notes;
    for (int k = 0; k < notes; k++) {
        if (rng.chance(0.2f)) continue;
        int start = (int)(k * step);
        float durS = rng.range(0.6f, 1.3f) * step / sr;
        sineGrainAt(b, sr, start, pickFreq(rng, 0, 1), durS, 0.45f);
        if (rng.chance(0.4f))
            sineGrainAt(b, sr, start, pickFreq(rng, 1, 2), durS * 0.7f, 0.15f);
    }
}

inline void genGlitchBubbly(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    OnePoleLp glide;
    glide.setTau(rng.range(20.f, 80.f), sr);
    float f = pickFreq(rng, 0, 2);
    int holdLen = (int)(rng.range(0.02f, 0.06f) * sr);
    int c = 0, n = (int)b.size();
    float phase = 0.f;
    for (int i = 0; i < n; i++) {
        if (++c >= holdLen) {
            c = 0;
            if (rng.chance(0.7f)) f = pickFreq(rng, 0, 2);
        }
        float fs = glide.process(f);
        phase += kTau * fs / sr;
        if (phase > kTau) phase -= kTau;
        b[i] = 0.5f * std::sin(phase);
    }
}

inline void genAmbientWash(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    Biquad bp;
    OnePoleLp pink;
    pink.setTau(rng.range(400.f, 1200.f), sr);
    float fc = pickFreq(rng, 1, 2);
    float sweepHz = rng.range(0.03f, 0.12f);
    int n = (int)b.size();
    for (int i = 0; i < n; i++) {
        if ((i & 255) == 0)
            bp.setBp(fc * (1.f + 0.3f * std::sin(kTau * sweepHz * i / sr)),
                     rng.range(3.f, 6.f), sr);
        b[i] = bp.process(pink.process(rng.bipolar())) * 3.f;
    }
    // faint chord bed underneath
    int oct = rng.irange(-1, 0);
    for (int v = 0; v < 3; v++)
        addPartial(b, sr, chordFreq(rng, v, oct), 0.08f,
                   rng.range(0.04f, 0.1f), 0.5f, rng.range(0.f, kTau));
}

inline void genAmbientTapePad(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    int oct = rng.irange(-1, 0);
    float wowHz = rng.range(0.4f, 1.2f);
    float wowDepth = rng.range(0.002f, 0.008f);
    for (int v = 0; v < 3; v++) {
        float f = chordFreq(rng, v, oct);
        float ph = rng.range(0.f, kTau), phase = 0.f;
        int n = (int)b.size();
        for (int i = 0; i < n; i++) {
            float wow = 1.f + wowDepth * std::sin(kTau * wowHz * i / sr + ph);
            phase += kTau * f * wow / sr;
            b[i] += 0.18f * std::sin(phase + ph);
        }
    }
    for (size_t i = 0; i < b.size(); i++)
        b[i] += rng.bipolar() * 0.002f;
}

inline void genAmbientChime(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    static const float ratios[3] = {1.f, 2.31f, 4.02f};
    int dings = rng.irange(2, 5);
    for (int k = 0; k < dings; k++) {
        int start = (int)(rng.uniform() * 0.8f * b.size());
        bellStrike(b, sr, start, pickFreq(rng, 1, 3), ratios, 3,
                   rng.range(0.5f, 1.2f), 0.25f, rng);
    }
}

// -------------------------------------------------- v2-only generators ---

// three-formant vowel table (F1/F2/F3 in Hz)
static const float kVowels[5][3] = {
    {700.f, 1220.f, 2600.f},   // a
    {400.f, 2000.f, 2550.f},   // e
    {240.f, 2400.f, 2900.f},   // i
    {400.f,  800.f, 2600.f},   // o
    {350.f,  600.f, 2700.f},   // u
};

// sustained formant drone: the pulsar micro archetype grown up. A glottal
// pulse train (plus breath noise and a little vibrato) through three
// bandpass formants morphing between two vowels. This is the one colour
// neither module had, and the reason sylla is named for a syllable.
inline void genVowelDrone(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    float f0 = pickFreq(rng, -1, 0);
    int va = rng.irange(0, 4);
    int vb = (va + 1 + rng.irange(0, 3)) % 5;      // never the same vowel twice
    float morphHz = rng.range(0.05f, 0.2f);
    float breath = rng.range(0.02f, 0.1f);
    float vib = rng.range(0.001f, 0.006f);
    float vibHz = rng.range(3.f, 6.f);
    int period = std::max(4, (int)(sr / f0));
    int gl = std::max(2, period / rng.irange(3, 8));   // glottal pulse width
    float q[3] = {rng.range(6.f, 12.f), rng.range(8.f, 16.f), rng.range(10.f, 20.f)};
    static const float amp[3] = {1.f, 0.5f, 0.22f};
    Biquad fmt[3];
    int n = (int)b.size();
    float phase = 0.f;
    for (int i = 0; i < n; i++) {
        if ((i & 255) == 0) {
            float m = 0.5f - 0.5f * std::cos(kTau * morphHz * i / sr);
            for (int k = 0; k < 3; k++)
                fmt[k].setBp(kVowels[va][k]
                             + (kVowels[vb][k] - kVowels[va][k]) * m, q[k], sr);
        }
        float fv = f0 * (1.f + vib * std::sin(kTau * vibHz * i / sr));
        phase += fv / sr;
        if (phase >= 1.f) phase -= 1.f;
        int ip = (int)(phase * period);
        // the bandpasses reject DC, so the pulse can stay unipolar
        float exc = (ip < gl ? 0.5f - 0.5f * std::cos(kTau * ip / gl) : 0.f)
                    + rng.bipolar() * breath;
        float s = 0.f;
        for (int k = 0; k < 3; k++)
            s += amp[k] * fmt[k].process(exc);
        b[i] = s;
    }
    normalizePeak(b, 0.8f);
}

// struck resonant bodies: metal-ish and wood-ish mode sets
static const float kBodyMetal[4] = {1.f, 1.83f, 2.41f, 3.77f};
static const float kBodyWood[4] = {1.f, 2.57f, 4.10f, 5.62f};

inline void genStruckBody(Rng& rng, float sr, std::vector<float>& b) {
    b.assign(loopLen(rng, sr), 0.f);
    const float* ratios = rng.chance(0.5f) ? kBodyMetal : kBodyWood;
    int hits = rng.irange(2, 5);
    for (int h = 0; h < hits; h++) {
        // the first strike lands on the buffer start. The older event
        // generators scatter every hit at random, which is why sylla's LEN
        // window (anchored at 0) so often finds nothing but silence.
        int start = h ? (int)(rng.uniform() * 0.8f * b.size()) : 0;
        struckBody(b, sr, start, pickFreq(rng, 0, 2), ratios, 4,
                   rng.range(0.3f, 1.f), 0.6f, rng);
    }
    normalizePeak(b, 0.8f);
}

// Family set v1, shipped in 2.9. Only sylla's selector reads the family
// field; imber's bank is populated from the weight column alone.
//
// Everything below this line is part of the save format. sylla and imber
// store a seed and regenerate, so the table's order, its length and its
// weights all feed the picks a given seed makes: appending one generator
// or changing one weight silently rewrites every saved patch. That is why
// the v1 table and v1 renderFamily are frozen, and why new material lands
// in the v2 table instead, selected per module from the context menu.
enum Family {
    FAM_DRONE, FAM_PAD, FAM_FRAG, FAM_BELL, FAM_AMBIENT, FAM_GLITCH,
    FAM_KARPLUS, FAM_SKIP, FAM_MICRO, FAM_COUNT
};

// Family set v2. Same ten knob positions, reordered into one continuum
// from bed to point (drone, pad, air, bell, pluck, phrase, dust, broken,
// micro, random), so the knob is a gesture rather than a menu.
//
// ambient is gone: it was a level and a register, not an excitation, which
// is why it sounded like a mixture of its neighbours. Its three generators
// went home to the families they were already made of (tape pad to pad,
// wash to air, chime to bell). frag's six unrelated recipes split across
// pluck / phrase / dust, glitch and skip merged into broken, and karplus
// is renamed for what it sounds like instead of who invented it.
enum Family2 {
    FAM2_DRONE, FAM2_PAD, FAM2_AIR, FAM2_BELL, FAM2_PLUCK,
    FAM2_PHRASE, FAM2_DUST, FAM2_BROKEN, FAM2_MICRO, FAM2_COUNT
};

struct GenEntry {
    GenFn fn;
    int family;
    float weight;   // bank population bias
};

inline const GenEntry* loopTable(int* count) {
    static const GenEntry table[] = {
        {genDronePure,      FAM_DRONE,   1.0f},
        {genDroneDetuned,   FAM_DRONE,   1.0f},
        {genDroneFm,        FAM_DRONE,   1.0f},
        {genDroneFiltNoise, FAM_DRONE,   1.0f},
        {genDroneSub,       FAM_DRONE,   0.7f},
        {genDroneComb,      FAM_DRONE,   0.8f},
        {genPadSlow,        FAM_PAD,     1.2f},
        {genPadCluster,     FAM_PAD,     0.9f},
        {genBellClassic,    FAM_BELL,    1.0f},
        {genBellInharmonic, FAM_BELL,    0.8f},
        {genKarplusPluck,   FAM_KARPLUS, 1.1f},
        {genKarplusRun,     FAM_KARPLUS, 0.7f},
        {genFragPluckDirty, FAM_FRAG,    1.2f},
        {genFragStutter,    FAM_FRAG,    1.0f},
        {genFragGranular,   FAM_FRAG,    1.1f},
        {genFragNoiseBurst, FAM_FRAG,    1.0f},
        {genFragChordStab,  FAM_FRAG,    0.9f},
        {genFragMelodic,    FAM_FRAG,    1.1f},
        {genGlitchBubbly,   FAM_GLITCH,  0.8f},
        {genAmbientWash,    FAM_AMBIENT, 1.1f},
        {genAmbientTapePad, FAM_AMBIENT, 1.0f},
        {genAmbientChime,   FAM_AMBIENT, 0.9f},
    };
    *count = (int)(sizeof(table) / sizeof(table[0]));
    return table;
}

// The v2 loop pool: the same 22 generators regrouped, plus the two colours
// the library was missing. genSkip and genMicro stay out of it, exactly as
// they stay out of imber's loop bank, because they finish themselves and
// are one-shots rather than beds; broken and micro reach them directly.
inline const GenEntry* loopTable2(int* count) {
    static const GenEntry table[] = {
        {genDronePure,      FAM2_DRONE,  1.0f},
        {genDroneDetuned,   FAM2_DRONE,  1.0f},
        {genDroneFm,        FAM2_DRONE,  1.0f},
        {genDroneComb,      FAM2_DRONE,  0.8f},
        {genDroneSub,       FAM2_DRONE,  0.7f},
        {genPadSlow,        FAM2_PAD,    1.2f},
        {genPadCluster,     FAM2_PAD,    0.9f},
        {genAmbientTapePad, FAM2_PAD,    1.0f},
        {genDroneFiltNoise, FAM2_AIR,    1.0f},
        {genAmbientWash,    FAM2_AIR,    1.1f},
        {genVowelDrone,     FAM2_AIR,    1.0f},
        {genBellClassic,    FAM2_BELL,   1.0f},
        {genBellInharmonic, FAM2_BELL,   0.8f},
        {genAmbientChime,   FAM2_BELL,   0.9f},
        {genStruckBody,     FAM2_BELL,   1.0f},
        {genKarplusPluck,   FAM2_PLUCK,  1.1f},
        {genFragPluckDirty, FAM2_PLUCK,  1.2f},
        {genFragMelodic,    FAM2_PHRASE, 1.1f},
        {genFragChordStab,  FAM2_PHRASE, 0.9f},
        {genKarplusRun,     FAM2_PHRASE, 0.7f},
        {genFragStutter,    FAM2_PHRASE, 1.0f},
        {genFragGranular,   FAM2_DUST,   1.1f},
        {genFragNoiseBurst, FAM2_DUST,   1.0f},
        {genGlitchBubbly,   FAM2_BROKEN, 0.8f},
    };
    *count = (int)(sizeof(table) / sizeof(table[0]));
    return table;
}

// ------------------------------------------------------- micro one-shots ---

inline void genMicro(Rng& rng, float sr, std::vector<float>& b) {
    int arch = rng.irange(0, 13);
    float durS;
    switch (arch) {
        case 0:  durS = rng.range(0.01f, 0.05f); break;   // sine grain
        case 1:  durS = rng.range(0.06f, 0.15f); break;   // soft bell
        case 2:  durS = rng.range(0.03f, 0.09f); break;   // pulsar
        case 3:  durS = rng.range(0.02f, 0.08f); break;   // chirp
        case 4:  durS = rng.range(0.08f, 0.2f);  break;   // micro chime
        case 5:  durS = rng.range(0.005f, 0.015f); break; // tap
        case 6:  durS = rng.range(0.015f, 0.04f); break;  // hf sine
        case 7:  durS = rng.range(0.01f, 0.03f); break;   // impulse
        case 8:  durS = rng.range(0.03f, 0.08f); break;   // data burst
        case 9:  durS = rng.range(0.04f, 0.1f);  break;   // noise gate
        case 10: durS = rng.range(0.02f, 0.05f); break;   // square blip
        case 11: durS = rng.range(0.02f, 0.06f); break;   // bit glitch
        case 12: durS = rng.range(0.003f, 0.01f); break;  // hf tick
        default: durS = rng.range(0.08f, 0.15f); break;   // sub thump
    }
    int n = std::max(16, (int)(durS * sr));
    b.assign(n, 0.f);
    switch (arch) {
        case 0:
            sineGrainAt(b, sr, 0, pickFreq(rng, 1, 3), durS, 0.8f);
            break;
        case 1: {
            static const float r[2] = {1.f, 2.4f};
            bellStrike(b, sr, 0, pickFreq(rng, 1, 3), r, 2, durS * 0.5f, 0.8f, rng);
            break;
        }
        case 2: {
            float f = pickFreq(rng, 1, 3);
            float form = f * rng.range(2.f, 6.f);
            float w = kTau * form / sr;
            int period = std::max(4, (int)(sr / f));
            int gl = period / rng.irange(2, 4);
            for (int i = 0; i < n; i++) {
                int ip = i % period;
                float win = ip < gl ? 0.5f - 0.5f * std::cos(kTau * ip / gl) : 0.f;
                b[i] = 0.7f * win * std::sin(w * i);
            }
            break;
        }
        case 3: {
            float f0 = pickFreq(rng, 1, 3);
            float f1 = f0 * (rng.chance(0.5f) ? rng.range(2.f, 6.f)
                                              : rng.range(0.15f, 0.5f));
            float phase = 0.f;
            for (int i = 0; i < n; i++) {
                float t = (float)i / n;
                float f = f0 * std::pow(f1 / f0, t);
                phase += kTau * f / sr;
                float win = 0.5f - 0.5f * std::cos(kTau * (float)i / n);
                b[i] = 0.7f * win * std::sin(phase);
            }
            break;
        }
        case 4: {
            static const float r[3] = {1.f, 2.31f, 4.02f};
            bellStrike(b, sr, 0, pickFreq(rng, 2, 3), r, 3, durS * 0.4f, 0.6f, rng);
            break;
        }
        case 5: {
            Biquad bp;
            bp.setBp(rng.range(1000.f, 6000.f), rng.range(4.f, 10.f), sr);
            for (int i = 0; i < n; i++)
                b[i] = bp.process(rng.bipolar()) * 3.f * (1.f - (float)i / n);
            break;
        }
        case 6:
            sineGrainAt(b, sr, 0, rng.range(3500.f, 8000.f), durS, 0.5f);
            break;
        case 7: {
            Biquad lp;
            lp.setLp(rng.range(1500.f, 6000.f), rng.range(4.f, 12.f), sr);
            for (int i = 0; i < n; i++)
                b[i] = lp.process(i < 2 ? 1.f : 0.f) * 2.f;
            break;
        }
        case 8: {
            int hold = std::max(2, (int)(sr / rng.range(1000.f, 4000.f)));
            float v = 0.f;
            for (int i = 0; i < n; i++) {
                if (i % hold == 0) v = rng.chance(0.5f) ? 0.6f : -0.6f;
                b[i] = v;
            }
            break;
        }
        case 9: {
            int seg = std::max(8, n / rng.irange(3, 6));
            bool on = true;
            for (int i = 0; i < n; i++) {
                if (i % seg == 0) on = rng.chance(0.6f);
                b[i] = on ? rng.bipolar() * 0.5f : 0.f;
            }
            break;
        }
        case 10: {
            float f = pickFreq(rng, 1, 2);
            float w = kTau * f / sr;
            float tau = n / 3.f;
            for (int i = 0; i < n; i++)
                b[i] = (std::sin(w * i) > 0.f ? 0.5f : -0.5f) * std::exp(-i / tau);
            break;
        }
        case 11: {
            float f = pickFreq(rng, 1, 3);
            float w = kTau * f / sr;
            float bits = rng.range(1.5f, 3.f);
            for (int i = 0; i < n; i++)
                b[i] = crush(0.7f * std::sin(w * i), bits);
            break;
        }
        case 12: {
            OnePoleHp hp;
            hp.setTau(6000.f, sr);
            for (int i = 0; i < n; i++)
                b[i] = hp.process(rng.bipolar()) * (1.f - (float)i / n);
            break;
        }
        default: {
            float f = rng.range(45.f, 85.f);
            float w = kTau * f / sr;
            float tau = n / 2.5f;
            for (int i = 0; i < n; i++)
                b[i] = 0.9f * std::sin(w * i) * std::exp(-i / tau);
            break;
        }
    }
    fadeEdges(b, sr, 1.5f);
    normalizePeak(b, 0.85f);
    safetyClip(b);
}

// ------------------------------------------------------------- CD skip ---

inline void genSkip(Rng& rng, float sr, std::vector<float>& b) {
    int n = (int)(rng.range(0.7f, 1.5f) * sr);
    b.assign(n, 0.f);
    // base material: a chord tone with some edge
    float f = pickFreq(rng, 0, 2);
    float w = kTau * f / sr;
    std::vector<float> base(n);
    float mix = rng.range(0.2f, 0.8f);
    for (int i = 0; i < n; i++) {
        float p = std::fmod(w * i, kTau) / kTau;
        base[i] = (1.f - mix) * std::sin(w * i) + mix * (2.f * p - 1.f);
    }
    // freeze a tiny segment and repeat it — the stuck CD
    int segLen = std::max(16, (int)(rng.range(0.008f, 0.06f) * sr));
    int segStart = rng.irange(0, n - segLen - 1);
    for (int i = 0; i < n; i++)
        b[i] = base[segStart + (i % segLen)];
    // comb + tanh saturation
    int combLen = std::max(2, (int)(sr / rng.range(200.f, 1200.f)));
    std::vector<float> dl(combLen, 0.f);
    int idx = 0;
    float fb = rng.range(0.5f, 0.8f);
    for (int i = 0; i < n; i++) {
        float x = std::tanh((b[i] + dl[idx] * fb) * rng.range(1.2f, 1.8f));
        dl[idx] = x;
        if (++idx >= combLen) idx = 0;
        b[i] = x;
    }
    // choppy on/off gate
    int gateLen = (int)(rng.range(0.008f, 0.03f) * sr);
    bool on = true;
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (++c >= gateLen) {
            c = 0;
            on = rng.chance(0.55f);
        }
        if (!on) b[i] = 0.f;
    }
    fadeEdges(b, sr, 3.f);
    normalizePeak(b, 0.8f);
    safetyClip(b);
}

// -------------------------------------------------------- render fronts ---

// one uniform() roll, walked down the weight column
inline int weightedPick(const GenEntry* table, int count, Rng& rng) {
    float total = 0.f;
    for (int i = 0; i < count; i++)
        total += table[i].weight;
    float roll = rng.uniform() * total;
    for (int i = 0; i < count; i++) {
        roll -= table[i].weight;
        if (roll <= 0.f) return i;
    }
    return 0;
}

// the shared tail every loop generator gets: grunge, edge fades, level
inline void finishLoop(std::vector<float>& b, Rng& rng, float sr) {
    dirtify(b, rng, sr, rng.range(0.f, 0.35f));
    fadeEdges(b, sr, 25.f);
    normalizePeak(b, 0.75f);
    safetyClip(b);
}

// how many candidates a family holds, and which table rows they are
inline int familyMatches(const GenEntry* table, int count, int family,
                         int* matches, int cap) {
    int nm = 0;
    for (int i = 0; i < count && nm < cap; i++)
        if (table[i].family == family)
            matches[nm++] = i;
    return nm;
}

inline void renderLoop(Rng& rng, float sr, std::vector<float>& b) {
    int count;
    const GenEntry* table = loopTable(&count);
    table[weightedPick(table, count, rng)].fn(rng, sr, b);
    finishLoop(b, rng, sr);
}

inline void renderFamily(int family, Rng& rng, float sr, std::vector<float>& b) {
    if (family == FAM_SKIP) { genSkip(rng, sr, b); return; }
    if (family == FAM_MICRO) { genMicro(rng, sr, b); return; }
    int count;
    const GenEntry* table = loopTable(&count);
    int matches[32];
    int nm = familyMatches(table, count, family, matches, 32);
    int pick = nm ? matches[rng.irange(0, nm - 1)] : 0;
    table[pick].fn(rng, sr, b);
    finishLoop(b, rng, sr);
}

// v2 random: weighted over the whole v2 loop pool, which is imber's own
// distribution. v1's random rolled seed % 9 instead, so it weighted a
// one-generator family as heavily as a six-generator one and handed out a
// CD skip or a 3 ms tick 22% of the time.
inline void renderLoop2(Rng& rng, float sr, std::vector<float>& b) {
    int count;
    const GenEntry* table = loopTable2(&count);
    table[weightedPick(table, count, rng)].fn(rng, sr, b);
    finishLoop(b, rng, sr);
}

inline void renderFamily2(int family, Rng& rng, float sr, std::vector<float>& b) {
    if (family == FAM2_MICRO) { genMicro(rng, sr, b); return; }
    int count;
    const GenEntry* table = loopTable2(&count);
    int matches[32];
    int nm = familyMatches(table, count, family, matches, 32);
    // broken is the one family with a self-finished member: genSkip already
    // fades and normalizes itself, so it sits outside the loop pool and is
    // appended here as one extra candidate
    int extra = (family == FAM2_BROKEN) ? 1 : 0;
    if (!nm && !extra) {
        table[0].fn(rng, sr, b);
        finishLoop(b, rng, sr);
        return;
    }
    int pick = rng.irange(0, nm + extra - 1);
    if (extra && pick >= nm) {
        genSkip(rng, sr, b);
        return;
    }
    table[matches[pick]].fn(rng, sr, b);
    finishLoop(b, rng, sr);
}

// ----------------------------------------------------------------- bank ---

struct Bank {
    std::vector<std::vector<float> > loops, skips, micros;
    uint64_t seed;
    float sr;
};

static const int kBankLoops = 64;
static const int kBankSkips = 64;
static const int kBankMicros = 64;

// progress counts rendered buffers, 0..(loops+skips+micros); abort lets
// the worker bail early when the module is being torn down
inline void buildBank(Bank& bank, uint64_t seed, float sr,
                      std::atomic<int>* progress,
                      std::atomic<bool>* abort) {
    bank.seed = seed;
    bank.sr = sr;
    bank.loops.resize(kBankLoops);
    bank.skips.resize(kBankSkips);
    bank.micros.resize(kBankMicros);
    Rng rng;
    int done = 0;
    for (int i = 0; i < kBankLoops; i++) {
        if (abort && abort->load()) return;
        rng.seed(seed ^ (0x1000ull + i) * 0x9e3779b97f4a7c15ull);
        renderLoop(rng, sr, bank.loops[i]);
        if (progress) progress->store(++done);
    }
    for (int i = 0; i < kBankSkips; i++) {
        if (abort && abort->load()) return;
        rng.seed(seed ^ (0x2000ull + i) * 0x9e3779b97f4a7c15ull);
        genSkip(rng, sr, bank.skips[i]);
        if (progress) progress->store(++done);
    }
    for (int i = 0; i < kBankMicros; i++) {
        if (abort && abort->load()) return;
        rng.seed(seed ^ (0x3000ull + i) * 0x9e3779b97f4a7c15ull);
        genMicro(rng, sr, bank.micros[i]);
        if (progress) progress->store(++done);
    }
}

} // namespace imber_gen
