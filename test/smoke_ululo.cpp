// smoke_ululo - offline sanity checks for the ululo module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/ululo.cpp"

static void testUlulo() {
    Ululo m;
    long frame = 0;
    m.params[Ululo::GAIN_PARAM].setValue(1.5f);   // well into howling range
    for (int i = 0; i < (int)(4 * SR); i++) m.process(makeArgs(frame++));
    Stats s;
    for (int i = 0; i < (int)(6 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "nans", s.nans, s.nans == 0);
    report("ululo", "howl_rms", s.rms(), s.rms() > 0.05);
    report("ululo", "peak", s.peak, s.peak < 6.f);   // tanh-bounded * 5V
    // sweep the dist/decay/tone CVs over their full range while howling
    m.inputs[Ululo::DIST_CV_INPUT].channels = 1;
    m.inputs[Ululo::DECAY_CV_INPUT].channels = 1;
    m.inputs[Ululo::TONE_CV_INPUT].channels = 1;
    Stats sc;
    for (int i = 0; i < (int)(4 * SR); i++) {
        float ph = (float)i / SR;
        m.inputs[Ululo::DIST_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 0.5f * ph));
        m.inputs[Ululo::DECAY_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 0.7f * ph));
        m.inputs[Ululo::TONE_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 1.3f * ph));
        m.process(makeArgs(frame++));
        sc.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "cv_sweep_nans", sc.nans, sc.nans == 0);
    report("ululo", "cv_sweep_peak", sc.peak, sc.peak < 6.f);
    m.inputs[Ululo::DIST_CV_INPUT].channels = 0;
    m.inputs[Ululo::DECAY_CV_INPUT].channels = 0;
    m.inputs[Ululo::TONE_CV_INPUT].channels = 0;
    // gain at zero must ring down to silence (decay knob still allows several
    // seconds of legitimate string ring-out)
    m.params[Ululo::GAIN_PARAM].setValue(0.f);
    m.params[Ululo::DECAY_PARAM].setValue(0.5f);
    for (int i = 0; i < (int)(8 * SR); i++) m.process(makeArgs(frame++));
    Stats s3;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        s3.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "silent_at_zero_gain", s3.rms(), s3.rms() < 0.01);
}

SMOKE_MAIN(testUlulo)
