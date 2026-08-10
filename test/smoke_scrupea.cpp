// smoke_scrupea — offline sanity checks for the scrupea module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// scrupea is a free-running chaotic bank, so the checks are about it staying
// alive and staying bounded: it must make sound with nothing patched, in all
// three topologies, at the extremes of the four macro mappings, and after a
// channel randomization it must neither go silent nor blow up.

#include "smoke_harness.hpp"
#include "../src/scrupea.cpp"

// Run the module for `seconds` and collect both channels.
static void run(Scrupea& m, long& frame, double seconds, Stats& l, Stats& r,
                Stats* cv = NULL) {
    const int n = (int)(seconds * SR);
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(frame++));
        l.add(m.outputs[Scrupea::LEFT_OUTPUT].getVoltage());
        r.add(m.outputs[Scrupea::RIGHT_OUTPUT].getVoltage());
        if (cv) cv->add(m.outputs[Scrupea::CV_OUTPUT].getVoltage());
    }
}

static void settle(Scrupea& m, long& frame, double seconds) {
    Stats a, b;
    run(m, frame, seconds, a, b);
}

static void testFreeRun() {
    Scrupea m;
    long frame = 0;
    settle(m, frame, 2.0);
    Stats l, r, cv;
    run(m, frame, 6.0, l, r, &cv);
    const long nans = l.nans + r.nans + cv.nans;
    report("scrupea", "nans", nans, nans == 0);
    report("scrupea", "self_oscillates", l.rms(), l.rms() > 0.05);
    report("scrupea", "peak_bounded", std::max(l.peak, r.peak),
           std::max(l.peak, r.peak) < 12.f);
    report("scrupea", "cv_range", cv.peak, cv.peak <= 5.01f);
    // The two channels are different mixes of the eight, so a Lissajous of
    // them should not be a straight line.
    const double diff = std::fabs(l.rms() - r.rms()) + 1e-9;
    report("scrupea", "stereo", diff, diff > 1e-6);
}

// All three topologies must run and none may go silent.
static void testTopologies() {
    static const char* label[3] = {"topo_loop", "topo_pre", "topo_bare"};
    for (int t = 0; t < 3; t++) {
        Scrupea m;
        m.params[Scrupea::MODE_PARAM].setValue((float)t);
        long frame = 0;
        settle(m, frame, 2.0);
        Stats l, r;
        run(m, frame, 4.0, l, r);
        report("scrupea", label[t], l.rms(),
               l.nans + r.nans == 0 && l.rms() > 0.02 && l.peak < 12.f);
    }
}

// The four macro knobs at both ends: the power mapping must not produce a
// non-finite target or an unbounded loop anywhere.
static void testMacroExtremes() {
    static const int knobs[4] = {Scrupea::PITCH_PARAM, Scrupea::CUTOFF_PARAM,
                                 Scrupea::DELAY_PARAM, Scrupea::FLOW_PARAM};
    static const char* label[4] = {"macro_pitch", "macro_cutoff",
                                   "macro_delay", "macro_flow"};
    for (int k = 0; k < 4; k++) {
        Stats l, r;
        for (int s = 0; s < 2; s++) {
            Scrupea m;
            m.params[knobs[k]].setValue(s ? 1.f : -1.f);
            long frame = 0;
            settle(m, frame, 1.0);
            run(m, frame, 3.0, l, r);
        }
        report("scrupea", label[k], std::max(l.peak, r.peak),
               l.nans + r.nans == 0 && std::max(l.peak, r.peak) < 12.f);
    }
}

// Randomizing the whole bank is one button press away, and the manual for
// the original warns about "unexpected bursts of noise". Bursts are fine,
// NaNs and runaway are not.
static void testRandomBanks() {
    Scrupea m;
    long frame = 0;
    Stats l, r;
    for (int i = 0; i < 12; i++) {
        m.randomizeChannels();
        run(m, frame, 1.0, l, r);
    }
    report("scrupea", "random_nans", l.nans + r.nans, l.nans + r.nans == 0);
    report("scrupea", "random_peak", std::max(l.peak, r.peak),
           std::max(l.peak, r.peak) < 12.f);
    report("scrupea", "random_alive", l.rms(), l.rms() > 0.01);
}

// With the audio input driven and every channel's own oscillator level left
// alone, the bank must still be stable: the input goes into all eight loops.
static void testAudioIn() {
    Scrupea m;
    long frame = 0;
    Stats l, r;
    const int n = (int)(4 * SR);
    float ph = 0.f;
    for (int i = 0; i < n; i++) {
        ph += 220.f / SR;
        if (ph >= 1.f) ph -= 1.f;
        m.inputs[Scrupea::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * ph));
        m.process(makeArgs(frame++));
        l.add(m.outputs[Scrupea::LEFT_OUTPUT].getVoltage());
        r.add(m.outputs[Scrupea::RIGHT_OUTPUT].getVoltage());
    }
    report("scrupea", "audio_in_nans", l.nans + r.nans, l.nans + r.nans == 0);
    report("scrupea", "audio_in_peak", std::max(l.peak, r.peak),
           std::max(l.peak, r.peak) < 12.f);
}

SMOKE_MAIN(testFreeRun, testTopologies, testMacroExtremes, testRandomBanks,
           testAudioIn)
