// smoke_quadrare — checks for the Walsh codec.
//
// Covers the spec's section 17 test list: round-trip transparency, the
// unity/zero/inverted gain cases, residual nullity, the components sum, the
// COEFF IN overlay and replace modes, freeze, and the COEFF OUT -> COEFF IN
// loopback that the two-block pipeline exists to make exact.
#include "smoke_harness.hpp"
#include "../src/quadrare.cpp"

static const char* MOD = "quadrare";

// The module's own latency: analyze at one block boundary, reconstruct at the
// next, so a sample arrives at the output 2*size frames later.
static int latencyOf(Quadrare& m) { return 2 * m.size; }

static void setSize(Quadrare& m, int sizeIndex) {
    m.params[Quadrare::SIZE_PARAM].setValue((float) sizeIndex);
}

static void setBands(Quadrare& m, float g) {
    for (int b = 0; b < kBands; ++b) m.params[Quadrare::BAND_PARAM + b].setValue(g);
}

static void defaults(Quadrare& m) {
    setBands(m, 1.f);
    m.params[Quadrare::LEVEL_PARAM].setValue(1.f);
    m.params[Quadrare::KEEP_PARAM].setValue(1.f);
    m.params[Quadrare::QUANT_PARAM].setValue(0.f);
    m.params[Quadrare::DRYWET_PARAM].setValue(1.f);
    m.params[Quadrare::FREEZE_PARAM].setValue(0.f);
}

// A signal with content in every band: a few tones plus noise.
static float testSignal(long i) {
    const float t = (float) i / SR;
    return 2.0f * std::sin(2.f * M_PI * 220.f * t)
         + 1.5f * std::sin(2.f * M_PI * 3300.f * t)
         + 1.0f * std::sin(2.f * M_PI * 11000.f * t)
         + 0.5f * (rack::random::uniform() * 2.f - 1.f);
}

// Drives the module for `frames` samples and compares AUDIO OUT against the
// input delayed by the pipeline latency and scaled by `expect`.
// Returns the largest absolute deviation.
static double runCompare(Quadrare& m, int sizeIndex, float expect, int frames,
                         double* residualPeak = nullptr) {
    defaults(m);
    setSize(m, sizeIndex);
    m.process(makeArgs(0));   // let the size change land and flush

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
        // Skip the priming interval, where the pipeline is still filling.
        if (i < lat + m.size) continue;
        const float want = in[i - lat] * expect;
        worst = std::max(worst, (double) std::fabs(got - want));
        worstRes = std::max(worstRes, (double) std::fabs(res));
    }
    if (residualPeak) *residualPeak = worstRes;
    return worst;
}

// 17.1 / 17.6 — unity gains reproduce the delayed input, and the residual is
// null. Checked at every size.
static void testTransparent() {
    for (int s = 0; s < kSizeCount; ++s) {
        Quadrare m;
        double res = 0.0;
        const double err = runCompare(m, s, 1.f, 12000, &res);
        report(MOD, string::f("transparent_n%d", sizeAt(s)).c_str(), err, err < 1e-3);
        report(MOD, string::f("residual_null_n%d", sizeAt(s)).c_str(), res, res < 1e-3);
    }
}

// 17.7 — the transform is linear, so inverting every coefficient inverts the
// signal. 17.5 — zero gains mute the wet path entirely.
static void testGainCases() {
    {
        Quadrare m;
        defaults(m);
        setSize(m, 2);
        m.process(makeArgs(0));
        setBands(m, -1.f);
        const int lat = latencyOf(m);
        double worst = 0.0;
        std::vector<float> in(8000);
        for (int i = 0; i < 8000; ++i) in[i] = testSignal(i);
        for (int i = 0; i < 8000; ++i) {
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(in[i]);
            m.process(makeArgs(i));
            if (i < lat + m.size) continue;
            worst = std::max(worst, (double) std::fabs(
                m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage() + in[i - lat]));
        }
        report(MOD, "inverted", worst, worst < 1e-3);
    }
    {
        Quadrare m;
        defaults(m);
        setSize(m, 2);
        m.process(makeArgs(0));
        setBands(m, 0.f);
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
            // With the wet muted the residual is the delayed dry.
            resErr = std::max(resErr, (double) std::fabs(
                m.outputs[Quadrare::RESIDUAL_OUTPUT].getVoltage() - in[i - lat]));
        }
        report(MOD, "muted_wet", wetPeak, wetPeak < 1e-4);
        report(MOD, "muted_residual_is_dry", resErr, resErr < 1e-3);
    }
}

// 17.8 — the 16 COMPONENTS channels sum to the wet signal.
static void testComponentsSum() {
    Quadrare m;
    defaults(m);
    setSize(m, 2);
    m.outputs[Quadrare::COMPONENTS_OUTPUT].channels = 1;   // mark as connected
    m.process(makeArgs(0));
    double worst = 0.0;
    for (int i = 0; i < 8000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
        if (i < 4 * m.size) continue;
        float sum = 0.f;
        for (int b = 0; b < kBands; ++b)
            sum += m.outputs[Quadrare::COMPONENTS_OUTPUT].getVoltage(b);
        worst = std::max(worst, (double) std::fabs(
            sum - m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
    }
    report(MOD, "components_sum", worst, worst < 1e-3);
}

// The property the two-block pipeline exists for: patching every COEFF OUT
// straight back into its COEFF IN must leave the signal untouched. The
// harness copies the ports with one frame of delay, the worst case Rack can
// impose on a feedback cable.
static void testCoeffLoopback() {
    for (int mode = 0; mode < 2; ++mode) {
        Quadrare m;
        defaults(m);
        setSize(m, 2);
        m.coeffMode = mode;
        for (int b = 0; b < kBands; ++b) m.inputs[Quadrare::COEFF_INPUT + b].channels = 1;
        m.process(makeArgs(0));

        const int lat = latencyOf(m);
        const int w = bandWidth(m.size);
        std::vector<float> in(8000);
        for (int i = 0; i < 8000; ++i) in[i] = testSignal(i);
        float carry[kBands][16] = {};
        double worst = 0.0;
        for (int i = 0; i < 8000; ++i) {
            // Deliver last frame's COEFF OUT to COEFF IN.
            for (int b = 0; b < kBands; ++b) {
                m.inputs[Quadrare::COEFF_INPUT + b].setChannels(w);
                for (int c = 0; c < w; ++c)
                    m.inputs[Quadrare::COEFF_INPUT + b].setVoltage(carry[b][c], c);
            }
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(in[i]);
            m.process(makeArgs(i));
            for (int b = 0; b < kBands; ++b)
                for (int c = 0; c < w; ++c)
                    carry[b][c] = m.outputs[Quadrare::COEFF_OUTPUT + b].getVoltage(c);
            if (i < lat + 2 * m.size) continue;
            worst = std::max(worst, (double) std::fabs(
                m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage() - in[i - lat]));
        }
        report(MOD, mode == 0 ? "loopback_overlay" : "loopback_replace",
               worst, worst < 1e-3);
    }
}

// 17.9 / 17.10 — a partially filled COEFF IN. Band 0 is driven from outside;
// in Overlay the untouched bands stay internal, in Replace the unfed channels
// of a *connected* jack go to zero.
static void testOverlayReplace() {
    for (int mode = 0; mode < 2; ++mode) {
        Quadrare m;
        defaults(m);
        setSize(m, 3);          // n = 128, so 8 channels per jack
        m.coeffMode = mode;
        m.inputs[Quadrare::COEFF_INPUT + 0].channels = 1;
        m.process(makeArgs(0));
        const int w = bandWidth(m.size);
        // Feed one channel of band 0 only.
        m.inputs[Quadrare::COEFF_INPUT + 0].setChannels(1);
        m.inputs[Quadrare::COEFF_INPUT + 0].setVoltage(0.f, 0);

        for (int i = 0; i < 6000; ++i) {
            m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
            m.process(makeArgs(i));
        }
        // Band 0 bin 0 was forced to zero in both modes.
        const float bin0 = m.recon[0];
        // Bins 1..w-1 of band 0: internal under Overlay, zeroed under Replace.
        float rest = 0.f;
        for (int c = 1; c < w; ++c) rest = std::max(rest, std::fabs(m.recon[c]));
        // A band with nothing patched is untouched in both modes.
        float other = 0.f;
        for (int c = 0; c < w; ++c) other = std::max(other, std::fabs(m.recon[bandLo(1, m.size) + c]));

        report(MOD, mode == 0 ? "overlay_fed_bin" : "replace_fed_bin",
               bin0, std::fabs(bin0) < 1e-6);
        if (mode == 0)
            report(MOD, "overlay_keeps_internal", rest, rest > 1e-6);
        else
            report(MOD, "replace_zeroes_absent", rest, rest < 1e-6);
        report(MOD, mode == 0 ? "overlay_other_band" : "replace_other_band",
               other, other > 1e-6);
    }
}

// 17.11 — freeze holds the coefficient vector while the sliders stay live.
static void testFreeze() {
    Quadrare m;
    defaults(m);
    setSize(m, 2);
    m.process(makeArgs(0));
    for (int i = 0; i < 4000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
    }
    m.freeze = true;
    float snapshot[kMaxSize];
    std::copy(m.held, m.held + m.size, snapshot);

    // Keep feeding completely different audio.
    double drift = 0.0, outPeak = 0.0;
    for (int i = 0; i < 4000; ++i) {
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

    // The sliders still act on the frozen vector.
    setBands(m, 0.f);
    for (int i = 0; i < 4 * m.size; ++i) m.process(makeArgs(i));
    double mutedPeak = 0.0;
    for (int i = 0; i < 4 * m.size; ++i) {
        m.process(makeArgs(i));
        mutedPeak = std::max(mutedPeak, (double) std::fabs(
            m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage()));
    }
    report(MOD, "freeze_sliders_live", mutedPeak, mutedPeak < 1e-4);
}

// KEEP and QUANT should alter the sound but never destabilize it.
static void testLossyStage() {
    Quadrare m;
    defaults(m);
    setSize(m, 3);
    m.process(makeArgs(0));
    m.params[Quadrare::KEEP_PARAM].setValue(0.f);      // keep 1 coefficient
    m.params[Quadrare::QUANT_PARAM].setValue(1.f);     // 2 levels
    Stats st;
    for (int i = 0; i < 12000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
        st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "lossy_finite", (double) st.nans, st.nans == 0);
    report(MOD, "lossy_bounded", st.peak, st.peak < 50.f);
    report(MOD, "lossy_audible", st.rms(), st.rms() > 1e-3);
}

// Nothing pathological when COEFF IN is driven hard with no audio at all:
// this is the standalone Walsh synthesizer case.
static void testSynthesizerMode() {
    Quadrare m;
    defaults(m);
    setSize(m, 1);
    m.coeffMode = Quadrare::MODE_REPLACE;
    for (int b = 0; b < kBands; ++b) m.inputs[Quadrare::COEFF_INPUT + b].channels = 1;
    m.process(makeArgs(0));
    const int w = bandWidth(m.size);
    for (int b = 0; b < kBands; ++b) {
        m.inputs[Quadrare::COEFF_INPUT + b].setChannels(w);
        for (int c = 0; c < w; ++c)
            m.inputs[Quadrare::COEFF_INPUT + b].setVoltage(b == 3 ? 5.f : 0.f, c);
    }
    Stats st;
    for (int i = 0; i < 8000; ++i) {
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(0.f);   // no audio at all
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
    for (int b = 0; b < kBands; ++b) m.inputs[Quadrare::COEFF_INPUT + b].channels = 1;
    Stats st;
    for (int i = 0; i < 40000; ++i) {
        if (i % 2000 == 0) setSize(m, (i / 2000) % kSizeCount);
        const int w = bandWidth(m.size);
        for (int b = 0; b < kBands; ++b) {
            m.inputs[Quadrare::COEFF_INPUT + b].setChannels(w);
            for (int c = 0; c < w; ++c)
                m.inputs[Quadrare::COEFF_INPUT + b].setVoltage(
                    rack::random::uniform() * 200.f - 100.f, c);
        }
        m.inputs[Quadrare::AUDIO_INPUT].setVoltage(testSignal(i));
        m.process(makeArgs(i));
        st.add(m.outputs[Quadrare::AUDIO_OUTPUT].getVoltage());
    }
    report(MOD, "stability_finite", (double) st.nans, st.nans == 0);
    report(MOD, "stability_clamped", st.peak, st.peak <= 100.f);
}

SMOKE_MAIN(testTransparent, testGainCases, testComponentsSum, testCoeffLoopback,
           testOverlayReplace, testFreeze, testLossyStage, testSynthesizerMode,
           testStability)
