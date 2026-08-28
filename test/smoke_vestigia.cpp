// smoke_vestigia - offline sanity checks for the vestigia module.
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
    m.params[Vestigia::HARMONY_PARAM].setValue(0.f);  // in-tune (octaves)
    Stats wet;
    long eventCount = 0;
    float maxStep = 0.f, prevL = 0.f;
    dsp::SchmittTrigger evTrig;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.inputs[Vestigia::IN_L_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        float l = m.outputs[Vestigia::OUT_L_OUTPUT].getVoltage();
        wet.add(l);
        wet.add(m.outputs[Vestigia::OUT_R_OUTPUT].getVoltage());
        maxStep = std::max(maxStep, std::fabs(l - prevL));
        prevL = l;
        if (evTrig.process(m.outputs[Vestigia::EVENT_OUTPUT].getVoltage(), 0.1f, 1.f))
            eventCount++;
    }
    report("vestigia", "recall_nans", wet.nans, wet.nans == 0);
    report("vestigia", "recall_alive", wet.rms(), wet.rms() > 0.05);
    report("vestigia", "recall_bounded", wet.peak, wet.peak <= 10.01f);
    report("vestigia", "events_fired", eventCount, eventCount > 3);
    // click proxy: no single-sample jump anywhere near a full-scale snap
    report("vestigia", "no_click_steps", maxStep, maxStep < 2.5f);

    // pitch quantization: harmony=0 recalls only octaves (0.5/1/2x)
    {
        Vestigia pm;
        int off = 0, oct = 0, bad = 0;
        for (int k = 0; k < 400; k++) {
            float r = pm.pitchRatio(0.f);
            float semi = 12.f * std::log2(r);
            float nearest = std::round(semi / 12.f) * 12.f;  // nearest octave
            if (std::fabs(semi - nearest) < 0.01f) oct++;
            else bad++;
            off++;
        }
        report("vestigia", "harmony0_octaves", bad, bad == 0 && oct == off);
        int spread = 0;
        for (int k = 0; k < 400; k++) {
            float semi = 12.f * std::log2(pm.pitchRatio(1.f));
            if (std::fabs(semi - std::round(semi / 12.f) * 12.f) > 0.5f) spread++;
        }
        report("vestigia", "harmony1_spread", spread, spread > 100);
    }

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

    // 6) sediment fold saturation stays finite and bounded when driven hard
    Vestigia fld;
    long f3 = 0;
    fld.params[Vestigia::MEMMODE_PARAM].setValue(2.f);
    fld.sedimentSatIdx = 2;      // fold
    fld.sedimentAmtIdx = 3;      // 2.0x
    fld.inputs[Vestigia::IN_L_INPUT].channels = 1;
    Stats fs;
    float pf = 0.f;
    for (int i = 0; i < (int)(10 * SR); i++) {
        pf += 140.f / SR; if (pf >= 1.f) pf -= 1.f;
        fld.inputs[Vestigia::IN_L_INPUT].setVoltage(6.f * std::sin(2.f * M_PI * pf));
        fld.process(makeArgs(f3++));
        fs.add(fld.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
    }
    report("vestigia", "fold_nans", fs.nans, fs.nans == 0);
    report("vestigia", "fold_bounded", fs.peak, fs.peak <= 10.01f);

    // 7) save-memory-with-patch round-trips the buffer through JSON
    Vestigia src;
    long f4 = 0;
    src.saveMemoryWithPatch = true;
    src.inputs[Vestigia::IN_L_INPUT].channels = 1;
    float pr = 0.f;
    for (int i = 0; i < (int)(1.5f * SR); i++) {
        pr += 220.f / SR; if (pr >= 1.f) pr -= 1.f;
        src.inputs[Vestigia::IN_L_INPUT].setVoltage(4.f * std::sin(2.f * M_PI * pr));
        src.process(makeArgs(f4++));
    }
    json_t* saved = src.dataToJson();
    Vestigia dst;
    dst.dataFromJson(saved);
    json_decref(saved);
    dst.params[Vestigia::MEMORY_PARAM].setValue(0.8f);
    dst.params[Vestigia::RECALL_PARAM].setValue(0.9f);
    dst.params[Vestigia::MODE_PARAM].setValue(2.f);
    dst.params[Vestigia::MEMMODE_PARAM].setValue(1.f);
    dst.params[Vestigia::MIX_PARAM].setValue(1.f);
    dst.frozenToggle = true;   // do not overwrite the restored buffer
    Stats rs;
    for (int i = 0; i < (int)(3 * SR); i++) {
        dst.process(makeArgs(i));
        rs.add(dst.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
    }
    report("vestigia", "buffer_restore_nans", rs.nans, rs.nans == 0);
    report("vestigia", "buffer_restore_alive", rs.rms(), rs.rms() > 0.02);

    // 8) "recall listens to wet" is a feedback loop in the recall engine:
    // seed it with material, then let it self-trigger and confirm it stays
    // finite and bounded (temper timing jitter on).
    Vestigia w;
    long f5 = 0;
    w.params[Vestigia::SOURCE_PARAM].setValue(2.f);    // wet
    w.temperTiming = true;
    w.params[Vestigia::RECALL_PARAM].setValue(0.8f);
    w.params[Vestigia::TEMPER_PARAM].setValue(0.8f);
    w.params[Vestigia::FB_PARAM].setValue(1.0f);       // hot feedback
    w.params[Vestigia::MEMORY_PARAM].setValue(0.7f);
    w.params[Vestigia::MODE_PARAM].setValue(2.f);      // dream
    w.params[Vestigia::MEMMODE_PARAM].setValue(1.f);
    w.params[Vestigia::MIX_PARAM].setValue(0.8f);
    w.inputs[Vestigia::IN_L_INPUT].channels = 1;
    float pw = 0.f;
    for (int i = 0; i < (int)(2 * SR); i++) {          // seed 2 s of tone
        pw += 300.f / SR; if (pw >= 1.f) pw -= 1.f;
        w.inputs[Vestigia::IN_L_INPUT].setVoltage(4.f * std::sin(2.f * M_PI * pw));
        w.process(makeArgs(f5++));
    }
    Stats ws;
    for (int i = 0; i < (int)(20 * SR); i++) {         // then self-trigger
        w.inputs[Vestigia::IN_L_INPUT].setVoltage(0.f);
        w.process(makeArgs(f5++));
        ws.add(w.outputs[Vestigia::OUT_L_OUTPUT].getVoltage());
        ws.add(w.outputs[Vestigia::OUT_R_OUTPUT].getVoltage());
    }
    report("vestigia", "wetsense_nans", ws.nans, ws.nans == 0);
    report("vestigia", "wetsense_bounded", ws.peak, ws.peak <= 10.01f);
}

SMOKE_MAIN(testVestigia)
