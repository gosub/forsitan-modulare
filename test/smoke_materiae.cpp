// smoke_materiae — offline sanity checks for the materiae module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// Deeper measurement of the engine lives in materiae_probe, which needs no
// Rack at all. What is checked here is the module wrapper: that the knobs
// reach the engine, that a hit sounds and then stops, that the whole control
// space stays finite, and that the two behaviours the design turns on --
// deterministic phase reset and trigger velocity -- actually hold.

#include "smoke_harness.hpp"
#include "../src/materiae.cpp"

static const char* MOD = "materiae";

static void setDefaults(Materiae& m) {
    for (int i = 0; i < Materiae::PARAMS_LEN; i++)
        m.params[i].setValue(m.getParamQuantity(i)->getDefaultValue());
}

// Fire the trigger input and render `seconds` of output.
static Stats hit(Materiae& m, float seconds, float trigVolts = 5.f) {
    Stats s;
    long frame = 0;
    // an idle sample first: the Schmitt trigger has to see a low before an edge
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(trigVolts);
    for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    int n = (int)(seconds * SR);
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Materiae::AUDIO_OUTPUT].getVoltage());
    }
    return s;
}

// A trigger sounds, and stays inside the declared output swing.
static void checkHit() {
    Materiae m;
    setDefaults(m);
    Stats s = hit(m, 0.5f);
    report(MOD, "hit_sounds", s.rms(), s.rms() > 0.05);
    report(MOD, "hit_finite", (double)s.nans, s.nans == 0);
    report(MOD, "hit_in_range", s.peak, s.peak <= 5.01f);
}

// ...and then stops. A ringing filter behind a decayed envelope is exactly
// the thing that would leave a voice humming forever.
static void checkSilence() {
    Materiae m;
    setDefaults(m);
    m.params[Materiae::DECAY_PARAM].setValue(0.3f);
    m.params[Materiae::RESO_PARAM].setValue(1.f);      // the worst case
    hit(m, 2.f);
    Stats tail;
    long frame = 0;
    for (int i = 0; i < (int)SR; i++) {
        m.process(makeArgs(frame++));
        tail.add(m.outputs[Materiae::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "tail_silent", tail.peak, tail.peak < 1e-4f);
}

// Every operator, at both ends of the blend, produces sound and stays finite.
static void checkOperators() {
    for (int op = 0; op < kNumOps; op++) {
        Materiae m;
        setDefaults(m);
        m.params[Materiae::RELATION_PARAM].setValue((float)op);
        m.params[Materiae::BLEND_PARAM].setValue(1.f);
        m.params[Materiae::CUTOFF_PARAM].setValue(1.f);
        m.params[Materiae::DECAY_PARAM].setValue(0.6f);
        Stats s = hit(m, 0.4f);
        char name[48];
        snprintf(name, sizeof(name), "op_%s_sounds", kOpNames[op]);
        report(MOD, name, s.rms(), s.rms() > 0.01 && s.nans == 0);
    }
}

// The GRID knob is meant to be swept, including across the point where the
// logic core rate crosses the host rate.
static void checkGrid() {
    bool ok = true;
    double lo = 0, hi = 0;
    for (int i = 0; i <= 10; i++) {
        Materiae m;
        setDefaults(m);
        m.params[Materiae::GRID_PARAM].setValue(i * 0.1f);
        m.params[Materiae::CUTOFF_PARAM].setValue(1.f);
        Stats s = hit(m, 0.3f);
        if (s.nans || s.peak > 5.01f || s.rms() < 0.005) ok = false;
        if (i == 0) lo = s.rms();
        if (i == 10) hi = s.rms();
    }
    report(MOD, "grid_sweep_clean", lo + hi, ok);
}

// The whole feedback range, both destinations, every division: nothing here
// may go non-finite or push past the output swing.
static void checkCrossMod() {
    long bad = 0;
    float worst = 0.f;
    for (int dest = 0; dest < 3; dest++)
        for (int div = 0; div < kNumDiv; div++)
            for (int t = -1; t <= 1; t++) {
                Materiae m;
                setDefaults(m);
                m.params[Materiae::XMOD_PARAM].setValue(1.f);
                m.params[Materiae::TILT_PARAM].setValue((float)t);
                m.params[Materiae::DEST_PARAM].setValue((float)dest);
                m.params[Materiae::DIV_PARAM].setValue((float)div);
                m.params[Materiae::RESO_PARAM].setValue(0.9f);
                m.params[Materiae::DECAY_PARAM].setValue(0.7f);
                Stats s = hit(m, 0.4f);
                if (s.nans || s.peak > 5.01f) bad++;
                worst = std::max(worst, s.peak);
            }
    report(MOD, "xmod_stable", (double)bad, bad == 0);
    report(MOD, "xmod_peak", worst, worst <= 5.01f);
}

// DIV has to do something with the cross-modulation switched off. It used to
// live only inside the cross-modulation cells, which made it inert at the
// default XMOD of zero -- measurably so, at a spectral distance of exactly
// 0.000. This is the guard against that coming back.
static void checkDivision() {
    auto render = [](int div) {
        Materiae m;
        setDefaults(m);
        m.params[Materiae::XMOD_PARAM].setValue(0.f);      // the point
        m.params[Materiae::DIV_PARAM].setValue((float)div);
        m.params[Materiae::RELATION_PARAM].setValue(2.f);  // ring
        m.params[Materiae::CUTOFF_PARAM].setValue(1.f);
        m.params[Materiae::DECAY_PARAM].setValue(0.6f);
        std::vector<float> out;
        long frame = 0;
        m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
        for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
        m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
        for (int i = 0; i < 8000; i++) {
            m.process(makeArgs(frame++));
            out.push_back(m.outputs[Materiae::AUDIO_OUTPUT].getVoltage());
        }
        return out;
    };
    std::vector<float> one = render(0);
    double worst = 1e9;
    for (int div = 1; div < kNumDiv; div++) {
        std::vector<float> d = render(div);
        double diff = 0;
        for (size_t i = 0; i < one.size(); i++) diff += std::fabs(one[i] - d[i]);
        worst = std::min(worst, diff / one.size());
    }
    report(MOD, "div_audible_at_xmod0", worst, worst > 0.05);
}

// Phase reset is what makes a hit repeatable. Two strikes of one patch must
// come out sample-identical; with the menu's free-run they must not.
static void checkRepeatable() {
    auto render = [](bool freeRun, int strike) {
        Materiae m;
        setDefaults(m);
        m.freeRun = freeRun;
        m.params[Materiae::RELATION_PARAM].setValue(3.f);   // the latch
        m.params[Materiae::RATIO_PARAM].setValue(9.f);      // sqrt2, never repeats
        m.params[Materiae::CUTOFF_PARAM].setValue(1.f);
        // The filter and the DC blocker are deliberately not reset on a
        // trigger -- a drum voice whose body stops dead on every strike would
        // be wrong -- so the comparison only means anything once the previous
        // hit has actually finished. Short decay, no resonance, one second of
        // silence between the two strikes.
        m.params[Materiae::DECAY_PARAM].setValue(0.15f);
        m.params[Materiae::RESO_PARAM].setValue(0.f);
        std::vector<float> out;
        long frame = 0;
        for (int k = 0; k <= strike; k++) {
            m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
            m.process(makeArgs(frame++));
            m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
            for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
            m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
            out.clear();
            for (int i = 0; i < 4000; i++) {
                m.process(makeArgs(frame++));
                out.push_back(m.outputs[Materiae::AUDIO_OUTPUT].getVoltage());
            }
            for (int i = 0; i < (int)SR; i++) m.process(makeArgs(frame++));
        }
        return out;
    };
    auto diff = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0;
        for (size_t i = 0; i < a.size(); i++) d += std::fabs(a[i] - b[i]);
        return d;
    };
    double same = diff(render(false, 0), render(false, 1));
    double free_ = diff(render(true, 0), render(true, 1));
    report(MOD, "reset_repeatable", same, same < 1e-6);
    report(MOD, "freerun_differs", free_, free_ > 1.0);
}

// A 5 V trigger is full level; an attenuated one plays quieter, and turning
// the option off makes both the same. The soft trigger has to clear the
// Schmitt trigger's own 1.5 V threshold, so the usable velocity range is
// 1.5 V to 5 V, not 0 V to 5 V.
static void checkVelocity() {
    Materiae m1, m2, m3;
    setDefaults(m1); setDefaults(m2); setDefaults(m3);
    double full = hit(m1, 0.3f, 5.f).rms();
    double soft = hit(m2, 0.3f, 2.f).rms();
    m3.useVelocity = false;
    double off = hit(m3, 0.3f, 2.f).rms();
    report(MOD, "velocity_scales", soft / (full + 1e-12),
           soft > 0.0 && soft < full * 0.6);
    report(MOD, "velocity_off", off / (full + 1e-12),
           std::fabs(off - full) < full * 0.01);
}

// Env 2 leaves the module as a usable 0..10 V envelope.
static void checkEnv2() {
    Materiae m;
    setDefaults(m);
    m.params[Materiae::DECAY2_PARAM].setValue(0.6f);
    Stats s;
    long frame = 0;
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
    for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Materiae::ENV2_OUTPUT].getVoltage());
    }
    report(MOD, "env2_range", s.peak, s.peak > 9.f && s.peak <= 10.01f);
    report(MOD, "env2_finite", (double)s.nans, s.nans == 0);
}

// Menu state survives a patch save and reload.
static void checkPatchRoundTrip() {
    Materiae a;
    a.freeRun = true; a.env2Free = true; a.trackCutoff = true;
    a.freeRatio = true; a.useVelocity = false;
    a.phaseIdx = 2; a.outputLevel = 2;
    json_t* j = a.dataToJson();
    Materiae b;
    b.dataFromJson(j);
    json_decref(j);
    bool ok = b.freeRun && b.env2Free && b.trackCutoff && b.freeRatio
           && !b.useVelocity && b.phaseIdx == 2 && b.outputLevel == 2;
    report(MOD, "patch_round_trip", ok ? 1 : 0, ok);
}

SMOKE_MAIN(checkHit, checkSilence, checkOperators, checkGrid, checkCrossMod,
           checkDivision, checkRepeatable, checkVelocity, checkEnv2,
           checkPatchRoundTrip)
