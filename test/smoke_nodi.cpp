// smoke_nodi - offline sanity checks for the nodi module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by nodi_probe, which needs no Rack. These are the
// module's own checks: that the defaults are a running eight-step sequencer,
// the RAMP -> X normal, the panel switches read the right way up, polyphony,
// the groups' gates and the switch, NaN at every input, other sample rates,
// and the context menu's state surviving a save.

#include "smoke_harness.hpp"
#include "../src/nodi.cpp"

static void connect(Nodi& m, int input, float v, int channels = 1) {
    m.inputs[input].channels = channels;
    for (int c = 0; c < channels; c++) m.inputs[input].setVoltage(v, c);
}

// Rack leaves an unpatched output mono whatever setChannels asks: patch them.
static void plugOutputs(Nodi& m) {
    for (int o = 0; o < Nodi::NUM_OUTPUTS; o++) m.outputs[o].channels = 1;
}

static void rampValues(Nodi& m) {
    m.params[Nodi::RANGE_PARAM].setValue(1.f);          // 0..5 V
    for (int k = 0; k < nodi::kStages; k++)
        m.params[Nodi::VALUE1_PARAM + k].setValue(k / 8.f);
}

// Out of the box, with the f(X) sliders spread: eight steps in order, one
// gate each, once per cycle of the default 0.25 Hz ramp.
static void testDefaults() {
    Nodi m; long fr = 0;
    rampValues(m);
    std::vector<float> seq;
    int gates = 0;
    bool gatePrev = false;
    float lo = 1e9f, hi = -1e9f;
    Stats s;
    for (long i = 0; i < (long)(4.f * SR) - 100; i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Nodi::FX_OUTPUT].getVoltage();
        s.add(y);
        if (seq.empty() || std::fabs(seq.back() - y) > 1e-4f) seq.push_back(y);
        bool g = m.outputs[Nodi::GATE_OUTPUT].getVoltage() > 5.f;
        if (g && !gatePrev) gates++;
        gatePrev = g;
        float r = m.outputs[Nodi::RAMP_OUTPUT].getVoltage();
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    bool inOrder = seq.size() == 8;
    for (size_t k = 0; inOrder && k < seq.size(); k++)
        inOrder = std::fabs(seq[k] - 5.f * k / 8.f) < 1e-4f;
    report("nodi", "defaults_nans", s.nans, s.nans == 0);
    report("nodi", "defaults_steps_in_order", seq.size(), inOrder);
    report("nodi", "defaults_gates_per_cycle", gates, gates == 8);
    report("nodi", "defaults_ramp_low", lo, lo > -5.01f && lo < -4.99f);
    report("nodi", "defaults_ramp_high", hi, hi < 5.01f && hi > 4.99f);
}

// A still X patched in stops the sequence on the stage it selects; pulling
// the cable hands X back to the ramp.
static void testNormal() {
    Nodi m; long fr = 0;
    rampValues(m);
    for (int i = 0; i < 1000; i++) m.process(makeArgs(fr++));
    connect(m, Nodi::X_INPUT, -5.f);
    for (int i = 0; i < 100; i++) m.process(makeArgs(fr++));
    m.inputs[Nodi::X_INPUT].setVoltage(-0.5f);          // rises past stages 2..5
    int changes = 0;
    float prev = 0.f;
    for (int i = 0; i < (int)SR * 5; i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Nodi::FX_OUTPUT].getVoltage();
        if (i > 0 && std::fabs(y - prev) > 1e-4f) changes++;
        prev = y;
    }
    report("nodi", "x_patched_holds_stage", m.engine.ch[0].active + 1, m.engine.ch[0].active == 3);
    report("nodi", "x_patched_no_steps_after", changes, changes <= 1);
    m.inputs[Nodi::X_INPUT].channels = 0;
    changes = 0;
    for (int i = 0; i < (int)SR * 5; i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Nodi::FX_OUTPUT].getVoltage();
        if (std::fabs(y - prev) > 1e-4f) changes++;
        prev = y;
    }
    report("nodi", "x_unpatched_ramp_resumes_steps", changes, changes >= 8);
}

// The switches read the way the panel draws them: the top throw is RISE,
// A, -5..+5 V, LOOP, FAST, POSIT.
static void testSwitches() {
    Nodi m;
    m.params[Nodi::DIR1_PARAM].setValue(2.f);
    m.params[Nodi::DIR2_PARAM].setValue(1.f);
    m.params[Nodi::DIR3_PARAM].setValue(0.f);
    m.params[Nodi::GROUP1_PARAM].setValue(2.f);
    m.params[Nodi::GROUP2_PARAM].setValue(1.f);
    m.params[Nodi::GROUP3_PARAM].setValue(0.f);
    m.params[Nodi::RANGE_PARAM].setValue(2.f);
    m.params[Nodi::LOOP_PARAM].setValue(1.f);
    m.params[Nodi::FAST_PARAM].setValue(1.f);
    m.params[Nodi::MODE_PARAM].setValue(1.f);
    m.readControls();
    bool ok = m.ctl.direction[0] == nodi::RISE && m.ctl.direction[1] == nodi::OFF &&
              m.ctl.direction[2] == nodi::FALL && m.ctl.group[0] == 0 && m.ctl.group[1] == 1 &&
              m.ctl.group[2] == 2 && m.ctl.range == nodi::RANGE_BIPOLAR && !m.ctl.once &&
              m.ctl.fast && !m.ctl.length;
    report("nodi", "switches_top_throw", 0, ok);
    Nodi d;
    d.readControls();
    report("nodi", "default_stage1_fall_rest_rise", 0,
           d.ctl.direction[0] == nodi::FALL && d.ctl.direction[7] == nodi::RISE && d.ctl.length &&
           d.ctl.range == nodi::RANGE_HALF);
}

// Four channels of X at four voltages: four stages at once, one per channel.
static void testPoly() {
    Nodi m; long fr = 0;
    plugOutputs(m);
    rampValues(m);
    m.params[Nodi::MODE_PARAM].setValue(1.f);            // POSIT.
    for (int k = 0; k < nodi::kStages; k++) {
        m.params[Nodi::THRESH1_PARAM + k].setValue(k / 8.f);
        m.params[Nodi::DIR1_PARAM + k].setValue(2.f);    // RISE
    }
    connect(m, Nodi::X_INPUT, -5.5f, 4);
    for (int i = 0; i < 10; i++) m.process(makeArgs(fr++));
    const float x[4] = {-4.f, -1.f, 1.f, 4.9f};
    for (int c = 0; c < 4; c++) m.inputs[Nodi::X_INPUT].setVoltage(x[c], c);
    for (int i = 0; i < 10; i++) m.process(makeArgs(fr++));
    bool ok = m.outputs[Nodi::FX_OUTPUT].getChannels() == 4 &&
              m.outputs[Nodi::GATE_OUTPUT].getChannels() == 4;
    const int want[4] = {0, 3, 4, 7};
    for (int c = 0; c < 4; c++) {
        float y = m.outputs[Nodi::FX_OUTPUT].getVoltage(c);
        ok = ok && std::fabs(y - 5.f * want[c] / 8.f) < 1e-4f;
    }
    report("nodi", "poly_x_four_stages", m.outputs[Nodi::FX_OUTPUT].getChannels(), ok);
    // A poly +Y with a mono X: the channel count follows +Y.
    Nodi n; fr = 0;
    plugOutputs(n);
    connect(n, Nodi::Y_INPUT, 1.f, 3);
    n.process(makeArgs(fr++));
    report("nodi", "poly_y_sets_channels", n.outputs[Nodi::FX_OUTPUT].getChannels(),
           n.outputs[Nodi::FX_OUTPUT].getChannels() == 3);
}

// Each stage's gate goes to its group's output only, and COM carries the
// active stage's group input.
static void testGroups() {
    Nodi m; long fr = 0;
    m.params[Nodi::DUR_PARAM].setValue(0.3f);
    connect(m, Nodi::SW_A_INPUT, 1.f);
    connect(m, Nodi::SW_B_INPUT, 2.f);
    connect(m, Nodi::SW_C_INPUT, 3.f);
    int gates[3] = {0, 0, 0};
    bool prev[3] = {false, false, false};
    bool comOk = true;
    for (long i = 0; i < (long)(4.f * SR) - 100; i++) {
        m.process(makeArgs(fr++));
        for (int g = 0; g < 3; g++) {
            bool v = m.outputs[Nodi::GATE_A_OUTPUT + g].getVoltage() > 5.f;
            if (v && !prev[g]) gates[g]++;
            prev[g] = v;
        }
        // The outputs lag the engine by one sample; check where it is steady.
        int a = m.engine.ch[0].active;
        if (i > 10 && a >= 0 && m.engine.ch[0].sinceActivation > 2) {
            float com = m.outputs[Nodi::COM_OUTPUT].getVoltage();
            comOk = comOk && std::fabs(com - (1.f + (a % 3))) < 1e-4f;
        }
    }
    // Default groups cycle A B C A B C A B: three, three and two stages.
    report("nodi", "group_gates_a", gates[0], gates[0] == 3);
    report("nodi", "group_gates_b", gates[1], gates[1] == 3);
    report("nodi", "group_gates_c", gates[2], gates[2] == 2);
    report("nodi", "com_follows_group", 0, comOk);
}

// NaN or infinity at every input: nothing non-finite comes out, and the
// sequence is running again once the inputs are sane.
static void testNan() {
    Nodi m; long fr = 0;
    rampValues(m);
    const float bad[2] = {NAN, INFINITY};
    Stats s;
    for (int b = 0; b < 2; b++) {
        for (int in = 0; in < Nodi::NUM_INPUTS; in++) connect(m, in, bad[b]);
        for (int i = 0; i < 4800; i++) {
            m.process(makeArgs(fr++));
            for (int o = 0; o < Nodi::NUM_OUTPUTS; o++) s.add(m.outputs[o].getVoltage());
        }
    }
    report("nodi", "nan_inputs_outputs_finite", s.nans, s.nans == 0);
    for (int in = 0; in < Nodi::NUM_INPUTS; in++) m.inputs[in].channels = 0;
    int changes = 0;
    float prev = m.outputs[Nodi::FX_OUTPUT].getVoltage();
    for (int i = 0; i < (int)(4.f * SR); i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Nodi::FX_OUTPUT].getVoltage();
        if (std::fabs(y - prev) > 1e-4f) changes++;
        prev = y;
    }
    report("nodi", "nan_then_sequence_recovers", changes, changes >= 7);
}

// One cycle at other sample rates: still eight steps.
static void testSampleRates() {
    const float rates[3] = {44100.f, 96000.f, 192000.f};
    for (float sr : rates) {
        Nodi m;
        rampValues(m);
        Module::SampleRateChangeEvent e;
        e.sampleRate = sr;
        e.sampleTime = 1.f / sr;
        m.onSampleRateChange(e);
        Module::ProcessArgs a;
        a.sampleRate = sr;
        a.sampleTime = 1.f / sr;
        int steps = 0;
        float prev = -1.f;
        for (long i = 0; i < (long)(4.f * sr) - 100; i++) {
            a.frame = i;
            m.process(a);
            float y = m.outputs[Nodi::FX_OUTPUT].getVoltage();
            if (std::fabs(y - prev) > 1e-4f) steps++;
            prev = y;
        }
        char name[64];
        snprintf(name, sizeof name, "steps_per_cycle_at_%dk", (int)(sr / 1000));
        report("nodi", name, steps, steps == 8);
    }
}

static void testJson() {
    Nodi m;
    m.antiAlias = nodi::AA_ON;
    json_t* j = m.dataToJson();
    Nodi n;
    n.dataFromJson(j);
    json_decref(j);
    report("nodi", "json_anti_alias", n.antiAlias, n.antiAlias == nodi::AA_ON);
    Nodi fresh;
    json_t* empty = json_object();
    fresh.dataFromJson(empty);
    json_decref(empty);
    report("nodi", "json_missing_key_is_auto", fresh.antiAlias, fresh.antiAlias == nodi::AA_AUTO);
}

SMOKE_MAIN(testDefaults, testNormal, testSwitches, testPoly, testGroups, testNan,
           testSampleRates, testJson)
