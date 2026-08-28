// smoke_viginti - offline sanity checks for the viginti module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The mathematics is checked by viginti_invariants and measured by
// viginti_probe, neither of which involves Rack. These are the module's own
// checks: the volt scaling, the knob mappings, polyphony and the CV paths.

#include "smoke_harness.hpp"
#include "../src/viginti.cpp"

static void setKnobs(Viginti& m, float cutoff, float res, float drive = 0.f,
                     float level = 0.f) {
    m.params[Viginti::CUTOFF_PARAM].setValue(cutoff);
    m.params[Viginti::RES_PARAM].setValue(res);
    m.params[Viginti::DRIVE_PARAM].setValue(drive);
    m.params[Viginti::LEVEL_PARAM].setValue(level);
}

static Stats feedSine(Viginti& m, long& frame, float f, float amp,
                      double settleS, double measS) {
    m.inputs[Viginti::AUDIO_INPUT].channels = 1;
    double ph = 0.0;
    const double w = 2.0 * M_PI * f / SR;
    for (long i = 0; i < (long)(settleS * SR); i++) {
        m.inputs[Viginti::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    Stats s;
    for (long i = 0; i < (long)(measS * SR); i++) {
        m.inputs[Viginti::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
        s.add(m.outputs[Viginti::AUDIO_OUTPUT].getVoltage());
    }
    return s;
}

// Run with no input and count zero crossings: self-oscillation frequency.
static int freeRunHz(Viginti& m, long& frame, double settleS, Stats* out) {
    for (long i = 0; i < (long)(settleS * SR); i++) m.process(makeArgs(frame++));
    int zc = 0;
    float prev = 0.f;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(frame++));
        const float y = m.outputs[Viginti::AUDIO_OUTPUT].getVoltage();
        if (out) out->add(y);
        if (prev <= 0.f && y > 0.f) zc++;
        prev = y;
    }
    return zc;
}

// Nothing is normalised behind the user's back: wide open, undriven, at a
// frequency well below the cutoff, the module passes audio at unity gain (and
// inverted, as the circuit does).
static void testUnity() {
    Viginti m; long fr = 0;
    setKnobs(m, 1.f, 0.f);
    Stats s = feedSine(m, fr, 50.f, 5.f, 0.3, 0.5);
    const double gain = s.rms() / (5.0 / std::sqrt(2.0));
    report("viginti", "unity_nans", s.nans, s.nans == 0);
    report("viginti", "unity_gain", gain, gain > 0.95 && gain < 1.05);

    // and the drive knob is 24 dB either way
    Viginti d; long fd = 0;
    setKnobs(d, 1.f, 0.f, 0.5f);   // +12 dB
    Stats sd = feedSine(d, fd, 50.f, 1.f, 0.3, 0.5);
    const double gd = sd.rms() / (1.0 / std::sqrt(2.0));
    report("viginti", "drive_12db", 20.0 * std::log10(gd),
           gd > 3.7 && gd < 4.3);
}

// It is a lowpass: two poles above the cutoff is a lot of rejection.
static void testLowpass() {
    Viginti m; long fr = 0;
    setKnobs(m, 0.3f, 0.2f);       // 160 Hz
    Stats hi = feedSine(m, fr, 8000.f, 2.f, 0.3, 0.3);
    Viginti m2; long fr2 = 0;
    setKnobs(m2, 0.3f, 0.2f);
    Stats lo = feedSine(m2, fr2, 40.f, 2.f, 0.3, 0.3);
    const double ratio = hi.rms() / std::max(lo.rms(), 1e-9);
    report("viginti", "lp_rejects_8k", ratio, ratio < 0.01);
    report("viginti", "lp_passes_40", lo.rms(), lo.rms() > 1.0);
}

// At the top of the knob it must sing on its own, from silence, at the cutoff
// frequency, and bounded by the diodes. Near the bottom it must not.
static void testSelfOscillation() {
    Viginti m; long fr = 0;
    setKnobs(m, 0.5f, 1.f);        // 640 Hz
    Stats s;
    const int zc = freeRunHz(m, fr, 3.0, &s);
    report("viginti", "selfosc_nans", s.nans, s.nans == 0);
    report("viginti", "selfosc_rms", s.rms(), s.rms() > 0.5);
    report("viginti", "selfosc_bounded", s.peak, s.peak < 6.f);
    report("viginti", "selfosc_hz", zc, zc > 560 && zc < 720);

    // one octave up on the V/oct input
    Viginti v; long fv = 0;
    setKnobs(v, 0.5f, 1.f);
    v.inputs[Viginti::VOCT_INPUT].channels = 1;
    v.inputs[Viginti::VOCT_INPUT].setVoltage(1.f);
    const int zcv = freeRunHz(v, fv, 3.0, nullptr);
    report("viginti", "voct_octave", (double)zcv / zc,
           zcv > 1.9 * zc && zcv < 2.1 * zc);

    // and quiet below the threshold
    Viginti q; long fq = 0;
    setKnobs(q, 0.5f, 0.5f);
    Stats sq;
    freeRunHz(q, fq, 2.0, &sq);
    report("viginti", "quiet_below_thresh", sq.rms(), sq.rms() < 1e-3);
}

// The module-level version of the property the whole model exists for: the
// resonant peak collapses as the level rises. Same patch, same tone at the
// cutoff, 40 dB apart in level.
static void testLevelDependence() {
    const float f = 640.f;
    Viginti quiet; long fq = 0;
    setKnobs(quiet, 0.5f, 0.8f);
    Stats sq = feedSine(quiet, fq, f, 0.05f, 0.4, 0.4);
    Viginti loud; long fl = 0;
    setKnobs(loud, 0.5f, 0.8f);
    Stats sl = feedSine(loud, fl, f, 5.f, 0.4, 0.4);
    const double gq = sq.rms() / 0.05, gl = sl.rms() / 5.0;
    report("viginti", "peak_falls_with_level", gq / gl, gq / gl > 3.0);
}

// Poly: the channel count follows the input, and the channels are independent
// (two V/oct values must oscillate at two different frequencies).
static void testPoly() {
    Viginti m; long fr = 0;
    setKnobs(m, 0.5f, 1.f);
    m.outputs[Viginti::AUDIO_OUTPUT].channels = 1;   // mark connected
    m.inputs[Viginti::VOCT_INPUT].channels = 2;
    m.inputs[Viginti::VOCT_INPUT].setVoltage(0.f, 0);
    m.inputs[Viginti::VOCT_INPUT].setVoltage(2.f, 1);
    for (long i = 0; i < (long)(3.0 * SR); i++) m.process(makeArgs(fr++));
    int zc[2] = {0, 0};
    float prev[2] = {0.f, 0.f};
    long nans = 0;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(fr++));
        for (int c = 0; c < 2; c++) {
            const float y = m.outputs[Viginti::AUDIO_OUTPUT].getVoltage(c);
            if (!std::isfinite(y)) nans++;
            if (prev[c] <= 0.f && y > 0.f) zc[c]++;
            prev[c] = y;
        }
    }
    report("viginti", "poly_channels",
           m.outputs[Viginti::AUDIO_OUTPUT].getChannels(),
           m.outputs[Viginti::AUDIO_OUTPUT].getChannels() == 2);
    report("viginti", "poly_nans", nans, nans == 0);
    report("viginti", "poly_independent", (double)zc[1] / std::max(zc[0], 1),
           zc[1] > 3.6 * zc[0] && zc[1] < 4.4 * zc[0]);
}

// Hot input, resonance CV past both ends of the knob, cutoff swept at audio
// rate: nothing may go non-finite or run away.
static void testAbuse() {
    Viginti m; long fr = 0;
    setKnobs(m, 0.9f, 1.f, 1.f, 1.f);      // +24 dB in and out
    m.params[Viginti::FM_PARAM].setValue(1.f);
    m.inputs[Viginti::AUDIO_INPUT].channels = 1;
    m.inputs[Viginti::RES_CV_INPUT].channels = 1;
    m.inputs[Viginti::FM_INPUT].channels = 1;
    Stats s;
    for (long i = 0; i < (long)(4.0 * SR); i++) {
        const double t = i / (double)SR;
        m.inputs[Viginti::AUDIO_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 110.0 * t));
        m.inputs[Viginti::RES_CV_INPUT].setVoltage(
            15.f * std::sin(2.0 * M_PI * 0.7 * t));
        m.inputs[Viginti::FM_INPUT].setVoltage(
            8.f * std::sin(2.0 * M_PI * 2000.0 * t));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Viginti::AUDIO_OUTPUT].getVoltage());
    }
    report("viginti", "abuse_nans", s.nans, s.nans == 0);
    report("viginti", "abuse_bounded", s.peak, s.peak <= 10.f);
    report("viginti", "abuse_alive", s.rms(), s.rms() > 0.1);
}

// Every oversampling setting must work, and none may change the filter's
// tuning: the same tone at the same cutoff, 1x to 16x.
static void testOversampling() {
    double gains[5];
    long nans = 0;
    for (int idx = 0; idx < 5; idx++) {
        Viginti m; long fr = 0;
        m.osIndex = idx;
        setKnobs(m, 0.5f, 0.6f);
        Stats s = feedSine(m, fr, 640.f, 1.f, 0.4, 0.4);
        gains[idx] = s.rms();
        nans += s.nans;
    }
    double lo = gains[0], hi = gains[0];
    for (int i = 1; i < 5; i++) {
        lo = std::min(lo, gains[i]);
        hi = std::max(hi, gains[i]);
    }
    report("viginti", "os_nans", nans, nans == 0);
    report("viginti", "os_consistent", hi / lo, hi / lo < 1.15);
}

SMOKE_MAIN(testUnity, testLowpass, testSelfOscillation, testLevelDependence,
           testPoly, testAbuse, testOversampling)
