// smoke_radix - offline sanity checks for the radix module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by radix_probe, which needs no Rack. These are the
// module's own checks: every program reachable by CV and finite, the stepped
// knobs' rounding, V/oct, the DC blocker, CV OUT, the feedback cable, and the
// context-menu state surviving a save.

#include "smoke_harness.hpp"
#include "../src/radix.cpp"

static void connect(Radix& m, int input, float v) {
    m.inputs[input].channels = 1;
    m.inputs[input].setVoltage(v);
}

static Stats run(Radix& m, long& frame, double settleS, double measS,
                 int output = Radix::AUDIO_OUTPUT) {
    for (long i = 0; i < (long)(settleS * SR); i++) m.process(makeArgs(frame++));
    Stats s;
    for (long i = 0; i < (long)(measS * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[output].getVoltage());
    }
    return s;
}

static int zeroCrossings(Radix& m, long& frame, double seconds) {
    int zc = 0;
    float prev = 0.f;
    for (long i = 0; i < (long)(seconds * SR); i++) {
        m.process(makeArgs(frame++));
        float y = m.outputs[Radix::AUDIO_OUTPUT].getVoltage();
        if (prev <= 0.f && y > 0.f) zc++;
        prev = y;
    }
    return zc;
}

// The defaults make a sound: a plain sine at C4, at +-5 V.
static void testDefault() {
    Radix m; long fr = 0;
    Stats s = run(m, fr, 0.1, 0.5);
    report("radix", "default_nans", s.nans, s.nans == 0);
    report("radix", "default_rms", s.rms(), s.rms() > 3.0 && s.rms() < 4.0);
    int zc = zeroCrossings(m, fr, 1.0);
    report("radix", "default_hz", zc, std::abs(zc - 262) <= 2);
}

// Every one of the 150 programs, reached through the CV inputs alone with
// every knob at zero, is finite, inside the rails, and carries no DC past the
// blocker.
static void testAllPrograms() {
    long nans = 0, overs = 0, dcs = 0, silent = 0;
    for (int s = 0; s < radix::NUM_SRC; s++)
        for (int l = 0; l < radix::NUM_LAW; l++)
            for (int t = 0; t < radix::NUM_TABLE; t++) {
                Radix m; long fr = 0;
                connect(m, Radix::SRC_CV_INPUT, (float)s);
                connect(m, Radix::LAW_CV_INPUT, (float)l);
                connect(m, Radix::TABLE_CV_INPUT, (float)t);
                connect(m, Radix::AUDIO_INPUT, 0.f);
                m.params[Radix::PARAM_PARAM].setValue(64.f);
                // the blocker's 2 Hz corner settles in about a second
                Stats st = run(m, fr, 1.5, 0.5);
                if (m.p.src != s || m.p.law != l || m.p.table != t) nans += 1000;
                nans += st.nans;
                if (st.peak > 10.f) overs++;
                if (std::fabs(st.sum / st.n) > 0.5) dcs++;
                if (st.rms() < 0.05) silent++;
            }
    report("radix", "programs_nans", nans, nans == 0);
    report("radix", "programs_over_rails", overs, overs == 0);
    report("radix", "programs_dc_over_0.5v", dcs, dcs == 0);
    // IN is patched to 0 V, so SRC IN is legitimately quiet in places; this
    // counts, it does not gate
    report("radix", "programs_silent", silent, silent < 15);
}

// Knob plus CV, rounded, clamped: 2.4 V on a knob at 0 is step 2, 2.6 is
// step 3, and past either end is the end.
static void testStepped() {
    struct Case { float knob, cv; int want; };
    const Case cases[] = {{0.f, 2.4f, 2}, {0.f, 2.6f, 3}, {1.f, 1.f, 2},
                          {4.f, 10.f, 4}, {3.f, -10.f, 0}};
    int bad = 0;
    for (const Case& c : cases) {
        Radix m; long fr = 0;
        m.params[Radix::LAW_PARAM].setValue(c.knob);
        connect(m, Radix::LAW_CV_INPUT, c.cv);
        m.process(makeArgs(fr++));
        if (m.p.law != c.want) bad++;
    }
    report("radix", "stepped_rounding", bad, bad == 0);
}

// One volt up is one octave up.
static void testVoct() {
    Radix m; long fr = 0;
    connect(m, Radix::VOCT_INPUT, 1.f);
    run(m, fr, 0.1, 0.0);
    int zc = zeroCrossings(m, fr, 1.0);
    report("radix", "voct_plus1_hz", zc, std::abs(zc - 523) <= 3);
}

// CV OUT stays in 0..10 V and moves.
static void testCvOut() {
    Radix m; long fr = 0;
    m.params[Radix::SRC_PARAM].setValue(radix::SRC_COUNT);
    m.params[Radix::TABLE_PARAM].setValue(radix::TAB_NOISE);
    m.params[Radix::PARAM_PARAM].setValue(255.f);
    float lo = 1e9f, hi = -1e9f;
    long nans = 0;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(fr++));
        float v = m.outputs[Radix::CV_OUTPUT].getVoltage();
        if (!std::isfinite(v)) nans++;
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    report("radix", "cvout_nans", nans, nans == 0);
    report("radix", "cvout_range", hi - lo, lo >= 0.f && hi <= 10.f && hi - lo > 5.f);
}

// OUT patched back into IN, SRC on IN, full depth, every law: the howl the
// module exists for stays finite and inside the rails for five seconds.
static void testFeedback() {
    long nans = 0, overs = 0;
    for (int l = 0; l < radix::NUM_LAW; l++) {
        Radix m; long fr = 0;
        m.params[Radix::SRC_PARAM].setValue(radix::SRC_IN);
        m.params[Radix::LAW_PARAM].setValue((float)l);
        m.params[Radix::PARAM_PARAM].setValue(255.f);
        m.inputs[Radix::AUDIO_INPUT].channels = 1;
        float y = 0.f;
        for (long i = 0; i < (long)(5 * SR); i++) {
            m.inputs[Radix::AUDIO_INPUT].setVoltage(y);   // one-sample cable
            m.process(makeArgs(fr++));
            y = m.outputs[Radix::AUDIO_OUTPUT].getVoltage();
            if (!std::isfinite(y)) { nans++; y = 0.f; }
            if (std::fabs(y) > 10.f) overs++;
        }
    }
    report("radix", "feedback_nans", nans, nans == 0);
    report("radix", "feedback_over_rails", overs, overs == 0);
}

// Garbage at the inputs does not reach the output.
static void testNanInputs() {
    Radix m; long fr = 0;
    m.params[Radix::SRC_PARAM].setValue(radix::SRC_IN);
    m.params[Radix::PARAM_PARAM].setValue(255.f);
    const int ins[] = {Radix::VOCT_INPUT, Radix::PARAM_CV_INPUT, Radix::CLOCK_CV_INPUT,
                       Radix::SRC_CV_INPUT, Radix::LAW_CV_INPUT, Radix::TABLE_CV_INPUT,
                       Radix::AUDIO_INPUT};
    for (int i : ins) connect(m, i, NAN);
    Stats s = run(m, fr, 0.0, 0.5);
    Stats c = run(m, fr, 0.0, 0.1, Radix::CV_OUTPUT);
    report("radix", "nan_inputs", s.nans + c.nans, s.nans + c.nans == 0);
}

// The TEXT string and the clock option survive a save and a load, and an
// empty string makes the TEXT table silent rather than stale.
static void testJson() {
    Radix a;
    a.setText("abcxyz");
    a.p.clockMovesPitch = true;
    json_t* j = a.dataToJson();
    Radix b;
    b.dataFromJson(j);
    json_decref(j);
    report("radix", "json_text", b.text == "abcxyz", b.text == "abcxyz");
    report("radix", "json_clock", b.p.clockMovesPitch, b.p.clockMovesPitch);

    Radix e; long fr = 0;
    e.setText("");
    e.params[Radix::TABLE_PARAM].setValue(radix::TAB_TEXT);
    Stats s = run(e, fr, 0.2, 0.3);
    report("radix", "empty_text_silent", s.rms(), s.rms() < 0.01);
}

// GRIT is audible from early in its travel: at a quarter it has already
// moved the default sine by more than -40 dB. It once spent its first 60%
// under -52 dB and sounded like a knob wired to nothing.
static void testGrit() {
    Radix a, b; long fa = 0, fb = 0;
    b.params[Radix::GRIT_PARAM].setValue(0.25f);
    double e = 0.0, s = 0.0;
    for (long i = 0; i < (long)(0.5 * SR); i++) {
        a.process(makeArgs(fa++));
        b.process(makeArgs(fb++));
        float x = a.outputs[Radix::AUDIO_OUTPUT].getVoltage();
        float y = b.outputs[Radix::AUDIO_OUTPUT].getVoltage();
        e += (double)(y - x) * (y - x);
        s += (double)x * x;
    }
    double db = 10.0 * std::log10(e / s + 1e-30);
    report("radix", "grit_quarter_db", db, db > -40.0);
}

SMOKE_MAIN(testDefault, testAllPrograms, testStepped, testVoct, testCvOut,
           testFeedback, testNanInputs, testJson, testGrit)
