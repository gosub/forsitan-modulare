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
    m.params[Sylla::FAMILY_PARAM].setValue(26.f);  // micro: a short one-shot
    size_t loopLen = regen();
    report("sylla", "gen_swaps_buffer", m.buffer.size(),
           m.buffer.size() != loopLen && m.buffer.size() < SR);
    Stats afterGen = run(0.5f);
    report("sylla", "gen_does_not_play", afterGen.rms(), afterGen.rms() < 1e-4);

    // back to a long drone loop for the transport tests
    m.params[Sylla::FAMILY_PARAM].setValue(0.f);   // drone pure
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
    m.params[Sylla::FAMILY_PARAM].setValue(0.f);   // drone pure: smooth material
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

    // the last position rolls the weighted pool instead of naming an engine
    m.params[Sylla::GATE_PARAM].setValue(0.f);
    m.params[Sylla::FAMILY_PARAM].setValue((float)imber_gen::engineCount());
    size_t prevLen = regen();
    report("sylla", "random_position_renders", m.buffer.size(),
           !m.buffer.empty() && m.buffer.size() != prevLen);
}

// The family set and the tuning are part of the save format: a patch stores
// a seed and regenerates, so a sample only reproduces under the settings
// that rendered it. These guard the compatibility rules.
static void testSyllaState() {
    {
        Sylla m;
        SwitchQuantity* q = dynamic_cast<SwitchQuantity*>(
            m.paramQuantities[Sylla::FAMILY_PARAM]);
        bool v2 = m.familySet == 1
                  && m.rootNote == imber_dsp::kDefaultRoot
                  && m.scaleIndex == imber_dsp::kDefaultScale
                  && q && q->labels[10] == "air vowel"
                  && q->maxValue == (float)imber_gen::engineCount();
        report("sylla", "fresh_defaults_v2", m.familySet, v2);
    }
    {
        // a 2.9-era patch carries no familySet key and must land on v1
        Sylla m;
        json_t* j = json_object();
        json_object_set_new(j, "sampleSeed", json_integer(12345));
        m.dataFromJson(j);
        json_decref(j);
        SwitchQuantity* q = dynamic_cast<SwitchQuantity*>(
            m.paramQuantities[Sylla::FAMILY_PARAM]);
        report("sylla", "legacy_patch_falls_back_v1", m.familySet,
               m.familySet == 0 && q && q->labels[2] == "Fragment"
               && q->maxValue == (float)imber_gen::FAM_COUNT);
    }
    {
        Sylla a;
        a.familySet = 0; a.rootNote = 7; a.scaleIndex = 3; a.sampleSeed = 999;
        json_t* j = a.dataToJson();
        Sylla b;
        b.dataFromJson(j);
        json_decref(j);
        bool ok = b.familySet == 0 && b.rootNote == 7 && b.scaleIndex == 3
                  && b.sampleSeed == 999;
        report("sylla", "state_round_trips", b.rootNote, ok);
    }
    // same seed and settings reproduce bit-exactly; a new root does not
    long frame = 0;
    auto renderWith = [&](int root, int set) {
        Sylla m;
        m.rootNote = root;
        m.familySet = set;
        m.params[Sylla::FAMILY_PARAM].setValue(2.f);
        m.sampleSeed = 0xFEEDull;
        for (int i = 0; i < 600 && m.buffer.empty(); i++) {
            for (int k = 0; k < 64; k++) m.process(makeArgs(frame++));
            if (m.buffer.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return m.buffer;
    };
    std::vector<float> d1 = renderWith(imber_dsp::kDefaultRoot, 1);
    std::vector<float> d2 = renderWith(imber_dsp::kDefaultRoot, 1);
    std::vector<float> up = renderWith(9, 1);
    std::vector<float> v1 = renderWith(imber_dsp::kDefaultRoot, 0);
    report("sylla", "seed_reproduces", d1.size(), !d1.empty() && d1 == d2);
    report("sylla", "root_changes_render", up.size(),
           !up.empty() && up != d1);
    // knob position 2 names one engine in v2 (drone FM) and a whole family
    // in v1 (fragment), so the same seed cannot land on the same material
    report("sylla", "generator_set_changes_render", v1.size(),
           !v1.empty() && v1 != d1);

    // The point of addressing engines rather than families: a knob position
    // must keep producing the same engine whatever the seed. The old pool
    // rolled a member per render, so position 0 could hand back anything
    // from a pure stack to comb-fed noise.
    long f2 = 0;
    auto renderAt = [&](int pos, uint64_t seed) {
        Sylla m;
        m.params[Sylla::FAMILY_PARAM].setValue((float)pos);
        m.sampleSeed = seed;
        for (int i = 0; i < 600 && m.buffer.empty(); i++) {
            for (int k = 0; k < 64; k++) m.process(makeArgs(f2++));
            if (m.buffer.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return m.buffer;
    };
    // micro is the one engine whose buffers are milliseconds rather than
    // seconds, so length alone identifies it across seeds
    int micro = imber_gen::engineCount() - 1;
    bool stable = true;
    for (uint64_t s = 1; s <= 4 && stable; s++)
        stable = renderAt(micro, 0x2000ull * s).size() < (size_t)(0.5f * SR);
    report("sylla", "position_holds_its_engine", stable ? 1 : 0, stable);
    // and a neighbouring position is a different engine entirely
    bool neighbour = renderAt(micro - 1, 0x2000ull).size() > (size_t)SR;
    report("sylla", "neighbour_is_another_engine", neighbour ? 1 : 0, neighbour);
}

// The random pool masks engines out of the last knob position. v1's random
// derives its family from the seed and must ignore it entirely.
static void testSyllaPool() {
    const float SR_ = SR;
    // identify which engine a roll produced: renderLoop2 spends one uniform()
    // on the pick before the generator runs, so that draw has to be replayed
    auto identify = [&](uint64_t seed, const std::vector<float>& got) {
        int n;
        const imber_gen::EngineEntry* t = imber_gen::engineTable(&n);
        for (int e = 0; e < n; e++) {
            imber_dsp::Rng rng;
            rng.seed(seed);
            rng.uniform();
            std::vector<float> b;
            t[e].fn(rng, SR_, b);
            if (!t[e].finished) imber_gen::finishLoop(b, rng, SR_);
            if (b == got) return e;
        }
        return -1;
    };
    // a pool of one always lands on that one
    bool pinned = true;
    for (int k = 0; k < 12 && pinned; k++) {
        uint64_t seed = 0x99ull * (k + 3);
        imber_dsp::Rng rng;
        rng.seed(seed);
        std::vector<float> b;
        imber_gen::renderLoop2(rng, SR_, b, 1u << 10);
        pinned = identify(seed, b) == 10;
    }
    report("sylla", "pool_of_one_pins_engine", pinned ? 1 : 0, pinned);
    // the self-finishing one-shots are never rolled, whatever the mask
    bool noOneShot = true;
    for (int k = 0; k < 24 && noOneShot; k++) {
        uint64_t seed = 0x5Aull * (k + 1);
        imber_dsp::Rng rng;
        rng.seed(seed);
        std::vector<float> b;
        imber_gen::renderLoop2(rng, SR_, b, imber_gen::kAllEngines);
        int e = identify(seed, b);
        int n;
        const imber_gen::EngineEntry* t = imber_gen::engineTable(&n);
        if (e >= 0 && t[e].weight <= 0.f) noOneShot = false;
    }
    report("sylla", "pool_excludes_one_shots", noOneShot ? 1 : 0, noOneShot);
    // an empty pool falls back to everything rather than rendering silence
    bool okEmpty = true;
    for (int k = 0; k < 6 && okEmpty; k++) {
        imber_dsp::Rng rng;
        rng.seed(0xE0ull * (k + 1));
        std::vector<float> b;
        imber_gen::renderLoop2(rng, SR_, b, 0u);
        okEmpty = !b.empty();
    }
    report("sylla", "empty_pool_falls_back", okEmpty ? 1 : 0, okEmpty);
    // the default argument is exactly the all-enabled mask
    bool same = true;
    for (int k = 0; k < 8 && same; k++) {
        uint64_t seed = 0x1234ull * (k + 1);
        imber_dsp::Rng a, b2;
        a.seed(seed);
        b2.seed(seed);
        std::vector<float> x, y;
        imber_gen::renderLoop2(a, SR_, x);
        imber_gen::renderLoop2(b2, SR_, y, imber_gen::kAllEngines);
        same = x == y;
    }
    report("sylla", "pool_default_is_all", same ? 1 : 0, same);
}

// Switching selections changes the detent count under a value that is
// already set, so the knob must stay in range and both sides keep working.
static void testSyllaSwitch() {
    Sylla m;
    long frame = 0;
    int nEng = imber_gen::engineCount();
    auto maxv = [&]() {
        return m.paramQuantities[Sylla::FAMILY_PARAM]->maxValue;
    };
    auto nlab = [&]() {
        return dynamic_cast<SwitchQuantity*>(
            m.paramQuantities[Sylla::FAMILY_PARAM])->labels.size();
    };
    // park on the last engine position, then drop to the narrower knob
    m.params[Sylla::FAMILY_PARAM].setValue((float)nEng);
    m.familySet = 0;
    m.applyFamilyLabels();
    bool narrowed = maxv() == (float)imber_gen::FAM_COUNT && nlab() == 10
                    && m.params[Sylla::FAMILY_PARAM].getValue() <= maxv();
    report("sylla", "switch_to_v1_narrows_knob", narrowed ? 1 : 0, narrowed);
    m.familySet = 1;
    m.applyFamilyLabels();
    bool widened = maxv() == (float)nEng && nlab() == (size_t)nEng + 1;
    report("sylla", "switch_to_v2_widens_knob", widened ? 1 : 0, widened);
    // hammer it: the value must never fall outside the knob in force
    bool stable = true;
    for (int i = 0; i < 200 && stable; i++) {
        m.familySet = i & 1;
        m.applyFamilyLabels();
        float v = m.params[Sylla::FAMILY_PARAM].getValue();
        stable = v >= 0.f && v <= maxv()
                 && nlab() == (m.familySet ? (size_t)nEng + 1 : (size_t)10);
    }
    report("sylla", "switch_repeatedly_consistent", stable ? 1 : 0, stable);
    (void)frame;
}

// A sample renders in 0.01 to 15 ms, inside a single 60 Hz video frame, so a
// busy light tied straight to the job is sampled back at zero and most GENs
// show nothing at all. It has to stay lit for longer than a frame.
static void testSyllaBusyLight() {
    Sylla m;
    long frame = 0;
    auto step = [&]() { m.process(makeArgs(frame++)); };
    auto settle = [&](float secs) {
        for (long i = 0; i < (long)(secs * SR); i++) step();
    };
    auto lightOn = [&]() {
        return m.lights[Sylla::BUSY_LIGHT].getBrightness() > 0.5f;
    };
    // The harness runs audio far faster than wall time, so a worker that
    // takes 11 ms of wall time would appear to take seconds of audio time.
    // Idle means: the render landed and no audio is being clocked past it.
    auto waitIdle = [&]() {
        for (int i = 0; i < 3000 && (m.job || m.buffer.empty()); i++) {
            settle(64.f / SR);
            if (m.job)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    waitIdle();
    settle(1.f);
    report("sylla", "busy_light_rests_off", lightOn() ? 1 : 0, !lightOn());

    // Press GEN, let the worker finish while the audio clock stands still,
    // then measure. The render costs almost no audio time that way, which is
    // the worst case for a light that is only on while the job is in flight
    // — and the honest model of a real Rack, where 11 ms of render passes
    // under a UI that only looks every 16.7 ms.
    auto measure = [&](bool viaInput) {
        if (viaInput) m.inputs[Sylla::GEN_INPUT].setVoltage(10.f);
        else          m.params[Sylla::GEN_PARAM].setValue(1.f);
        step();
        if (viaInput) m.inputs[Sylla::GEN_INPUT].setVoltage(0.f);
        else          m.params[Sylla::GEN_PARAM].setValue(0.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        long on = 0;
        for (long i = 0; i < (long)(2.f * SR); i++) {
            step();
            if (lightOn())
                on++;
            else if (on > 0)
                break;
        }
        waitIdle();
        settle(0.5f);
        return on / SR;
    };
    double byButton = measure(false);
    double byInput  = measure(true);
    // one frame at 60 Hz is 16.7 ms; ask for six of them
    report("sylla", "busy_light_button_s", byButton, byButton >= 0.1);
    report("sylla", "busy_light_input_s", byInput, byInput >= 0.1);

    (void)frame;
}

SMOKE_MAIN(testSylla, testSyllaState, testSyllaPool, testSyllaSwitch,
           testSyllaBusyLight)
