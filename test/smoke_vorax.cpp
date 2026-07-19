// smoke_vorax — offline sanity checks for the vorax module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/vorax.cpp"

static void testVorax() {
    Vorax m;
    long frame = 0;
    // feedback fully down: the loop must stay essentially silent
    Stats quiet;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        quiet.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
    }
    report("vorax", "silent_at_no_fb", quiet.rms(), quiet.rms() < 0.01);
    // raise feedback past unity: the loop must self-excite from the
    // -90 dBFS noise seed alone
    m.params[Vorax::FEEDBACK_PARAM].setValue(6.f);
    for (int i = 0; i < (int)(6 * SR); i++) m.process(makeArgs(frame++));
    Stats s, sr_;
    for (int i = 0; i < (int)(6 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
        sr_.add(m.outputs[Vorax::RIGHT_OUTPUT].getVoltage());
    }
    report("vorax", "nans", s.nans + sr_.nans, s.nans + sr_.nans == 0);
    report("vorax", "self_osc_rms", s.rms(), s.rms() > 0.05);
    report("vorax", "peak", std::max(s.peak, sr_.peak),
           std::max(s.peak, sr_.peak) < 12.f);
    // stereo: the 4-sample body offset must decorrelate the channels
    report("vorax", "right_alive", sr_.rms(), sr_.rms() > 0.05);
    // everything hostile at once: reverb wet + max echo feedback + short
    // time + body sweep, must stay bounded (each echo repeat is clipped)
    m.params[Vorax::VERB_MIX_PARAM].setValue(1.f);
    m.params[Vorax::VERB_DECAY_PARAM].setValue(1.f);
    m.params[Vorax::ECHO_SEND_PARAM].setValue(1.f);
    m.params[Vorax::ECHO_FB_PARAM].setValue(1.5f);
    m.params[Vorax::ECHO_TIME_PARAM].setValue(0.1f);
    m.params[Vorax::FEEDBACK_PARAM].setValue(12.f);
    Stats h;
    for (int i = 0; i < (int)(10 * SR); i++) {
        m.params[Vorax::BODY_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.2f * i / SR));
        m.process(makeArgs(frame++));
        h.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
        h.add(m.outputs[Vorax::RIGHT_OUTPUT].getVoltage());
    }
    report("vorax", "hostile_nans", h.nans, h.nans == 0);
    report("vorax", "hostile_bounded", h.peak, h.peak <= 10.01f);
    report("vorax", "hostile_alive", h.rms(), h.rms() > 0.05);
    // feedback back down: the drone must die away
    m.params[Vorax::FEEDBACK_PARAM].setValue(-60.f);
    m.params[Vorax::ECHO_FB_PARAM].setValue(0.f);
    m.params[Vorax::ECHO_SEND_PARAM].setValue(0.f);
    m.params[Vorax::VERB_MIX_PARAM].setValue(0.f);
    for (int i = 0; i < (int)(10 * SR); i++) m.process(makeArgs(frame++));
    Stats dead;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        dead.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
    }
    report("vorax", "dies_at_no_fb", dead.rms(), dead.rms() < 0.01);
}

SMOKE_MAIN(testVorax)
