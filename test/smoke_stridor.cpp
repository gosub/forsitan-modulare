// smoke_stridor — offline sanity checks for the friction voice.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The level plane, the creak-to-squeal transition and the CPU figure live in
// stridor_probe.cpp, which is not run here.

#include "smoke_harness.hpp"

#include "../src/stridor.cpp"

static Stats run(Stridor& m, double settleS, double measS, float sr = SR) {
    long fr = 0;
    Module::ProcessArgs args;
    args.sampleRate = sr;
    args.sampleTime = 1.f / sr;
    for (int i = 0; i < (int)(settleS * sr); i++) {
        args.frame = fr++;
        m.process(args);
    }
    Stats s;
    for (int i = 0; i < (int)(measS * sr); i++) {
        args.frame = fr++;
        m.process(args);
        s.add(m.outputs[Stridor::AUDIO_OUTPUT].getVoltage());
    }
    return s;
}

// Barely moving (the VEL knob bottoms out at 1.5 mm/s, not at zero): the
// contact mostly sticks and the object only murmurs. Dragged at speed it
// sings. The ratio is the whole instrument.
static void testForceAndVelocity() {
    Stridor still;
    still.params[Stridor::FORCE_PARAM].setValue(0.6f);
    still.params[Stridor::VEL_PARAM].setValue(0.f);
    const Stats a = run(still, 0.4, 0.6);

    Stridor moving;
    moving.params[Stridor::FORCE_PARAM].setValue(0.6f);
    moving.params[Stridor::VEL_PARAM].setValue(0.85f);
    const Stats b = run(moving, 0.4, 0.6);

    report("stridor", "crawl_quiet", a.rms(), a.rms() < 0.4);
    report("stridor", "moving_sounds", b.rms(), b.rms() > 0.3);
    report("stridor", "velocity_ratio", b.rms() / std::max(a.rms(), 1e-9),
           b.rms() > 5.0 * a.rms());
    report("stridor", "moving_nans", b.nans, b.nans == 0);

    // No normal force means no contact at all, whatever the velocity.
    Stridor lifted;
    lifted.params[Stridor::FORCE_PARAM].setValue(0.f);
    lifted.params[Stridor::VEL_PARAM].setValue(0.85f);
    const Stats c = run(lifted, 0.4, 0.6);
    report("stridor", "no_force_silent", c.rms(), c.rms() < 0.02);
}

// Level rises monotonically with normal force.
static void testForceMonotonic() {
    double prev = -1.0;
    bool mono = true;
    for (int i = 1; i <= 5; i++) {
        Stridor m;
        m.params[Stridor::FORCE_PARAM].setValue(i / 5.f);
        m.params[Stridor::VEL_PARAM].setValue(0.7f);
        const double r = run(m, 0.3, 0.5).rms();
        if (r < prev * 0.98) mono = false;
        prev = r;
    }
    report("stridor", "force_monotonic", mono ? 1 : 0, mono);
}

// Somewhere in the velocity range the contact must stick and slip rather than
// slide: that is the difference between a voice and a noise generator.
static void testSlips() {
    int best = 0;
    for (float v = 0.3f; v <= 0.95f; v += 0.05f) {
        Stridor m;
        m.params[Stridor::FORCE_PARAM].setValue(0.7f);
        m.params[Stridor::VEL_PARAM].setValue(v);
        long fr = 0;
        for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(fr++));
        int n = 0;
        bool prev = false;
        for (int i = 0; i < (int)SR; i++) {
            m.process(makeArgs(fr++));
            const bool s = m.outputs[Stridor::SLIP_OUTPUT].getVoltage() > 5.f;
            if (s && !prev) n++;
            prev = s;
        }
        best = std::max(best, n);
    }
    report("stridor", "stickslip_present", best, best > 20);
}

// Pitch tracks the V/oct input, measured as the zero-crossing rate of the
// output, which follows the fundamental closely enough for an octave test.
static void testPitch() {
    auto zcr = [](Stridor& m) {
        long fr = 0;
        for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(fr++));
        int z = 0;
        float prev = 0.f;
        const int n = (int)SR;
        for (int i = 0; i < n; i++) {
            m.process(makeArgs(fr++));
            const float y = m.outputs[Stridor::AUDIO_OUTPUT].getVoltage();
            if ((y > 0.f) != (prev > 0.f)) z++;
            prev = y;
        }
        return z * SR / 2.0 / n;
    };
    Stridor lo, hi;
    for (Stridor* m : {&lo, &hi}) {
        m->params[Stridor::FORCE_PARAM].setValue(0.7f);
        m->params[Stridor::VEL_PARAM].setValue(0.75f);
        m->params[Stridor::ROUGH_PARAM].setValue(0.1f);   // less broadband hiss
        m->params[Stridor::DECAY_PARAM].setValue(0.8f);
        m->inputs[Stridor::VOCT_INPUT].channels = 1;
    }
    hi.inputs[Stridor::VOCT_INPUT].setVoltage(2.f);
    const double a = zcr(lo), b = zcr(hi);
    report("stridor", "voct_raises_pitch", b / std::max(a, 1e-9), b > 1.6 * a);
}

// Same patch at three sample rates: the model is discretised per rate, so the
// level should not depend on it.
static void testSampleRates() {
    double ref = 0.0;
    bool ok = true;
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        Stridor m;
        m.params[Stridor::FORCE_PARAM].setValue(0.7f);
        m.params[Stridor::VEL_PARAM].setValue(0.8f);
        const Stats s = run(m, 0.4, 0.6, sr);
        if (s.nans) ok = false;
        if (ref == 0.0)
            ref = s.rms();
        else if (s.rms() < 0.4 * ref || s.rms() > 2.5 * ref)
            ok = false;
    }
    report("stridor", "samplerate_stable", ok ? 1 : 0, ok);
}

// Every knob to both ends, CV slamming, for ten seconds. Nothing may blow up.
static void testStress() {
    Stridor m;
    long fr = 0;
    for (int i = 0; i < Stridor::INPUTS_LEN; i++) m.inputs[i].channels = 1;
    Stats s;
    for (int i = 0; i < (int)(10 * SR); i++) {
        const float t = (float)i / SR;
        m.inputs[Stridor::FORCE_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.7f * t));
        m.inputs[Stridor::VEL_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 1.3f * t));
        m.inputs[Stridor::VOCT_INPUT].setVoltage(6.f * std::sin(2.f * M_PI * 0.31f * t));
        m.inputs[Stridor::ROUGH_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 2.9f * t));
        const float saw = std::fmod(t * 0.9f, 1.f);
        m.params[Stridor::STIFF_PARAM].setValue(saw);
        m.params[Stridor::DISS_PARAM].setValue(1.f - saw);
        m.params[Stridor::VISC_PARAM].setValue(std::fmod(t * 1.7f, 1.f));
        m.params[Stridor::DECAY_PARAM].setValue(std::fmod(t * 0.4f, 1.f));
        m.params[Stridor::MAT_PARAM].setValue(std::fmod(t * 0.23f, 1.f));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Stridor::AUDIO_OUTPUT].getVoltage());
    }
    report("stridor", "stress_nans", s.nans, s.nans == 0);
    report("stridor", "stress_peak", s.peak, s.peak <= 10.f);
    report("stridor", "stress_alive", s.rms(), s.rms() > 0.05);
}

SMOKE_MAIN(testForceAndVelocity, testForceMonotonic, testSlips, testPitch,
           testSampleRates, testStress)
