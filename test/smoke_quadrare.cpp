// smoke_quadrare — checks for the Walsh codec.
//
// Covers the spec's section 17 list, the COEFF OUT -> COEFF IN loopback that
// the two-block pipeline exists to make exact, and the `above` switch.
//
// A note on sizes: the 16 sliders always own the lowest 16 coefficients, so at
// n=16 the window is the whole spectrum and the spec's transparency / mute /
// invert cases hold exactly. Above n=16 they hold only for the windowed part,
// so those checks run at n=16 and the rest at larger sizes.
#include "smoke_harness.hpp"
#include "../src/quadrare.cpp"

static const char* MOD = "quadrare";

// Analyze at one block boundary, reconstruct at the next.
static int latencyOf(Quadrare& m) { return 2 * m.size; }

static void setSize(Quadrare& m, int sizeIndex) {
    m.params[Quadrare::SIZE_PARAM].setValue((float) sizeIndex);
}
static void setSliders(Quadrare& m, float g) {
    for (int b = 0; b < kSliders; ++b) m.params[Quadrare::BAND0_PARAM + b].setValue(g);
}
static void setAbove(Quadrare& m, bool mute) {
    m.params[Quadrare::ABOVE_PARAM].setValue(mute ? 1.f : 0.f);
}

static void defaults(Quadrare& m) {
    setSliders(m, 1.f);
    setAbove(m, false);                 // pass everything above the window
    m.params[Quadrare::LEVEL_PARAM].setValue(1.f);
    m.params[Quadrare::KEEP_PARAM].setValue(1.f);
    m.params[Quadrare::QUANT_PARAM].setValue(0.f);
    m.params[Quadrare::DRYWET_PARAM].setValue(1.f);
    m.params[Quadrare::FREEZE_PARAM].setValue(0.f);
}

// Content across the whole spectrum.
static float testSignal(long i) {
    const float t = (float) i / SR;
    return 2.0f * std::sin(2.f * M_PI * 220.f * t)
         + 1.5f * std::sin(2.f * M_PI * 3300.f * t)
         + 1.0f * std::sin(2.f * M_PI * 11000.f * t)
         + 0.5f * (rack::random::uniform() * 2.f - 1.f);
}

// Drives the module and compares AUDIO OUT against the input delayed by the
// pipeline latency, scaled by `expect`. Returns the worst deviation.
static double runCompare(Quadrare& m, int sizeIndex, float expect, int frames,
                         double* residualPeak = nullptr) {
    setSize(m, sizeIndex);
    m.process(makeArgs(0));           // let the size change land and flush

    std::vector<float> in(frames, 0.f);
    for (int i = 0; i < frames; ++i) in[i] = testSignal(i);

    const int lat = latencyOf(m);
    double worst = 0.0, worstRes = 0.0;
    for (int i = 0; i < frames; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(in[i]);
        m.process(makeArgs(i));
        const float got = m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage();
        const float res = m.outputs[Quadrare::RESIDUAL_OUTPUT].getVoltage();
        if (!std::isfinite(got)) return 1e9;
        if (i < lat + m.size) continue;          // priming
        worst = std::max(worst, (double) std::fabs(got - in[i - lat] * expect));
        worstRes = std::max(worstRes, (double) std::fabs(res));
    }
    if (residualPeak) *residualPeak = worstRes;
    return worst;
}

// 17.1 / 17.6: the round trip is transparent and the residual nulls, at every
// size. Coefficients above the window pass through, so this holds throughout.
static void testTransparent() {
    for (int s = 0; s < kSizeCount; ++s) {
        Quadrare m;
        defaults(m);
        double res = 0.0;
        const double err = runCompare(m, s, 1.f, 16000, &res);
        report(MOD, string::f("transparent_n%d", sizeAt(s)).c_str(), err, err < 1e-3);
        report(MOD, string::f("residual_null_n%d", sizeAt(s)).c_str(), res, res < 1e-3);
    }
}

// 17.7 / 17.5 at n=16, where the 16 sliders cover the whole spectrum.
static void testGainCases() {
    {
        Quadrare m;
        defaults(m);
        setSliders(m, -1.f);
        const double err = runCompare(m, 0, -1.f, 8000);
        report(MOD, "inverted", err, err < 1e-3);
    }
    {
        Quadrare m;
        defaults(m);
        setSliders(m, 0.f);
        setSize(m, 0);
        m.process(makeArgs(0));
        const int lat = latencyOf(m);
        double wetPeak = 0.0, resErr = 0.0;
        std::vector<float> in(8000);
        for (int i = 0; i < 8000; ++i) in[i] = testSignal(i);
        for (int i = 0; i < 8000; ++i) {
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(in[i]);
            m.process(makeArgs(i));
            if (i < lat + m.size) continue;
            wetPeak = std::max(wetPeak, (double) std::fabs(
                m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
            resErr = std::max(resErr, (double) std::fabs(
                m.outputs[Quadrare::RESIDUAL_OUTPUT].getVoltage() - in[i - lat]));
        }
        report(MOD, "muted_wet", wetPeak, wetPeak < 1e-4);
        report(MOD, "muted_residual_is_dry", resErr, resErr < 1e-3);
    }
}

// The `above` switch: inert at n=16 (nothing above the window), and a lowpass
// at the window edge once the window is narrower than the spectrum.
static void testAboveSwitch() {
    {
        Quadrare m;
        defaults(m);
        setAbove(m, true);
        const double err = runCompare(m, 0, 1.f, 8000);
        report(MOD, "above_inert_at_n16", err, err < 1e-3);
    }
    {
        // n=256: the window is 0-1500 Hz. 220 Hz survives, 11 kHz should not.
        double rms[2];
        const float hz[2] = {220.f, 11000.f};
        for (int k = 0; k < 2; ++k) {
            Quadrare m;
            defaults(m);
            setAbove(m, true);
            setSize(m, 4);
            m.process(makeArgs(0));
            Stats st;
            for (int i = 0; i < 24000; ++i) {
                m.inputs[Quadrare::AUDIO_INPUT].setVoltage(
                    3.f * std::sin(2.f * M_PI * hz[k] * i / SR));
                m.process(makeArgs(i));
                if (i > 4 * m.size) st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
            }
            rms[k] = st.rms();
        }
        report(MOD, "above_mute_keeps_low", rms[0], rms[0] > 1.0);
        report(MOD, "above_mute_drops_high", rms[1], rms[1] < 0.3);
    }
}

// 17.8: the 16 COMPONENTS channels sum to the wet signal, once everything
// above the window is muted (they only carry the exposed coefficients).
static void testComponentsSum() {
    const int sizes[2] = {0, 4};
    for (int k = 0; k < 2; ++k) {
        Quadrare m;
        defaults(m);
        setAbove(m, true);
        setSize(m, sizes[k]);
        m.outputs[Quadrare::COMPONENTS_OUTPUT].channels = 1;   // mark connected
        m.process(makeArgs(0));
        double worst = 0.0;
        for (int i = 0; i < 16000; ++i) {
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
            m.process(makeArgs(i));
            if (i < 4 * m.size) continue;
            float sum = 0.f;
            for (int b = 0; b < kSliders; ++b)
                sum += m.outputs[Quadrare::COMPONENTS_OUTPUT].getVoltage(b);
            worst = std::max(worst, (double) std::fabs(
                sum - m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
        }
        report(MOD, string::f("components_sum_n%d", sizeAt(sizes[k])).c_str(),
               worst, worst < 1e-3);
    }
}

// What the two-block pipeline exists for: every COEFF OUT patched straight
// back into its COEFF IN must leave the signal untouched. The harness copies
// the ports with one frame of delay, the worst case Rack imposes on a
// feedback cable.
static void testCoeffLoopback() {
    for (int mode = 0; mode < 2; ++mode) {
        Quadrare m;
        defaults(m);
        m.coeffMode = mode;
        for (int b = 0; b < kSliders; ++b) m.inputs[Quadrare::COEFF0_INPUT + b].channels = 1;
        setSize(m, 4);
        m.process(makeArgs(0));

        const int lat = latencyOf(m);
        std::vector<float> in(16000);
        for (int i = 0; i < 16000; ++i) in[i] = testSignal(i);
        float carry[kSliders] = {};
        double worst = 0.0;
        for (int i = 0; i < 16000; ++i) {
            for (int b = 0; b < kSliders; ++b)
                m.inputs[Quadrare::COEFF0_INPUT + b].setVoltage(carry[b]);
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(in[i]);
            m.process(makeArgs(i));
            for (int b = 0; b < kSliders; ++b)
                carry[b] = m.outputs[Quadrare::COEFF0_OUTPUT + b].getVoltage();
            if (i < lat + 2 * m.size) continue;
            worst = std::max(worst, (double) std::fabs(
                m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage() - in[i - lat]));
        }
        report(MOD, mode == 0 ? "loopback_overlay" : "loopback_replace",
               worst, worst < 1e-3);
    }
}

// 17.9 / 17.10: drive coefficient 0 only. Overlay leaves the other exposed
// coefficients internal; Replace zeroes them once anything is driven.
static void testOverlayReplace() {
    for (int mode = 0; mode < 2; ++mode) {
        Quadrare m;
        defaults(m);
        m.coeffMode = mode;
        m.inputs[Quadrare::COEFF0_INPUT].channels = 1;
        setSize(m, 4);
        m.process(makeArgs(0));
        m.inputs[Quadrare::COEFF0_INPUT].setVoltage(0.f);

        for (int i = 0; i < 8000; ++i) {
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
            m.process(makeArgs(i));
        }
        float rest = 0.f;
        for (int b = 1; b < kSliders; ++b) rest = std::max(rest, std::fabs(m.recon[b]));
        float above = 0.f;
        for (int i = kSliders; i < m.size; ++i) above = std::max(above, std::fabs(m.recon[i]));

        report(MOD, mode == 0 ? "overlay_driven_coeff" : "replace_driven_coeff",
               m.recon[0], std::fabs(m.recon[0]) < 1e-6);
        if (mode == 0) report(MOD, "overlay_keeps_internal", rest, rest > 1e-6);
        else           report(MOD, "replace_zeroes_undriven", rest, rest < 1e-6);
        // Coefficients above the window follow the switch, not the mode.
        report(MOD, mode == 0 ? "overlay_above_passes" : "replace_above_passes",
               above, above > 1e-6);
    }
}

// 17.11: freeze holds the vector while the sliders stay live.
static void testFreeze() {
    Quadrare m;
    defaults(m);
    setSize(m, 2);
    m.process(makeArgs(0));
    for (int i = 0; i < 6000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
    }
    m.freeze = true;
    float snapshot[kMaxSize];
    std::copy(m.held, m.held + m.size, snapshot);

    double drift = 0.0, outPeak = 0.0;
    for (int i = 0; i < 6000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(
            4.f * std::sin(2.f * M_PI * 777.f * (float) i / SR));
        m.process(makeArgs(i));
        outPeak = std::max(outPeak, (double) std::fabs(
            m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
    }
    for (int i = 0; i < m.size; ++i)
        drift = std::max(drift, (double) std::fabs(m.held[i] - snapshot[i]));
    report(MOD, "freeze_holds_vector", drift, drift < 1e-9);
    report(MOD, "freeze_still_sounds", outPeak, outPeak > 1e-3);

    // Sliders and the above switch still carve the frozen vector.
    setSliders(m, 0.f);
    setAbove(m, true);
    for (int i = 0; i < 6 * m.size; ++i) m.process(makeArgs(i));
    double mutedPeak = 0.0;
    for (int i = 0; i < 6 * m.size; ++i) {
        m.process(makeArgs(i));
        mutedPeak = std::max(mutedPeak, (double) std::fabs(
            m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
    }
    report(MOD, "freeze_sliders_live", mutedPeak, mutedPeak < 1e-4);
}

// KEEP and QUANT act on the whole transform, not just the window.
static void testLossyStage() {
    Quadrare m;
    defaults(m);
    setSize(m, 4);
    m.process(makeArgs(0));
    m.params[Quadrare::KEEP_PARAM].setValue(0.f);      // keep 1 coefficient
    m.params[Quadrare::QUANT_PARAM].setValue(1.f);     // 2 levels
    Stats st;
    for (int i = 0; i < 16000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
        st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "lossy_finite", (double) st.nans, st.nans == 0);
    report(MOD, "lossy_bounded", st.peak, st.peak < 50.f);
    report(MOD, "lossy_audible", st.rms(), st.rms() > 1e-3);
}

// The standalone Walsh synthesizer: COEFF IN driven with no audio at all.
static void testSynthesizerMode() {
    Quadrare m;
    defaults(m);
    m.coeffMode = Quadrare::MODE_REPLACE;
    for (int b = 0; b < kSliders; ++b) m.inputs[Quadrare::COEFF0_INPUT + b].channels = 1;
    setSize(m, 2);
    m.process(makeArgs(0));
    for (int b = 0; b < kSliders; ++b)
        m.inputs[Quadrare::COEFF0_INPUT + b].setVoltage(b == 3 ? 5.f : 0.f);
    Stats st;
    for (int i = 0; i < 8000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(0.f);
        m.process(makeArgs(i));
        if (i > 4 * m.size) st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "synth_finite", (double) st.nans, st.nans == 0);
    report(MOD, "synth_sounds", st.rms(), st.rms() > 0.1);
    report(MOD, "synth_bounded", st.peak, st.peak < 20.f);
}

// Size changes and hostile COEFF IN voltages must not produce non-finite audio.
static void testStability() {
    Quadrare m;
    defaults(m);
    for (int b = 0; b < kSliders; ++b) m.inputs[Quadrare::COEFF0_INPUT + b].channels = 1;
    Stats st;
    for (int i = 0; i < 60000; ++i) {
        if (i % 2000 == 0) setSize(m, (i / 2000) % kSizeCount);
        for (int b = 0; b < kSliders; ++b)
            m.inputs[Quadrare::COEFF0_INPUT + b].setVoltage(
                rack::random::uniform() * 200.f - 100.f);
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
        st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "stability_finite", (double) st.nans, st.nans == 0);
    report(MOD, "stability_clamped", st.peak, st.peak <= 100.f);
}

SMOKE_MAIN(testTransparent, testGainCases, testAboveSwitch, testComponentsSum,
           testCoeffLoopback, testOverlayReplace, testFreeze, testLossyStage,
           testSynthesizerMode, testStability)
