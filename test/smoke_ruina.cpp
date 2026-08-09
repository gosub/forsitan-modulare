// smoke_ruina — offline sanity checks for the object under load.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The lifetime laws, the Weibull statistics and the emission ramp live in
// ruina_probe.cpp, which is not run here.

#include "smoke_harness.hpp"

#include "../src/ruina.cpp"

// Run until failure, returning the lifetime in seconds (or the cap).
static double lifetime(Ruina& m, double capS, float sr = SR) {
    Module::ProcessArgs args;
    args.sampleRate = sr;
    args.sampleTime = 1.f / sr;
    const int cap = (int)(capS * sr);
    for (int i = 0; i < cap; i++) {
        args.frame = i;
        m.process(args);
        if (m.outputs[Ruina::BREAK_OUTPUT].getVoltage() > 5.f) return i / (double)sr;
    }
    return capS;
}

static void setLoaded(Ruina& m, float load, float tough, float brit) {
    m.params[Ruina::LOAD_PARAM].setValue(load);
    m.params[Ruina::TOUGH_PARAM].setValue(tough);
    m.params[Ruina::BRIT_PARAM].setValue(brit);
}

// Loaded, it fails. Unloaded, it does not: the object must not break on its
// own, or LOAD is decoration.
static void testFailsUnderLoad() {
    Ruina loaded;
    setLoaded(loaded, 1.f, 0.2f, 1.f);
    const double life = lifetime(loaded, 20.0);
    report("ruina", "fails_under_load", life, life < 5.0);

    Ruina idle;
    setLoaded(idle, 0.f, 0.2f, 1.f);
    const double idleLife = lifetime(idle, 20.0);
    report("ruina", "never_fails_unloaded", idleLife, idleLife >= 20.0);
}

// The lifetime follows the cube of the load: half the load is eight times the
// life, which is what makes LOAD worth automating.
static void testCubeLaw() {
    auto meanLife = [](float load) {
        double s = 0;
        const int reps = 5;
        for (int i = 0; i < reps; i++) {
            Ruina m;
            setLoaded(m, load, 0.35f, 1.f);
            s += lifetime(m, 200.0);
        }
        return s / reps;
    };
    const double full = meanLife(1.f), half = meanLife(0.5f);
    const double ratio = half / std::max(full, 1e-9);
    report("ruina", "cube_law", ratio, ratio > 5.0 && ratio < 12.0);
}

// Brittleness is the spread of the lifetime, and that spread is the module:
// the same gesture twice has to give two different answers.
static void testWeibullSpread() {
    auto cv = [](float brit) {
        double s = 0, s2 = 0;
        const int reps = 16;
        for (int i = 0; i < reps; i++) {
            Ruina m;
            setLoaded(m, 1.f, 0.3f, brit);
            const double t = lifetime(m, 60.0);
            s += t;
            s2 += t * t;
        }
        const double mean = s / reps;
        return std::sqrt(std::max(0.0, s2 / reps - mean * mean)) / std::max(mean, 1e-9);
    };
    const double soft = cv(0.1f), hard = cv(1.f);
    report("ruina", "ductile_is_random", soft, soft > 0.3);
    report("ruina", "brittle_is_punctual", hard, hard < 0.15);
    report("ruina", "brittleness_narrows", soft / std::max(hard, 1e-9), soft > 3.0 * hard);
}

// STRAIN climbs while the object holds, and a new object starts from zero.
// Sampled a second after the break, with a toughness that gives a twelve
// second life, so the reading is the fresh object and not the next one's
// progress.
static void testStrain() {
    Ruina m;
    setLoaded(m, 1.f, 0.7f, 1.f);
    long fr = 0;
    float peak = 0.f, atBreak = 0.f, after = -1.f;
    long breakFrame = -1;
    for (int i = 0; i < (int)(40 * SR); i++) {
        m.process(makeArgs(fr++));
        const float s = m.outputs[Ruina::STRAIN_OUTPUT].getVoltage();
        if (breakFrame < 0) {
            peak = std::max(peak, s);
            if (m.outputs[Ruina::BREAK_OUTPUT].getVoltage() > 5.f) {
                atBreak = s;
                breakFrame = i;
            }
        } else if (i == breakFrame + (int)SR) {
            after = s;
            break;
        }
    }
    report("ruina", "strain_climbs", peak, peak > 2.f);
    report("ruina", "strain_at_break", atBreak, atBreak > 2.f);
    report("ruina", "strain_resets", after, after >= 0.f && after < 0.3f * atBreak);
}

// The collapse makes a sound, and it is louder than the creaking before it.
static void testCollapse() {
    Ruina m;
    setLoaded(m, 1.f, 0.f, 1.f);
    long fr = 0;
    Stats before;
    while (!m.failing && fr < (long)(20 * SR)) {
        m.process(makeArgs(fr++));
        before.add(m.outputs[Ruina::AUDIO_OUTPUT].getVoltage());
    }
    Stats during;
    int n = 0;
    while (m.failing && n < (int)(4 * SR)) {
        m.process(makeArgs(fr++));
        during.add(m.outputs[Ruina::AUDIO_OUTPUT].getVoltage());
        n++;
    }
    report("ruina", "collapse_happens", n, n > 100);
    report("ruina", "collapse_loud", during.rms(), during.rms() > 0.5);
    report("ruina", "collapse_ends", n, n < (int)(4 * SR));
    report("ruina", "collapse_nans", during.nans, during.nans == 0);
}

// RESET hands the patch a new object: the damage goes back to zero.
static void testReset() {
    Ruina m;
    setLoaded(m, 1.f, 0.5f, 1.f);
    m.inputs[Ruina::RESET_INPUT].channels = 1;
    long fr = 0;
    for (int i = 0; i < (int)(1.5 * SR); i++) m.process(makeArgs(fr++));
    const float loaded = m.outputs[Ruina::STRAIN_OUTPUT].getVoltage();
    for (int i = 0; i < 200; i++) {
        m.inputs[Ruina::RESET_INPUT].setVoltage(i < 100 ? 5.f : 0.f);
        m.process(makeArgs(fr++));
    }
    const float after = m.outputs[Ruina::STRAIN_OUTPUT].getVoltage();
    report("ruina", "reset_clears_damage", after, after < 0.2f * loaded);
}

static void testSampleRates() {
    bool ok = true;
    double ref = 0.0;
    for (float sr : {44100.f, 48000.f, 96000.f}) {
        Ruina m;
        setLoaded(m, 1.f, 0.4f, 1.f);
        const double life = lifetime(m, 60.0, sr);
        if (ref == 0.0)
            ref = life;
        else if (life < 0.6 * ref || life > 1.6 * ref)
            ok = false;
    }
    report("ruina", "lifetime_samplerate_stable", ok ? 1 : 0, ok);
}

static void testStress() {
    Ruina m;
    long fr = 0;
    for (int i = 0; i < Ruina::INPUTS_LEN; i++) m.inputs[i].channels = 1;
    Stats s, str;
    for (int i = 0; i < (int)(20 * SR); i++) {
        const float t = (float)i / SR;
        m.inputs[Ruina::LOAD_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.5f * t));
        m.inputs[Ruina::TOUGH_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 0.13f * t));
        m.inputs[Ruina::RESET_INPUT].setVoltage(std::fmod(t, 1.7f) < 0.005f ? 5.f : 0.f);
        m.inputs[Ruina::VOCT_INPUT].setVoltage(6.f * std::sin(2.f * M_PI * 0.27f * t));
        m.params[Ruina::BRIT_PARAM].setValue(std::fmod(t * 0.31f, 1.f));
        m.params[Ruina::ENERGY_PARAM].setValue(std::fmod(t * 0.7f, 1.f));
        m.params[Ruina::HARD_PARAM].setValue(std::fmod(t * 0.9f, 1.f));
        m.params[Ruina::MAT_PARAM].setValue(std::fmod(t * 0.17f, 1.f));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Ruina::AUDIO_OUTPUT].getVoltage());
        str.add(m.outputs[Ruina::STRAIN_OUTPUT].getVoltage());
    }
    report("ruina", "stress_nans", s.nans, s.nans == 0);
    report("ruina", "stress_peak", s.peak, s.peak <= 10.f);
    report("ruina", "stress_alive", s.rms(), s.rms() > 0.02);
    report("ruina", "stress_strain_bounded", str.peak, str.peak <= 10.f && str.nans == 0);
}

SMOKE_MAIN(testFailsUnderLoad, testCubeLaw, testWeibullSpread, testStrain, testCollapse,
           testReset, testSampleRates, testStress)
