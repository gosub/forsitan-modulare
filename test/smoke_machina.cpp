// smoke_machina — offline sanity checks for the combustion engine.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The firing-rate table, the per-tap levels and the CPU figures live in
// machina_probe.cpp, which is not run here.

#include "smoke_harness.hpp"

#include "../src/machina.cpp"

struct Taps {
    Stats s[4];
    int trigs = 0;
    double seconds = 0;
};

static Taps render(Machina& m, double settleS, double measS, float sr = SR) {
    Module::ProcessArgs args;
    args.sampleRate = sr;
    args.sampleTime = 1.f / sr;
    long fr = 0;
    for (int i = 0; i < (int)(settleS * sr); i++) {
        args.frame = fr++;
        m.process(args);
    }
    Taps t;
    t.seconds = measS;
    bool prev = false;
    for (int i = 0; i < (int)(measS * sr); i++) {
        args.frame = fr++;
        m.process(args);
        for (int o = 0; o < 4; o++) t.s[o].add(m.outputs[o].getVoltage());
        const bool g = m.outputs[Machina::TRIG_OUTPUT].getVoltage() > 5.f;
        if (g && !prev) t.trigs++;
        prev = g;
    }
    return t;
}

// It runs, and every tap produces something.
static void testRuns() {
    Machina m;
    m.params[Machina::LOAD_PARAM].setValue(0.5f);
    const Taps t = render(m, 0.6, 1.5);
    const char* names[4] = {"mix", "intake", "block", "pipe"};
    for (int o = 0; o < 4; o++) {
        char n[64];
        snprintf(n, sizeof n, "%s_sounds", names[o]);
        report("machina", n, t.s[o].rms(), t.s[o].rms() > 0.05);
        snprintf(n, sizeof n, "%s_nans", names[o]);
        report("machina", n, t.s[o].nans, t.s[o].nans == 0);
        snprintf(n, sizeof n, "%s_bounded", names[o]);
        report("machina", n, t.s[o].peak, t.s[o].peak <= 10.f);
    }
}

// The firing rate is the engine's own clock: rpm/120 four-stroke, rpm/60 two.
static void testFiringRate() {
    for (int stroke = 0; stroke < 2; stroke++) {
        const double div = stroke ? 60.0 : 120.0;
        bool ok = true;
        double worst = 0;
        for (float k : {0.2f, 0.5f, 0.8f}) {
            Machina m;
            m.params[Machina::RPM_PARAM].setValue(k);
            m.params[Machina::STROKE_PARAM].setValue((float)stroke);
            const Taps t = render(m, 0.5, 2.0);
            const double want = 200.0 * std::pow(60.0, k) / div;
            const double got = t.trigs / t.seconds;
            const double err = std::fabs(got - want) / want;
            worst = std::max(worst, err);
            if (err > 0.08) ok = false;
        }
        report("machina", stroke ? "two_stroke_rate" : "four_stroke_rate", worst, ok);
    }
}

// Throttle at zero still turns the engine over: the pistons pump whether or
// not anything is burning. What the throttle adds is combustion, which is a
// transient once per firing rather than more of the same, so it shows up in
// the crest factor far more than in the RMS.
static void testLoad() {
    auto measure = [](float load, double& rms, double& crest) {
        Machina m;
        m.params[Machina::LOAD_PARAM].setValue(load);
        m.limiter = false;
        long fr = 0;
        for (int i = 0; i < (int)(0.6 * SR); i++) m.process(makeArgs(fr++));
        double s2 = 0, peak = 0;
        const int n = (int)(1.5 * SR);
        for (int i = 0; i < n; i++) {
            m.process(makeArgs(fr++));
            const double v = m.outputs[Machina::MIX_OUTPUT].getVoltage();
            s2 += v * v;
            peak = std::max(peak, std::fabs(v));
        }
        rms = std::sqrt(s2 / n);
        crest = peak / std::max(rms, 1e-9);
    };
    double ra, rb, ca, cb;
    measure(0.f, ra, ca);
    measure(1.f, rb, cb);
    report("machina", "load_raises_level", rb / std::max(ra, 1e-9), rb > 1.1 * ra);
    report("machina", "load_adds_firing", cb / std::max(ca, 1e-9), cb > 1.3 * ca);
}

// A shut muffler lets nothing out of the tailpipe, which is what a
// fully reflective termination means. Open, it does.
static void testMuffler() {
    Machina open, shut;
    for (Machina* m : {&open, &shut}) m->params[Machina::LOAD_PARAM].setValue(0.5f);
    open.params[Machina::MUFF_PARAM].setValue(0.f);
    shut.params[Machina::MUFF_PARAM].setValue(1.f);
    const double a = render(open, 0.6, 1.0).s[Machina::PIPE_OUTPUT].rms();
    const double b = render(shut, 0.6, 1.0).s[Machina::PIPE_OUTPUT].rms();
    report("machina", "muffler_open_pipe", a, a > 0.5);
    report("machina", "muffler_shut_pipe", b, b < 0.05);
}

// Every cylinder count has to run.
static void testCylinders() {
    bool ok = true;
    double worst = 0;
    for (int c = 1; c <= 12; c++) {
        Machina m;
        m.params[Machina::CYL_PARAM].setValue((float)c);
        m.params[Machina::LOAD_PARAM].setValue(0.5f);
        const Taps t = render(m, 0.5, 0.7);
        if (t.s[Machina::MIX_OUTPUT].nans || t.s[Machina::MIX_OUTPUT].rms() < 0.02) ok = false;
        worst = std::max(worst, (double)t.s[Machina::MIX_OUTPUT].peak);
    }
    report("machina", "all_cylinder_counts", worst, ok && worst <= 10.0);
}

static void testSampleRates() {
    bool ok = true;
    double ref = 0.0;
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        Machina m;
        m.params[Machina::LOAD_PARAM].setValue(0.5f);
        m.params[Machina::RPM_PARAM].setValue(0.4f);
        const Taps t = render(m, 0.6, 1.5, sr);
        // The firing rate is a mechanical fact and must not move with the
        // sample rate at all.
        const double rate = t.trigs / t.seconds;
        if (t.s[Machina::MIX_OUTPUT].nans) ok = false;
        if (ref == 0.0)
            ref = rate;
        else if (std::fabs(rate - ref) > 0.1 * ref)
            ok = false;
    }
    report("machina", "samplerate_stable", ok ? 1 : 0, ok);
}

// Rev up and down while modulating every pipe length in the model at once.
static void testStress() {
    Machina m;
    long fr = 0;
    for (int i = 0; i < Machina::INPUTS_LEN; i++) m.inputs[i].channels = 1;
    m.params[Machina::BACK_PARAM].setValue(0.8f);
    Stats s;
    for (int i = 0; i < (int)(15 * SR); i++) {
        const float t = (float)i / SR;
        m.inputs[Machina::RPM_INPUT].setVoltage(5.f + 6.f * std::sin(2.f * M_PI * 0.17f * t));
        m.inputs[Machina::LOAD_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.7f * t));
        m.inputs[Machina::DISP_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 1.3f * t));
        m.inputs[Machina::EXH_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.43f * t));
        m.params[Machina::CYL_PARAM].setValue(1.f + std::fmod(t * 3.f, 11.f));
        m.params[Machina::STROKE_PARAM].setValue(std::fmod(t, 2.f) < 1.f ? 0.f : 1.f);
        m.params[Machina::MUFF_PARAM].setValue(std::fmod(t * 0.31f, 1.f));
        m.params[Machina::EXPAND_PARAM].setValue(std::fmod(t * 0.19f, 1.f));
        m.params[Machina::SPARK_PARAM].setValue(std::fmod(t * 0.53f, 1.f));
        m.params[Machina::ASYM_PARAM].setValue(std::fmod(t * 0.11f, 1.f));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Machina::MIX_OUTPUT].getVoltage());
    }
    report("machina", "stress_nans", s.nans, s.nans == 0);
    report("machina", "stress_peak", s.peak, s.peak <= 10.f);
    report("machina", "stress_alive", s.rms(), s.rms() > 0.05);
}

SMOKE_MAIN(testRuns, testFiringRate, testLoad, testMuffler, testCylinders,
           testSampleRates, testStress)
