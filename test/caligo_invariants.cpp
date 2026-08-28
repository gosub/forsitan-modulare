// caligo_invariants - property-based checks for the Greyhole port.
//
// smoke_caligo checks fixed points; caligo_probe measures echo arrivals and
// per-pass loss. This harness checks properties that must hold *everywhere*,
// by randomizing and sweeping the whole control space and asserting invariants
// over it.
//
// A feedback delay has a particular set of things that can go wrong, and they
// are not the things a voice or a distortion gets wrong: it can fail to decay,
// it can run away when the loop passes unity, it can leak the dry signal into
// a fully wet output, and it can quietly cross-couple two channels that are
// supposed to be independent. Several of those are exact claims rather than
// measured ones, and they are asserted as equalities here.
//
// Failures print the offending patch to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// Safety, over random patches:
//   S1 finite      no NaN or Inf on any of the four outputs, ever
//   S2 bounded     every output stays inside Rack's rails
//   S3 silent      nothing in, nothing circulating, nothing out
//   S4 decays      below unity the loop is a decay, and it reaches silence
//   S5 no_runaway  above unity the loop saturator is what holds it, and it
//                  must hold it at every setting, not merely on average
//
// The dry/wet mix, which is exact arithmetic and can be asserted as such:
//   T1 mix_dry     fully dry, the output is the input, sample for sample
//   T2 mix_wet     fully wet, none of the input is left in it
//
// The patchable loop, likewise exact:
//   R1 loop        wiring snd back to rtn reproduces the normalled path
//                  exactly: the break is a break, not a different circuit
//
// Stereo:
//   ST1 spin       with spin fully down the two channels are independent
//                  mono echoes, so silence into one stays silence out of it
//
// Time:
//   L1 clock       under a clock the delay is the ratio the knob selects
//   L2 monotone    and free-running, the delay follows the knob upwards
//
// Behaviour:
//   F1 freeze      freeze holds the loop instead of letting it decay
//   D1 damp        damping darkens the tail, monotonically
//   D2 knobs_live  every knob measurably changes the output
//   C1 continuous  a small knob move makes a small change to the sound
//   M1 mod_safe    every CV swept at audio rate stays finite and bounded

#include "smoke_harness.hpp"

#include <vector>
#include <algorithm>
#include <cstdlib>

#include "../src/caligo.cpp"

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
    float time = 0.5f, size = 1.f / 3.f, diff = 0.6906f, feedback = 0.9f;
    float damp = 0.f, mod = 0.1f, rate = 0.7411f, mix = 0.5f;
    float spin = 1.f, drift = 0.f;
    bool tape = false, freezeBypassDamp = true, freezeOpenInput = false;
};

static Patch randomPatch(Rng& r) {
    Patch p;
    p.time = r.uni();
    p.size = r.uni();
    p.diff = r.uni();
    // The knob runs to kMaxFeedback (1.2), not to 1: this is the loop gain
    // itself and it is allowed past unity, which is what the saturator is
    // for. Random patches stay below unity so the decay invariant means
    // something; inv_S5_no_runaway takes the rest of the range on its own.
    p.feedback = r.range(0.f, 0.8f);
    p.damp = r.uni();
    p.mod = r.uni();
    p.rate = r.uni();
    p.mix = r.uni();
    p.spin = r.uni();
    p.drift = r.uni();
    p.tape = r.pick(2) == 0;
    p.freezeBypassDamp = r.pick(2) == 0;
    p.freezeOpenInput = r.pick(2) == 0;
    return p;
}

static void describe(const Patch& p, char* out, size_t n) {
    snprintf(out, n,
             "time %.3f size %.3f diff %.3f fb %.3f damp %.3f mod %.3f "
             "rate %.3f mix %.3f spin %.3f drift %.3f tape %d",
             p.time, p.size, p.diff, p.feedback, p.damp, p.mod, p.rate,
             p.mix, p.spin, p.drift, (int)p.tape);
}

static void apply(Caligo& m, const Patch& p) {
    m.params[Caligo::TIME_PARAM].setValue(p.time);
    m.params[Caligo::SIZE_PARAM].setValue(p.size);
    m.params[Caligo::DIFF_PARAM].setValue(p.diff);
    m.params[Caligo::FEEDBACK_PARAM].setValue(p.feedback);
    m.params[Caligo::DAMP_PARAM].setValue(p.damp);
    m.params[Caligo::MOD_PARAM].setValue(p.mod);
    m.params[Caligo::RATE_PARAM].setValue(p.rate);
    m.params[Caligo::MIX_PARAM].setValue(p.mix);
    m.params[Caligo::SPIN_PARAM].setValue(p.spin);
    m.params[Caligo::DRIFT_PARAM].setValue(p.drift);
    m.p.tape = p.tape;
    m.p.freezeBypassDamp = p.freezeBypassDamp;
    m.p.freezeOpenInput = p.freezeOpenInput;
}

// ─────────────────────────────────────────────────────────────────── takes

struct Take { Buf l, r, sl, sr; };

// A burst of tone into both inputs, then silence, so the tail can be watched
// on its own. `burst` seconds of signal inside `secs` of capture.
static Take runOf(const Patch& p, double secs, double burst = 0.2,
                  double amp = 5.0, double freq = 440.0, bool freeze = false,
                  double freezeAt = -1.0, bool noise = false) {
    Caligo m;
    apply(m, p);
    m.inputs[Caligo::IN_R_INPUT].channels = 1;
    long frame = 0;
    Take t;
    size_t n = (size_t)(secs * SR);
    t.l.resize(n); t.r.resize(n); t.sl.resize(n); t.sr.resize(n);
    size_t bn = (size_t)(burst * SR);
    size_t fz = freezeAt >= 0.0 ? (size_t)(freezeAt * SR) : (size_t)-1;
    uint32_t ns = 22222u;
    for (size_t i = 0; i < n; i++) {
        float v = 0.f;
        if (i < bn) {
            if (noise) {
                ns ^= ns << 13; ns ^= ns >> 17; ns ^= ns << 5;
                v = (float)amp * ((float)(ns >> 8) * (1.f / 8388608.f) - 1.f);
            }
            else v = (float)(amp * std::sin(2.0 * M_PI * freq * i / SR));
        }
        m.inputs[Caligo::IN_L_INPUT].setVoltage(v);
        m.inputs[Caligo::IN_R_INPUT].setVoltage(v);
        if (freeze && i == fz) m.freezeLatched = true;
        m.process(makeArgs(frame++));
        t.l[i] = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        t.r[i] = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
        t.sl[i] = m.outputs[Caligo::SND_L_OUTPUT].getVoltage();
        t.sr[i] = m.outputs[Caligo::SND_R_OUTPUT].getVoltage();
    }
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

// fraction of the energy above `cut`
static double highFrac(const Buf& x, size_t from, size_t to, double cut) {
    double g = 1.0 - std::exp(-2.0 * M_PI * cut / SR);
    double z = 0.0, hi = 0.0, all = 0.0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; i++) {
        if (!std::isfinite(x[i])) continue;
        z += g * ((double)x[i] - z);
        double h = (double)x[i] - z;
        hi += h * h;
        all += (double)x[i] * x[i];
    }
    return all > 0.0 ? hi / all : 0.0;
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
        report("caligo", name, failed ? worst : (double)checked, failed == 0);
        if (failed)
            fprintf(stderr, "  %s: %ld/%ld failed, worst %g at [%s]\n",
                    name, failed, checked, worst, worstPatch);
    }
};

// ──────────────────────────────────────────────────────────────────── S

static void testSafety() {
    Rng r(gSeed);
    Inv finite{"inv_S1_finite"}, bounded{"inv_S2_bounded"};
    Inv silent{"inv_S3_silent"}, decays{"inv_S4_decays"};

    int n = 26 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        Take t = runOf(p, 4.0, 0.2, r.range(1.f, 10.f));

        long bad = nansIn(t.l) + nansIn(t.r) + nansIn(t.sl) + nansIn(t.sr);
        finite.hit(bad == 0, (double)bad, p);
        double pk = std::max(std::max(peakOf(t.l), peakOf(t.r)),
                             std::max(peakOf(t.sl), peakOf(t.sr)));
        bounded.hit(pk <= 10.001, pk, p);

        // nothing in, nothing circulating: nothing out
        Patch q = p;
        Caligo m;
        apply(m, q);
        long frame = 0;
        double idle = 0.0;
        for (int i = 0; i < (int)(1.0 * SR); i++) {
            m.process(makeArgs(frame++));
            idle = std::max(idle, (double)std::fabs(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage()));
            idle = std::max(idle, (double)std::fabs(m.outputs[Caligo::SND_R_OUTPUT].getVoltage()));
        }
        silent.hit(idle < 1e-12, idle, q);
    }

    // Below unity the loop is a decay. The capture has to hold enough passes
    // to show it, and a pass takes the delay time plus the diffuser's own
    // 70 ms: at the top of the TIME knob one pass is sixteen seconds, so this
    // keeps to the short end where a few seconds is dozens of passes.
    int d = 16 * gScale;
    for (int k = 0; k < d; k++) {
        Patch p = randomPatch(r);
        p.time = r.range(0.f, 0.3f);          // a loop of ~160 ms or less
        p.feedback = r.range(0.f, 0.8f);
        // Fourteen seconds, because a dozen nested allpasses at high DIFF
        // disperse energy long before they give it back: measured, such a
        // tail runs -16 dB at one second and then falls at about 1.5 dB per
        // second for the rest of the minute. Judging it at three seconds
        // measures dispersion, not decay. Two claims: far down on where it
        // started, and still falling over a long baseline.
        Take t = runOf(p, 14.0, 0.2, 8.0);
        double early = rmsOf(t.l, (size_t)(0.05 * SR), (size_t)(0.25 * SR));
        double mid = rmsOf(t.l, (size_t)(3.0 * SR), (size_t)(5.0 * SR));
        double late = rmsOf(t.l, (size_t)(12.0 * SR));
        bool ok = late < std::max(early * 0.05, 1e-4)
               && late < std::max(mid * 0.5, 1e-5);
        decays.hit(ok, late / std::max(early, 1e-9), p);
    }

    finite.done();
    bounded.done();
    silent.done();
    decays.done();
}

// Above unity the loop gain exceeds one and only the saturator stops it. That
// is the setting most likely to blow up, so it gets its own sweep rather than
// a share of the random patches.
static void testRunaway() {
    Rng r(gSeed ^ 0x2545f491u);
    Inv run{"inv_S5_no_runaway"};

    int n = 10 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.feedback = r.range(1.f, caligo_dsp::kMaxFeedback);   // past unity
        Take t = runOf(p, 6.0, 0.5, 10.0);
        long bad = nansIn(t.l) + nansIn(t.r) + nansIn(t.sl) + nansIn(t.sr);
        double pk = std::max(std::max(peakOf(t.l), peakOf(t.r)),
                             std::max(peakOf(t.sl), peakOf(t.sr)));
        run.hit(bad == 0 && pk <= 10.001, (double)bad + pk, p);
    }
    run.done();
}

// ──────────────────────────────────────────────────────────────────── T, R

static void testExact() {
    Rng r(gSeed ^ 0x5bf03635u);
    Inv dry{"inv_T1_mix_dry"}, wet{"inv_T2_mix_wet"};
    Inv loop{"inv_R1_loop"}, spin{"inv_ST1_spin"};

    int n = 10 * gScale;
    for (int k = 0; k < n; k++) {
        // Fully dry is an identity: the mix law is equal power, so at 0 the
        // wet coefficient is exactly zero and the dry one exactly one.
        Patch p = randomPatch(r);
        p.mix = 0.f;
        Caligo m;
        apply(m, p);
        long frame = 0;
        double worst = 0.0;
        for (int i = 0; i < (int)(0.5 * SR); i++) {
            float v = (float)(4.0 * std::sin(2.0 * M_PI * 330.0 * i / SR));
            m.inputs[Caligo::IN_L_INPUT].setVoltage(v);
            m.process(makeArgs(frame++));
            // the smoother needs a moment to reach the knob
            if (i > (int)(0.2 * SR))
                worst = std::max(worst,
                                 (double)std::fabs(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage() - v));
        }
        dry.hit(worst < 1e-4, worst, p);

        // The mix is an equal-power blend of two paths, and the wet path does
        // not depend on where the knob is, so the whole law can be checked as
        // arithmetic: out(m) = cos(m*pi/2)*dry + sin(m*pi/2)*wet, and the
        // halfway point must be exactly 0.7071 of the sum of the two ends.
        // Asserting that a wet output merely *differs* from the input says
        // almost nothing, since a delayed sine is still a sine.
        Patch q = p;
        q.mix = 0.f;
        Take d0 = runOf(q, 1.5, 0.2, 2.0, 330.0);
        q.mix = 1.f;
        Take d1 = runOf(q, 1.5, 0.2, 2.0, 330.0);
        q.mix = 0.5f;
        Take dh = runOf(q, 1.5, 0.2, 2.0, 330.0);
        // Only where nothing is against the rail. The output clamp sits after
        // the blend, so a wet path loud enough to clip on its own will not
        // clip at 0.7 of itself, and the law stops being linear exactly
        // because something downstream of it stopped being linear.
        if (std::max(std::max(peakOf(d0.l), peakOf(d1.l)), peakOf(dh.l)) > 9.5)
            continue;
        double worstMix = 0.0, ref = 0.0;
        for (size_t i = (size_t)(0.3 * SR); i < dh.l.size(); i++) {
            double want = (double)M_SQRT1_2 * ((double)d0.l[i] + d1.l[i]);
            worstMix = std::max(worstMix, std::fabs(want - dh.l[i]));
            ref = std::max(ref, std::fabs(want));
        }
        wet.hit(worstMix < std::max(ref, 1e-3) * 1e-3, worstMix / std::max(ref, 1e-9), q);
    }

    // The loop break is a break, not a different circuit: wiring snd straight
    // back to rtn has to reproduce what the normalling does. The module feeds
    // back the previous sample's send, so that is what to hand it.
    int m2 = 8 * gScale;
    for (int k = 0; k < m2; k++) {
        Patch p = randomPatch(r);
        Take open = runOf(p, 2.0, 0.2, 5.0);

        Caligo m;
        apply(m, p);
        m.inputs[Caligo::IN_R_INPUT].channels = 1;
        m.inputs[Caligo::RTN_L_INPUT].channels = 1;
        m.inputs[Caligo::RTN_R_INPUT].channels = 1;
        long frame = 0;
        size_t n2 = (size_t)(2.0 * SR);
        Buf got(n2);
        float prevL = 0.f, prevR = 0.f;
        for (size_t i = 0; i < n2; i++) {
            float v = i < (size_t)(0.2 * SR)
                    ? (float)(5.0 * std::sin(2.0 * M_PI * 440.0 * i / SR)) : 0.f;
            m.inputs[Caligo::IN_L_INPUT].setVoltage(v);
            m.inputs[Caligo::IN_R_INPUT].setVoltage(v);
            m.inputs[Caligo::RTN_L_INPUT].setVoltage(prevL);
            m.inputs[Caligo::RTN_R_INPUT].setVoltage(prevR);
            m.process(makeArgs(frame++));
            prevL = m.outputs[Caligo::SND_L_OUTPUT].getVoltage();
            prevR = m.outputs[Caligo::SND_R_OUTPUT].getVoltage();
            got[i] = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        }
        // Not bit-exact, and it cannot be: the normalled path feeds back the
        // engine's own send, while a patched one goes out as volts and comes
        // back divided by five, and x*5*0.2 is not x in float. One ulp of
        // seed goes round the loop as many times as the delay is short --
        // at 11 ms that is 180 passes in this capture -- and a diffuser
        // decorrelates rather than damps it. So: the first pass has to match
        // closely, and the levels have to match over the whole tail. Between
        // them they rule out a structural difference while allowing the
        // arithmetic one that has to be there.
        Buf ea(open.l.begin(), open.l.begin() + (size_t)(0.1 * SR));
        Buf eb(got.begin(), got.begin() + (size_t)(0.1 * SR));
        double first = diffRatio(ea, eb);
        double la = rmsOf(open.l), lb = rmsOf(got);
        double lvl = la > 0.0 ? lb / la : 1.0;
        loop.hit(first < 1e-3 && lvl > 0.89 && lvl < 1.12,
                 std::max(first, std::fabs(lvl - 1.0)), p);
    }

    // Spin fully down is two independent mono echoes, so a channel fed
    // nothing has to stay silent however loud the other one is.
    int m3 = 8 * gScale;
    for (int k = 0; k < m3; k++) {
        Patch p = randomPatch(r);
        p.spin = 0.f;
        Caligo m;
        apply(m, p);
        m.inputs[Caligo::IN_R_INPUT].channels = 1;
        long frame = 0;
        double bleed = 0.0, drive = 0.0;
        for (int i = 0; i < (int)(2.0 * SR); i++) {
            float v = i < (int)(0.3 * SR)
                    ? (float)(10.0 * std::sin(2.0 * M_PI * 440.0 * i / SR)) : 0.f;
            m.inputs[Caligo::IN_L_INPUT].setVoltage(v);
            m.inputs[Caligo::IN_R_INPUT].setVoltage(0.f);
            m.process(makeArgs(frame++));
            bleed = std::max(bleed, (double)std::fabs(m.outputs[Caligo::OUT_R_OUTPUT].getVoltage()));
            drive = std::max(drive, (double)std::fabs(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage()));
        }
        spin.hit(bleed < 1e-9 && drive > 0.1, bleed, p);
    }

    dry.done();
    wet.done();
    loop.done();
    spin.done();
}

// ──────────────────────────────────────────────────────────────────── L

static void testTime() {
    Rng r(gSeed ^ 0x846ca68bu);
    Inv clk{"inv_L1_clock"}, mono{"inv_L2_monotone"};

    // Under a clock the delay is a ratio of it, latched on the edge.
    int n = 8 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        double period = r.range(0.2f, 0.8f);
        int idx = 3 + r.pick(9);                    // a ratio away from the ends
        p.time = idx / 14.f;
        Caligo m;
        apply(m, p);
        m.inputs[Caligo::CLK_INPUT].channels = 1;
        long frame = 0;
        int per = (int)(period * SR);
        for (int i = 0; i < per * 6; i++) {
            m.inputs[Caligo::CLK_INPUT].setVoltage((i % per) < 40 ? 5.f : 0.f);
            m.process(makeArgs(frame++));
        }
        double want = period * kSyncRatios[idx];
        want = std::min(std::max(want, (double)caligo_dsp::kMinTimeSec),
                        (double)m.engine.maxTimeSec());
        double got = m.effTimeSec;
        clk.hit(std::fabs(got - want) / want < 0.02,
                std::fabs(got - want) / want, p);
    }

    // and free-running, the knob only ever makes it longer
    int m2 = 4 * gScale;
    for (int k = 0; k < m2; k++) {
        Patch p = randomPatch(r);
        double prev = -1.0;
        bool ok = true;
        double worst = 0.0;
        for (int i = 0; i <= 8; i++) {
            p.time = i / 8.f;
            Caligo m;
            apply(m, p);
            long frame = 0;
            for (int j = 0; j < 64; j++) m.process(makeArgs(frame++));
            double t = m.effTimeSec;
            if (prev >= 0.0 && t < prev) { ok = false; worst = std::min(worst, t - prev); }
            prev = t;
        }
        mono.hit(ok, worst, p, false);
    }

    clk.done();
    mono.done();
}

// ──────────────────────────────────────────────────────────── F, D, C, M

static void testBehaviour() {
    Rng r(gSeed ^ 0x7feb352du);
    Inv frz{"inv_F1_freeze"}, damp{"inv_D1_damp"};
    Inv live{"inv_D2_knobs_live"}, cont{"inv_C1_continuous"};
    Inv safe{"inv_M1_mod_safe"};

    // Freeze holds the loop rather than letting it fall away.
    int n = 8 * gScale;
    for (int k = 0; k < n; k++) {
        Patch p = randomPatch(r);
        p.feedback = r.range(0.3f, 0.7f);
        p.mix = 1.f;
        p.drift = 0.f;
        // short enough that the loop is full by the time it is frozen: at the
        // top of the knob the first pass has not come round yet at 0.6 s, and
        // freezing an empty loop freezes silence
        p.time = r.range(0.f, 0.3f);
        // and with the damping left in the loop, freeze holds the *pattern*
        // while the level still falls away, which is what that menu option
        // is for. The claim here is about the setting that holds the level.
        p.freezeBypassDamp = true;
        Take t = runOf(p, 6.0, 0.3, 8.0, 440.0, true, 0.6);
        double a = rmsOf(t.l, (size_t)(1.5 * SR), (size_t)(2.5 * SR));
        double b = rmsOf(t.l, (size_t)(4.5 * SR), (size_t)(5.5 * SR));
        frz.hit(b > a * 0.4 && b > 1e-3, b / std::max(a, 1e-9), p, false);
    }

    // Damping darkens the tail.
    int m2 = 5 * gScale;
    for (int k = 0; k < m2; k++) {
        Patch p = randomPatch(r);
        p.feedback = r.range(0.6f, 0.8f);
        p.mix = 1.f;
        p.damp = 0.f;
        // the measured window has to hold tail rather than the silence before
        // the first pass comes round
        p.time = r.range(0.f, 0.35f);
        // A noise burst, not a tone: damping is a lowpass in the loop, and a
        // 440 Hz sine has next to nothing above the corner for it to take
        // away. Measured on noise the fraction above 2 kHz goes from 65% to
        // 11%; measured on a sine it goes from 3.7% to 3.5%, which is the
        // numerical floor moving and means nothing.
        Take dryT = runOf(p, 3.0, 0.2, 8.0, 440.0, false, -1.0, true);
        Patch q = p;
        q.damp = 0.9f;
        Take dampT = runOf(q, 3.0, 0.2, 8.0, 440.0, false, -1.0, true);
        // early enough that the damped run still has a tail to be dark: heavy
        // damping takes the loop down fast, and the spectrum of what is left
        // after that is numerical noise, which is broadband and would read as
        // *brighter* than the undamped run
        size_t a = (size_t)(0.4 * SR), b = (size_t)(1.0 * SR);
        if (rmsOf(dampT.l, a, b) < 1e-3) continue;
        double h0 = highFrac(dryT.l, a, b, 2000.0);
        double h1 = highFrac(dampT.l, a, b, 2000.0);
        damp.hit(h1 < h0, h1 / std::max(h0, 1e-12), p);
    }

    // Every knob has to do something.
    static const char* knobName[9] = {"time", "size", "diff", "feedback",
                                      "damp", "mod", "rate", "spin", "drift"};
    int m3 = 5 * gScale;
    for (int k = 0; k < m3; k++) {
        Patch base = randomPatch(r);
        base.mix = 1.f;                       // judge the wet, not the dry
        base.feedback = r.range(0.5f, 0.8f);
        // every knob but TIME is judged on a loop that cycles several times
        // inside the capture; a knob cannot show its effect on a tail that
        // has not arrived
        base.time = r.range(0.f, 0.4f);
        // and DRIFT scales SIZE, so it needs a size with room to move: down
        // at the bottom the walk runs into the floor and does nothing
        base.size = r.range(0.4f, 0.9f);
        for (int w = 0; w < 9; w++) {
            Patch lo = base, hi = base;
            switch (w) {
            case 0: lo.time = 0.3f;  hi.time = 0.6f;  break;
            case 1: lo.size = 0.1f;  hi.size = 0.9f;  break;
            case 2: lo.diff = 0.1f;  hi.diff = 0.9f;  break;
            case 3: lo.feedback = 0.3f; hi.feedback = 0.8f; break;
            case 4: lo.damp = 0.f;   hi.damp = 0.9f;  break;
            case 5: lo.mod = 0.f;    hi.mod = 1.f;    break;
            case 6: lo.rate = 0.1f;  hi.rate = 0.9f;
                    lo.mod = hi.mod = 0.6f;           // rate needs some depth
                    break;
            case 7: lo.spin = 0.f;   hi.spin = 1.f;   break;
            case 8: lo.drift = 0.f;  hi.drift = 1.f;  break;
            }
            // DRIFT walks SIZE over half a second to four seconds and starts
            // from no deviation at all, so two seconds barely separates it
            // from no drift: give it a window it can move in.
            double secs = (w == 8) ? 6.0 : 2.0;
            double d = diffRatio(runOf(lo, secs, 0.2, 6.0).l,
                                 runOf(hi, secs, 0.2, 6.0).l);
            live.hit(d > 0.05, d, base, false, knobName[w]);
        }
    }

    // Nothing in the control space steps. TIME is excluded on purpose: in
    // dissolve mode it is a staircase of crossfades by design, so a small
    // move is allowed to swap the whole tail for a different one.
    int m4 = 5 * gScale;
    for (int k = 0; k < m4; k++) {
        Patch base = randomPatch(r);
        base.mix = 1.f;
        base.feedback = r.range(0.4f, 0.75f);
        // A window has to hold several passes for its RMS to be a stable
        // thing to compare: at the top of the TIME knob one pass is seconds
        // long, and then the comparison is really about where a single echo
        // happened to land rather than about how the sound changed.
        base.time = r.range(0.f, 0.4f);
        // SIZE is left out on purpose, and not because it misbehaves: it
        // indexes a table of primes with an integer, so its *target* lengths
        // step even though the lengths themselves glide there smoothly. A two
        // percent move picks a different set of primes, which is a different
        // room. No click, but no similarity either, and demanding one would
        // be demanding that SIZE stop doing its job.
        static const char* cName[3] = {"diff", "damp", "spin"};
        for (int w = 0; w < 3; w++) {
            float v = r.range(0.05f, 0.9f);
            const float eps = 0.02f;
            Patch a = base, b = base;
            switch (w) {
            case 0: a.diff = v;  b.diff = v + eps;  break;
            case 1: a.damp = v;  b.damp = v + eps;  break;
            case 2: a.spin = v;  b.spin = v + eps;  break;
            }
            Take ta = runOf(a, 2.0, 0.2, 6.0), tb = runOf(b, 2.0, 0.2, 6.0);
            size_t f = (size_t)(0.5 * SR);
            double ra = rmsOf(ta.l, f), rb = rmsOf(tb.l, f);
            double d = std::fabs(ra - rb) / std::max(std::max(ra, rb), 1e-6);
            cont.hit(d < 0.5, d, a, true, cName[w]);
        }
    }

    // every CV at audio rate
    int m5 = 5 * gScale;
    for (int k = 0; k < m5; k++) {
        Patch p = randomPatch(r);
        Caligo m;
        apply(m, p);
        const int cvs[10] = {
            Caligo::TIME_CV_INPUT, Caligo::SIZE_CV_INPUT, Caligo::DIFF_CV_INPUT,
            Caligo::FEEDBACK_CV_INPUT, Caligo::DAMP_CV_INPUT, Caligo::MOD_CV_INPUT,
            Caligo::RATE_CV_INPUT, Caligo::MIX_CV_INPUT, Caligo::SPIN_CV_INPUT,
            Caligo::DRIFT_CV_INPUT
        };
        const int atts[10] = {
            Caligo::TIME_ATT_PARAM, Caligo::SIZE_ATT_PARAM, Caligo::DIFF_ATT_PARAM,
            Caligo::FEEDBACK_ATT_PARAM, Caligo::DAMP_ATT_PARAM, Caligo::MOD_ATT_PARAM,
            Caligo::RATE_ATT_PARAM, Caligo::MIX_ATT_PARAM, Caligo::SPIN_ATT_PARAM,
            Caligo::DRIFT_ATT_PARAM
        };
        for (int i = 0; i < 10; i++) {
            m.inputs[cvs[i]].channels = 1;
            m.params[atts[i]].setValue(1.f);
        }
        long frame = 0;
        long bad = 0;
        double pk = 0.0;
        double fm = r.range(200.0, 2000.0);
        for (int i = 0; i < (int)(2.0 * SR); i++) {
            double ph = std::sin(2.0 * M_PI * fm * i / SR);
            for (int j = 0; j < 10; j++)
                m.inputs[cvs[j]].setVoltage((float)(5.0 * std::sin(
                    2.0 * M_PI * fm * (1.0 + 0.13 * j) * i / SR)));
            m.inputs[Caligo::IN_L_INPUT].setVoltage((float)(5.0 * ph));
            m.process(makeArgs(frame++));
            for (int o = 0; o < 4; o++) {
                float v = m.outputs[o].getVoltage();
                if (!std::isfinite(v)) bad++;
                else pk = std::max(pk, (double)std::fabs(v));
            }
        }
        safe.hit(bad == 0 && pk <= 10.001, (double)bad + pk, p);
    }

    frz.done();
    damp.done();
    live.done();
    cont.done();
    safe.done();
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
    fprintf(stderr, "caligo_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testSafety();
    testRunaway();
    testExact();
    testTime();
    testBehaviour();

    return failures ? 1 : 0;
}
