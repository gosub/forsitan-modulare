// smoke_rubigo - offline sanity checks for the rubigo module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by rubigo_probe, which needs no Rack. These are the
// module's own checks: that the defaults are a running kick, the panel
// switches read the right way up, the TRIGGER button, the jacks (clock, trig,
// the audio input), NaN at every input, other sample rates, the menu and the
// locked loop surviving a save, the reasoned random, and every effect and
// Mod assign target running finite.

#include "smoke_harness.hpp"
#include "../src/rubigo.cpp"

static void connect(Rubigo& m, int input, float v) {
    m.inputs[input].channels = 1;
    m.inputs[input].setVoltage(v);
}

struct Counter {
    bool prev = false;
    int n = 0;
    void add(float v) {
        bool h = v > 5.f;
        if (h && !prev) n++;
        prev = h;
    }
};

// Out of the box: a kick at TEMPO 0.3 (1.97 Hz), every step firing.
static void testDefaults() {
    Rubigo m; long fr = 0;
    Stats s;
    Counter trig, clk;
    for (long i = 0; i < (long)(4.f * SR); i++) {
        m.process(makeArgs(fr++));
        s.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
        trig.add(m.outputs[Rubigo::TRIG_OUTPUT].getVoltage());
        clk.add(m.outputs[Rubigo::CLOCK_OUTPUT].getVoltage());
    }
    report("rubigo", "defaults_nans", s.nans, s.nans == 0);
    report("rubigo", "defaults_peak_v", s.peak, s.peak > 2.f && s.peak < 10.f);
    report("rubigo", "defaults_clocks_in_4s", clk.n, clk.n >= 7 && clk.n <= 9);
    report("rubigo", "defaults_every_step_fires", trig.n, trig.n == clk.n);
}

// The panel's switches: up is SAW, HP, RUST, RUN and PITCH, as on the
// hardware; the widget numbers its throws from the bottom.
static void testSwitches() {
    Rubigo m; long fr = 0;
    m.params[Rubigo::WAVE_PARAM].setValue(1.f);
    m.params[Rubigo::FILTER_PARAM].setValue(1.f);
    m.params[Rubigo::RUST_PARAM].setValue(1.f);
    m.params[Rubigo::DEST_PARAM].setValue(0.f);
    m.process(makeArgs(fr++));
    bool up = m.ctl.wave == rubigo::SAW && m.ctl.highpass && m.ctl.rust && m.ctl.dest == rubigo::DEST_CUTOFF;
    m.params[Rubigo::DEST_PARAM].setValue(2.f);
    m.params[Rubigo::DEST_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    bool mid = m.ctl.dest == rubigo::DEST_NOISE;
    m.params[Rubigo::DEST_PARAM].setValue(2.f);
    m.process(makeArgs(fr++));
    bool top = m.ctl.dest == rubigo::DEST_PITCH;
    report("rubigo", "switches_up_is_saw_hp_rust", up, up);
    report("rubigo", "dest_switch_order", mid && top, mid && top);

    Rubigo s; fr = 0;
    s.params[Rubigo::RUN_PARAM].setValue(0.f);
    Counter clk;
    Stats a;
    for (long i = 0; i < (long)(2.f * SR); i++) {
        s.process(makeArgs(fr++));
        clk.add(s.outputs[Rubigo::CLOCK_OUTPUT].getVoltage());
        a.add(s.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
    }
    report("rubigo", "stop_no_clock", clk.n, clk.n == 0);
    report("rubigo", "stop_silent", a.peak, a.peak < 0.01f);
}

// TRIGGER fires the voice with the sequencer stopped.
static void testButton() {
    Rubigo m; long fr = 0;
    m.params[Rubigo::RUN_PARAM].setValue(0.f);
    for (int i = 0; i < 100; i++) m.process(makeArgs(fr++));
    m.params[Rubigo::TRIGGER_PARAM].setValue(1.f);
    Counter trig;
    Stats a;
    for (long i = 0; i < (long)(0.3f * SR); i++) {
        if (i == 100) m.params[Rubigo::TRIGGER_PARAM].setValue(0.f);
        m.process(makeArgs(fr++));
        trig.add(m.outputs[Rubigo::TRIG_OUTPUT].getVoltage());
        a.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
    }
    report("rubigo", "button_fires_once", trig.n, trig.n == 1);
    report("rubigo", "button_sounds", a.peak, a.peak > 1.f);
}

static void testJacks() {
    {
        // CLOCK at 10 Hz takes over from TEMPO, here in its x1 zone (the
        // default 0.3 is /2, as on the hardware).
        Rubigo m; long fr = 0;
        m.params[Rubigo::TEMPO_PARAM].setValue(0.5f);
        connect(m, Rubigo::CLOCK_INPUT, 0.f);
        Counter clk;
        for (long i = 0; i < (long)(3.f * SR); i++) {
            m.inputs[Rubigo::CLOCK_INPUT].setVoltage(i % 4800 < 240 ? 10.f : 0.f);
            m.process(makeArgs(fr++));
            if (i >= (long)SR) clk.add(m.outputs[Rubigo::CLOCK_OUTPUT].getVoltage());
        }
        report("rubigo", "clock_in_10hz_steps_in_2s", clk.n, clk.n >= 19 && clk.n <= 21);
    }
    {
        // TRIG in fires the voice without the sequencer.
        Rubigo m; long fr = 0;
        m.params[Rubigo::RUN_PARAM].setValue(0.f);
        connect(m, Rubigo::TRIG_INPUT, 0.f);
        Counter trig;
        for (long i = 0; i < (long)SR; i++) {
            // Off the first sample: a SchmittTrigger starts high.
            m.inputs[Rubigo::TRIG_INPUT].setVoltage(i % 12000 >= 1000 && i % 12000 < 1100 ? 10.f : 0.f);
            m.process(makeArgs(fr++));
            trig.add(m.outputs[Rubigo::TRIG_OUTPUT].getVoltage());
        }
        report("rubigo", "trig_in_fires", trig.n, trig.n == 4);
    }
    {
        // The input replaces the noise: NOISE at full passes a sine, a dead
        // cable at full is silent (resonance off).
        for (int dead = 0; dead < 2; dead++) {
            Rubigo m; long fr = 0;
            m.params[Rubigo::NOISE_PARAM].setValue(1.f);
            m.params[Rubigo::RES_PARAM].setValue(0.f);
            m.params[Rubigo::CUTOFF_PARAM].setValue(1.f);
            m.params[Rubigo::VOLUME_DECAY_PARAM].setValue(1.f);
            connect(m, Rubigo::AUDIO_INPUT, 0.f);
            Stats a;
            for (long i = 0; i < (long)SR; i++) {
                float x = dead ? 0.f : 5.f * std::sin(2.f * M_PI * 220.f * i / SR);
                m.inputs[Rubigo::AUDIO_INPUT].setVoltage(x);
                m.process(makeArgs(fr++));
                if (i > SR / 2) a.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
            }
            if (dead) report("rubigo", "input_dead_cable_silent", a.peak, a.peak < 0.05f);
            else report("rubigo", "input_passes_audio", a.rms(), a.rms() > 0.5f);
        }
    }
}

static void testNan() {
    Rubigo m; long fr = 0;
    for (int i = 0; i < Rubigo::INPUTS_LEN; i++) connect(m, i, NAN);
    m.params[Rubigo::RES_PARAM].setValue(1.f);
    Stats s;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(fr++));
        for (int o = 0; o < Rubigo::OUTPUTS_LEN; o++) s.add(m.outputs[o].getVoltage());
    }
    report("rubigo", "nan_inputs_outputs_finite", s.nans, s.nans == 0);
}

static void testSampleRates() {
    for (float sr : {44100.f, 96000.f, 192000.f}) {
        Rubigo m;
        m.params[Rubigo::RUST_PARAM].setValue(1.f);
        m.params[Rubigo::EFFECT_PARAM].setValue(0.7f);
        m.params[Rubigo::RES_PARAM].setValue(0.9f);
        Stats s;
        Counter clk;
        Module::ProcessArgs a;
        a.sampleRate = sr;
        a.sampleTime = 1.f / sr;
        for (long i = 0; i < (long)(2.f * sr); i++) {
            a.frame = i;
            m.process(a);
            s.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
            clk.add(m.outputs[Rubigo::CLOCK_OUTPUT].getVoltage());
        }
        char name[48];
        snprintf(name, sizeof name, "sr_%d_finite_and_clocked", (int)sr);
        report("rubigo", name, clk.n, s.nans == 0 && s.peak > 1.f && s.peak < 10.f && clk.n >= 3 && clk.n <= 5);
    }
}

// The step values of `n` clocks, read at each rising edge of CLOCK out. The
// clock rises 200 samples in, so a module whose last clock is still high
// has let it fall first.
static std::vector<float> stepValues(Rubigo& m, long& fr, int n) {
    std::vector<float> v;
    connect(m, Rubigo::CLOCK_INPUT, 0.f);
    bool prev = true;
    for (long i = 0; (int)v.size() < n; i++) {
        long ph = i % 2400;
        m.inputs[Rubigo::CLOCK_INPUT].setVoltage(ph >= 200 && ph < 300 ? 10.f : 0.f);
        m.process(makeArgs(fr++));
        bool high = m.outputs[Rubigo::CLOCK_OUTPUT].getVoltage() > 5.f;
        if (high && !prev) v.push_back(m.outputs[Rubigo::STEPMOD_OUTPUT].getVoltage());
        prev = high;
    }
    return v;
}

// The menu's state and the locked loop survive a save.
static void testJson() {
    Rubigo m; long fr = 0;
    m.effect = rubigo::FX_FLANGER;
    m.assign = rubigo::ASSIGN_VOLUME;
    m.altLengths = true;
    m.restartOnRun = true;
    m.noAutoStart = true;
    m.params[Rubigo::STEPMOD_PARAM].setValue(1.f);
    m.params[Rubigo::STEPS_PARAM].setValue(3.f);         // 7 steps in the alternative set
    stepValues(m, fr, 20);
    json_t* j = m.dataToJson();
    Rubigo n; long fn = 0;
    n.params[Rubigo::STEPMOD_PARAM].setValue(1.f);
    n.params[Rubigo::STEPS_PARAM].setValue(3.f);
    connect(n, Rubigo::CLOCK_INPUT, 0.f);                // patched when the patch loads
    n.dataFromJson(j);
    json_decref(j);
    bool menu = n.effect == rubigo::FX_FLANGER && n.assign == rubigo::ASSIGN_VOLUME && n.altLengths &&
                n.restartOnRun && n.noAutoStart;
    std::vector<float> a = stepValues(m, fr, 14), b = stepValues(n, fn, 14);
    bool same = a == b;
    if (!same)
        for (size_t k = 0; k < a.size(); k++) printf("#   %zu %g %g\n", k, a[k], b[k]);
    bool loops = true;
    for (int k = 0; k < 7; k++) loops = loops && a[k] == a[k + 7];
    report("rubigo", "json_menu_state", menu, menu);
    report("rubigo", "json_loop_of_7_repeats", loops, loops);
    report("rubigo", "json_loop_survives_reload", same, same);
}

// Ctrl-R and the menu: a playing, audible patch each time.
static void testReasoned() {
    int quiet = 0, total = 0;
    for (int a = -1; a < rubigo::ARCH_LEN; a++) {
        for (int k = 0; k < 4; k++) {
            Rubigo m; long fr = 0;
            if (a < 0) m.onRandomize(Module::RandomizeEvent());
            else m.reasonedRandom(a);
            Stats s;
            for (long i = 0; i < (long)(4.f * SR); i++) {
                m.process(makeArgs(fr++));
                s.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
            }
            total++;
            if (s.peak < 0.5f || s.nans) quiet++;
        }
    }
    report("rubigo", "reasoned_random_audible", total - quiet, quiet <= 1);
}

static void testEffects() {
    Stats s;
    for (int fx = 0; fx < rubigo::FX_LEN; fx++)
        for (int as = 0; as < rubigo::ASSIGN_LEN; as++) {
            Rubigo m; long fr = 0;
            m.effect = fx;
            m.assign = as;
            m.params[Rubigo::DEST_PARAM].setValue(0.f);      // cutoff: the assigned target
            m.params[Rubigo::STEPMOD_PARAM].setValue(1.f);
            m.params[Rubigo::EFFECT_PARAM].setValue(0.8f);
            m.params[Rubigo::TEMPO_PARAM].setValue(0.6f);
            connect(m, Rubigo::CUTOFF_INPUT, 5.f);
            for (long i = 0; i < (long)(0.5f * SR); i++) {
                m.process(makeArgs(fr++));
                s.add(m.outputs[Rubigo::AUDIO_OUTPUT].getVoltage());
            }
        }
    report("rubigo", "every_effect_and_assign_finite", s.nans, s.nans == 0 && s.peak <= 10.f);
}

SMOKE_MAIN(testDefaults, testSwitches, testButton, testJacks, testNan, testSampleRates,
           testJson, testReasoned, testEffects)
