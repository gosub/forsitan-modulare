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

// Hostile CV and knob motion at the extremes, on all four outputs.
static void testStress() {
    Vespae m; long fr = 0;
    m.inputs[Vespae::AUDIO_INPUT].channels = 1;
    m.inputs[Vespae::VOCT_INPUT].channels = 1;
    m.inputs[Vespae::FM_INPUT].channels = 1;
    m.inputs[Vespae::RES_CV_INPUT].channels = 1;
    m.params[Vespae::RES_PARAM].setValue(1.f);
    m.params[Vespae::DRIVE_PARAM].setValue(1.f);
    m.params[Vespae::FM_PARAM].setValue(1.f);
    Stats s[4];
    for (int i = 0; i < (int)(12 * SR); i++) {
        float t = (float)i / SR;
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 137.f * t));
        m.inputs[Vespae::VOCT_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 3.1f * t));
        m.inputs[Vespae::FM_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 511.f * t));
        m.inputs[Vespae::RES_CV_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.7f * t));
        m.params[Vespae::CUTOFF_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.23f * t));
        m.params[Vespae::GRIT_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.13f * t));
        m.process(makeArgs(fr++));
        for (int o = 0; o < 4; o++) s[o].add(m.outputs[Vespae::LP_OUTPUT + o].getVoltage());
    }
    const char* nm[4] = {"lp", "hp", "bp", "notch"};
    for (int o = 0; o < 4; o++) {
        char name[64];
        snprintf(name, sizeof name, "stress_%s_nans", nm[o]);
        report("vespae", name, s[o].nans, s[o].nans == 0);
        snprintf(name, sizeof name, "stress_%s_peak", nm[o]);
        report("vespae", name, s[o].peak, s[o].peak <= 10.f);
    }
}

SMOKE_MAIN(testSeparation, testResonance, testDrive, testOversampling, testStress)
