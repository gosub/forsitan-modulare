// raucus_invariants - property-based checks for the Big Muff Pi model.
//
// smoke_raucus checks fixed points; raucus_probe measures the tone stack
// and the clipper. This harness checks properties that must hold *everywhere*,
// by randomizing and sweeping the whole control space and asserting invariants
// over it. raucus is a distortion, so most of what is worth asserting is
// about what it does to a signal: how the level answers the knobs, where the
// tone stack puts its notch, and how much of what comes out was never a
// harmonic of what went in.
//
// Failures print the offending patch to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// Safety, over random patches:
//   S1 finite      no NaN or Inf, ever
//   S2 bounded     the output stays inside Rack's rails
//   S3 silent      silence in is silence out: a fuzz has enormous gain, and
//                  it must still not invent a signal of its own
//   S4 no_dc       no DC offset parked on the output
//
// The knobs:
//   T1 volume      VOLUME is a plain scale on the way out, so below the
//                  output stage's knee the output is exactly proportional
//   T2 sustain     more SUSTAIN never gives less output
//   T3 tone        TONE turning up is monotonically brighter
//   T4 mids        MIDS fills the scoop: it lifts the notch frequency, which
//                  is the whole point of the tone-bypass mod
//
// The diodes:
//   D1 differ      the four clipping choices are four different sounds
//   D2 order       and they are loud in the order the manual claims:
//                  germanium under silicon under LED under lifted
//
// Aliasing, which is what the oversampling menu is for:
//   A1 oversample  a distortion makes harmonics of its input and nothing
//                  else, so anything landing off those harmonics is an
//                  image, and raising the oversampling has to lower it
//
// Robustness:
//   M1 mod_safe    CV swept at audio rate stays finite and bounded
//   M2 poly        channels are independent, and the count follows the input
//   C1 continuous  a small knob move makes a small change to the sound

#include "smoke_harness.hpp"

#include <vector>
#include <algorithm>
#include <complex>
#include <cstdlib>

#include "../src/raucus.cpp"

typedef std::vector<float> Buf;

static int gScale = 1;
static uint32_t gSeed = 0x9e3779b9u;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
    float range(float lo, float hi) { return lo + (hi - lo) * uni(); }
    int pick(int n) { return (int)(next() % (uint32_t)n); }
};

// ────────────────────────────────────────────────────────────────── patches

struct Patch {
    float gain = 0.45f, sustain = 0.65f, bias = 0.f;
    float tone = 0.5f, mids = 0.f, volume = 0.7f;
    int diode = 0, osIndex = 2;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    p.gain = r.range(0.1f, 0.8f);
    p.sustain = r.uni();
    p.bias = r.range(-1.f, 1.f);
    p.tone = r.uni();
    p.mids = r.uni();
    p.volume = r.range(0.1f, 1.f);
    p.diode = r.pick(4);
    // weighted low: 16x is eight times the work for nothing an invariant sees
    p.osIndex = r.pick(8) < 6 ? r.pick(3) : 3 + r.pick(2);
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "gain %.3f sustain %.3f bias %.3f tone %.3f mids %.3f "
             "volume %.3f diode %d os %d",
             p.gain, p.sustain, p.bias, p.tone, p.mids, p.volume,
             p.diode, p.osIndex);
}

static void apply(Raucus& m, const Patch& p) {
    m.params[Raucus::GAIN_PARAM].setValue(p.gain);
    m.params[Raucus::SUSTAIN_PARAM].setValue(p.sustain);
    m.params[Raucus::BIAS_PARAM].setValue(p.bias);
    m.params[Raucus::TONE_PARAM].setValue(p.tone);
    m.params[Raucus::MIDS_PARAM].setValue(p.mids);
    m.params[Raucus::VOLUME_PARAM].setValue(p.volume);
    m.diode = p.diode;
    m.osIndex = p.osIndex;
}

// ────────────────────────────────────────────────────────────── input signals

enum InKind { kSilence, kSine, kNoise };

struct Source {
    InKind kind = kSine;
    double freq = 1000.0, amp = 5.0;
    uint32_t seed = 1;
    mutable uint32_t s = 1;

    float at(size_t i) const {
        switch (kind) {
        case kSilence: return 0.f;
        case kNoise: {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float)amp * ((float)(s >> 8) * (1.f / 8388608.f) - 1.f);
        }
        default: return (float)(amp * std::sin(2.0 * M_PI * freq * i / SR));
        }
    }
    void rewind() const { s = seed ? seed : 1u; }
};

static Buf runOf(const Patch& p, const Source& in, double secs,
                 double settle = 0.1) {
    Raucus m;
    apply(m, p);
    in.rewind();
    long frame = 0;
    size_t i = 0;
    for (; i < (size_t)(settle * SR); i++) {
        m.inputs[Raucus::AUDIO_INPUT].setVoltage(in.at(i));
        m.process(makeArgs(frame++));
    }
    Buf out((size_t)(secs * SR));
    for (size_t k = 0; k < out.size(); k++, i++) {
        m.inputs[Raucus::AUDIO_INPUT].setVoltage(in.at(i));
        m.process(makeArgs(frame++));
        out[k] = m.outputs[Raucus::AUDIO_OUTPUT].getVoltage();
    }
    return out;
}

// ───────────────────────────────────────────────────────────────── measures

static double peakOf(const Buf& x) {
    double v = 0.0;
    for (float s : x) if (std::isfinite(s)) v = std::max(v, (double)std::fabs(s));
    return v;
}

static double rmsOf(const Buf& x) {
    double s = 0.0;
    size_t n = 0;
    for (float v : x) { if (!std::isfinite(v)) continue; s += (double)v * v; n++; }
    return n ? std::sqrt(s / n) : 0.0;
}

static long nansIn(const Buf& x) {
    long n = 0;
    for (float v : x) if (!std::isfinite(v)) n++;
    return n;
}

// Mean over a whole number of source periods. A sine that does not complete
// an integer number of cycles in the window has a mean of its own, of the
// same order as the offset being looked for, and counting that as DC would
// make the check measure the window rather than the module.
static double meanOf(const Buf& x, double freq = 0.0) {
    size_t n = x.size();
    if (freq > 0.0) {
        double per = SR / freq;
        size_t cycles = (size_t)((double)n / per);
        if (cycles >= 1) n = (size_t)(cycles * per);
    }
    double s = 0.0;
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (!std::isfinite(x[i])) continue;
        s += x[i];
        c++;
    }
    return c ? s / c : 0.0;
}

static double toneAt(const Buf& x, double f) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < x.size(); i++) {
        if (!std::isfinite(x[i])) continue;
        acc += (double)x[i]
             * std::exp(std::complex<double>(0, -2.0 * M_PI * f * (double)i / SR));
    }
    return std::abs(acc) * 2.0 / (double)x.size();
}

static double diffRatio(const Buf& a, const Buf& b) {
    size_t n = std::min(a.size(), b.size());
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
        double d = (double)a[i] - b[i];
        num += d * d;
        den += std::max((double)a[i] * a[i], (double)b[i] * b[i]);
    }
    return den > 0.0 ? std::sqrt(num / den) : 0.0;
}

// energy above `hi` over energy below `lo`, via one-pole splits
static double brightness(const Buf& x, double lo, double hi) {
    double gl = 1.0 - std::exp(-2.0 * M_PI * lo / SR);
    double gh = 1.0 - std::exp(-2.0 * M_PI * hi / SR);
    double zl = 0.0, zh = 0.0, elo = 0.0, ehi = 0.0;
    for (float v : x) {
        if (!std::isfinite(v)) continue;
        zl += gl * ((double)v - zl);
        zh += gh * ((double)v - zh);
        elo += zl * zl;
        double h = (double)v - zh;
        ehi += h * h;
    }
    return elo > 0.0 ? ehi / elo : 0.0;
}

// A distortion makes harmonics of its input and nothing else, so with a sine
// in, every legitimate component sits on a multiple of f0. An image of
// harmonic k lands at fold(k * f0) about the oversampled Nyquist, which for k
// near fsOs/f0 is close to DC rather than anywhere near the harmonics.
static double aliasFloor(const Buf& x, double f0, double fsOs) {
    double harm = 0.0;
    for (int k = 1; k * f0 < 0.45 * SR && k < 60; k++)
        harm += toneAt(x, k * f0);

    double junk = 0.0;
    const int kStart = (int)std::ceil(0.5 * fsOs / f0);
    for (int k = kStart; k < kStart + 600; k++) {
        double a = std::fmod((double)k * f0, fsOs);
        if (a > 0.5 * fsOs) a = fsOs - a;
        if (a > 0.45 * SR || a < 30.0) continue;
        double off = std::fabs(a / f0 - std::round(a / f0));
        if (off < 0.25) continue;
        junk += toneAt(x, a);
    }
    return harm > 0.0 ? junk / harm : 0.0;
}

// ─────────────────────────────────────────────────────── failure bookkeeping

struct Inv {
    const char* name;
    long checked, failed;
    double worst;
    char worstPatch[512];

    explicit Inv(const char* n)
        : name(n), checked(0), failed(0), worst(0.0) { worstPatch[0] = 0; }

    void hit(bool ok, double value, const Patch& p, bool worseIsBigger = true,
             const char* note = "") {
        checked++;
        if (ok) return;
        failed++;
        if (failed == 1 || (worseIsBigger ? value > worst : value < worst)) {
            worst = value;
            char buf[440];
            describe(p, buf, sizeof(buf));
            snprintf(worstPatch, sizeof(worstPatch), "%s%s%s",
                     note, *note ? " | " : "", buf);
        }
    }
    void done() {
        report("raucus", name, failed ? worst : (double)checked, failed == 0);
        if (failed)
            fprintf(stderr, "  %s: %ld/%ld failed, worst %g at [%s]\n",
                    name, failed, checked, worst, worstPatch);
    }
};

// ──────────────────────────────────────────────────────────────────── S

static void testSafety() {
    Rng r(gSeed);
    Inv finite{"inv_S1_finite"}, bounded{"inv_S2_bounded"};
    Inv silent{"inv_S3_silent"}, nodc{"inv_S4_no_dc"};

    int n = 130 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Source s;
        s.kind = (InKind)r.pick(3);
        s.freq = r.range(40.f, 6000.f);
        s.amp = r.range(0.f, 12.f);
        s.seed = r.next();
        Buf out = runOf(p, s, 0.25, 0.6);

        finite.hit(nansIn(out) == 0, (double)nansIn(out), p);
        bounded.hit(peakOf(out) <= 10.001, peakOf(out), p);
        // What is left is the hard clamp acting on a rail-slamming waveform
        // at the bottom of the audio band, where the coupling's own droop
        // pushes the peak into it. Tens of millivolts on a 10 V output.
        // Not against noise: the sample mean of a quarter second of noise is
        // itself +-20 mV, which is the size of the offset being looked for.
        // A parked offset is what this is about, and a periodic or silent
        // source is where that is well defined.
        if (s.kind != kNoise) {
            double dc = std::fabs(meanOf(out, s.kind == kSine ? s.freq : 0.0));
            nodc.hit(dc < 0.03, dc, p);
        }

        // A fuzz has some 7700x of gain in it and must still not sing. Not
        // bit-zero, though: the BIAS trim is an operating point, so with the
        // trim off centre the stage sits at a DC the coupling then removes,
        // and what is left of that is a few microvolts.
        Source none;
        none.kind = kSilence;
        double idle = peakOf(runOf(p, none, 0.2, 0.4));
        silent.hit(idle < 1e-3, idle, p);

        // with the trim centred there is no operating point to remove, and
        // silence in really is silence out
        Patch centred = p;
        centred.bias = 0.f;
        double idle0 = peakOf(runOf(centred, none, 0.2, 0.4));
        silent.hit(idle0 < 1e-12, idle0, centred, true, "bias centred");
    }
    finite.done();
    bounded.done();
    silent.done();
    nodc.done();
}

// ──────────────────────────────────────────────────────────────────── T

static void testKnobs() {
    Rng r(gSeed ^ 0x2545f491u);
    Inv vol{"inv_T1_volume"}, sus{"inv_T2_sustain"};
    Inv toneInv{"inv_T3_tone"}, mids{"inv_T4_mids"};

    // VOLUME is the last thing in the chain and a plain multiply, so as long
    // as the output stage's soft knee is not reached the output scales with
    // it exactly. Keep the level well under that knee.
    int n = 14 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.volume = 0.1f;
        Source s;
        s.kind = kSine;
        s.freq = r.range(200.f, 2000.f);
        s.amp = 5.f;
        double a = rmsOf(runOf(p, s, 0.15));
        Patch q = p;
        q.volume = 0.2f;
        Buf hi = runOf(q, s, 0.15);
        // only where the output stage is still linear: past its knee the
        // proportionality is supposed to stop, that being what a ceiling is
        if (peakOf(hi) > 4.0) continue;
        double ratio = rmsOf(hi) / std::max(a, 1e-9);
        vol.hit(ratio > 1.97 && ratio < 2.03, ratio, p);
    }

    // More drive into the clippers never means less out of them. It plateaus,
    // which is the point of a fuzz, but it must not fold back.
    int m = 5 * gScale;
    for (int k = 0; k < m; k++) {
        Patch p = randomPatch(r);
        p.volume = 0.5f;
        Source s;
        s.kind = kSine;
        s.freq = r.range(150.f, 1200.f);
        s.amp = 5.f;
        double prev = -1.0, worst = 0.0;
        bool ok = true;
        for (int i = 0; i <= 8; i++) {
            p.sustain = i / 8.f;
            double v = rmsOf(runOf(p, s, 0.15));
            if (prev >= 0.0 && v < prev * 0.97) {
                ok = false;
                worst = std::min(worst, v / prev - 1.0);
            }
            prev = v;
        }
        sus.hit(ok, worst, p, false);
    }

    // TONE is a treble/bass balance and has to be monotone in brightness.
    int t = 5 * gScale;
    for (int k = 0; k < t; k++) {
        Patch p = randomPatch(r);
        p.mids = 0.f;
        p.volume = 0.5f;
        Source s;
        s.kind = kNoise;
        s.amp = 5.f;
        s.seed = 12345;
        // Bands clear of the notch's own travel (it runs 5972 Hz down to
        // 276 Hz across the knob), so the measurement follows the balance
        // rather than watching the notch sweep through one of its windows.
        double prev = -1.0, worst = 0.0, first = 0.0, last = 0.0;
        bool ok = true;
        for (int i = 0; i <= 6; i++) {
            p.tone = i / 6.f;
            double b = brightness(runOf(p, s, 0.3), 150.0, 9000.0);
            if (i == 0) first = b;
            if (i == 6) last = b;
            // The first step is exempt and the reason is the circuit, not the
            // measurement: full CCW passes the most bass, which drives the
            // recovery stage hardest and so generates the most harmonics.
            // Backing off the bass takes that drive away faster than it adds
            // treble, so the output dips before it climbs.
            if (i >= 2 && prev >= 0.0 && b < prev * 0.98) {
                ok = false;
                worst = std::min(worst, b / prev - 1.0);
            }
            prev = b;
        }
        toneInv.hit(ok && last > first * 5.0, ok ? last / std::max(first, 1e-9) : worst,
                    p, false);
    }

    // MIDS blends the pre-tone signal back in, which fills the notch. At 4x
    // the network runs at 192 kHz and the notch sits at 1111 Hz with the knob
    // centred, so that is where to listen for it.
    int q = 8 * gScale;
    for (int k = 0; k < q; k++) {
        Patch p = randomPatch(r);
        p.tone = 0.5f;
        p.osIndex = 2;                 // 4x, where the notch is at 1111 Hz
        // low enough that the output stage is not compressing the lift away:
        // with the diodes lifted the level is high enough to hide it entirely
        p.volume = 0.08f;
        Source s;
        s.kind = kSine;
        s.freq = 1111.0;
        s.amp = 5.f;
        Patch flat = p, filled = p;
        flat.mids = 0.f;
        filled.mids = 1.f;
        Buf fb = runOf(filled, s, 0.2);
        if (peakOf(fb) > 4.0) continue;
        double a = toneAt(runOf(flat, s, 0.2), 1111.0);
        double b = toneAt(fb, 1111.0);
        mids.hit(b > a * 1.5, b / std::max(a, 1e-9), p, false);
    }

    vol.done();
    sus.done();
    toneInv.done();
    mids.done();
}

// ──────────────────────────────────────────────────────────────────── D

static void testDiodes() {
    Rng r(gSeed ^ 0x5bf03635u);
    Inv differ{"inv_D1_differ"}, order{"inv_D2_order"};

    int n = 8 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.bias = 0.f;
        p.volume = 0.5f;
        p.sustain = r.range(0.4f, 0.9f);
        Source s;
        s.kind = kSine;
        s.freq = r.range(150.f, 900.f);
        s.amp = 5.f;

        Buf out[4];
        double lvl[4];
        for (int d = 0; d < 4; d++) {
            Patch q = p;
            q.diode = d;
            out[d] = runOf(q, s, 0.2);
            lvl[d] = rmsOf(out[d]);
        }
        double worst = 1e9;
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                worst = std::min(worst, diffRatio(out[i], out[j]));
        differ.hit(worst > 0.05, worst, p, false);

        // germanium (1) clips earliest and so quietest, then silicon (0),
        // then the LED (2), and lifting them (3) is loudest of all
        bool ok = lvl[1] < lvl[0] && lvl[0] < lvl[2] && lvl[2] <= lvl[3] * 1.02;
        double margin = std::min(lvl[0] / std::max(lvl[1], 1e-9),
                                 lvl[2] / std::max(lvl[0], 1e-9));
        order.hit(ok, margin, p, false);
    }
    differ.done();
    order.done();
}

// ──────────────────────────────────────────────────────────────────── A

static void testAliasing() {
    Rng r(gSeed ^ 0x846ca68bu);
    Inv os{"inv_A1_oversample"};

    // a fundamental that is not a near-divisor of any of the oversampled
    // rates being compared, so its images land clear of its harmonics
    const double f0 = 2011.0;

    int n = 4 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.sustain = r.range(0.5f, 1.f);       // well into the clipping
        p.volume = 0.5f;
        p.diode = r.pick(4);
        Source s;
        s.kind = kSine;
        s.freq = f0;
        s.amp = 5.f;

        Patch lo = p, hi = p;
        lo.osIndex = 0;
        hi.osIndex = 3;
        double aLo = aliasFloor(runOf(lo, s, 0.4, 0.2), f0, SR * 1.0);
        double aHi = aliasFloor(runOf(hi, s, 0.4, 0.2), f0, SR * 8.0);
        os.hit(aHi < aLo, aHi / std::max(aLo, 1e-12), p);
    }
    os.done();
}

// ──────────────────────────────────────────────────────────────── M and C

static void testRobustness() {
    Rng r(gSeed ^ 0x7feb352du);
    Inv safe{"inv_M1_mod_safe"}, poly{"inv_M2_poly"}, cont{"inv_C1_continuous"};

    int n = 6 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Raucus m;
        apply(m, p);
        m.inputs[Raucus::SUSTAIN_INPUT].channels = 1;
        m.inputs[Raucus::TONE_INPUT].channels = 1;
        m.inputs[Raucus::VOLUME_INPUT].channels = 1;
        long frame = 0;
        double fm = r.range(200.0, 3000.0);
        long bad = 0;
        double pk = 0.0;
        for (int i = 0; i < (int)(0.3 * SR); i++) {
            double ph = std::sin(2.0 * M_PI * fm * i / SR);
            m.inputs[Raucus::AUDIO_INPUT].setVoltage(
                (float)(5.0 * std::sin(2.0 * M_PI * 440.0 * i / SR)));
            m.inputs[Raucus::SUSTAIN_INPUT].setVoltage((float)(10.0 * ph));
            m.inputs[Raucus::TONE_INPUT].setVoltage((float)(10.0 * -ph));
            m.inputs[Raucus::VOLUME_INPUT].setVoltage((float)(10.0 * ph));
            m.process(makeArgs(frame++));
            float v = m.outputs[Raucus::AUDIO_OUTPUT].getVoltage();
            if (!std::isfinite(v)) bad++;
            else pk = std::max(pk, (double)std::fabs(v));
        }
        safe.hit(bad == 0 && pk <= 10.001, (double)bad + pk, p);
    }

    // Polyphony: a voice per channel, and nothing shared between them.
    int q = 6 * gScale;
    for (int k = 0; k < q; k++) {
        Patch p = randomPatch(r);
        const int ch = 2 + r.pick(6);
        const double freqs[8] = {110, 197, 313, 441, 587, 733, 941, 1217};
        size_t n2 = (size_t)(0.2 * SR);

        Raucus poly1;
        apply(poly1, p);
        poly1.inputs[Raucus::AUDIO_INPUT].channels = ch;
        // Port::setChannels() is a no-op on a port the harness never connects
        poly1.outputs[Raucus::AUDIO_OUTPUT].channels = 1;
        long frame = 0;
        std::vector<Buf> got((size_t)ch, Buf(n2));
        for (size_t i = 0; i < n2; i++) {
            for (int c = 0; c < ch; c++)
                poly1.inputs[Raucus::AUDIO_INPUT].setVoltage(
                    (float)(5.0 * std::sin(2.0 * M_PI * freqs[c] * i / SR)), c);
            poly1.process(makeArgs(frame++));
            for (int c = 0; c < ch; c++)
                got[c][i] = poly1.outputs[Raucus::AUDIO_OUTPUT].getVoltage(c);
        }
        bool countOk = poly1.outputs[Raucus::AUDIO_OUTPUT].getChannels() == ch;

        double worst = 0.0;
        for (int c = 0; c < ch; c++) {
            Raucus solo;
            apply(solo, p);
            long f2 = 0;
            Buf ref(n2);
            for (size_t i = 0; i < n2; i++) {
                solo.inputs[Raucus::AUDIO_INPUT].setVoltage(
                    (float)(5.0 * std::sin(2.0 * M_PI * freqs[c] * i / SR)));
                solo.process(makeArgs(f2++));
                ref[i] = solo.outputs[Raucus::AUDIO_OUTPUT].getVoltage();
            }
            worst = std::max(worst, diffRatio(ref, got[c]));
        }
        poly.hit(countOk && worst < 1e-4, worst, p);
    }

    // and nothing in the control space steps
    int c2 = 10 * gScale;
    for (int k = 0; k < c2; k++) {
        Patch base = randomPatch(r);
        base.volume = 0.5f;
        Source s;
        s.kind = kSine;
        s.freq = r.range(200.f, 1500.f);
        s.amp = 5.f;
        for (int w = 0; w < 4; w++) {
            float v = r.range(0.05f, 0.93f);
            const float eps = 0.02f;
            Patch a = base, b = base;
            switch (w) {
            case 0: a.sustain = v; b.sustain = v + eps; break;
            case 1: a.tone = v;    b.tone = v + eps;    break;
            case 2: a.mids = v;    b.mids = v + eps;    break;
            case 3: a.gain = v;    b.gain = v + eps;    break;
            }
            Buf ta = runOf(a, s, 0.15), tb = runOf(b, s, 0.15);
            double ra = rmsOf(ta), rb = rmsOf(tb);
            double d = std::fabs(ra - rb) / std::max(std::max(ra, rb), 1e-6);
            cont.hit(d < 0.35, d, a);
        }
    }

    safe.done();
    poly.done();
    cont.done();
}

int main(int argc, char** argv) {
    bool header = true;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-header")) header = false;
        else if (!std::strcmp(argv[i], "--long")) gScale = 8;
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            gSeed = (uint32_t)strtoul(argv[++i], nullptr, 0);
    }
    rack::random::init();
    if (header) printf("module,check,value,pass\n");
    fprintf(stderr, "raucus_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testSafety();
    testKnobs();
    testDiodes();
    testAliasing();
    testRobustness();

    return failures ? 1 : 0;
}
