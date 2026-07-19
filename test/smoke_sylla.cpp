// smoke_sylla — offline sanity checks for the sylla module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/sylla.cpp"

static void testSylla() {
    Sylla m;
    long frame = 0;
    // helpers: run n seconds and measure, optionally starting fresh
    auto run = [&](float secs) {
        Stats st;
        for (long i = 0; i < (long)(secs * SR); i++) {
            m.process(makeArgs(frame++));
            st.add(m.outputs[Sylla::OUT_OUTPUT].getVoltage());
        }
        return st;
    };
    auto settle = [&](float secs) {
        for (long i = 0; i < (long)(secs * SR); i++) m.process(makeArgs(frame++));
    };
    // press GEN and wait for the worker to swap a different buffer in
    auto regen = [&]() {
        size_t was = m.buffer.size();
        m.params[Sylla::GEN_PARAM].setValue(1.f);
        settle(64.f / SR);
        m.params[Sylla::GEN_PARAM].setValue(0.f);
        long waited = 0;
        while (m.buffer.size() == was && waited < (long)(30 * SR)) {
            m.process(makeArgs(frame++));
            if (++waited % (long)SR == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return was;
    };
    auto tap = [&](float secs) {   // a PLAY button press
        m.params[Sylla::TRIG_PARAM].setValue(1.f);
        settle(secs);
        m.params[Sylla::TRIG_PARAM].setValue(0.f);
        settle(1e-3f);
    };
    auto pulse = [&](float secs) { // a trigger on the TRIG input
        m.inputs[Sylla::TRIG_INPUT].setVoltage(10.f);
        settle(secs);
        m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
        settle(1e-3f);
    };

    // default: one-shot, trigger mode, nothing patched -> silent until
    // something asks it to play
    long waited = 0;
    while (m.buffer.empty() && waited < (long)(30 * SR)) {
        m.process(makeArgs(frame++));
        if (++waited % (long)SR == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report("sylla", "render_lands_s", waited / SR, !m.buffer.empty());
    if (m.buffer.empty())
        return;
    Stats idle = run(0.5f);
    report("sylla", "default_is_silent", idle.rms(), idle.rms() < 1e-4);

    // flipping LOOP up starts the run latch, which defaults on
    m.params[Sylla::LOOP_PARAM].setValue(1.f);
    Stats s;
    int eoc = 0;
    float prevE = 0.f;
    for (long i = 0; i < (long)(9 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Sylla::OUT_OUTPUT].getVoltage());
        float e = m.outputs[Sylla::EOC_OUTPUT].getVoltage();
        if (e > 5.f && prevE <= 5.f) eoc++;
        prevE = e;
    }
    report("sylla", "nans", s.nans, s.nans == 0);
    report("sylla", "freerun_rms", s.rms(), s.rms() > 0.02);
    report("sylla", "eoc_fires", eoc, eoc >= 1);

    // loop + trigger mode: the button toggles the loop off, then on again
    tap(0.02f);
    Stats offed = run(0.5f);
    report("sylla", "loop_trig_toggles_off", offed.rms(), offed.rms() < 1e-4);
    tap(0.02f);
    Stats onned = run(0.5f);
    report("sylla", "loop_trig_toggles_on", onned.rms(), onned.rms() > 0.01);

    // GEN must never start playback: stop the loop, regenerate, stay silent
    tap(0.02f);
    settle(0.1f);
    m.params[Sylla::FAMILY_PARAM].setValue(8.f);   // micro
    size_t loopLen = regen();
    report("sylla", "gen_swaps_buffer", m.buffer.size(),
           m.buffer.size() != loopLen && m.buffer.size() < SR);
    Stats afterGen = run(0.5f);
    report("sylla", "gen_does_not_play", afterGen.rms(), afterGen.rms() < 1e-4);

    // back to a long drone loop for the transport tests
    m.params[Sylla::FAMILY_PARAM].setValue(0.f);   // drone
    regen();

    // one-shot + trigger: an input edge plays the window once, then stops
    m.params[Sylla::LOOP_PARAM].setValue(0.f);
    m.inputs[Sylla::TRIG_INPUT].channels = 1;
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    settle(0.05f);
    m.params[Sylla::LEN_PARAM].setValue(0.02f);    // short window
    pulse(1e-3f);
    Stats shot = run(0.15f);
    Stats tail = run(0.5f);
    report("sylla", "oneshot_trig_plays", shot.peak, shot.peak > 0.05f);
    report("sylla", "oneshot_trig_stops", tail.rms(), tail.rms() < 1e-4);

    // retriggering under a live signal must not step: compare the biggest
    // sample-to-sample jump across the retrigger with steady playback
    auto maxStep = [&](float secs) {
        float prev = m.outputs[Sylla::OUT_OUTPUT].getVoltage();
        float worst = 0.f;
        for (long i = 0; i < (long)(secs * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Sylla::OUT_OUTPUT].getVoltage();
            worst = std::max(worst, std::fabs(v - prev));
            prev = v;
        }
        return worst;
    };
    // a plain sine stands in for the generated sample: its own adjacent
    // samples barely move, so any step in the output is the artifact
    m.buffer.assign((size_t)(2 * SR), 0.f);
    for (size_t i = 0; i < m.buffer.size(); i++)
        m.buffer[i] = 0.5f * std::sin(2.f * M_PI * 55.f * (float)i / SR);
    m.params[Sylla::LEN_PARAM].setValue(1.f);      // full-length window
    pulse(1e-3f);
    settle(0.3f);                                  // well inside the window
    float steady = maxStep(0.3f);
    m.params[Sylla::TRIG_PARAM].setValue(1.f);     // retrigger mid-flight
    float jump = maxStep(0.05f);
    m.params[Sylla::TRIG_PARAM].setValue(0.f);
    settle(0.01f);
    report("sylla", "retrigger_step_ratio", jump / std::max(steady, 1e-6f),
           jump <= steady * 3.f);
    settle(0.5f);

    // and a fresh sample landing under a playing head must not step
    // either: the sine plays on until the render swaps in under it. It
    // rides on an offset, so cutting it is a step at any phase, while
    // the zero-mean material that lands cannot mask one.
    m.buffer.assign((size_t)(2 * SR), 0.f);
    for (size_t i = 0; i < m.buffer.size(); i++)
        m.buffer[i] = 0.4f + 0.3f * std::sin(2.f * M_PI * 55.f * (float)i / SR);
    m.params[Sylla::LOOP_PARAM].setValue(1.f);     // keep it sounding
    tap(0.02f);
    settle(0.3f);
    steady = maxStep(0.3f);
    m.params[Sylla::FAMILY_PARAM].setValue(0.f);   // drone: smooth material
    m.params[Sylla::GEN_PARAM].setValue(1.f);
    settle(64.f / SR);
    m.params[Sylla::GEN_PARAM].setValue(0.f);
    // The step is only interesting on the handful of samples where the
    // buffer actually changes hands; measuring a whole window instead
    // just picks up the new material's own liveliness and hides it.
    float swap = 0.f;
    {
        size_t was = m.buffer.size();
        float prev = m.outputs[Sylla::OUT_OUTPUT].getVoltage();
        long spun = 0, since = -1;
        while (since < 8 && spun < (long)(30 * SR)) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Sylla::OUT_OUTPUT].getVoltage();
            if (since >= 0 || m.buffer.size() != was) {
                swap = std::max(swap, std::fabs(v - prev));
                since++;
            }
            prev = v;
            if (++spun % (long)SR == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    // cutting the fixture dead would step by its offset alone, about
    // 0.4 * 0.8 * 5 = 1.6 V; a crossfaded handover moves by a few mV
    report("sylla", "regen_step_v", swap, swap < 0.5f);
    tap(0.02f);                                    // toggle the loop back off
    m.params[Sylla::LOOP_PARAM].setValue(0.f);
    settle(0.5f);

    // one-shot + gate: sounds while high, and a held gate still stops at
    // the window end rather than retriggering forever
    m.params[Sylla::GATE_PARAM].setValue(1.f);
    m.params[Sylla::LEN_PARAM].setValue(0.02f);    // short window again
    settle(0.05f);
    m.inputs[Sylla::TRIG_INPUT].setVoltage(10.f);
    Stats gateOn = run(0.15f);
    Stats gateHeldPast = run(0.5f);
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    settle(0.05f);
    report("sylla", "oneshot_gate_plays", gateOn.peak, gateOn.peak > 0.05f);
    report("sylla", "oneshot_gate_ends_window", gateHeldPast.rms(),
           gateHeldPast.rms() < 1e-4);

    // a falling gate cuts a window that is still playing
    m.params[Sylla::LEN_PARAM].setValue(1.f);      // full-length window
    m.inputs[Sylla::TRIG_INPUT].setVoltage(10.f);
    Stats cutOn = run(0.2f);
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    settle(0.05f);
    Stats cutOff = run(0.3f);
    report("sylla", "oneshot_gate_cuts", cutOn.peak, cutOn.peak > 0.05f);
    report("sylla", "oneshot_gate_released", cutOff.rms(), cutOff.rms() < 1e-4);

    // loop + gate: loops while high, silent on release
    m.params[Sylla::LOOP_PARAM].setValue(1.f);
    m.inputs[Sylla::TRIG_INPUT].setVoltage(10.f);
    Stats loopGate = run(1.0f);
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    settle(0.05f);
    Stats loopGateOff = run(0.3f);
    report("sylla", "loop_gate_holds", loopGate.rms(), loopGate.rms() > 0.01);
    report("sylla", "loop_gate_releases", loopGateOff.rms(),
           loopGateOff.rms() < 1e-4);

    // the PLAY button is a gate source of its own with nothing patched
    m.inputs[Sylla::TRIG_INPUT].channels = 0;
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    settle(0.05f);
    m.params[Sylla::TRIG_PARAM].setValue(1.f);
    Stats btnHeld = run(0.5f);
    m.params[Sylla::TRIG_PARAM].setValue(0.f);
    settle(0.05f);
    Stats btnFree = run(0.3f);
    report("sylla", "gate_button_holds", btnHeld.rms(), btnHeld.rms() > 0.01);
    report("sylla", "gate_button_releases", btnFree.rms(), btnFree.rms() < 1e-4);

    // family = random renders from the seed instead of the knob
    m.params[Sylla::GATE_PARAM].setValue(0.f);
    m.params[Sylla::FAMILY_PARAM].setValue(9.f);   // random
    size_t prevLen = regen();
    report("sylla", "random_family_renders", m.buffer.size(),
           !m.buffer.empty() && m.buffer.size() != prevLen);
}

SMOKE_MAIN(testSylla)
