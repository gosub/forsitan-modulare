// smoke_vespae — offline sanity checks for the vespae module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// For the measurements that characterise the filter (response, Q vs cutoff,
// THD, self-oscillation level) see vespae_probe.cpp, which is not run here.

#include "smoke_harness.hpp"
#include "../src/vespae.cpp"

// Feed a sine and collect one output.
static Stats feedSine(Vespae& m, long& frame, float f, float amp, int out,
                      double settleS, double measS) {
    m.inputs[Vespae::AUDIO_INPUT].channels = 1;
    double ph = 0.0, w = 2.0 * M_PI * f / SR;
    for (int i = 0; i < (int)(settleS * SR); i++) {
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    Stats s;
    for (int i = 0; i < (int)(measS * SR); i++) {
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
        s.add(m.outputs[out].getVoltage());
    }
    return s;
}

// The filter must actually filter: with the cutoff at 160 Hz an 8 kHz tone
// belongs in the HP output and not in the LP one.
static void testSeparation() {
    Vespae m; long fr = 0;
    m.params[Vespae::CUTOFF_PARAM].setValue(0.3f);   // 160 Hz
    m.params[Vespae::RES_PARAM].setValue(0.2f);
    m.params[Vespae::DRIVE_PARAM].setValue(0.4f);
    Stats lo = feedSine(m, fr, 8000.f, 2.f, Vespae::LP_OUTPUT, 0.3, 0.3);

    Vespae m2; long fr2 = 0;
    m2.params[Vespae::CUTOFF_PARAM].setValue(0.3f);
    m2.params[Vespae::RES_PARAM].setValue(0.2f);
    m2.params[Vespae::DRIVE_PARAM].setValue(0.4f);
    Stats hi = feedSine(m2, fr2, 8000.f, 2.f, Vespae::HP_OUTPUT, 0.3, 0.3);

    report("vespae", "lp_rejects_8k_nans", lo.nans, lo.nans == 0);
    // two poles over ~5.6 octaves is a lot of rejection; be generous
    report("vespae", "lp_over_hp_8k", lo.rms() / std::max(hi.rms(), 1e-9),
           lo.rms() / std::max(hi.rms(), 1e-9) < 0.02);

    // ...and the other way round at 40 Hz with the cutoff up at 5 kHz
    Vespae m3; long fr3 = 0;
    m3.params[Vespae::CUTOFF_PARAM].setValue(0.8f);  // 5120 Hz
    m3.params[Vespae::RES_PARAM].setValue(0.2f);
    m3.params[Vespae::DRIVE_PARAM].setValue(0.4f);
    Stats hp40 = feedSine(m3, fr3, 40.f, 2.f, Vespae::HP_OUTPUT, 0.5, 0.5);
    Vespae m4; long fr4 = 0;
    m4.params[Vespae::CUTOFF_PARAM].setValue(0.8f);
    m4.params[Vespae::RES_PARAM].setValue(0.2f);
    m4.params[Vespae::DRIVE_PARAM].setValue(0.4f);
    Stats lp40 = feedSine(m4, fr4, 40.f, 2.f, Vespae::LP_OUTPUT, 0.5, 0.5);
    report("vespae", "hp_over_lp_40hz", hp40.rms() / std::max(lp40.rms(), 1e-9),
           hp40.rms() / std::max(lp40.rms(), 1e-9) < 0.02);
}

// At the top of the RES knob it must sing on its own, and near the bottom it
// must not.
static void testResonance() {
    Vespae m; long fr = 0;
    m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
    m.params[Vespae::RES_PARAM].setValue(1.f);
    for (int i = 0; i < (int)(3 * SR); i++) m.process(makeArgs(fr++));
    Stats s;
    int zc = 0; float prev = 0;
    for (int i = 0; i < (int)SR; i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Vespae::BP_OUTPUT].getVoltage();
        s.add(y);
        if (prev <= 0.f && y > 0.f) zc++;
        prev = y;
    }
    report("vespae", "selfosc_nans", s.nans, s.nans == 0);
    report("vespae", "selfosc_rms", s.rms(), s.rms() > 0.2);
    report("vespae", "selfosc_bounded", s.peak, s.peak < 8.f);
    // knob 0.5 is a nominal 640 Hz; saturation pulls it flat, allow a fourth
    report("vespae", "selfosc_hz", zc, zc > 480 && zc < 860);

    Vespae q; long fq = 0;
    q.params[Vespae::RES_PARAM].setValue(0.1f);
    for (int i = 0; i < (int)(2 * SR); i++) q.process(makeArgs(fq++));
    Stats sq;
    for (int i = 0; i < (int)SR; i++) {
        q.process(makeArgs(fq++));
        sq.add(q.outputs[Vespae::BP_OUTPUT].getVoltage());
    }
    report("vespae", "quiet_at_low_res", sq.rms(), sq.rms() < 0.005);
}

// Drive must add harmonics without the loop latching or running away. A
// clipper with a flat top once parked the states on a rail for good.
static void testDrive() {
    for (float grit : {0.f, 0.5f, 1.f}) {
        Vespae m; long fr = 0;
        m.params[Vespae::CUTOFF_PARAM].setValue(0.6f);
        m.params[Vespae::RES_PARAM].setValue(0.4f);
        m.params[Vespae::DRIVE_PARAM].setValue(1.f);
        m.params[Vespae::GRIT_PARAM].setValue(grit);
        Stats s = feedSine(m, fr, 200.f, 5.f, Vespae::LP_OUTPUT, 0.5, 1.0);
        char name[64];
        snprintf(name, sizeof name, "driven_rms_grit%.0f", grit * 10);
        report("vespae", name, s.rms(), s.rms() > 1.0 && s.rms() < 6.0);
        snprintf(name, sizeof name, "driven_peak_grit%.0f", grit * 10);
        report("vespae", name, s.peak, s.peak < 8.f);
    }
}

// Every oversampling setting must be stable and roughly level-matched.
static void testOversampling() {
    double rms[5];
    for (int os = 0; os <= 4; os++) {
        Vespae m; long fr = 0;
        m.osIndex = os;
        m.params[Vespae::CUTOFF_PARAM].setValue(0.55f);
        m.params[Vespae::RES_PARAM].setValue(0.8f);
        m.params[Vespae::DRIVE_PARAM].setValue(0.7f);
        Stats s = feedSine(m, fr, 300.f, 5.f, Vespae::LP_OUTPUT, 0.4, 0.5);
        rms[os] = s.rms();
        char name[64];
        snprintf(name, sizeof name, "os%dx_nans", 1 << os);
        report("vespae", name, s.nans, s.nans == 0);
    }
    double lo = rms[0], hi = rms[0];
    for (int i = 1; i <= 4; i++) { lo = std::min(lo, rms[i]); hi = std::max(hi, rms[i]); }
    report("vespae", "os_level_spread", hi / std::max(lo, 1e-9), hi / std::max(lo, 1e-9) < 1.5);
}

// The LP/HP mix pot: its ends must land exactly on the dedicated LP and HP
// jacks, and its middle must notch at the cutoff.
static void testMix() {
    // Probe quietly, on purpose. A notch this deep only exists in the linear
    // regime: once the signal is big enough to saturate the OTAs they pull
    // the cutoff around at twice the signal frequency, the null moves with
    // it, and the cancellation stops being anywhere near exact. That is the
    // circuit's behaviour, not an artefact — at 1 V in, the null here is only
    // about 30 dB deep instead of 89.
    auto run = [](float mixKnob, int out, float f) {
        Vespae m; long fr = 0;
        m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);   // 640 Hz
        m.params[Vespae::RES_PARAM].setValue(0.2f);
        m.params[Vespae::DRIVE_PARAM].setValue(0.4f);
        m.params[Vespae::MIX_PARAM].setValue(mixKnob);
        return feedSine(m, fr, f, 0.05f, out, 0.4, 0.4).rms();
    };

    // fully counter-clockwise is the lowpass, fully clockwise the highpass
    double mix0 = run(0.f, Vespae::MIX_OUTPUT, 200.f);
    double lp   = run(0.f, Vespae::LP_OUTPUT,  200.f);
    double mix1 = run(1.f, Vespae::MIX_OUTPUT, 5000.f);
    double hp   = run(1.f, Vespae::HP_OUTPUT,  5000.f);
    report("vespae", "mix0_is_lp", std::fabs(mix0 - lp) / std::max(lp, 1e-9),
           std::fabs(mix0 - lp) / std::max(lp, 1e-9) < 0.01);
    report("vespae", "mix1_is_hp", std::fabs(mix1 - hp) / std::max(hp, 1e-9),
           std::fabs(mix1 - hp) / std::max(hp, 1e-9) < 0.01);

    // centred, the LP and HP shares cancel at the cutoff
    double notchAtFc = run(0.5f, Vespae::MIX_OUTPUT, 640.f);
    double passAtFc  = run(0.5f, Vespae::LP_OUTPUT,  640.f);
    report("vespae", "mix_notches_at_fc", notchAtFc / std::max(passAtFc, 1e-9),
           notchAtFc / std::max(passAtFc, 1e-9) < 0.01);
    // ...and passes what is well away from it
    double away = run(0.5f, Vespae::MIX_OUTPUT, 60.f);
    report("vespae", "mix_passes_off_notch", away / std::max(notchAtFc, 1e-12),
           away > 20.0 * notchAtFc);

    // off-centre the null moves: at 0.25 it sits near fc*sqrt(3) = 1109 Hz,
    // so 640 Hz is no longer nulled
    double offCentre = run(0.25f, Vespae::MIX_OUTPUT, 640.f);
    report("vespae", "mix_null_moves", offCentre / std::max(notchAtFc, 1e-12),
           offCentre > 20.0 * notchAtFc);

    // the crossfade is a convex blend of two bounded nodes, so it can never
    // exceed them however hard the filter is driven
    Vespae m; long fr = 0;
    m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
    m.params[Vespae::RES_PARAM].setValue(1.f);
    m.params[Vespae::DRIVE_PARAM].setValue(1.f);
    m.params[Vespae::MIX_PARAM].setValue(0.5f);
    m.inputs[Vespae::MIX_CV_INPUT].channels = 1;
    Stats s;
    for (int i = 0; i < (int)(4 * SR); i++) {
        float t = (float)i / SR;
        m.inputs[Vespae::AUDIO_INPUT].channels = 1;
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 90.f * t));
        m.inputs[Vespae::MIX_CV_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 3.7f * t));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Vespae::MIX_OUTPUT].getVoltage());
    }
    report("vespae", "mix_cv_nans", s.nans, s.nans == 0);
    report("vespae", "mix_cv_peak", s.peak, s.peak < 8.f);
}

// The two panel mods, neither of which is on the A-124. They work in
// opposite regimes: bias only bites once something is already clipping,
// hiss only matters when little else is going on.
static void testMods() {
    auto driven = [](float bias) {
        Vespae m; long fr = 0;
        m.params[Vespae::CUTOFF_PARAM].setValue(0.45f);
        m.params[Vespae::RES_PARAM].setValue(0.85f);
        m.params[Vespae::DRIVE_PARAM].setValue(0.75f);
        m.params[Vespae::BIAS_PARAM].setValue(bias);
        return feedSine(m, fr, 220.f, 4.f, Vespae::LP_OUTPUT, 0.8, 0.6);
    };
    Stats b0 = driven(0.f), b1 = driven(1.f);
    double c0 = b0.peak / std::max(b0.rms(), 1e-9);
    double c1 = b1.peak / std::max(b1.rms(), 1e-9);
    report("vespae", "bias_nans", b1.nans, b1.nans == 0);
    // lopsided clipping pushes one half of the wave out further
    report("vespae", "bias_raises_crest", c1 / c0, c1 > c0 * 1.05);
    // ...without being a level control
    report("vespae", "bias_keeps_level", b1.rms() / std::max(b0.rms(), 1e-9),
           std::fabs(b1.rms() / std::max(b0.rms(), 1e-9) - 1.0) < 0.10);

    auto floorAt = [](float hiss) {
        Vespae m; long fr = 0;
        m.params[Vespae::CUTOFF_PARAM].setValue(0.6f);
        m.params[Vespae::RES_PARAM].setValue(0.2f);
        m.params[Vespae::HISS_PARAM].setValue(hiss);
        for (int i = 0; i < (int)(1.0 * SR); i++) m.process(makeArgs(fr++));
        Stats s;
        for (int i = 0; i < (int)SR; i++) {
            m.process(makeArgs(fr++));
            s.add(m.outputs[Vespae::LP_OUTPUT].getVoltage());
        }
        return s;
    };
    Stats h0 = floorAt(0.f), h1 = floorAt(1.f);
    // fully down, the idle dither must stay inaudible so silence is silence
    report("vespae", "hiss_off_is_silent", h0.rms(), h0.rms() < 0.005);
    report("vespae", "hiss_on_is_audible", h1.rms(), h1.rms() > 0.01);
    report("vespae", "hiss_nans", h1.nans, h1.nans == 0);
    report("vespae", "hiss_bounded", h1.peak, h1.peak < 3.f);

    // both mods maxed, driven hard, resonance at the top: still bounded
    Vespae m; long fr = 0;
    m.params[Vespae::CUTOFF_PARAM].setValue(0.45f);
    m.params[Vespae::RES_PARAM].setValue(1.f);
    m.params[Vespae::DRIVE_PARAM].setValue(1.f);
    m.params[Vespae::BIAS_PARAM].setValue(1.f);
    m.params[Vespae::HISS_PARAM].setValue(1.f);
    Stats s = feedSine(m, fr, 110.f, 8.f, Vespae::LP_OUTPUT, 1.0, 2.0);
    report("vespae", "mods_max_nans", s.nans, s.nans == 0);
    report("vespae", "mods_max_peak", s.peak, s.peak < 8.f);
}

// Hostile CV and knob motion at the extremes, on all five outputs.
static void testStress() {
    Vespae m; long fr = 0;
    m.inputs[Vespae::AUDIO_INPUT].channels = 1;
    m.inputs[Vespae::VOCT_INPUT].channels = 1;
    m.inputs[Vespae::FM_INPUT].channels = 1;
    m.inputs[Vespae::RES_CV_INPUT].channels = 1;
    m.params[Vespae::RES_PARAM].setValue(1.f);
    m.params[Vespae::DRIVE_PARAM].setValue(1.f);
    m.params[Vespae::FM_PARAM].setValue(1.f);
    Stats s[5];
    for (int i = 0; i < (int)(12 * SR); i++) {
        float t = (float)i / SR;
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 137.f * t));
        m.inputs[Vespae::VOCT_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 3.1f * t));
        m.inputs[Vespae::FM_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 511.f * t));
        m.inputs[Vespae::RES_CV_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.7f * t));
        m.params[Vespae::CUTOFF_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.23f * t));
        m.params[Vespae::GRIT_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.13f * t));
        m.params[Vespae::BIAS_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.09f * t));
        m.params[Vespae::HISS_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.17f * t));
        m.process(makeArgs(fr++));
        for (int o = 0; o < 5; o++) s[o].add(m.outputs[Vespae::LP_OUTPUT + o].getVoltage());
    }
    const char* nm[5] = {"lp", "hp", "bp", "notch", "mix"};
    for (int o = 0; o < 5; o++) {
        char name[64];
        snprintf(name, sizeof name, "stress_%s_nans", nm[o]);
        report("vespae", name, s[o].nans, s[o].nans == 0);
        snprintf(name, sizeof name, "stress_%s_peak", nm[o]);
        report("vespae", name, s[o].peak, s[o].peak <= 10.f);
    }
}

SMOKE_MAIN(testSeparation, testResonance, testDrive, testMix, testMods,
           testOversampling, testStress)
