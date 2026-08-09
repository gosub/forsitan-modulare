// turba_invariants — property-based checks for the chaotic bank.
//
// smoke_turba checks a handful of fixed points; turba_probe measures
// character. This harness checks properties that must hold *everywhere*, by
// randomizing the whole 64-bar control space plus the macros and the
// topology, and asserting invariants over it. turba is eight saturating
// feedback loops wired into a ring, all of them reachable from a single
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
//   S3 no_dc       eight saturating loops leave no offset parked on the jack
//   S4 silent      output level at zero is exactly silence
//
// The engine:
//   E1 alive       a random bank makes sound rather than sitting dead
//   E2 recovers    driven to the extremes and let go, it comes back
//   E3 no_pump     the loop limiter holds a level, it does not oscillate
//
// The macros, which are power mappings rather than offsets:
//   M1 monotone    the pitch macro raises every channel's frequency, and the
//                  cutoff macro every channel's cutoff, monotonically
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

#include "../src/turba.cpp"

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
    float pitch = 0.f, cutoff = 0.f, delay = 0.f, flow = 0.f, level = 0.5f;
    int topology = 0;
    bool ring = true;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    for (int f = 0; f < NFUNC; f++)
        for (int c = 0; c < NCH; c++) p.ch[f][c] = r.uni();
    p.pitch = r.range(-1.f, 1.f);
    p.cutoff = r.range(-1.f, 1.f);
    p.delay = r.range(-1.f, 1.f);
    p.flow = r.range(-1.f, 1.f);
    p.level = r.range(0.f, 1.f);
    p.topology = r.pick(3);
    p.ring = r.pick(8) != 0;
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "topo %d ring %d pitch %+.3f cutoff %+.3f delay %+.3f "
             "flow %+.3f level %.3f bars[0] %.2f %.2f %.2f %.2f",
             p.topology, (int)p.ring, p.pitch, p.cutoff, p.delay, p.flow,
             p.level, p.ch[0][0], p.ch[0][1], p.ch[0][2], p.ch[0][3]);
}

static void apply(Turba& m, const Patch& p) {
    for (int f = 0; f < NFUNC; f++)
        for (int c = 0; c < NCH; c++)
            m.params[Turba::CH_PARAM + f * NCH + c].setValue(p.ch[f][c]);
    m.params[Turba::PITCH_PARAM].setValue(p.pitch);
    m.params[Turba::CUTOFF_PARAM].setValue(p.cutoff);
    m.params[Turba::DELAY_PARAM].setValue(p.delay);
    m.params[Turba::FLOW_PARAM].setValue(p.flow);
    m.params[Turba::LEVEL_PARAM].setValue(p.level);
    m.params[Turba::MODE_PARAM].setValue((float)p.topology);
    m.ringCoupling = p.ring;
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
    Turba m;
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
        if (input) m.inputs[Turba::AUDIO_INPUT].setVoltage(input[i % inputLen]);
        Module::ProcessArgs a;
        a.sampleRate = sampleRate;
        a.sampleTime = 1.f / sampleRate;
        a.frame = frame++;
        m.process(a);
        if (i < nw) continue;
        const float lv = m.outputs[Turba::LEFT_OUTPUT].getVoltage();
        const float rv = m.outputs[Turba::RIGHT_OUTPUT].getVoltage();
        const float cv = m.outputs[Turba::CV_OUTPUT].getVoltage();
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
        report("turba", name, failed ? worst : (double)checked, failed == 0);
        if (failed)
            fprintf(stderr, "  %s: %ld/%ld failed, worst %g at [%s]\n",
                    name, failed, checked, worst, worstPatch);
    }
};

// ──────────────────────────────────────────────────────────────────────── S

static void testSafety() {
    Rng r(gSeed);
    Inv finite("inv_S1_finite"), bounded("inv_S2_bounded");
    Inv nodc("inv_S3_no_dc"), silent("inv_S4_silent");

    const int n = 60 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Trace t = run(p, 1.5, 2.0);

        finite.hit(t.nans == 0, (double)t.nans, p);
        bounded.hit(t.peak <= 10.001 && t.cvPeak <= 5.001,
                    std::max(t.peak - 10.001, t.cvPeak - 5.001), p);
        // Two seconds of a chaotic signal averages to zero if the DC blocker
        // is doing its job; anything left is an offset, not slow content.
        nodc.hit(std::fabs(t.mean) < 0.05, std::fabs(t.mean), p);

        Patch q = p;
        q.level = 0.f;
        Trace s = run(q, 0.5, 0.3);
        double loud = 0;
        for (size_t i = 0; i < s.l.size(); i++)
            loud = std::max(loud, (double)std::max(std::fabs(s.l[i]),
                                                   std::fabs(s.r[i])));
        silent.hit(loud == 0.0, loud, q);
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
        for (int c = 0; c < NCH; c++) p.ch[F_LEVEL][c] = 0.6f + 0.4f * r.uni();
        p.pitch = p.cutoff = p.delay = p.flow = 0.f;
        p.level = 0.5f;
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
            p.ch[F_LEVEL][c] = 1.f;
        }
        Patch hot = p;
        hot.pitch = hot.cutoff = hot.delay = hot.flow = 1.f;

        Turba m;
        apply(m, hot);
        long frame = 0;
        for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
        apply(m, p);
        Stats s;
        for (int i = 0; i < (int)(3 * SR); i++) {
            m.process(makeArgs(frame++));
            s.add(m.outputs[Turba::LEFT_OUTPUT].getVoltage());
        }
        recovers.hit(s.nans == 0 && s.peak < 10.001 && s.rms() > 0.005,
                     s.rms(), p, false);
    }

    alive.done();
    recovers.done();
    nopump.done();
}

// ──────────────────────────────────────────────────────────────────────── M

// The macros map the bars through v^gamma. Check the mapping itself, which
// is the part a listener has to be able to predict, rather than the audio.
static void testMacros() {
    Rng r(gSeed ^ 0x2222u);
    Inv monotone("inv_M1_monotone"), identity("inv_M2_identity");

    for (int k = 0; k < 200 * gScale; k++) {
        Patch p;
        const float v = r.range(0.001f, 0.999f);
        float prev = -1.f;
        bool ok = true;
        for (int i = -10; i <= 10; i++) {
            const float knob = i / 10.f;
            const float g = std::pow(5.f, -knob);
            const float mapped = Turba::mapValue(v, g);
            if (mapped < prev - 1e-6f) ok = false;
            prev = mapped;
        }
        monotone.hit(ok, v, p);
        identity.hit(std::fabs(Turba::mapValue(v, 1.f) - v) < 1e-6f,
                     std::fabs(Turba::mapValue(v, 1.f) - v), p);
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
        base.pitch = base.cutoff = base.delay = base.flow = 0.f;
        for (int c = 0; c < NCH; c++) {
            base.ch[F_LEVEL][c] = 0.8f;
            // The delay macro can only be heard through a loop that is
            // actually recirculating, so give every channel some feedback.
            base.ch[F_FBK][c] = 0.5f + 0.5f * r.uni();
        }
        base.level = 0.5f;
        for (int q = 0; q < 4; q++) {
            // The bare topology has no filter, so the cutoff macro has
            // nothing to map there and being inaudible is correct.
            if (q == 1 && base.topology == turba_dsp::TOPO_BARE) continue;
            Patch lo = base, hi = base;
            float* slot[4] = {&lo.pitch, &lo.cutoff, &lo.delay, &lo.flow};
            float* slotH[4] = {&hi.pitch, &hi.cutoff, &hi.delay, &hi.flow};
            *slot[q] = -0.7f;
            *slotH[q] = 0.7f;
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
        report("turba", name, rate, rate >= 0.95);
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
        Turba m;
        apply(m, p);
        m.params[Turba::PITCH_ATT_PARAM].setValue(1.f);
        m.params[Turba::CUTOFF_ATT_PARAM].setValue(-1.f);
        m.params[Turba::DELAY_ATT_PARAM].setValue(1.f);
        m.params[Turba::FLOW_ATT_PARAM].setValue(-1.f);
        long frame = 0;
        Stats s, c;
        float ph[4] = {0.f, 0.25f, 0.5f, 0.75f};
        const float f[4] = {311.f, 1170.f, 47.f, 2903.f};
        for (int i = 0; i < (int)(3 * SR); i++) {
            for (int j = 0; j < 4; j++) {
                ph[j] += f[j] / SR;
                if (ph[j] >= 1.f) ph[j] -= 1.f;
                m.inputs[Turba::PITCH_CV_INPUT + j]
                    .setVoltage(5.f * std::sin(2.f * (float)M_PI * ph[j]));
            }
            m.process(makeArgs(frame++));
            s.add(m.outputs[Turba::LEFT_OUTPUT].getVoltage());
            c.add(m.outputs[Turba::CV_OUTPUT].getVoltage());
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
    fprintf(stderr, "turba_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testSafety();
    testEngine();
    testMacros();
    testRobustness();

    return failures ? 1 : 0;
}
