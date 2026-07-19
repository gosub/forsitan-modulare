// smoke_perge — offline sanity checks for the perge module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/perge.cpp"

static void testPerge() {
    Perge m;
    long frame = 0;
    m.inputs[Perge::IN_L_INPUT].channels = 1;
    m.params[Perge::MIX_PARAM].setValue(1.f);      // wet only
    m.params[Perge::TEMPO_PARAM].setValue(0.7f);   // fast-ish repeats
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    // feed 0.4 s of a 330 Hz burst at +-5 V, then silence
    float phase = 0.f;
    for (int i = 0; i < (int)(0.4f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Perge::IN_L_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.process(makeArgs(frame++));
    }
    m.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    // repeats must appear after the input stops
    Stats rep;
    for (int i = 0; i < (int)(3 * SR); i++) {
        m.process(makeArgs(frame++));
        rep.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
        rep.add(m.outputs[Perge::OUT_R_OUTPUT].getVoltage());
    }
    report("perge", "nans", rep.nans, rep.nans == 0);
    report("perge", "repeats_rms", rep.rms(), rep.rms() > 0.02);
    report("perge", "peak", rep.peak, rep.peak < 12.f);
    // low sustain: repeats must die out
    m.params[Perge::SUSTAIN_PARAM].setValue(0.05f);
    for (int i = 0; i < (int)(6 * SR); i++) m.process(makeArgs(frame++));
    Stats dead;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        dead.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "repeats_decay", dead.rms(), dead.rms() < 0.01);
    // freeze: repeats persist over 8 s with everything cranked
    m.onReset();
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    m.params[Perge::GLITCH_PARAM].setValue(1.f);    // full dimension
    m.params[Perge::LOFI_PARAM].setValue(-1.f);
    m.params[Perge::RVRB_PARAM].setValue(-1.f);
    m.params[Perge::PITCH_PARAM].setValue(0.7f);
    phase = 0.f;
    for (int i = 0; i < (int)(0.4f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Perge::IN_L_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.process(makeArgs(frame++));
    }
    m.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    // let the capture commit and the repeats get going, then freeze for 7 s
    // (the wait must outlast the envelope release, or the still-open capture
    // is what freezes and slot ages never come into play)
    for (int i = 0; i < (int)(2.0f * SR); i++) m.process(makeArgs(frame++));
    m.params[Perge::SUSTAIN_PARAM].setValue(1.f);   // freeze zone
    for (int i = 0; i < (int)(7 * SR); i++) m.process(makeArgs(frame++));
    Stats froz;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        froz.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "freeze_nans", froz.nans, froz.nans == 0);
    report("perge", "freeze_persists", froz.rms(), froz.rms() > 0.02);
    report("perge", "freeze_peak", froz.peak, froz.peak < 12.f);
    // unfreeze: the repeat train must resume decaying, not vanish
    // (skip 1.5 s so the freeze-era reverb tail doesn't mask the repeats)
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    for (int i = 0; i < (int)(1.5f * SR); i++) m.process(makeArgs(frame++));
    Stats unfr;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        unfr.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "unfreeze_repeats", unfr.rms(), unfr.rms() > 0.05);
    // capture gate: forces a capture of material below the threshold
    Perge m2;
    long f2 = 0;
    m2.inputs[Perge::IN_L_INPUT].channels = 1;
    m2.inputs[Perge::CAPTURE_GATE_INPUT].channels = 1;
    m2.params[Perge::MIX_PARAM].setValue(1.f);
    m2.params[Perge::TEMPO_PARAM].setValue(0.7f);
    m2.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    m2.params[Perge::THRESH_PARAM].setValue(1.f);   // way above the input
    phase = 0.f;
    for (int i = 0; i < (int)(0.6f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m2.inputs[Perge::IN_L_INPUT].setVoltage(0.5f * std::sin(2.f * M_PI * phase));
        bool gate = i >= (int)(0.1f * SR) && i < (int)(0.4f * SR);
        m2.inputs[Perge::CAPTURE_GATE_INPUT].setVoltage(gate ? 10.f : 0.f);
        m2.process(makeArgs(f2++));
    }
    m2.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    Stats forced;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m2.process(makeArgs(f2++));
        forced.add(m2.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "capture_gate", forced.rms(), forced.rms() > 0.005);
    // dimension: the previous captures must come back as audible layers
    // (they decay with the current train's age, not their own, or the
    // slots are inaudible by the time they are "previous")
    auto dimRun = [](float dimKnob) {
        Perge p;
        long f = 0;
        p.inputs[Perge::IN_L_INPUT].channels = 1;
        p.params[Perge::MIX_PARAM].setValue(1.f);
        p.params[Perge::TEMPO_PARAM].setValue(0.7f);
        p.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
        p.params[Perge::GLITCH_PARAM].setValue(dimKnob);
        float ph = 0.f;
        Stats s;
        for (long i = 0; i < (long)(9.f * SR); i++) {
            float t = i / SR;
            float v = 0.f;
            for (int k = 0; k < 3; k++) {   // three distinct phrases
                float t0 = 0.5f + 2.f * k;
                if (t >= t0 && t < t0 + 0.4f) {
                    ph += (200.f + 90.f * k) / SR;
                    if (ph >= 1.f) ph -= 1.f;
                    v = std::sin(2.f * (float)M_PI * ph);
                }
            }
            p.inputs[Perge::IN_L_INPUT].setVoltage(5.f * v);
            p.process(makeArgs(f++));
            if (t >= 5.5f)
                s.add(p.outputs[Perge::OUT_L_OUTPUT].getVoltage());
        }
        return s.rms();
    };
    double dimOff = dimRun(0.f);
    double dimOn = dimRun(1.f);
    report("perge", "dimension_layers", dimOn / std::max(dimOff, 1e-9),
           dimOn > 1.08 * dimOff);
    // captures own their audio: a slot must survive the 8 s ring
    // lapping the tape it was cut from
    Perge m4;
    long f4 = 0;
    m4.inputs[Perge::IN_L_INPUT].channels = 1;
    phase = 0.f;
    for (long i = 0; i < (long)(11.f * SR); i++) {
        float t = i / SR;
        // one phrase at 0.5 s, a second at 9.5 s (first tape lapped ~8.5 s)
        float v = 0.f;
        if ((t >= 0.5f && t < 0.9f) || (t >= 9.5f && t < 9.9f)) {
            phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
            v = std::sin(2.f * (float)M_PI * phase);
        }
        m4.inputs[Perge::IN_L_INPUT].setVoltage(5.f * v);
        m4.process(makeArgs(f4++));
    }
    report("perge", "slot_survives_lap", m4.slots[1].len, m4.slots[1].len > 0);
}

SMOKE_MAIN(testPerge)
