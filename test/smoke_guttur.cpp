// smoke_guttur — offline sanity checks for the guttur module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/guttur.cpp"

static void testGuttur() {
    Guttur m;
    long frame = 0;
    // defaults (drive .2, omega 2e-4, both banks on preset 6, level 1.4):
    // the chaos must self-start from the ignition kick, then breathe with
    // the ~24 s forcing cycle (surges near t=1 s and t=12 s, quiet between)
    // — so measure across one full cycle from the start
    Stats s, d;
    for (int i = 0; i < (int)(24 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        d.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
    }
    report("guttur", "nans", s.nans + d.nans, s.nans + d.nans == 0);
    report("guttur", "self_start_rms", s.rms(), s.rms() > 0.01);
    report("guttur", "peak", s.peak, s.peak <= 10.01f);
    report("guttur", "duff_alive", d.rms(), d.rms() > 0.01);
    report("guttur", "duff_bounded", d.peak, d.peak <= 10.01f);
    // reset trigger: state must clear and the sound re-ignite, no NaNs
    m.inputs[Guttur::RESET_INPUT].channels = 1;
    m.inputs[Guttur::RESET_INPUT].setVoltage(10.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.inputs[Guttur::RESET_INPUT].setVoltage(0.f);
    Stats r;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.process(makeArgs(frame++));
        r.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
    }
    report("guttur", "reset_reignites", r.rms(), r.nans == 0 && r.rms() > 0.01);
    // hostile: everything cranked + spread + fastest glide preset sweeps
    m.params[Guttur::DRIVE_PARAM].setValue(10.f);
    m.params[Guttur::RATE_PARAM].setValue(5.f);
    m.params[Guttur::Q_PARAM].setValue(1.f);
    m.params[Guttur::LEVEL_PARAM].setValue(3.5f);
    m.params[Guttur::SPREAD_PARAM].setValue(1.f);
    m.params[Guttur::GAINA_PARAM].setValue(2.f);
    m.params[Guttur::GAINB_PARAM].setValue(2.f);
    Stats h;
    for (int i = 0; i < (int)(10 * SR); i++) {
        if (i % (int)SR == 0) {
            // stomp through the factory banks while everything is hot
            m.params[Guttur::BANKA_PARAM].setValue(1.f + (i / (int)SR) * 2.f);
            m.params[Guttur::BANKB_PARAM].setValue(20.f - (i / (int)SR) * 2.f);
        }
        m.process(makeArgs(frame++));
        h.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        h.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
    }
    report("guttur", "hostile_nans", h.nans, h.nans == 0);
    report("guttur", "hostile_bounded", h.peak, h.peak <= 10.01f);
    // raw-Duffing mode ("snazzy clicks"): bounded by the unit clip, alive
    m.params[Guttur::FILT_PARAM].setValue(0.f);
    m.params[Guttur::LEVEL_PARAM].setValue(1.4f);
    for (int i = 0; i < (int)(1 * SR); i++) m.process(makeArgs(frame++));
    Stats raw;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.process(makeArgs(frame++));
        raw.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
    }
    report("guttur", "raw_nans", raw.nans, raw.nans == 0);
    report("guttur", "raw_alive", raw.rms(), raw.rms() > 0.01);
    report("guttur", "raw_bounded", raw.peak, raw.peak <= 10.01f);
    // back to filters + distortion sweep at runtime: click-free-ish is by
    // ear, but at least every type must stay finite
    m.params[Guttur::FILT_PARAM].setValue(1.f);
    Stats dist;
    for (int type = 0; type <= 4; type++) {
        m.params[Guttur::DIST_PARAM].setValue((float)type);
        for (int i = 0; i < (int)(1 * SR); i++) {
            m.process(makeArgs(frame++));
            dist.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        }
    }
    report("guttur", "dist_sweep_nans", dist.nans, dist.nans == 0);
    report("guttur", "dist_sweep_bounded", dist.peak, dist.peak <= 10.01f);
}

SMOKE_MAIN(testGuttur)
