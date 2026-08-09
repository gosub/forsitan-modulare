// smoke_crepitus — offline sanity checks for the fracture point process.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The drive/crit plane, the avalanche statistics and the CPU figure live in
// crepitus_probe.cpp, which is not run here.

#include "smoke_harness.hpp"

#include "../src/crepitus.cpp"

struct Run {
    Stats s;
    int events = 0;
    double integrity = 0;
    double seconds = 0;
};

static Run render(Crepitus& m, double settleS, double measS, float sr = SR) {
    long fr = 0;
    Module::ProcessArgs args;
    args.sampleRate = sr;
    args.sampleTime = 1.f / sr;
    for (int i = 0; i < (int)(settleS * sr); i++) {
        args.frame = fr++;
        m.process(args);
    }
    Run r;
    r.seconds = measS;
    const int n = (int)(measS * sr);
    bool prev = false;
    double iSum = 0;
    for (int i = 0; i < n; i++) {
        args.frame = fr++;
        m.process(args);
        r.s.add(m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage());
        const bool e = m.outputs[Crepitus::EVENT_OUTPUT].getVoltage() > 5.f;
        if (e && !prev) r.events++;
        prev = e;
        iSum += m.engine.integrity;
    }
    r.integrity = iSum / n;
    return r;
}

// The event rate has to follow DRIVE over the whole knob, and the module has
// to be silent when nothing is happening.
static void testDrive() {
    double prev = -1.0;
    bool mono = true;
    for (int i = 0; i <= 4; i++) {
        Crepitus m;
        m.params[Crepitus::DRIVE_PARAM].setValue(i / 4.f);
        m.params[Crepitus::CRIT_PARAM].setValue(0.3f);
        const Run r = render(m, 0.5, 2.0);
        const double rate = r.events / r.seconds;
        if (rate < prev) mono = false;
        prev = rate;
        if (i == 0) report("crepitus", "slowest_quiet", r.s.rms(), r.s.rms() < 0.5);
        if (i == 4) report("crepitus", "fastest_loud", r.s.rms(), r.s.rms() > 0.5);
    }
    report("crepitus", "drive_monotonic", mono ? 1 : 0, mono);
}

// Criticality has to raise the rate at a fixed drive: that is the knob's
// entire job.
static void testCriticality() {
    Crepitus lo, hi;
    for (Crepitus* m : {&lo, &hi}) m->params[Crepitus::DRIVE_PARAM].setValue(0.4f);
    lo.params[Crepitus::CRIT_PARAM].setValue(0.f);
    hi.params[Crepitus::CRIT_PARAM].setValue(2.f);
    const Run a = render(lo, 1.0, 3.0);
    const Run b = render(hi, 1.0, 3.0);
    const double ra = a.events / a.seconds, rb = b.events / b.seconds;
    report("crepitus", "crit_raises_rate", rb / std::max(ra, 1e-9), rb > 2.0 * ra);

    // Above criticality the process must park at integrity ~ 1/crit rather
    // than either running away or grinding to a halt.
    report("crepitus", "self_limits", b.integrity, b.integrity > 0.25 && b.integrity < 0.8);
    report("crepitus", "crit_nans", b.s.nans, b.s.nans == 0);

    // The setting that should be loudest must not be the one that goes
    // silent: without a refractory period a supercritical process saturates
    // at one event per sample and pins the hammer instead of striking.
    Crepitus wild;
    wild.params[Crepitus::DRIVE_PARAM].setValue(1.f);
    wild.params[Crepitus::CRIT_PARAM].setValue(3.f);
    const Run c = render(wild, 1.0, 2.0);
    report("crepitus", "supercritical_sounds", c.s.rms(), c.s.rms() > 0.5);
}

// A strike from outside makes a sound with the process otherwise idle.
static void testStrike() {
    Crepitus m;
    m.params[Crepitus::DRIVE_PARAM].setValue(0.f);
    m.params[Crepitus::CRIT_PARAM].setValue(0.f);
    m.inputs[Crepitus::STRIKE_INPUT].channels = 1;
    long fr = 0;
    for (int i = 0; i < (int)(0.2 * SR); i++) m.process(makeArgs(fr++));
    Stats before;
    for (int i = 0; i < (int)(0.2 * SR); i++) {
        m.process(makeArgs(fr++));
        before.add(m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage());
    }
    Stats after;
    for (int i = 0; i < (int)(0.2 * SR); i++) {
        m.inputs[Crepitus::STRIKE_INPUT].setVoltage(i < 100 ? 5.f : 0.f);
        m.process(makeArgs(fr++));
        after.add(m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage());
    }
    report("crepitus", "strike_sounds", after.peak, after.peak > 0.5f);
    report("crepitus", "strike_above_idle", after.peak / std::max(before.peak, 1e-6f),
           after.peak > 4.f * before.peak);
}

// Pitch follows V/oct.
static void testPitch() {
    auto zcr = [](Crepitus& m) {
        long fr = 0;
        for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(fr++));
        int z = 0;
        float prev = 0.f;
        const int n = (int)(2 * SR);
        for (int i = 0; i < n; i++) {
            m.process(makeArgs(fr++));
            const float y = m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage();
            if ((y > 0.f) != (prev > 0.f)) z++;
            prev = y;
        }
        return z * SR / 2.0 / n;
    };
    Crepitus lo, hi;
    for (Crepitus* m : {&lo, &hi}) {
        m->params[Crepitus::DRIVE_PARAM].setValue(0.35f);
        m->params[Crepitus::CRIT_PARAM].setValue(0.2f);
        m->params[Crepitus::DECAY_PARAM].setValue(0.8f);
        m->params[Crepitus::SIZE_PARAM].setValue(1.f);
        m->inputs[Crepitus::VOCT_INPUT].channels = 1;
    }
    hi.inputs[Crepitus::VOCT_INPUT].setVoltage(2.f);
    const double a = zcr(lo), b = zcr(hi);
    report("crepitus", "voct_raises_pitch", b / std::max(a, 1e-9), b > 1.6 * a);
}

static void testSampleRates() {
    double ref = 0.0;
    bool ok = true;
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        Crepitus m;
        m.params[Crepitus::DRIVE_PARAM].setValue(0.6f);
        m.params[Crepitus::CRIT_PARAM].setValue(1.f);
        const Run r = render(m, 0.5, 2.0, sr);
        if (r.s.nans) ok = false;
        if (ref == 0.0)
            ref = r.s.rms();
        else if (r.s.rms() < 0.4 * ref || r.s.rms() > 2.5 * ref)
            ok = false;
    }
    report("crepitus", "samplerate_stable", ok ? 1 : 0, ok);
}

static void testStress() {
    Crepitus m;
    long fr = 0;
    for (int i = 0; i < Crepitus::INPUTS_LEN; i++) m.inputs[i].channels = 1;
    Stats s;
    for (int i = 0; i < (int)(10 * SR); i++) {
        const float t = (float)i / SR;
        m.inputs[Crepitus::DRIVE_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.6f * t));
        m.inputs[Crepitus::CRIT_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 1.1f * t));
        m.inputs[Crepitus::VOCT_INPUT].setVoltage(6.f * std::sin(2.f * M_PI * 0.29f * t));
        m.inputs[Crepitus::STRIKE_INPUT].setVoltage(std::fmod(t, 0.31f) < 0.005f ? 5.f : 0.f);
        m.params[Crepitus::ENERGY_PARAM].setValue(std::fmod(t * 0.7f, 1.f));
        m.params[Crepitus::FRAG_PARAM].setValue(std::fmod(t * 1.3f, 1.f));
        m.params[Crepitus::SIZE_PARAM].setValue(std::fmod(t * 0.41f, 1.f));
        m.params[Crepitus::HARD_PARAM].setValue(std::fmod(t * 0.9f, 1.f));
        m.params[Crepitus::DECAY_PARAM].setValue(std::fmod(t * 0.23f, 1.f));
        m.params[Crepitus::MAT_PARAM].setValue(std::fmod(t * 0.17f, 1.f));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage());
    }
    report("crepitus", "stress_nans", s.nans, s.nans == 0);
    report("crepitus", "stress_peak", s.peak, s.peak <= 10.f);
    report("crepitus", "stress_alive", s.rms(), s.rms() > 0.05);
}

SMOKE_MAIN(testDrive, testCriticality, testStrike, testPitch, testSampleRates, testStress)
