// cartilago_invariants — property-based checks for the Gristleizer modulator.
//
// smoke_cartilago checks fixed points; cartilago_probe measures character.
// This harness checks properties that must hold *everywhere*, by randomizing
// and sweeping the whole control space and asserting invariants over it. It
// is the same approach as tundo_invariants, but cartilago is an effect rather
// than a voice, so the invariants are about what it does to a signal handed
// to it rather than about the signal it makes.
//
// Failures print the offending patch to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// Safety, over random patches:
//   S1 finite      no NaN or Inf on either output, ever
//   S2 bounded     audio within +-10 V, LFO within its own +-7.5 V clamp
//   S3 silent      nothing in and the tick switched off is exactly silence
//   S4 no_dc       no DC offset parked on the output
//
// Signal path, the things an effect owes its input:
//   T1 unity       wide open and undriven, it passes audio at unity gain
//   T2 vca_range   the attenuator spans the range its divider implies, and
//                  never actually closes: it is a FET, not a switch
//   T3 vca_mono    the gain rises monotonically with the control. A curve
//                  that folds back would make the tremolo stutter, and an
//                  earlier draft of this attenuator did exactly that
//
// The LFO:
//   L1 lfo_bounded the LFO output stays inside its clamp
//   L2 lfo_rate    it runs at the frequency the knob asks for
//   L3 lfo_voct    and tracks 1 V/oct on top of that
//   L4 sync        a sync pulse resets the phase
//
// Distinctness, over random patches:
//   D1 modes       VCA and VCF are different effects at the same settings
//   D2 knobs_live  every knob measurably changes the output, within the
//                  preconditions its own function implies
//   D3 waves       the four LFO shapes are four different modulations
//
// Continuity:
//   C1 continuous  a small knob move makes a small change to the sound
//
// Robustness:
//   M1 mod_safe    CV swept at audio rate stays finite and bounded
//   M2 poly        channels are independent, and the count follows the input
//
// Aliasing, which is the reason the LFO is band-limited at all:
//   A1 bandlimit   at an audio-rate LFO, band-limiting lowers the alias floor
//                  it leaves in the audio path
//   A2 oversample  and so does oversampling

#include "smoke_harness.hpp"

#include <vector>
#include <algorithm>
#include <complex>
#include <cstdlib>

#include "../src/cartilago.cpp"

typedef std::vector<float> Buf;

static int gScale = 1;          // --long multiplies the patch counts
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
    int wave = 0;
    float rate = 0.f;         // octaves, the knob's own units
    int mode = 0;             // 0 VCA, 1 VCF
    float depth = 0.75f, bias = 0.f, shape = 0.5f;
    float drive = 0.5f, res = 0.45f, level = 1.f;
    int osIndex = 1;
    bool bandLimit = true, feedthrough = true, lowpassMode = false;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    p.wave = r.pick(4);
    p.rate = r.range(-2.f, 5.f);          // 0.25 Hz to 32 Hz
    p.mode = r.pick(2);
    p.depth = r.range(0.f, 1.25f);
    p.bias = r.range(-1.f, 1.f);
    p.shape = r.range(0.02f, 0.98f);
    p.drive = r.range(0.2f, 0.8f);
    p.res = r.uni();
    p.level = r.range(0.f, 2.f);
    // weighted low: 16x oversampling is eight times the work and adds nothing
    // an invariant can see that 4x does not
    p.osIndex = r.pick(8) < 6 ? r.pick(3) : 3 + r.pick(2);
    p.bandLimit = r.pick(4) != 0;
    p.feedthrough = r.pick(2) == 0;
    p.lowpassMode = r.pick(2) == 0;
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "wave %d rate %.3f mode %d depth %.3f bias %.3f shape %.3f "
             "drive %.3f res %.3f level %.3f os %d bl %d feed %d lp %d",
             p.wave, p.rate, p.mode, p.depth, p.bias, p.shape, p.drive,
             p.res, p.level, p.osIndex, (int)p.bandLimit, (int)p.feedthrough,
             (int)p.lowpassMode);
}

static void apply(Cartilago& m, const Patch& p) {
    m.params[Cartilago::WAVE_PARAM].setValue((float)p.wave);
    m.params[Cartilago::RATE_PARAM].setValue(p.rate);
    m.params[Cartilago::MODE_PARAM].setValue((float)p.mode);
    m.params[Cartilago::DEPTH_PARAM].setValue(p.depth);
    m.params[Cartilago::BIAS_PARAM].setValue(p.bias);
    m.params[Cartilago::SHAPE_PARAM].setValue(p.shape);
    m.params[Cartilago::DRIVE_PARAM].setValue(p.drive);
    m.params[Cartilago::RES_PARAM].setValue(p.res);
    m.params[Cartilago::LEVEL_PARAM].setValue(p.level);
    m.osIndex = p.osIndex;
    m.bandLimit = p.bandLimit;
    m.feedthrough = p.feedthrough;
    m.lowpassMode = p.lowpassMode;
}

// ────────────────────────────────────────────────────────────── input signals

enum InKind { kSilence, kSine, kDC, kNoise };

struct Source {
    InKind kind = kSine;
    double freq = 1000.0, amp = 2.0;
    uint32_t seed = 1;
    mutable uint32_t s = 1;

    float at(size_t i) const {
        switch (kind) {
        case kSilence: return 0.f;
        case kDC: return (float)amp;
        case kNoise: {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float)amp * ((float)(s >> 8) * (1.f / 8388608.f) - 1.f);
        }
        default: return (float)(amp * std::sin(2.0 * M_PI * freq * i / SR));
        }
    }
    void rewind() const { s = seed ? seed : 1u; }
};

struct Take { Buf audio, lfo; };

// settle the gate one-pole and the filter, then capture
static Take runOf(const Patch& p, const Source& in, double secs,
                  double settle = 0.15) {
    Cartilago m;
    apply(m, p);
    in.rewind();
    long frame = 0;
    size_t i = 0;
    for (; i < (size_t)(settle * SR); i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(in.at(i));
        m.process(makeArgs(frame++));
    }
    Take t;
    size_t n = (size_t)(secs * SR);
    t.audio.resize(n);
    t.lfo.resize(n);
    for (size_t k = 0; k < n; k++, i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(in.at(i));
        m.process(makeArgs(frame++));
        t.audio[k] = m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage();
        t.lfo[k] = m.outputs[Cartilago::LFO_OUTPUT].getVoltage();
    }
    return t;
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

static double meanOf(const Buf& x) {
    double s = 0.0;
    size_t n = 0;
    for (float v : x) { if (!std::isfinite(v)) continue; s += v; n++; }
    return n ? s / n : 0.0;
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

// magnitude at f over the whole buffer
static double toneAt(const Buf& x, double f) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < x.size(); i++) {
        if (!std::isfinite(x[i])) continue;
        acc += (double)x[i]
             * std::exp(std::complex<double>(0, -2.0 * M_PI * f * (double)i / SR));
    }
    return std::abs(acc) * 2.0 / (double)x.size();
}

// gain of the whole chain on a small sine, which is small enough to stay off
// the saturator's knee
static double gainAt(const Patch& p, double freq = 1000.0, double amp = 0.5) {
    Source s;
    s.kind = kSine;
    s.freq = freq;
    s.amp = amp;
    Take t = runOf(p, s, 0.1, 0.3);
    return toneAt(t.audio, freq) / amp;
}

// LFO frequency from rising zero crossings, with hysteresis so the polyBLEP
// ringing around an edge cannot be counted as a crossing of its own
static double lfoFreq(const Buf& x, double hyst = 1.0) {
    bool armed = false;
    double first = -1.0, last = -1.0;
    int n = 0;
    for (size_t i = 1; i < x.size(); i++) {
        if (x[i] < -hyst) armed = true;
        if (armed && x[i - 1] < 0.f && x[i] >= 0.f) {
            double t = (i - 1) + (double)(-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            else { last = t; n++; }
            armed = false;
        }
    }
    return n < 1 || last <= first ? 0.0 : SR * n / (last - first);
}

// Energy at the frequencies the modulator's own harmonics fold back to, over
// the energy at its harmonics. With a DC input in VCA mode the output is
// exactly the gain waveform, so everything legitimate sits on a multiple of
// f0 and an image of harmonic k lands at fold(k * f0) about the oversampled
// Nyquist.
//
// Where those images land is the whole point, and it is not midway between
// the harmonics: for k near fsOs/f0 an image lands within a few hertz of DC,
// which is exactly the region a naive check would never look at. Probing the
// midpoints instead finds nothing and calls the band-limiting useless.
static double aliasFloor(const Buf& x, double f0, double fsOs) {
    double harm = 0.0;
    for (int k = 1; k * f0 < 0.45 * SR && k < 60; k++)
        harm += toneAt(x, k * f0);

    double junk = 0.0;
    const int kStart = (int)std::ceil(0.5 * fsOs / f0);   // the first image
    for (int k = kStart; k < kStart + 600; k++) {
        double a = std::fmod((double)k * f0, fsOs);
        if (a > 0.5 * fsOs) a = fsOs - a;
        // above this the decimation filter has already removed it; below it,
        // the output coupling has
        if (a > 0.45 * SR || a < 30.0) continue;
        // an image landing on a harmonic cannot be told from the harmonic
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
        report("cartilago", name, failed ? worst : (double)checked, failed == 0);
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

    int n = 150 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Source s;
        s.kind = (InKind)r.pick(4);
        s.freq = r.range(50.f, 6000.f);
        s.amp = r.range(0.f, 10.f);
        s.seed = r.next();
        Take t = runOf(p, s, 0.35);

        long bad = nansIn(t.audio) + nansIn(t.lfo);
        finite.hit(bad == 0, (double)bad, p);

        double pa = peakOf(t.audio), pl = peakOf(t.lfo);
        bounded.hit(pa <= 10.001 && pl <= 7.501,
                    std::max(pa - 10.001, pl - 7.501), p);

        // The output coupling has to keep a *static* offset off the jack: a
        // constant in, the modulation held still, and nothing should come
        // out. Under modulation the output carries the modulation, and at a
        // slow rate that is legitimately sub-audio content rather than an
        // offset, so measuring a mean over a fraction of an LFO cycle would
        // be measuring the tremolo and calling it DC.
        Patch stat = p;
        stat.depth = 0.f;
        Source dc;
        dc.kind = kDC;
        dc.amp = 5.0;
        // the mean, not the peak: an offset is what this is about, and a
        // resonant filter still ringing down from the step at startup (at
        // full resonance and 46 Hz it takes the best part of a second) is a
        // transient that averages away, not an offset
        double resid = std::fabs(meanOf(runOf(stat, dc, 0.25, 0.6).audio));
        nodc.hit(resid < 0.02, resid, stat);

        // nothing in, tick off: exactly silence. (With the tick on it is
        // supposed to click, which is what inv_D2 checks the switch for.)
        Patch q = p;
        q.feedthrough = false;
        Source none;
        none.kind = kSilence;
        double idle = peakOf(runOf(q, none, 0.25).audio);
        silent.hit(idle < 1e-9, idle, q);
    }
    finite.done();
    bounded.done();
    silent.done();
    nodc.done();
}

// ──────────────────────────────────────────────────────────────────── T

static void testSignalPath() {
    Rng r(gSeed ^ 0x2545f491u);
    Inv unity{"inv_T1_unity"}, rangeInv{"inv_T2_vca_range"};
    Inv mono{"inv_T3_vca_mono"};

    int n = 12 * gScale;
    for (int k = 0; k < n; k++) {
        // wide open: no modulation, the FET pinched off, unity drive
        Patch p;
        p.mode = 0;
        p.depth = 0.f;
        p.bias = 1.f;
        p.drive = 0.5f;               // 2^0 = 1
        p.level = 1.f;
        p.osIndex = r.pick(4);
        p.bandLimit = r.pick(2) == 0;
        p.feedthrough = r.pick(2) == 0;
        double g = gainAt(p, r.range(200.f, 4000.f), 0.5);
        unity.hit(g > 0.94 && g < 1.06, g, p);

        // and the two ends of the control, which is a divider and so has a
        // floor: R/Ron = 20 puts it at about -26 dB, never at silence
        Patch open = p, shut = p;
        shut.bias = -1.f;
        double go = gainAt(open, 1000.0, 0.5), gs = gainAt(shut, 1000.0, 0.5);
        double db = 20.0 * std::log10(go / std::max(gs, 1e-9));
        rangeInv.hit(db > 20.0 && db < 32.0 && gs > 0.005, db, p);
    }

    // The gain has to climb monotonically with the control. This is the
    // property the first draft of the attenuator got wrong, by smoothstepping
    // a value that could exceed 1, and a fold-back here makes the tremolo
    // stutter at one end of its travel.
    int m = 3 * gScale;
    for (int k = 0; k < m; k++) {
        Patch p;
        p.mode = 0;
        p.depth = 0.f;
        p.drive = 0.5f;
        p.level = 1.f;
        p.osIndex = 1;
        p.feedthrough = false;
        double prev = -1.0, worst = 1.0;
        bool ok = true;
        for (int i = 0; i <= 20; i++) {
            p.bias = -1.f + 2.f * i / 20.f;
            double g = gainAt(p, 1000.0, 0.5);
            if (prev >= 0.0) {
                double step = g - prev;
                if (step < -1e-4) { ok = false; worst = std::min(worst, step); }
            }
            prev = g;
        }
        mono.hit(ok, worst, p, false);
    }

    unity.done();
    rangeInv.done();
    mono.done();
}

// ──────────────────────────────────────────────────────────────────── L

static void testLfo() {
    Rng r(gSeed ^ 0x5bf03635u);
    Inv bounded{"inv_L1_lfo_bounded"}, rateInv{"inv_L2_lfo_rate"};
    Inv voct{"inv_L3_lfo_voct"}, sync{"inv_L4_sync"};

    int n = 10 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.rate = r.range(1.f, 5.f);        // 2 to 32 Hz, measurable in a second
        p.depth = 1.f;
        Source none;
        none.kind = kSilence;
        Take t = runOf(p, none, 1.2, 0.05);

        bounded.hit(peakOf(t.lfo) <= 7.501, peakOf(t.lfo), p);

        double want = std::exp2((double)p.rate);
        double got = lfoFreq(t.lfo);
        if (got > 0.0) {
            double cents = std::fabs(1200.0 * std::log2(got / want));
            rateInv.hit(cents < 40.0, cents, p);
        }

        // 1 V/oct on top of the knob
        for (int o = 1; o <= 2; o++) {
            Cartilago m;
            apply(m, p);
            m.inputs[Cartilago::VOCT_INPUT].channels = 1;
            m.inputs[Cartilago::VOCT_INPUT].setVoltage((float)o);
            long frame = 0;
            for (int i = 0; i < (int)(0.05 * SR); i++) m.process(makeArgs(frame++));
            Buf lfo((size_t)(1.2 * SR));
            for (size_t i = 0; i < lfo.size(); i++) {
                m.process(makeArgs(frame++));
                lfo[i] = m.outputs[Cartilago::LFO_OUTPUT].getVoltage();
            }
            double g2 = lfoFreq(lfo);
            if (g2 > 0.0) {
                double cents = std::fabs(1200.0 * std::log2(g2 / (want * std::exp2((double)o))));
                voct.hit(cents < 40.0, cents, p);
            }
        }
    }

    // a sync pulse puts the phase back to the start
    int q = 8 * gScale;
    for (int k = 0; k < q; k++) {
        Patch p = randomPatch(r);
        p.rate = r.range(-1.f, 4.f);
        Cartilago m;
        apply(m, p);
        long frame = 0;
        m.inputs[Cartilago::SYNC_INPUT].channels = 1;
        for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(frame++));
        m.inputs[Cartilago::SYNC_INPUT].setVoltage(5.f);
        m.process(makeArgs(frame++));
        // one block of phase advance is all it may have moved
        double dt = std::exp2((double)p.rate) / (SR * (1 << p.osIndex));
        sync.hit(m.lfo.phase <= dt * (1 << p.osIndex) * 1.5 + 1e-6,
                 m.lfo.phase, p);
    }

    bounded.done();
    rateInv.done();
    voct.done();
    sync.done();
}

// ──────────────────────────────────────────────────────────────────── D

static void testDistinct() {
    Rng r(gSeed ^ 0x846ca68bu);
    Inv modes{"inv_D1_modes"}, live{"inv_D2_knobs_live"}, waves{"inv_D3_waves"};

    Source sig;
    sig.kind = kSine;
    sig.amp = 3.0;

    int n = 20 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.level = r.range(0.5f, 2.f);      // an audible output either way
        sig.freq = r.range(200.f, 2000.f);
        Patch a = p, b = p;
        a.mode = 0;
        b.mode = 1;
        double d = diffRatio(runOf(a, sig, 0.3).audio, runOf(b, sig, 0.3).audio);
        modes.hit(d > 0.1, d, p, false);
    }

    // the four LFO shapes have to be four different modulations
    int w = 12 * gScale;
    for (int k = 0; k < w; k++) {
        Patch p = randomPatch(r);
        p.depth = r.range(0.6f, 1.25f);    // enough to hear the shape
        p.bias = r.range(-0.3f, 0.3f);
        p.level = 1.f;
        p.rate = r.range(0.f, 3.f);
        sig.freq = r.range(400.f, 2000.f);
        Buf out[4];
        for (int i = 0; i < 4; i++) {
            Patch q = p;
            q.wave = i;
            out[i] = runOf(q, sig, 0.5).audio;
        }
        double worst = 1e9;
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                worst = std::min(worst, diffRatio(out[i], out[j]));
        waves.hit(worst > 0.05, worst, p, false);
    }

    // Every knob, over its own travel. The preconditions are the ones the
    // circuit implies, not workarounds: SHAPE is the LFO's symmetry and the
    // two ramps have none to change, and RES belongs to the filter.
    static const char* knobName[8] = {"wave", "rate", "depth", "bias",
                                      "shape", "drive", "res", "level"};
    int m = 10 * gScale;
    for (int k = 0; k < m; k++) {
        Patch base = randomPatch(r);
        base.depth = r.range(0.5f, 1.25f);   // the LFO has to reach the audio
        base.level = 1.f;
        base.bias = r.range(-0.4f, 0.4f);
        base.rate = r.range(0.f, 3.f);
        base.drive = 0.5f;
        sig.freq = r.range(300.f, 2000.f);
        for (int i = 0; i < 8; i++) {
            Patch lo = base, hi = base;
            switch (i) {
            case 0: lo.wave = 0;      hi.wave = 3;      break;
            case 1: lo.rate = 0.f;    hi.rate = 3.f;    break;
            case 2: lo.depth = 0.f;   hi.depth = 1.25f; break;
            case 3: lo.bias = -1.f;   hi.bias = 1.f;    break;
            case 4: lo.shape = 0.1f;  hi.shape = 0.9f;
                    lo.wave = hi.wave = r.pick(2) ? 0 : 3;   // tri or pulse
                    break;
            case 5: lo.drive = 0.2f;  hi.drive = 0.8f;  break;
            case 6: lo.res = 0.f;     hi.res = 1.f;
                    lo.mode = hi.mode = 1;                   // VCF only
                    break;
            case 7: lo.level = 0.5f;  hi.level = 1.5f;  break;
            }
            double d = diffRatio(runOf(lo, sig, 0.4).audio, runOf(hi, sig, 0.4).audio);
            live.hit(d > 0.05, d, base, false, knobName[i]);
        }
    }

    modes.done();
    live.done();
    waves.done();
}

// ──────────────────────────────────────────────────────────────────── C

static void testContinuity() {
    Rng r(gSeed ^ 0x7feb352du);
    Inv cont{"inv_C1_continuous"};

    Source sig;
    sig.kind = kSine;
    sig.amp = 2.0;
    sig.freq = 800.0;

    int n = 14 * gScale;
    for (int k = 0; k < n; k++) {
        Patch base = randomPatch(r);
        base.level = 1.f;
        for (int w = 0; w < 5; w++) {
            float v = r.range(0.05f, 0.9f);
            const float eps = 0.02f;
            Patch a = base, b = base;
            switch (w) {
            case 0: a.depth = v * 1.25f;  b.depth = (v + eps) * 1.25f;  break;
            case 1: a.bias = v * 2.f - 1.f; b.bias = (v + eps) * 2.f - 1.f; break;
            case 2: a.shape = 0.02f + v * 0.96f;
                    b.shape = 0.02f + (v + eps) * 0.96f; break;
            case 3: a.drive = v;          b.drive = v + eps;            break;
            case 4: a.res = v;            b.res = v + eps;
                    a.mode = b.mode = 1; break;
            }
            Take ta = runOf(a, sig, 0.3), tb = runOf(b, sig, 0.3);
            double pa = peakOf(ta.audio), pb = peakOf(tb.audio);
            double ra = rmsOf(ta.audio), rb = rmsOf(tb.audio);
            double dPeak = std::fabs(pa - pb) / std::max(std::max(pa, pb), 1e-6);
            double dRms = std::fabs(ra - rb) / std::max(std::max(ra, rb), 1e-6);
            cont.hit(std::max(dPeak, dRms) < 0.5, std::max(dPeak, dRms), a);
        }
    }
    cont.done();
}

// ──────────────────────────────────────────────────────────────────── M

static void testRobustness() {
    Rng r(gSeed ^ 0x1b873593u);
    Inv safe{"inv_M1_mod_safe"}, poly{"inv_M2_poly"};

    // every CV input wiggled at audio rate
    int n = 8 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Cartilago m;
        apply(m, p);
        m.inputs[Cartilago::DEPTH_INPUT].channels = 1;
        m.inputs[Cartilago::BIAS_INPUT].channels = 1;
        m.inputs[Cartilago::MOD_INPUT].channels = 1;
        long frame = 0;
        double fm = r.range(200.0, 3000.0);
        long bad = 0;
        double pk = 0.0;
        int steps = (int)(0.4 * SR);
        for (int i = 0; i < steps; i++) {
            double ph = std::sin(2.0 * M_PI * fm * i / SR);
            m.inputs[Cartilago::AUDIO_INPUT].setVoltage(
                (float)(4.0 * std::sin(2.0 * M_PI * 700.0 * i / SR)));
            m.inputs[Cartilago::DEPTH_INPUT].setVoltage((float)(5.0 * ph));
            m.inputs[Cartilago::BIAS_INPUT].setVoltage((float)(5.0 * -ph));
            m.inputs[Cartilago::MOD_INPUT].setVoltage((float)(5.0 * ph));
            m.process(makeArgs(frame++));
            float v = m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage();
            float l = m.outputs[Cartilago::LFO_OUTPUT].getVoltage();
            if (!std::isfinite(v) || !std::isfinite(l)) bad++;
            else pk = std::max(pk, (double)std::fabs(v));
        }
        safe.hit(bad == 0 && pk <= 10.001, (double)bad + pk, p);
    }

    // Polyphony: one LFO for all of them, but the audio paths are their own.
    // Each channel must come out exactly as it would have alone.
    int q = 8 * gScale;
    for (int k = 0; k < q; k++) {
        Patch p = randomPatch(r);
        p.level = 1.f;
        const int ch = 2 + r.pick(6);
        const double freqs[8] = {220, 337, 512, 749, 913, 1201, 1607, 1999};

        Cartilago poly1;
        apply(poly1, p);
        poly1.inputs[Cartilago::AUDIO_INPUT].channels = ch;
        // Port::setChannels() is a no-op on a port the harness never connects
        poly1.outputs[Cartilago::AUDIO_OUTPUT].channels = 1;
        long frame = 0;
        size_t n2 = (size_t)(0.25 * SR);
        std::vector<Buf> got((size_t)ch, Buf(n2));
        for (size_t i = 0; i < n2; i++) {
            for (int c = 0; c < ch; c++)
                poly1.inputs[Cartilago::AUDIO_INPUT].setVoltage(
                    (float)(3.0 * std::sin(2.0 * M_PI * freqs[c] * i / SR)), c);
            poly1.process(makeArgs(frame++));
            for (int c = 0; c < ch; c++) got[c][i] = poly1.outputs[Cartilago::AUDIO_OUTPUT].getVoltage(c);
        }
        bool countOk = poly1.outputs[Cartilago::AUDIO_OUTPUT].getChannels() == ch;

        double worst = 0.0;
        for (int c = 0; c < ch; c++) {
            Cartilago solo;
            apply(solo, p);
            long f2 = 0;
            Buf ref(n2);
            for (size_t i = 0; i < n2; i++) {
                solo.inputs[Cartilago::AUDIO_INPUT].setVoltage(
                    (float)(3.0 * std::sin(2.0 * M_PI * freqs[c] * i / SR)));
                solo.process(makeArgs(f2++));
                ref[i] = solo.outputs[Cartilago::AUDIO_OUTPUT].getVoltage();
            }
            worst = std::max(worst, diffRatio(ref, got[c]));
        }
        poly.hit(countOk && worst < 1e-4, worst, p);
    }

    safe.done();
    poly.done();
}

// ──────────────────────────────────────────────────────────────────── A

// The LFO is band-limited so that running it into the audio band ring
// modulates instead of aliasing. With a DC input in VCA mode the output is
// exactly the gain waveform, so every legitimate component sits on a multiple
// of the LFO frequency and everything between them arrived by aliasing.
static void testAliasing() {
    Rng r(gSeed ^ 0x3c6ef372u);
    Inv bl{"inv_A1_bandlimit"}, os{"inv_A2_oversample"};

    Source dc;
    dc.kind = kDC;
    dc.amp = 5.0;

    // A rate whose images land *between* the harmonics at every oversampling
    // setting being compared, so they can be told apart from the harmonics at
    // all. This matters more than it looks: at 1477 Hz the oversampled rate
    // is 65.0 times the modulator, every image lands on top of a harmonic,
    // and both floors measure exactly zero. 1489 Hz puts 48k, 96k and 384k at
    // 32.2, 64.5 and 257.9 times the rate.
    //
    // Past the knob's own top, so a user reaches it through v/oct. Setting
    // the param directly is the same state, without a second cable to drive.
    const float rateOct = std::log2(1489.f);

    int n = 4 * gScale;
    for (int k = 0; k < n; k++) {
        // Ramp and pulse only. Those are the shapes with a step in them, and
        // a step is what polyBLEP is for: measured, band-limiting takes their
        // alias floor from 0.21-0.28 down to 0.026, a factor of ten. The
        // triangle has no step, only a corner, and polyBLAMP against a
        // spectrum already falling as 1/k^2 moves the floor by 1.5%, which is
        // inside the noise of this measurement. Claiming it here would make
        // the invariant a coin toss.
        for (int wave = 1; wave <= 3; wave += 2) {    // ramp up, pulse
            Patch p;
            p.wave = wave;
            p.rate = rateOct;
            p.mode = 0;
            p.depth = 1.f;
            p.bias = 0.f;
            p.shape = r.range(0.3f, 0.7f);
            p.drive = 0.5f;
            p.level = 1.f;
            p.osIndex = 1;
            p.feedthrough = false;

            Patch on = p, off = p;
            on.bandLimit = true;
            off.bandLimit = false;
            const double fsOs = SR * (1 << p.osIndex);
            double aOn = aliasFloor(runOf(on, dc, 0.5, 0.1).audio, 1489.0, fsOs);
            double aOff = aliasFloor(runOf(off, dc, 0.5, 0.1).audio, 1489.0, fsOs);
            bl.hit(aOn < aOff, aOn / std::max(aOff, 1e-12), p, true,
                   wave == 1 ? "ramp" : "pulse");

            // and more oversampling has to help too, not hurt
            Patch lo = on, hi = on;
            lo.osIndex = 0;
            hi.osIndex = 3;
            double aLo = aliasFloor(runOf(lo, dc, 0.5, 0.1).audio, 1489.0,
                                    SR * (1 << lo.osIndex));
            double aHi = aliasFloor(runOf(hi, dc, 0.5, 0.1).audio, 1489.0,
                                    SR * (1 << hi.osIndex));
            os.hit(aHi < aLo, aHi / std::max(aLo, 1e-12), p, true,
                   wave == 1 ? "ramp" : "pulse");
        }
    }
    bl.done();
    os.done();
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
    fprintf(stderr, "cartilago_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testSafety();
    testSignalPath();
    testLfo();
    testDistinct();
    testContinuity();
    testRobustness();
    testAliasing();

    return failures ? 1 : 0;
}
