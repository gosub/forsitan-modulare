// smoke_vestigia — offline sanity checks for the vestigia module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/vestigia.cpp"

// feed a burst of tone into the module for n samples, then measure the
// wet-only output while recall runs on the stored material.
static void testVestigia() {
    Vestigia m;
    long frame = 0;
    m.params[Vestigia::MIX_PARAM].setValue(1.f);       // wet only
    m.params[Vestigia::RECALL_PARAM].setValue(0.9f);   // dense recall
    m.params[Vestigia::MEMORY_PARAM].setValue(0.8f);   // ~5 s horizon
    m.params[Vestigia::MODE_PARAM].setValue(2.f);      // dream (recall in silence)
    m.params[Vestigia::MEMMODE_PARAM].setValue(1.f);   // remanence: traces persist
    m.inputs[Vestigia::IN_L_INPUT].channels = 1;

    // 1) silence in, wet out: nothing to recall -> essentially silent
    Stats quiet;
    for (int i = 0; i < (int)(3 * SR); i++) {
        m.inputs[Vestigia::IN_L_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        quiet.add(m.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
    }
    report("vestigia", "silent_no_material", quiet.rms(), quiet.rms() < 0.05);

    // 2) record ~2 s of rhythmic 330 Hz bursts (200 ms on / 150 ms off)
    float phase = 0.f;
    for (int i = 0; i < (int)(2.5f * SR); i++) {
        int perBurst = (int)(0.35f * SR);
        bool on = (i % perBurst) < (int)(0.2f * SR);
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        float v = on ? 5.f * std::sin(2.f * M_PI * phase) : 0.f;
        m.inputs[Vestigia::IN_L_INPUT].setVoltage(v);
        m.process(makeArgs(frame++));
    }

    // 3) go quiet and let recall replay stored fragments for 4 s
    Stats wet;
    long eventCount = 0;
    dsp::SchmittTrigger evTrig;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.inputs[Vestigia::IN_L_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        wet.add(m.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
        wet.add(m.outputs[Vestigia::OUT_R_OUTPUT].getVoltage());
        if (evTrig.process(m.outputs[Vestigia::EVENT_OUTPUT].getVoltage(), 0.1f, 1.f))
            eventCount++;
    }
    report("vestigia", "recall_nans", wet.nans, wet.nans == 0);
    report("vestigia", "recall_alive", wet.rms(), wet.rms() > 0.05);
    report("vestigia", "recall_bounded", wet.peak, wet.peak <= 10.01f);
    report("vestigia", "events_fired", eventCount, eventCount > 3);

    // 4) hostile: sediment accumulation + low forget (high feedback) +
    // max age + smear, driven hard, must stay finite and bounded
    Vestigia h;
    long f2 = 0;
    h.params[Vestigia::MEMMODE_PARAM].setValue(2.f);  // sediment
    h.params[Vestigia::FORGET_PARAM].setValue(0.f);   // max feedback persistence
    h.params[Vestigia::AGE_PARAM].setValue(1.f);
    h.params[Vestigia::SMEAR_PARAM].setValue(1.f);
    h.params[Vestigia::RECALL_PARAM].setValue(1.f);
    h.params[Vestigia::MIX_PARAM].setValue(0.7f);
    h.params[Vestigia::OUTPUT_PARAM].setValue(1.f);
    h.inputs[Vestigia::IN_L_INPUT].channels = 1;
    Stats hs;
    float ph = 0.f;
    for (int i = 0; i < (int)(20 * SR); i++) {
        ph += 110.f / SR; if (ph >= 1.f) ph -= 1.f;
        h.inputs[Vestigia::IN_L_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * ph));
        h.process(makeArgs(f2++));
        hs.add(h.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
        hs.add(h.outputs[Vestigia::OUT_R_OUTPUT].getVoltage());
    }
    report("vestigia", "hostile_nans", hs.nans, hs.nans == 0);
    report("vestigia", "hostile_bounded", hs.peak, hs.peak <= 10.01f);

    // 5) freeze stops writing but recall keeps running; clear silences it
    h.frozenToggle = true;
    Stats fr;
    for (int i = 0; i < (int)(2 * SR); i++) {
        h.inputs[Vestigia::IN_L_INPUT].setVoltage(0.f);
        h.process(makeArgs(f2++));
        fr.add(h.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
    }
    report("vestigia", "frozen_recall_alive", fr.rms(), fr.rms() > 0.02);
    h.clearMemory();
    for (int i = 0; i < (int)(1 * SR); i++) h.process(makeArgs(f2++));
    Stats cl;
    for (int i = 0; i < (int)(1 * SR); i++) {
        h.process(makeArgs(f2++));
        cl.add(h.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
    }
    report("vestigia", "cleared_silent", cl.rms(), cl.rms() < 0.05);
}

SMOKE_MAIN(testVestigia)
