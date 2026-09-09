// smoke_draen - offline sanity checks for the dræn module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// draen_sweep already measures every engine's level and DC across octaves.
// This harness is about the module around them: the lazy initialisation of
// the engines, and the fade machine that decides which one is rendered. An
// engine only allocates its delay lines in init(), so rendering one that was
// never initialised reads a null buffer. That is a segfault, not a failed
// check, so most of these checks pass by surviving.

#include "smoke_harness.hpp"
#include "../src/draen.cpp"

static const char* MOD = "draen";

static const int NENG = 37;   // engines per bank, both banks

// Both banks hold the same number of engines, and the knob spans them.
static void testBanks() {
    Draen m;
    report(MOD, "bank0_size", (double) m.banks[0].size(), (int) m.banks[0].size() == NENG);
    report(MOD, "bank1_size", (double) m.banks[1].size(), (int) m.banks[1].size() == NENG);
    float top = m.paramQuantities[Draen::ENGINE_PARAM]->maxValue;
    report(MOD, "engine_knob_range", top, top == (float) (NENG - 1));
}

// Run the module for `sec` seconds at engine `idx`, gathering both outputs.
static void run(Draen& m, long& frame, float sec, Stats& s) {
    int n = (int) (sec * SR);
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Draen::LEFT_OUTPUT].getVoltage());
        s.add(m.outputs[Draen::RIGHT_OUTPUT].getVoltage());
    }
}

// A preset as Rack applies one: the params first, then dataFromJson.
static void applyPreset(Draen& m, int bank, int engine) {
    m.params[Draen::ENGINE_PARAM].setValue((float) engine);
    json_t* root = json_object();
    json_object_set_new(root, "hzMode", json_integer(Draen::HZ_VOCT));
    json_object_set_new(root, "fadeTime", json_real(2.0));
    json_object_set_new(root, "bank", json_integer(bank));
    m.dataFromJson(root);
    json_decref(root);
}

// Every engine of both banks, selected from cold, renders finite audio in
// range. This is the lazy init working for the ordinary path.
static void testEveryEngine() {
    long nans = 0;
    bool inRange = true;
    float peak = 0.f;
    for (int bank = 0; bank < 2; bank++) {
        for (int idx = 0; idx < NENG; idx++) {
            Draen m;
            long frame = 0;
            applyPreset(m, bank, idx);
            Stats s;
            run(m, frame, 0.15f, s);
            nans += s.nans;
            peak = std::max(peak, s.peak);
            if (s.peak > 10.f + 1e-4f) inRange = false;
        }
    }
    report(MOD, "every_engine_finite", (double) nans, nans == 0);
    report(MOD, "every_engine_in_range", peak, inRange);
}

// The regression for issue #21. A preset applied to a running module can move
// the bank, and the engine at the same index in the other bank has never been
// init()ed. Rendering it dereferenced an unallocated delay line: on the
// unfixed build this check does not fail, it crashes.
static void testPresetCrossesBanks() {
    long nans = 0;
    bool landed = true;
    for (int idx = 0; idx < NENG; idx++) {
        for (int from = 0; from < 2; from++) {
            int to = 1 - from;
            Draen m;
            long frame = 0;
            // settle on `idx` of the source bank, then jump banks at the same
            // index: that is the case where nothing new was ever initialised
            applyPreset(m, from, idx);
            Stats s;
            run(m, frame, 0.1f, s);
            applyPreset(m, to, idx);
            run(m, frame, 0.1f, s);
            nans += s.nans;
            if (m.bank != to || m.activeIdx != idx) landed = false;
        }
    }
    report(MOD, "bank_cross_finite", (double) nans, nans == 0);
    report(MOD, "bank_cross_lands", 0.0, landed);
}

// The same jump, but to a different engine index as well, and with the fade
// left half-way: the switch must still only ever render an initialised engine.
static void testPresetDuringFade() {
    long nans = 0;
    Draen m;
    long frame = 0;
    applyPreset(m, 0, 0);
    Stats s;
    run(m, frame, 0.1f, s);
    for (int i = 0; i < 40; i++) {
        applyPreset(m, i % 2, (i * 11 + 3) % NENG);
        run(m, frame, 0.05f, s);   // well inside the 2 s fade
    }
    nans += s.nans;
    report(MOD, "preset_during_fade_finite", (double) nans, nans == 0);
    report(MOD, "preset_during_fade_in_range", s.peak, s.peak <= 10.f + 1e-4f);
}

// A preset fades its engine in from silence rather than cutting to it.
static void testPresetFadesIn() {
    Draen m;
    long frame = 0;
    applyPreset(m, 0, 0);
    Stats s;
    run(m, frame, 1.f, s);
    applyPreset(m, 1, 20);
    Stats first;
    run(m, frame, 0.01f, first);          // the first 10 ms after the preset
    report(MOD, "preset_fades_in", first.peak, first.peak < 0.5f);
    report(MOD, "preset_fade_phase", (double) m.fadePhase, m.fadePhase == Draen::FADE_IN);
}

// Engine selection follows the knob and the CV, and both are clamped.
static void testEngineSelect() {
    Draen m;
    m.params[Draen::ENGINE_PARAM].setValue(0.f);
    report(MOD, "select_knob_low", (double) m.selectEngine(), m.selectEngine() == 0);
    m.params[Draen::ENGINE_PARAM].setValue((float) (NENG - 1));
    report(MOD, "select_knob_high", (double) m.selectEngine(), m.selectEngine() == NENG - 1);
    m.inputs[Draen::ENGINE_CV_INPUT].setChannels(1);
    m.inputs[Draen::ENGINE_CV_INPUT].setVoltage(5.f);
    report(MOD, "select_cv_clamped", (double) m.selectEngine(), m.selectEngine() == NENG - 1);
    m.params[Draen::ENGINE_PARAM].setValue(0.f);
    m.inputs[Draen::ENGINE_CV_INPUT].setVoltage(-5.f);
    report(MOD, "select_cv_clamped_low", (double) m.selectEngine(), m.selectEngine() == 0);
}

// The saved settings survive a round trip, and a bank outside 0..1 is clamped
// rather than indexing off the end of the vector.
static void testPatchRoundTrip() {
    Draen m;
    m.hzMode = Draen::HZ_LINEAR;
    m.fadeTime = 0.75f;
    m.bankRequest = 1;
    json_t* root = m.dataToJson();
    Draen n;
    n.dataFromJson(root);
    json_decref(root);
    report(MOD, "roundtrip_hzmode", (double) n.hzMode, n.hzMode == Draen::HZ_LINEAR);
    report(MOD, "roundtrip_fadetime", n.fadeTime, n.fadeTime == 0.75f);
    report(MOD, "roundtrip_bank", (double) n.bankRequest, n.bankRequest == 1);

    Draen bad;
    json_t* j = json_object();
    json_object_set_new(j, "bank", json_integer(9));
    bad.dataFromJson(j);
    json_decref(j);
    long frame = 0;
    Stats s;
    run(bad, frame, 0.05f, s);
    report(MOD, "bank_clamped", (double) bad.bank, bad.bank >= 0 && bad.bank <= 1);
}

// onReset from a bank the module is playing must leave a rendered engine
// initialised too.
static void testReset() {
    Draen m;
    long frame = 0;
    applyPreset(m, 1, 30);
    Stats s;
    run(m, frame, 0.1f, s);
    m.onReset();
    run(m, frame, 0.1f, s);
    report(MOD, "reset_finite", (double) s.nans, s.nans == 0);
    report(MOD, "reset_to_first", (double) m.activeIdx, m.activeIdx == 0);
}

// A sample-rate change re-initialises the engine that is playing.
static void testSampleRateChange() {
    Draen m;
    long frame = 0;
    applyPreset(m, 0, 30);
    Stats s;
    run(m, frame, 0.1f, s);
    Module::ProcessArgs args;
    args.sampleRate = 96000.f;
    args.sampleTime = 1.f / 96000.f;
    for (int i = 0; i < 9600; i++) { args.frame = frame++; m.process(args); }
    report(MOD, "samplerate_change_adopted", m.curSampleRate, m.curSampleRate == 96000.f);
    float v = m.outputs[Draen::LEFT_OUTPUT].getVoltage();
    report(MOD, "samplerate_change_finite", v, std::isfinite(v));
}

SMOKE_MAIN(testBanks, testEngineSelect, testEveryEngine, testPresetCrossesBanks,
           testPresetDuringFade, testPresetFadesIn, testPatchRoundTrip,
           testReset, testSampleRateChange)
