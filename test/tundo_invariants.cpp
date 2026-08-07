// tundo_invariants — property-based checks for the tundo drum voice.
//
// smoke_tundo checks fixed points; tundo_probe measures character at fixed
// points. This harness checks *properties that must hold everywhere*, by
// sweeping and randomizing the whole control space and asserting invariants
// over it.
//
// The reason it exists: every defect found in tundo by ear so far was
// invisible to a single-point measurement, because each one was a *relation*
// between two settings rather than a bad number at one setting. Skin and
// Metal rendering the same signal, SPREAD appearing to retune, a slow attack
// cancelling a short decay: all of them need two renders compared against
// each other, which is what this file does.
//
// Failures print the offending patch to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// Safety, over random patches:
//   S1 finite      no NaN or Inf on either output, ever
//   S2 bounded     |audio| within the selected Vpp, env within 0..10 V
//   S3 silent      an untriggered voice is exactly silent
//   S4 no_dc       no DC offset parked on the output after the hit
//
// Level, over random patches:
//   A1 audible     every strike is audible, wherever the knobs are
//   A2 level_range the loudest and quietest patches sit within a bounded
//                  spread: HARM and FOLD change timbre, not level
//   A3 decays      the voice returns to silence in bounded time (not free-run)
//
// Pitch, over random patches:
//   P0 estimator   the pitch estimator itself is checked against synthetic
//                  tones detuned by known amounts, before anything trusts it
//   P1 pitch_fixed only PITCH and RANGE change the pitch: HARM, MORPH, FOLD,
//                  ATTACK and DECAY move neither f0 nor any partial ratio,
//                  and SPREAD moves the others but never the first
//   P2 pitch_track PITCH tracks 1 V/oct, measured on real audio
//   P3 ratio0      the first partial is exactly the fundamental under both
//                  spread laws, at every SPREAD setting
//   P4 ordered     a spread law never reorders the partials
//
// P1, P3 and P4 are equalities on the engine's own state rather than
// measurements of its output, and deliberately so. A partial's frequency is
// f0 * ratio[i], so those two quantities settle the question exactly, where
// measurement cannot: folding a spectrum whose partials sit a couple of
// percent off harmonic drops intermodulation right beside f0, and any
// phase-based estimate reads the resulting beat as a pitch error of tens of
// cents when nothing has moved. P2 keeps the end-to-end claim honest by
// measuring real audio, on a bare sine where the estimator is exact to about
// a cent (which P0 verifies).
//
// Distinctness, over random patches:
//   D1 modes       the three modes render measurably different signals from
//                  the same knobs
//   D2 knobs_live  every continuous knob measurably changes the output over
//                  its full travel: no dead zones
//
// Continuity, over random patches:
//   C1 continuous  a small knob move makes a small change to the sound: no
//                  snaps, steps or discontinuities anywhere in the space
//
// Robustness:
//   M1 mod_safe    every control swept at audio rate stays finite and bounded
//   M2 no_zipper   ... and produces no sample steps a sustained tone would not
//   M3 retrigger   every trigger sounds, at any rate, with consistent level

#include "smoke_harness.hpp"

#include <vector>
#include <algorithm>
#include <complex>
#include <cstdlib>

#include "../src/tundo.cpp"

typedef std::vector<float> Buf;

// ───────────────────────────────────────────────────────────── scale factors

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
    float pitch = 0.f, harm = 0.3f, spread = 0.f, morph = 0.f;
    float fold = 0.f, attack = 0.5f, decay = 0.35f;
    int mode = 0, range = 0, outputLevel = 1;
    bool cleanRate = false, quantize = true, extendedSpread = false;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    p.pitch = r.range(-1.f, 3.f);
    p.harm = r.uni();
    p.spread = r.uni();
    p.morph = r.uni();
    p.fold = r.uni();
    p.attack = r.uni();
    p.decay = r.range(0.f, 0.7f);       // capped: the render has to contain it
    p.mode = r.pick(3);
    p.range = r.pick(3);
    p.outputLevel = r.pick(3);
    p.cleanRate = r.pick(4) == 0;
    p.quantize = r.pick(2) == 0;
    p.extendedSpread = r.pick(2) == 0;
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "pitch %.3f harm %.3f spread %.3f morph %.3f fold %.3f "
             "attack %.3f decay %.3f mode %d range %d out %d clean %d quant %d ext %d",
             p.pitch, p.harm, p.spread, p.morph, p.fold, p.attack, p.decay,
             p.mode, p.range, p.outputLevel, (int)p.cleanRate, (int)p.quantize,
             (int)p.extendedSpread);
}

static void apply(Tundo& m, const Patch& p) {
    m.params[Tundo::PITCH_PARAM].setValue(p.pitch);
    m.params[Tundo::HARM_PARAM].setValue(p.harm);
    m.params[Tundo::SPREAD_PARAM].setValue(p.spread);
    m.params[Tundo::MORPH_PARAM].setValue(p.morph);
    m.params[Tundo::FOLD_PARAM].setValue(p.fold);
    m.params[Tundo::ATTACK_PARAM].setValue(p.attack);
    m.params[Tundo::DECAY_PARAM].setValue(p.decay);
    m.params[Tundo::MODE_PARAM].setValue((float)p.mode);
    m.params[Tundo::RANGE_PARAM].setValue((float)p.range);
    m.outputLevel = p.outputLevel;
    m.cleanRate = p.cleanRate;
    m.quantize = p.quantize;
    m.extendedSpread = p.extendedSpread;
}

// expected fundamental, before the engine's own clamp
static double expectedF0(const Patch& p) {
    return tundo_dsp::kBaseHz * std::exp2((double)p.pitch + 2.0 * p.range);
}

struct Take {
    Buf audio, env;
    double decayMs = 0.0;
    double halfVpp = 5.0;
};

// settle the smoothers, strike once, capture
static Take strikeOf(const Patch& p, double secs, double preRoll = 0.0) {
    Tundo m;
    apply(m, p);
    Take t;
    long frame = 0;
    // preRoll captures the untriggered state when asked for
    for (int i = 0; i < (int)(0.02 * SR); i++) m.process(makeArgs(frame++));
    if (preRoll > 0.0) {
        for (int i = 0; i < (int)(preRoll * SR); i++) {
            m.process(makeArgs(frame++));
            t.env.push_back(m.outputs[Tundo::AUDIO_OUTPUT].getVoltage());
        }
        t.audio = t.env;      // caller only wants the untriggered samples
        t.env.clear();
        return t;
    }
    size_t n = (size_t)(secs * SR);
    t.audio.resize(n);
    t.env.resize(n);
    for (size_t i = 0; i < n; i++) {
        m.inputs[Tundo::TRIG_INPUT].setVoltage(i < 50 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        t.audio[i] = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
        t.env[i] = m.outputs[Tundo::ENV_OUTPUT].getVoltage();
    }
    t.decayMs = m.p.decayMs;
    t.halfVpp = 0.5 * (p.outputLevel == 0 ? 5.0 : p.outputLevel == 1 ? 10.0 : 14.0);
    return t;
}

// ───────────────────────────────────────────────────────────────── measures

static double peakOf(const Buf& x, size_t from = 0, size_t to = (size_t)-1) {
    double v = 0.0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; i++)
        if (std::isfinite(x[i])) v = std::max(v, (double)std::fabs(x[i]));
    return v;
}

static double rmsOf(const Buf& x, size_t from = 0, size_t to = (size_t)-1) {
    double s = 0.0;
    size_t n = 0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; i++) {
        if (!std::isfinite(x[i])) continue;
        s += (double)x[i] * x[i];
        n++;
    }
    return n ? std::sqrt(s / n) : 0.0;
}

static long nansIn(const Buf& x) {
    long n = 0;
    for (float v : x) if (!std::isfinite(v)) n++;
    return n;
}

// energy in the sample difference over total energy: a brightness proxy
static double brightOf(const Buf& x, size_t to) {
    double hi = 0.0, all = 0.0;
    to = std::min(to, x.size());
    for (size_t i = 1; i < to; i++) {
        if (!std::isfinite(x[i]) || !std::isfinite(x[i - 1])) continue;
        double d = (double)x[i] - x[i - 1];
        hi += d * d;
        all += (double)x[i] * x[i];
    }
    return all > 0.0 ? hi / all : 0.0;
}

// Deviation of the actual fundamental from `f0`, in cents. Takes the complex
// amplitude at f0 over two adjacent windows: the phase advance between them
// is the frequency error. Returns 0 when there is no energy there to measure.
static double pitchCents(const Buf& x, double f0) {
    if (f0 <= 0.0) return 0.0;
    // Eight cycles, always. Too few and there is no phase to resolve (at
    // 16 Hz a fixed 50 ms window is under one cycle); too many and the phase
    // advance wraps, which reads a large detuning as a small one. Eight puts
    // the unambiguous range at +-104 cents, comfortably wider than anything
    // these invariants call a pass.
    size_t L = (size_t)std::max(64.0, 8.0 * SR / f0);
    size_t t0 = (size_t)(0.01 * SR);
    if (x.size() < t0 + 2 * L) return 0.0;
    // the exponent must use the *absolute* sample index: referencing each
    // window to its own start leaves the f0 term in the phase difference and
    // the measurement reads the whole carrier phase instead of the error
    auto amp = [&](size_t off) {
        std::complex<double> acc(0, 0);
        for (size_t i = 0; i < L; i++) {
            if (!std::isfinite(x[off + i])) continue;
            acc += (double)x[off + i]
                 * std::exp(std::complex<double>(
                       0, -2.0 * M_PI * f0 * (double)(off + i) / SR));
        }
        return acc / (double)L;
    };
    std::complex<double> a = amp(t0), b = amp(t0 + L);
    // Only measure when there is a fundamental worth measuring. Heavy folding
    // redistributes the energy upwards and can leave the component at f0 down
    // in the leakage from its neighbours, where the phase is noise and the
    // estimate is meaningless rather than wrong. An absolute floor does not
    // catch that; the level relative to the signal itself does.
    double ref = rmsOf(x, t0, t0 + 2 * L);
    double floor = std::max(2e-3, 0.15 * ref);
    if (std::abs(a) < floor || std::abs(b) < floor) return 0.0;
    double dphi = std::arg(b / a);
    double df = dphi / (2.0 * M_PI * (double)L / SR);
    return 1200.0 * std::log2((f0 + df) / f0);
}

// sample-wise difference of two takes, relative to the louder one
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

// ─────────────────────────────────────────────────────── failure bookkeeping

struct Inv {
    const char* name;
    long checked, failed;
    double worst;
    char worstPatch[512];

    // a member initializer would make this a non-aggregate under C++11, and
    // brace-init of an aggregate is how every one of these is declared
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
        report("tundo", name, failed ? worst : (double)checked, failed == 0);
        if (failed)
            fprintf(stderr, "  %s: %ld/%ld failed, worst %g at [%s]\n",
                    name, failed, checked, worst, worstPatch);
    }
};

// ──────────────────────────────────────────────────────────────────── S, A

static void testSafetyAndLevel() {
    Rng r(gSeed);
    Inv finite{"inv_S1_finite"}, bounded{"inv_S2_bounded"};
    Inv silent{"inv_S3_silent"}, nodc{"inv_S4_no_dc"};
    Inv audible{"inv_A1_audible"}, decays{"inv_A3_decays"};
    double loudest = 0.0, quietest = 1e9;
    Patch loudP, quietP;

    int n = 220 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Take t = strikeOf(p, 1.2);

        long bad = nansIn(t.audio) + nansIn(t.env);
        finite.hit(bad == 0, (double)bad, p);

        double pk = peakOf(t.audio);
        double envPk = peakOf(t.env);
        double cap = t.halfVpp * 1.02 + 0.05;
        bounded.hit(pk <= cap && envPk <= 10.05, std::max(pk - cap, envPk - 10.05), p);

        // untriggered: exactly silent
        Take q = strikeOf(p, 0.0, 0.15);
        double idle = peakOf(q.audio);
        silent.hit(idle < 1e-9, idle, p);

        // audible, and normalized against the patch's own output setting
        double rel = pk / t.halfVpp;
        audible.hit(rel > 0.25, rel, p, false);
        if (rel > loudest) { loudest = rel; loudP = p; }
        if (rel < quietest) { quietest = rel; quietP = p; }

        // gone by three decay times plus the attack it had to climb first
        double gone = 0.001 * t.decayMs * 3.0 + 0.001 * 2000.0 * p.attack + 0.05;
        if (gone < 1.0) {
            size_t from = (size_t)(gone * SR);
            double tail = peakOf(t.audio, from);
            decays.hit(tail < 0.02 * t.halfVpp, tail / t.halfVpp, p);

            // and no offset parked on the output behind it. Measured only
            // once the voice is actually over: during a slow swell the mean
            // is legitimately nonzero, and that is not a parked offset.
            double dc = 0.0;
            long cnt = 0;
            for (size_t i = from; i < t.audio.size(); i++)
                if (std::isfinite(t.audio[i])) { dc += t.audio[i]; cnt++; }
            dc = cnt ? std::fabs(dc / cnt) : 0.0;
            nodc.hit(dc < 0.002 * t.halfVpp, dc / t.halfVpp, p);
        }
    }

    finite.done();
    bounded.done();
    silent.done();
    audible.done();
    decays.done();
    nodc.done();

    double spreadDb = 20.0 * std::log10(loudest / std::max(quietest, 1e-9));
    report("tundo", "inv_A2_level_range_db", spreadDb, spreadDb < 18.0);
    if (spreadDb >= 18.0) {
        char a[512], b[512];
        describe(loudP, a, sizeof(a));
        describe(quietP, b, sizeof(b));
        fprintf(stderr, "  level range %.1f dB\n    loudest  [%s]\n    quietest [%s]\n",
                spreadDb, a, b);
    }
}

// ──────────────────────────────────────────────────────────────────── P

// A steady tone: free-run holds the envelopes open, so nothing is decaying
// while the pitch is measured. That matters, because the folder is a
// level-dependent nonlinearity: under a decaying envelope the fundamental's
// phase rotates as the fold depth unwinds, and a phase-based frequency
// estimate reads that rotation as a pitch error of a couple of hundred cents
// when the pitch has not moved at all.
static Buf steadyOf(const Patch& p, double secs) {
    Tundo m;
    apply(m, p);
    m.freeRun = true;
    m.params[Tundo::DECAY_PARAM].setValue(1.f);   // free-run arms at full CW
    long frame = 0;
    for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(frame++));
    Buf out((size_t)(secs * SR));
    for (size_t i = 0; i < out.size(); i++) {
        m.process(makeArgs(frame++));
        out[i] = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
    }
    return out;
}

// Before trusting anything the pitch estimator says, check the instrument:
// synthesise tones detuned by known amounts and confirm it reports them. A
// broken estimator looks exactly like a broken engine, and this one has
// already been wrong twice (an absolute rather than relative phase
// reference, and a window too short to hold a cycle at low pitch).
static void testPitchEstimator() {
    Inv est{"inv_P0_estimator"};
    const double freqs[4] = {60.0, 131.0, 440.0, 1500.0};
    const double cents[5] = {0.0, -37.0, 4.0, 61.0, -12.0};
    for (int f = 0; f < 4; f++) {
        for (int c = 0; c < 5; c++) {
            double f0 = freqs[f];
            double actual = f0 * std::exp2(cents[c] / 1200.0);
            Buf x((size_t)(0.5 * SR));
            for (size_t i = 0; i < x.size(); i++)
                x[i] = (float)(2.0 * std::sin(2.0 * M_PI * actual * i / SR + 0.7));
            double err = std::fabs(pitchCents(x, f0) - cents[c]);
            Patch dummy;
            est.hit(err < 2.0, err, dummy);   // the instrument is good to ~1 cent
        }
    }
    est.done();
}

// Structural invariants on the spread law itself. Exact, no rendering, and
// they resolve what the audio estimator cannot: a fundamental a few cents off
// is invisible under partial leakage but is a plain equality here.
static void testSpreadLaw() {
    Rng r(gSeed ^ 0x1b873593u);
    Inv ratio0{"inv_P3_ratio0"}, ordered{"inv_P4_ratios_ordered"};

    int n = 120 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Tundo m;
        apply(m, p);
        // enough blocks for the spread smoother to reach the knob
        for (int i = 0; i < 4000; i++) m.process(makeArgs(i));

        double d = std::fabs((double)m.engine.ratio[0] - 1.0);
        ratio0.hit(d < 1e-5, d, p);

        double worstGap = 1e9;
        for (int i = 1; i < tundo_dsp::kNumOsc; i++)
            worstGap = std::min(worstGap,
                                (double)m.engine.ratio[i] - m.engine.ratio[i - 1]);
        ordered.hit(worstGap > 0.0, worstGap, p, false);
    }
    ratio0.done();
    ordered.done();
}

static const char* kPitchNeutralName[6] = {"harm", "morph", "fold", "attack",
                                           "decay", "spread"};

// Every partial's frequency is f0 * ratio[i], so "does this knob change the
// pitch" has an exact answer that needs no audio at all: does it move f0, or
// does it move the ratios. That is worth settling structurally, because
// measurement cannot settle it. Folding a spectrum whose partials sit a
// couple of percent off harmonic drops intermodulation right beside f0, and
// a phase estimate reads the resulting beat as a pitch error of tens of
// cents when nothing has moved at all. inv_P2_pitch_track keeps the
// end-to-end claim honest by measuring real audio, on a bare sine where the
// estimator is exact.
static void testPitchStructural() {
    Rng r(gSeed ^ 0x3c6ef372u);
    Inv fixed{"inv_P1_pitch_fixed"};

    int n = 70 * gScale;
    for (int k = 0; k < n; k++) {
        Patch base = randomPatch(r);
        Tundo ref;
        apply(ref, base);
        for (int i = 0; i < 4000; i++) ref.process(makeArgs(i));
        double f0Ref = ref.p.f0;
        float ratioRef[tundo_dsp::kNumOsc];
        for (int i = 0; i < tundo_dsp::kNumOsc; i++)
            ratioRef[i] = ref.engine.ratio[i];

        for (int w = 0; w < 6; w++) {
            Patch p = base;
            float v = r.uni();
            switch (w) {
            case 0: p.harm = v; break;
            case 1: p.morph = v; break;
            case 2: p.fold = v; break;
            case 3: p.attack = v; break;
            case 4: p.decay = v; break;
            case 5: p.spread = v; break;   // moves the others, never the first
            }
            Tundo m;
            apply(m, p);
            for (int i = 0; i < 4000; i++) m.process(makeArgs(i));

            double worst = std::fabs(m.p.f0 - f0Ref) / std::max(f0Ref, 1e-9);
            int upTo = (w == 5) ? 1 : tundo_dsp::kNumOsc;
            for (int i = 0; i < upTo; i++)
                worst = std::max(worst,
                                 (double)std::fabs(m.engine.ratio[i] - ratioRef[i]));
            fixed.hit(worst < 1e-5, worst, p, true, kPitchNeutralName[w]);
        }
    }
    fixed.done();
}

// The end-to-end check: a bare sine, where the estimator is exact, has to
// come out at the frequency the knobs asked for, over four octaves and all
// three range settings.
static void testPitch() {
    Rng r(gSeed ^ 0x5bf03635u);
    Inv track{"inv_P2_pitch_track"};

    int n = 10 * gScale;
    for (int k = 0; k < n; k++) {
        Patch base;
        base.mode = 0;
        base.harm = 0.f;        // one partial
        base.spread = r.uni();  // which SPREAD cannot move
        base.morph = 0.f;       // a sine
        base.fold = 0.f;        // and no waveshaping
        base.outputLevel = r.pick(3);
        base.quantize = r.pick(2) == 0;
        for (int o = 0; o < 4; o++) {
            Patch p = base;
            p.pitch = (float)o;
            p.range = r.pick(3);
            double cents = std::fabs(pitchCents(steadyOf(p, 0.4), expectedF0(p)));
            track.hit(cents < 5.0, cents, p);
        }
    }
    track.done();
}

static void testDistinct() {
    Rng r(gSeed ^ 0x2545f491u);
    Inv modes{"inv_D1_modes"}, live{"inv_D2_knobs_live"};

    int n = 40 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.decay = r.range(0.3f, 0.7f);
        // HARM has to give the modes something to be different about. Below
        // about 0.1 there is one oscillator and nothing for Metal to modulate
        // or Liquid to sweep, and all three converge on the same decaying
        // tone within a few milliseconds: measured, they sit 9% apart at
        // HARM 0.02 against 90% at the 0.3 default. That convergence is the
        // knob working, not the modes failing.
        p.harm = r.range(0.1f, 1.f);
        Take a = strikeOf((p.mode = 0, p), 0.5);
        Take b = strikeOf((p.mode = 1, p), 0.5);
        Take c = strikeOf((p.mode = 2, p), 0.5);
        double ab = diffRatio(a.audio, b.audio);
        double ac = diffRatio(a.audio, c.audio);
        double bc = diffRatio(b.audio, c.audio);
        double worst = std::min(ab, std::min(ac, bc));
        modes.hit(worst > 0.1, worst, p, false);
    }

    // Every continuous knob, swept end to end, has to change the sound. The
    // preconditions are the documented ones: SPREAD needs a second partial to
    // move, and the pitch-neutral knobs need a voice that is actually ringing.
    static const char* knobName[7] = {"pitch", "harm", "spread", "morph",
                                      "fold", "attack", "decay"};
    int m = 26 * gScale;
    for (int k = 0; k < m; k++) {
        Patch base = randomPatch(r);
        base.harm = r.range(0.35f, 1.f);      // SPREAD needs partials to space
        base.decay = r.range(0.3f, 0.7f);
        // a slow attack pushes the whole hit past the end of the capture, so
        // hold it short except where ATTACK is the knob under test
        base.attack = r.range(0.f, 0.5f);
        for (int w = 0; w < 7; w++) {
            Patch lo = base, hi = base;
            switch (w) {
            case 0: lo.pitch = 0.f;  hi.pitch = 1.f;  break;
            case 1: lo.harm = 0.35f; hi.harm = 1.f;   break;
            case 2: lo.spread = 0.f; hi.spread = 1.f; break;
            case 3: lo.morph = 0.f;  hi.morph = 1.f;  break;
            case 4: lo.fold = 0.f;   hi.fold = 1.f;   break;
            case 5: lo.attack = 0.f; hi.attack = 0.8f; break;
            case 6: lo.decay = 0.1f; hi.decay = 0.7f; break;
            }
            double d = diffRatio(strikeOf(lo, 1.6).audio, strikeOf(hi, 1.6).audio);
            live.hit(d > 0.05, d, base, false, knobName[w]);
        }
    }
    modes.done();
    live.done();
}

// ──────────────────────────────────────────────────────────────────── C

static void testContinuity() {
    Rng r(gSeed ^ 0x846ca68bu);
    Inv cont{"inv_C1_continuous"};

    int n = 26 * gScale;
    for (int k = 0; k < n; k++) {
        Patch base = randomPatch(r);
        base.decay = r.range(0.25f, 0.7f);
        // The capture has to contain the hit it is comparing. ATTACK is
        // exponential, so a 1.5% knob move at the top of its travel is a 25%
        // move in attack time, and against a window shorter than the attack
        // that reads as a discontinuity in a sound that is perfectly smooth.
        base.attack = r.range(0.f, 0.7f);
        for (int w = 0; w < 6; w++) {
            float v = r.range(0.02f, 0.98f);
            const float eps = 0.015f;
            Patch a = base, b = base;
            switch (w) {
            case 0: a.harm = v;   b.harm = v + eps;   break;
            case 1: a.spread = v; b.spread = v + eps; break;
            case 2: a.morph = v;  b.morph = v + eps;  break;
            case 3: a.fold = v;   b.fold = v + eps;   break;
            case 4: a.attack = v * 0.7f; b.attack = v * 0.7f + eps; break;
            case 5: a.decay = v * 0.7f; b.decay = v * 0.7f + eps; break;
            }
            Take ta = strikeOf(a, 0.4), tb = strikeOf(b, 0.4);
            size_t win = (size_t)(0.2 * SR);
            double pa = peakOf(ta.audio), pb = peakOf(tb.audio);
            double ra = rmsOf(ta.audio, 0, win), rb = rmsOf(tb.audio, 0, win);
            double ba = brightOf(ta.audio, win), bb = brightOf(tb.audio, win);
            // features must move by less than a step a 1.5% knob move could
            // plausibly justify
            double dPeak = std::fabs(pa - pb) / std::max(ta.halfVpp, 1e-9);
            double dRms = std::fabs(ra - rb) / std::max(std::max(ra, rb), 1e-6);
            double dBright = std::fabs(ba - bb) / std::max(std::max(ba, bb), 1e-6);
            double worst = std::max(dPeak, std::max(dRms, dBright));
            cont.hit(worst < 0.5, worst, a);
        }
    }
    cont.done();
}

// ──────────────────────────────────────────────────────────────────── M

static void testModulation() {
    Rng r(gSeed ^ 0x7feb352du);
    Inv safe{"inv_M1_mod_safe"}, zip{"inv_M2_no_zipper"}, retrig{"inv_M3_retrigger"};

    // Every knob wiggled at audio rate, over a sustaining voice, so any
    // coefficient that jumps shows up as a step the tone itself would not make.
    int n = 6 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.decay = 1.f;
        p.attack = 0.2f;
        Tundo m;
        apply(m, p);
        m.freeRun = true;
        long frame = 0;
        for (int i = 0; i < (int)(0.2 * SR); i++) m.process(makeArgs(frame++));
        long bad = 0;
        double pk = 0.0, biggest = 0.0, prev = 0.0;
        int knob = r.pick(6);
        double fmod = r.range(50.0, 900.0);
        int steps = (int)(0.5 * SR);
        for (int i = 0; i < steps; i++) {
            float ph = (float)(0.5 + 0.5 * std::sin(2.0 * M_PI * fmod * i / SR));
            switch (knob) {
            case 0: m.params[Tundo::HARM_PARAM].setValue(ph); break;
            case 1: m.params[Tundo::SPREAD_PARAM].setValue(ph); break;
            case 2: m.params[Tundo::MORPH_PARAM].setValue(ph); break;
            case 3: m.params[Tundo::FOLD_PARAM].setValue(ph * 0.7f); break;
            case 4: m.params[Tundo::PITCH_PARAM].setValue(ph); break;
            case 5: m.params[Tundo::ATTACK_PARAM].setValue(ph); break;
            }
            m.process(makeArgs(frame++));
            float v = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
            if (!std::isfinite(v)) { bad++; v = 0.f; }
            pk = std::max(pk, (double)std::fabs(v));
            if (i > 0) biggest = std::max(biggest, std::fabs((double)v - prev));
            prev = v;
        }
        double cap = 0.5 * (p.outputLevel == 0 ? 5.0 : p.outputLevel == 1 ? 10.0 : 14.0);
        safe.hit(bad == 0 && pk <= cap * 1.02 + 0.05, (double)bad + pk, p);
        (void)biggest;
    }

    // Zipper is a *relational* property: a square wave steps a full scale in
    // one sample all by itself, so an absolute step bound measures the
    // waveform, not the control. Sweep a knob slowly, and require the biggest
    // step to stay within what the same patch already produces standing still
    // at points along that knob's travel.
    static const char* zipName[6] = {"harm", "spread", "morph", "fold",
                                     "pitch", "attack"};
    int z = 5 * gScale;
    for (int k = 0; k < z; k++) {
        Patch p = randomPatch(r);
        p.decay = 1.f;
        p.attack = 0.2f;
        int knob = r.pick(6);

        auto setKnob = [&](Tundo& m, float ph) {
            switch (knob) {
            case 0: m.params[Tundo::HARM_PARAM].setValue(ph); break;
            case 1: m.params[Tundo::SPREAD_PARAM].setValue(ph); break;
            case 2: m.params[Tundo::MORPH_PARAM].setValue(ph); break;
            case 3: m.params[Tundo::FOLD_PARAM].setValue(ph * 0.7f); break;
            case 4: m.params[Tundo::PITCH_PARAM].setValue(p.pitch + ph); break;
            case 5: m.params[Tundo::ATTACK_PARAM].setValue(ph); break;
            }
        };
        auto maxStep = [&](bool sweep, float fixedAt) {
            Tundo m;
            apply(m, p);
            m.freeRun = true;
            long frame = 0;
            setKnob(m, fixedAt);
            for (int i = 0; i < (int)(0.2 * SR); i++) m.process(makeArgs(frame++));
            double big = 0.0, prev = 0.0;
            int steps = (int)(0.35 * SR);
            for (int i = 0; i < steps; i++) {
                if (sweep)
                    setKnob(m, (float)(0.5 + 0.5 * std::sin(2.0 * M_PI * 3.0 * i / SR)));
                m.process(makeArgs(frame++));
                float v = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
                if (!std::isfinite(v)) v = 0.f;
                if (i > 0) big = std::max(big, std::fabs((double)v - prev));
                prev = v;
            }
            return big;
        };
        double statik = 0.0;
        for (int s = 0; s <= 8; s++)
            statik = std::max(statik, maxStep(false, s / 8.f));
        double swept = maxStep(true, 0.5f);
        double ratio = swept / std::max(statik, 1e-6);
        zip.hit(ratio < 1.4, ratio, p, true, zipName[knob]);
    }

    // Retriggering: every hit sounds, and at fixed settings they match.
    int q = 10 * gScale;
    for (int k = 0; k < q; k++) {
        Patch p = randomPatch(r);
        p.decay = r.range(0.f, 0.4f);
        p.attack = r.range(0.f, 0.6f);
        Tundo m;
        apply(m, p);
        long frame = 0;
        for (int i = 0; i < (int)(0.05 * SR); i++) m.process(makeArgs(frame++));
        int period = (int)(SR / r.range(4.f, 12.f));
        double lo = 1e9, hi = 0.0;
        for (int h = 0; h < 8; h++) {
            double pk = 0.0;
            for (int i = 0; i < period; i++) {
                m.inputs[Tundo::TRIG_INPUT].setVoltage(i < 50 ? 5.f : 0.f);
                m.process(makeArgs(frame++));
                float v = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
                if (std::isfinite(v)) pk = std::max(pk, (double)std::fabs(v));
            }
            if (h == 0) continue;             // the first rings up from silence
            lo = std::min(lo, pk);
            hi = std::max(hi, pk);
        }
        double cap = 0.5 * (p.outputLevel == 0 ? 5.0 : p.outputLevel == 1 ? 10.0 : 14.0);
        bool ok = lo > 0.15 * cap && hi / std::max(lo, 1e-9) < 1.6;
        retrig.hit(ok, lo / cap, p, false);
    }

    safe.done();
    zip.done();
    retrig.done();
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
    fprintf(stderr, "tundo_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testPitchEstimator();
    testSafetyAndLevel();
    testSpreadLaw();
    testPitchStructural();
    testPitch();
    testDistinct();
    testContinuity();
    testModulation();

    return failures ? 1 : 0;
}
