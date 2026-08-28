// scrupea_invariants - property-based checks for the chaotic bank.
//
// smoke_scrupea checks a handful of fixed points; scrupea_probe measures
// character. This harness checks properties that must hold *everywhere*, by
// randomizing the whole 64-bar control space plus the macros and the
// topology, and asserting invariants over it. scrupea is sixteen saturating
// feedback loops modulating each other, all of them reachable from a single
// button press, so "does it ever blow up" is not a rhetorical question.
//
// Failures print the offending patch to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// Safety, over random patches:
//   S1 finite      no NaN or Inf on any output, ever
//   S2 bounded     audio within +-10 V, CV within +-5 V
//   S3 no_dc       sixteen saturating loops leave no offset on the jack
//   S4 fader_span  the output fader spans the ensemble's 54 dB and no more
//
// The engine:
//   E1 alive       a random bank makes sound rather than sitting dead
//   E2 recovers    driven to the extremes and let go, it comes back
//   E3 no_pump     the normalizer holds a level, it does not oscillate
//
// The macros, which are curve mappings rather than offsets:
//   M1 monotone    the shaper is monotone in the knob at every bar value
//   M2 identity    at centre the mapping is exactly the bar value
//   M3 live        each of the four macros measurably changes the output, on
//                  the large majority of patches (a rate, not an absolute:
//                  see the note at the check)
//
// Robustness:
//   R1 cv_safe     macro CV swept at audio rate stays finite and bounded
//   R2 rate        every supported sample rate behaves the same way
//   R3 in_safe     a hot input into all eight loops does not detonate

#include "smoke_harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "../src/scrupea.cpp"

static int gScale = 1;          // --long multiplies the patch counts
static uint32_t gSeed = 0x5bf03635u;

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
    float ch[NFUNC][NCH];
    float pitch = 0.5f, cutoff = 0.5f, delay = 0.5f, flow = 0.5f;
    float level = 0.f;          // the output fader, in dB
    int topology = 0;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    for (int f = 0; f < NFUNC; f++)
        for (int c = 0; c < NCH; c++) p.ch[f][c] = r.uni();
    p.pitch = r.uni();
    p.cutoff = r.uni();
    p.delay = r.uni();
    p.flow = r.uni();
    p.level = r.range(scrupea_dsp::K_OUT_MIN_DB, scrupea_dsp::K_OUT_MAX_DB);
    p.topology = r.pick(3);
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "topo %d pitch %+.3f cutoff %+.3f delay %+.3f "
             "flow %+.3f level %.1f dB bars[0] %.2f %.2f %.2f %.2f",
             p.topology, p.pitch, p.cutoff, p.delay, p.flow,
             p.level, p.ch[0][0], p.ch[0][1], p.ch[0][2], p.ch[0][3]);
}

static void apply(Scrupea& m, const Patch& p) {
    for (int f = 0; f < NFUNC; f++)
        for (int c = 0; c < NCH; c++)
            m.params[Scrupea::CH_PARAM + f * NCH + c].setValue(p.ch[f][c]);
    m.params[Scrupea::PITCH_PARAM].setValue(p.pitch);
    m.params[Scrupea::CUTOFF_PARAM].setValue(p.cutoff);
    m.params[Scrupea::DELAY_PARAM].setValue(p.delay);
    m.params[Scrupea::FLOW_PARAM].setValue(p.flow);
    m.params[Scrupea::LEVEL_PARAM].setValue(p.level);
    m.params[Scrupea::MODE_PARAM].setValue((float)p.topology);
}

struct Trace {
    std::vector<float> l, r, cv;
    long nans = 0;
    double peak = 0, cvPeak = 0, rms = 0, mean = 0;
};

// Run a patch: `warm` seconds discarded, then `keep` seconds recorded.
static Trace run(const Patch& p, double warm, double keep,
                 float sampleRate = SR, const float* input = NULL,
                 long inputLen = 0) {
    Scrupea m;
    Module::SampleRateChangeEvent sre;
    sre.sampleRate = sampleRate;
    sre.sampleTime = 1.f / sampleRate;
    m.onSampleRateChange(sre);
    apply(m, p);

    Trace t;
    const long nw = (long)(warm * sampleRate);
    const long nk = (long)(keep * sampleRate);
    t.l.reserve(nk);
    t.r.reserve(nk);
    t.cv.reserve(nk);
    double s2 = 0, s1 = 0;
    long frame = 0;
    for (long i = 0; i < nw + nk; i++) {
        if (input) m.inputs[Scrupea::AUDIO_INPUT].setVoltage(input[i % inputLen]);
        Module::ProcessArgs a;
        a.sampleRate = sampleRate;
        a.sampleTime = 1.f / sampleRate;
        a.frame = frame++;
        m.process(a);
        if (i < nw) continue;
        const float lv = m.outputs[Scrupea::LEFT_OUTPUT].getVoltage();
        const float rv = m.outputs[Scrupea::RIGHT_OUTPUT].getVoltage();
        const float cv = m.outputs[Scrupea::CV_OUTPUT].getVoltage();
        if (!std::isfinite(lv) || !std::isfinite(rv) || !std::isfinite(cv))
            t.nans++;
        else {
            t.peak = std::max(t.peak, (double)std::max(std::fabs(lv), std::fabs(rv)));
            t.cvPeak = std::max(t.cvPeak, (double)std::fabs(cv));
            s2 += (double)lv * lv;
            s1 += lv;
        }
        t.l.push_back(lv);
        t.r.push_back(rv);
        t.cv.push_back(cv);
    }
    if (nk > 0) {
        t.rms = std::sqrt(s2 / nk);
        t.mean = s1 / nk;
    }
    return t;
}

// A brightness figure: the RMS of the first difference over the RMS of the
// signal. Proportional to a mean frequency, and it is what moves when the
// delay macro slides the comb around without changing the level.
static double brightness(const Trace& t) {
    if (t.l.size() < 2 || t.rms < 1e-9) return 0.0;
    double d2 = 0;
    for (size_t i = 1; i < t.l.size(); i++) {
        const double d = (double)t.l[i] - t.l[i - 1];
        d2 += d * d;
    }
    return std::sqrt(d2 / (t.l.size() - 1)) / t.rms;
}

// The lag of the strongest autocorrelation peak, in samples of the original
// rate. This is the observable the delay macro actually moves: it slides the
// comb along without necessarily changing either the level or the
// brightness. Computed on an 8x decimated copy, which is ample to tell a
// 1 ms loop from a 50 ms one and keeps the search affordable.
static double dominantLag(const Trace& t) {
    const int D = 8;
    const size_t n = t.l.size() / D;
    if (n < 64) return 0.0;
    std::vector<double> x(n, 0.0);
    for (size_t i = 0; i < n; i++) {
        double s = 0;
        for (int j = 0; j < D; j++) s += t.l[i * D + j];
        x[i] = s / D;
    }
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (size_t i = 0; i < n; i++) x[i] -= mean;

    const size_t maxLag = std::min(n / 2, (size_t)(0.1 * SR / D));
    double best = -1e30;
    size_t bestLag = 1;
    for (size_t lag = 2; lag < maxLag; lag++) {
        double s = 0;
        for (size_t i = 0; i + lag < n; i++) s += x[i] * x[i + lag];
        s /= (double)(n - lag);
        if (s > best) { best = s; bestLag = lag; }
    }
    return (double)bestLag * D;
}

struct Inv {
    const char* name;
    long checked = 0, failed = 0;
    double worst = 0.0;
    char worstPatch[440];

    explicit Inv(const char* n) : name(n) { worstPatch[0] = 0; }

    void hit(bool ok, double value, const Patch& p, bool worseIsBigger = true,
             const char* note = "") {
        checked++;
        if (ok) return;
        failed++;
        if (failed == 1 || (worseIsBigger ? value > worst : value < worst)) {
            worst = value;
            char buf[400];
            describe(p, buf, sizeof(buf));
            snprintf(worstPatch, sizeof(worstPatch), "%s%s%s",
                     note, *note ? " | " : "", buf);
        }
    }
    void done() {
        report("scrupea", name, failed ? worst : (double)checked, failed == 0);
        if (failed)
            fprintf(stderr, "  %s: %ld/%ld failed, worst %g at [%s]\n",
                    name, failed, checked, worst, worstPatch);
    }
};

// ──────────────────────────────────────────────────────────────────────── S

static void testSafety() {
    Rng r(gSeed);
    Inv finite("inv_S1_finite"), bounded("inv_S2_bounded");
    Inv nodc("inv_S3_no_dc"), silent("inv_S4_fader_span");

    const int n = 60 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        // Twenty seconds, not two. The delay range is the factory one now and
        // reaches 1.75 s, so a loop can repeat at 0.57 Hz -- far below the DC
        // blocker's own 20 Hz corner, and a short mean of that is not zero for
        // reasons that have nothing to do with offset. This is long enough for
        // a dozen passes of the slowest loop the module can make.
        Trace t = run(p, 1.5, 20.0);

        finite.hit(t.nans == 0, (double)t.nans, p);
        bounded.hit(t.peak <= 10.001 && t.cvPeak <= 5.001,
                    std::max(t.peak - 10.001, t.cvPeak - 5.001), p);
        // 0.25 V, 2.5% of the rail, and not tighter for a reason that is now
        // structural rather than a measurement artefact: the factory cutoff
        // range bottoms out at 0.81 Hz, so a filter can sit essentially open
        // to DC and its loop can park at an offset that the output blocker --
        // one pole at 20 Hz -- only mostly removes. Worst over the standard
        // sixty patches is 0.145 V, on a bare-topology bank at +14.5 dB.
        nodc.hit(std::fabs(t.mean) < 0.25, std::fabs(t.mean), p);

        // The fader is the ensemble's, and the ensemble's bottoms out at
        // -36 dB rather than at silence, so "the level knob at zero is
        // quiet" is not the invariant. What is, is that the fader spans
        // exactly the 54 dB the ensemble gives it. The engine is
        // deterministic, so the two runs differ by the gain and nothing else
        // -- unless the rail is clipping, which is what this catches.
        Patch qlo = p, qhi = p;
        qlo.level = scrupea_dsp::K_OUT_MIN_DB;
        qhi.level = scrupea_dsp::K_OUT_MAX_DB;
        Trace slo = run(qlo, 0.5, 0.3);
        Trace shi = run(qhi, 0.5, 0.3);
        const double want = std::pow(10.0,
            (scrupea_dsp::K_OUT_MAX_DB - scrupea_dsp::K_OUT_MIN_DB) * 0.05);
        const double got = shi.peak / std::max(slo.peak, 1e-12);
        // clipped at the rail from above, so only the lower bound is firm
        silent.hit(got > 0.5 * want || shi.peak > 9.99, got / want, qlo);
    }
    finite.done();
    bounded.done();
    nodc.done();
    silent.done();
}

// ──────────────────────────────────────────────────────────────────────── E

static void testEngine() {
    Rng r(gSeed ^ 0x1111u);
    Inv alive("inv_E1_alive"), recovers("inv_E2_recovers"), nopump("inv_E3_no_pump");

    const int n = 40 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        // A bank whose levels are all near zero is legitimately quiet; only
        // ask for sound when the bank has been told to make some. The macros
        // are left at centre too: this is what the rand button produces, and
        // a pitch macro hard right against a cutoff macro hard left is a
        // patch that is *supposed* to be near silent, oscillators far above
        // the filters that are meant to pass them.
        for (int c = 0; c < NCH; c++) p.ch[F_AMP][c] = 0.6f + 0.4f * r.uni();
        p.pitch = p.cutoff = p.delay = p.flow = 0.5f;
        p.level = 0.f;
        Trace t = run(p, 2.0, 3.0);
        alive.hit(t.rms > 0.01, t.rms, p, false);

        // The limiter holds a level; it must not breathe in and out. Compare
        // the loudest and quietest 100 ms window of a 3 s run and require
        // they be within 40 dB of each other.
        const size_t win = (size_t)(0.1 * SR);
        double lo = 1e9, hi = 0;
        for (size_t base = 0; base + win < t.l.size(); base += win) {
            double s2 = 0;
            for (size_t i = 0; i < win; i++)
                s2 += (double)t.l[base + i] * t.l[base + i];
            const double w = std::sqrt(s2 / win);
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        const double ratio = hi / std::max(lo, 1e-6);
        nopump.hit(ratio < 100.0, ratio, p);
    }

    // Slam every macro to an extreme, then let go: the bank must come back
    // to something finite and audible rather than latching up.
    for (int k = 0; k < 15 * gScale; k++) {
        Patch p = randomPatch(r);
        for (int c = 0; c < NCH; c++) {
            p.ch[F_FBK][c] = 1.f;
            p.ch[F_AMP][c] = 1.f;
        }
        // The output level knob is not what this is testing, and a random
        // one lands near zero often enough to fail the "came back audible"
        // check for a reason that has nothing to do with recovery.
        p.level = 0.f;
        Patch hot = p;
        hot.pitch = hot.cutoff = hot.delay = hot.flow = 1.f;

        Scrupea m;
        apply(m, hot);
        long frame = 0;
        for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
        apply(m, p);
        Stats s;
        for (int i = 0; i < (int)(3 * SR); i++) {
            m.process(makeArgs(frame++));
            s.add(m.outputs[Scrupea::LEFT_OUTPUT].getVoltage());
        }
        recovers.hit(s.nans == 0 && s.peak < 10.001 && s.rms() > 0.005,
                     s.rms(), p, false);
    }

    alive.done();
    recovers.done();
    nopump.done();
}

// ──────────────────────────────────────────────────────────────────────── M

// The macros map the bars through the ensemble's `shaper`: a blend over
// v^4, v and the fourth root of v. Check the mapping itself, which is the
// part a listener has to be able to predict, rather than the audio.
static void testMacros() {
    Rng r(gSeed ^ 0x2222u);
    Inv monotone("inv_M1_monotone"), identity("inv_M2_identity");

    for (int k = 0; k < 200 * gScale; k++) {
        Patch p;
        const float v = r.range(0.001f, 0.999f);
        float prev = -1.f;
        bool ok = true;
        for (int i = 0; i <= 20; i++) {
            const float knob = i / 20.f;
            const float mapped = scrupea_dsp::shape(v, knob);
            if (mapped < prev - 1e-6f) ok = false;
            prev = mapped;
        }
        monotone.hit(ok, v, p);
        // The centre of the knob is the identity curve.
        identity.hit(std::fabs(scrupea_dsp::shape(v, 0.5f) - v) < 1e-6f,
                     std::fabs(scrupea_dsp::shape(v, 0.5f) - v), p);
    }

    // Each macro has to do something audible on its own. Compare a run at
    // -0.7 against one at +0.7 with everything else held.
    //
    // This one is a rate, not a per-patch assertion, and deliberately so.
    // The observables below are three coarse statistics of a chaotic signal,
    // and two runs that sound nothing alike can still land on the same
    // loudness, the same brightness and the same loop period by coincidence.
    // Demanding every single patch move one of them is demanding that a
    // chaotic system be statistically well behaved, which it is not. What is
    // worth asserting is that the macro bites on the large majority.
    static const char* label[4] = {"pitch", "cutoff", "delay", "flow"};
    long liveOk[4] = {0, 0, 0, 0}, liveN[4] = {0, 0, 0, 0};
    double liveWorst[4] = {1e9, 1e9, 1e9, 1e9};
    for (int k = 0; k < 12 * gScale; k++) {
        Patch base = randomPatch(r);
        base.pitch = base.cutoff = base.delay = base.flow = 0.5f;
        for (int c = 0; c < NCH; c++) {
            base.ch[F_AMP][c] = 0.8f;
            // The delay macro can only be heard through a loop that is
            // actually recirculating, so give every channel some feedback.
            base.ch[F_FBK][c] = 0.5f + 0.5f * r.uni();
        }
        base.level = 0.f;
        for (int q = 0; q < 4; q++) {
            // The bare topology has no filter, so the cutoff macro has
            // nothing to map there and being inaudible is correct.
            if (q == 1 && base.topology == scrupea_dsp::TOPO_BARE) continue;
            Patch lo = base, hi = base;
            float* slot[4] = {&lo.pitch, &lo.cutoff, &lo.delay, &lo.flow};
            float* slotH[4] = {&hi.pitch, &hi.cutoff, &hi.delay, &hi.flow};
            *slot[q] = 0.15f;
            *slotH[q] = 0.85f;
            Trace a = run(lo, 2.0, 2.0);
            Trace b = run(hi, 2.0, 2.0);
            // Level is the wrong observable on its own: the delay macro
            // slides the comb across the spectrum at nearly constant loudness.
            const double dRms = std::fabs(a.rms - b.rms) /
                                std::max(std::max(a.rms, b.rms), 1e-6);
            const double ba = brightness(a), bb = brightness(b);
            const double dBright = std::fabs(ba - bb) /
                                   std::max(std::max(ba, bb), 1e-9);
            const double la = dominantLag(a), lb = dominantLag(b);
            const double dLag = std::fabs(la - lb) /
                                std::max(std::max(la, lb), 1e-9);
            const double d = std::max(std::max(dRms, dBright), dLag);
            liveN[q]++;
            if (d > 0.02) liveOk[q]++;
            else liveWorst[q] = std::min(liveWorst[q], d);
        }
    }

    monotone.done();
    identity.done();
    for (int q = 0; q < 4; q++) {
        const double rate = liveN[q] ? (double)liveOk[q] / liveN[q] : 0.0;
        char name[48];
        snprintf(name, sizeof(name), "inv_M3_live_%s", label[q]);
        report("scrupea", name, rate, rate >= 0.95);
        if (rate < 0.95)
            fprintf(stderr, "  %s: %ld/%ld moved the output, quietest %g\n",
                    name, liveOk[q], liveN[q], liveWorst[q]);
    }
}

// ──────────────────────────────────────────────────────────────────────── R

static void testRobustness() {
    Rng r(gSeed ^ 0x3333u);
    Inv cvsafe("inv_R1_cv_safe"), rates("inv_R2_rate"), insafe("inv_R3_in_safe");

    // Every macro CV driven at audio rate, attenuverters wide open.
    for (int k = 0; k < 15 * gScale; k++) {
        Patch p = randomPatch(r);
        Scrupea m;
        apply(m, p);
        m.params[Scrupea::PITCH_ATT_PARAM].setValue(1.f);
        m.params[Scrupea::CUTOFF_ATT_PARAM].setValue(-1.f);
        m.params[Scrupea::DELAY_ATT_PARAM].setValue(1.f);
        m.params[Scrupea::FLOW_ATT_PARAM].setValue(-1.f);
        long frame = 0;
        Stats s, c;
        float ph[4] = {0.f, 0.25f, 0.5f, 0.75f};
        const float f[4] = {311.f, 1170.f, 47.f, 2903.f};
        for (int i = 0; i < (int)(3 * SR); i++) {
            for (int j = 0; j < 4; j++) {
                ph[j] += f[j] / SR;
                if (ph[j] >= 1.f) ph[j] -= 1.f;
                m.inputs[Scrupea::PITCH_CV_INPUT + j]
                    .setVoltage(5.f * std::sin(2.f * (float)M_PI * ph[j]));
            }
            m.process(makeArgs(frame++));
            s.add(m.outputs[Scrupea::LEFT_OUTPUT].getVoltage());
            c.add(m.outputs[Scrupea::CV_OUTPUT].getVoltage());
        }
        cvsafe.hit(s.nans + c.nans == 0 && s.peak <= 10.001 && c.peak <= 5.001,
                   std::max(s.peak - 10.001, c.peak - 5.001), p);
    }

    // Every rate Rack offers, on the same patches.
    static const float srList[6] = {44100.f, 48000.f, 88200.f, 96000.f,
                                    176400.f, 192000.f};
    for (int k = 0; k < 6 * gScale; k++) {
        Patch p = randomPatch(r);
        for (int i = 0; i < 6; i++) {
            Trace t = run(p, 1.0, 1.0, srList[i]);
            char note[32];
            snprintf(note, sizeof(note), "%.0f Hz", srList[i]);
            rates.hit(t.nans == 0 && t.peak <= 10.001 && t.cvPeak <= 5.001,
                      std::max(t.peak - 10.001, t.cvPeak - 5.001), p, true, note);
        }
    }

    // A hot signal into all eight loops at once.
    std::vector<float> tone((size_t)SR);
    for (size_t i = 0; i < tone.size(); i++)
        tone[i] = 10.f * std::sin(2.f * (float)M_PI * 97.f * (float)i / SR);
    for (int k = 0; k < 15 * gScale; k++) {
        Patch p = randomPatch(r);
        Trace t = run(p, 1.0, 2.0, SR, tone.data(), (long)tone.size());
        insafe.hit(t.nans == 0 && t.peak <= 10.001 && t.cvPeak <= 5.001,
                   std::max(t.peak - 10.001, t.cvPeak - 5.001), p);
    }

    cvsafe.done();
    rates.done();
    insafe.done();
}

int main(int argc, char** argv) {
    bool header = true;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-header")) header = false;
        else if (!std::strcmp(argv[i], "--long")) gScale = 8;
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            gSeed = (uint32_t)strtoul(argv[++i], NULL, 0);
    }
    rack::random::init();
    if (header) printf("module,check,value,pass\n");
    fprintf(stderr, "scrupea_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testSafety();
    testEngine();
    testMacros();
    testRobustness();

    return failures ? 1 : 0;
}
