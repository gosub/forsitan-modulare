// smoke_lustro — offline sanity checks for the lustro module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/lustro.cpp"

static void testLustro() {
    Lustro m;
    long frame = 0;
    uint32_t rng = 0x12345u;
    auto white = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((rng >> 8) * (2.f / 16777216.f) - 1.f) * 5.f;
    };
    m.inputs[Lustro::AUDIO_INPUT].channels = 1;
    m.inputs[Lustro::EXCITE_INPUT].channels = 1;
    // string at rest: wet-only output should be (near) silent
    Stats rest;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.inputs[Lustro::AUDIO_INPUT].setVoltage(white());
        m.process(makeArgs(frame++));
        rest.add(m.outputs[Lustro::AUDIO_OUTPUT].getVoltage());
    }
    // pluck, then measure the bloom
    Stats bloom;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.inputs[Lustro::EXCITE_INPUT].setVoltage(i < 100 ? 5.f : 0.f);
        m.inputs[Lustro::AUDIO_INPUT].setVoltage(white());
        m.process(makeArgs(frame++));
        bloom.add(m.outputs[Lustro::AUDIO_OUTPUT].getVoltage());
    }
    report("lustro", "nans", rest.nans + bloom.nans, rest.nans + bloom.nans == 0);
    report("lustro", "rest_rms", rest.rms(), rest.rms() < 0.05);
    report("lustro", "bloom_rms", bloom.rms(), bloom.rms() > 5.0 * (rest.rms() + 1e-4));
    report("lustro", "peak", bloom.peak, bloom.peak < 12.f);
}

SMOKE_MAIN(testLustro)
