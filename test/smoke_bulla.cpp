// smoke_bulla - offline sanity checks for the bulla module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/bulla.cpp"

static void testBulla() {
    Bulla m;
    long frame = 0;
    for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
    Stats s, r;
    for (int i = 0; i < (int)(8 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Bulla::AUDIO_OUTPUT].getVoltage());
        r.add(m.outputs[Bulla::RUNGLER_OUTPUT].getVoltage());
    }
    report("bulla", "nans", s.nans + r.nans, s.nans + r.nans == 0);
    report("bulla", "audio_rms", s.rms(), s.rms() > 0.05);
    report("bulla", "peak", s.peak, s.peak < 12.f);
    report("bulla", "rungler_range", r.peak, r.peak <= 10.01f);
    report("bulla", "rungler_moves", r.rms(), r.rms() > 0.01);
}

SMOKE_MAIN(testBulla)
