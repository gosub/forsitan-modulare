// machina_probe — measurement harness for the combustion engine.
//
// What it reports:
//   - firing rate against RPM and the cycle switch, which is the module's
//     contract as a rhythm source: 4-stroke fires once per two revolutions;
//   - level at each of the four taps across the RPM range, which is where the
//     per-output gains come from;
//   - what the plumbing knobs do to the level and the spectral balance;
//   - a rev-up and rev-down sweep, checking that nothing rings or blows up
//     while every pipe length is being modulated;
//   - CPU, which is the highest of the four SDT modules: 42 waveguides.
//
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/machina.cpp"

#include <chrono>
#include <vector>

struct Taps {
    double rms[5] = {0, 0, 0, 0, 0};
    double peak[5] = {0, 0, 0, 0, 0};
    int trigs = 0;
    double seconds = 0;
    long nans = 0;
};

static Taps render(Machina& m, double settleS, double measS) {
    long fr = 0;
    for (int i = 0; i < (int)(settleS * SR); i++) m.process(makeArgs(fr++));
    Taps t;
    t.seconds = measS;
    const int n = (int)(measS * SR);
    double s2[5] = {0, 0, 0, 0, 0};
    bool prev = false;
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(fr++));
        for (int o = 0; o < 4; o++) {
            const double v = m.outputs[o].getVoltage();
            if (!std::isfinite(v)) t.nans++;
            s2[o] += v * v;
            t.peak[o] = std::max(t.peak[o], std::fabs(v));
        }
        const bool g = m.outputs[Machina::TRIG_OUTPUT].getVoltage() > 5.f;
        if (g && !prev) t.trigs++;
        prev = g;
    }
    for (int o = 0; o < 4; o++) t.rms[o] = std::sqrt(s2[o] / n);
    return t;
}

static void firingRate() {
    printf("\n# firing rate against RPM. Four stroke fires once per two turns,\n");
    printf("# so trig/s = rpm/120; two stroke is rpm/60.\n");
    printf("knob   rpm      4-stroke trig/s (want)   2-stroke trig/s (want)\n");
    for (float k : {0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f}) {
        const double rpm = 200.0 * std::pow(60.0, k);
        Machina a, b;
        a.params[Machina::RPM_PARAM].setValue(k);
        b.params[Machina::RPM_PARAM].setValue(k);
        b.params[Machina::STROKE_PARAM].setValue(1.f);
        const Taps ta = render(a, 0.5, 2.0);
        const Taps tb = render(b, 0.5, 2.0);
        printf("%-6.2f %-8.0f %-10.1f (%-8.1f)  %-10.1f (%.1f)\n", k, rpm,
               ta.trigs / ta.seconds, rpm / 120.0, tb.trigs / tb.seconds, rpm / 60.0);
    }
}

static void levels() {
    printf("\n# level at each tap (Vrms) across the RPM range, load 0.5\n");
    printf("rpm      mix      intake   block    pipe\n");
    for (float k : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        Machina m;
        m.params[Machina::RPM_PARAM].setValue(k);
        m.params[Machina::LOAD_PARAM].setValue(0.5f);
        const Taps t = render(m, 0.6, 2.0);
        printf("%-8.0f %-8.3f %-8.3f %-8.3f %.3f\n", 200.0 * std::pow(60.0, k),
               t.rms[Machina::MIX_OUTPUT], t.rms[Machina::INTAKE_OUTPUT],
               t.rms[Machina::BLOCK_OUTPUT], t.rms[Machina::PIPE_OUTPUT]);
    }
}

static void knobSweep(const char* name, int id, float lo, float hi) {
    printf("\n# %s\n", name);
    printf("value    mix rms  mix peak  pipe rms\n");
    for (int i = 0; i <= 4; i++) {
        const float v = lo + (hi - lo) * i / 4.f;
        Machina m;
        m.params[Machina::LOAD_PARAM].setValue(0.5f);
        m.params[id].setValue(v);
        const Taps t = render(m, 0.6, 2.0);
        printf("%-8.2f %-8.3f %-9.3f %.3f\n", v, t.rms[Machina::MIX_OUTPUT],
               t.peak[Machina::MIX_OUTPUT], t.rms[Machina::PIPE_OUTPUT]);
    }
}

// Sweep the speed up and back down over ten seconds with the plumbing moving
// too: every delay length in the model is being modulated at once.
static void revSweep() {
    printf("\n# rev sweep with the plumbing moving\n");
    Machina m;
    m.inputs[Machina::RPM_INPUT].channels = 1;
    m.inputs[Machina::EXH_INPUT].channels = 1;
    m.params[Machina::LOAD_PARAM].setValue(0.6f);
    m.params[Machina::BACK_PARAM].setValue(0.6f);
    long fr = 0;
    double s2 = 0, peak = 0;
    long nans = 0;
    const int n = (int)(10 * SR);
    for (int i = 0; i < n; i++) {
        const float t = (float)i / SR;
        m.inputs[Machina::RPM_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 0.1f * t));
        m.inputs[Machina::EXH_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.37f * t));
        m.process(makeArgs(fr++));
        const double v = m.outputs[Machina::MIX_OUTPUT].getVoltage();
        if (!std::isfinite(v)) nans++;
        s2 += v * v;
        peak = std::max(peak, std::fabs(v));
    }
    printf("rms %.3f V, peak %.3f V, non-finite %ld\n", std::sqrt(s2 / n), peak, nans);
}

static void cpu() {
    Machina m;
    m.params[Machina::CYL_PARAM].setValue(12.f);
    m.params[Machina::RPM_PARAM].setValue(0.6f);
    m.params[Machina::LOAD_PARAM].setValue(0.7f);
    long fr = 0;
    for (int i = 0; i < (int)SR; i++) m.process(makeArgs(fr++));
    const int n = (int)(SR * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) m.process(makeArgs(fr++));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n# cpu: %.2f%% of one core at %g Hz, twelve cylinders\n",
           100.0 * dt / (n / SR), SR);

    Machina one;
    one.params[Machina::CYL_PARAM].setValue(1.f);
    long fr2 = 0;
    for (int i = 0; i < (int)SR; i++) one.process(makeArgs(fr2++));
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) one.process(makeArgs(fr2++));
    const double dt1 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    printf("# cpu: %.2f%% of one core, one cylinder\n", 100.0 * dt1 / (n / SR));
}

int main() {
    rack::random::init();
    firingRate();
    levels();
    knobSweep("displacement", Machina::DISP_PARAM, 0.f, 1.f);
    knobSweep("exhaust length (and the pipes that scale with it)", Machina::EXH_PARAM, 0.f, 1.f);
    knobSweep("muffler", Machina::MUFF_PARAM, 0.f, 1.f);
    knobSweep("expansion", Machina::EXPAND_PARAM, 0.f, 1.f);
    knobSweep("compression ratio", Machina::COMP_PARAM, 5.f, 20.f);
    revSweep();
    cpu();
    return 0;
}
