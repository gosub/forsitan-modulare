// smoke_cartilago — offline sanity checks for the cartilago module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The spectral measurements (alias floor with and without band-limiting, the
// FET transfer curve) live in cartilago_probe.cpp, which is not run here.

#include "smoke_harness.hpp"
#include "../src/cartilago.cpp"

// Feed a sine and collect the audio output.
static Stats feedSine(Cartilago& m, long& frame, float f, float amp,
                      double settleS, double measS, int out = Cartilago::AUDIO_OUTPUT) {
    m.inputs[Cartilago::AUDIO_INPUT].channels = 1;
    double ph = 0.0, w = 2.0 * M_PI * f / SR;
    for (int i = 0; i < (int)(settleS * SR); i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    Stats s;
    for (int i = 0; i < (int)(measS * SR); i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
        s.add(m.outputs[out].getVoltage());
    }
    return s;
}

// Park the LFO at one end or the other with bias, depth off, and read the gain.
static double staticGain(float bias, bool vcf, float toneHz) {
    Cartilago m; long fr = 0;
    m.params[Cartilago::DEPTH_PARAM].setValue(0.f);
    m.params[Cartilago::BIAS_PARAM].setValue(bias);
    m.params[Cartilago::DRIVE_PARAM].setValue(0.5f);   // unity
    m.params[Cartilago::MODE_PARAM].setValue(vcf ? 1.f : 0.f);
    m.feedthrough = false;
    return feedSine(m, fr, toneHz, 1.f, 0.5, 0.5).rms() / (1.f / M_SQRT2);
}

// The FET attenuator: fully open passes, fully closed does not — but never
// reaches silence, which is the whole point of a shunt FET.
static void testVca() {
    const double open = staticGain(1.f, false, 400.f);
    const double shut = staticGain(-1.f, false, 400.f);
    report("cartilago", "vca_open_gain", open, open > 0.6 && open < 1.2);
    report("cartilago", "vca_shut_gain", shut, shut < 0.15);
    report("cartilago", "vca_floor_not_zero", shut, shut > 0.01);
    report("cartilago", "vca_range", open / std::max(shut, 1e-9),
           open / std::max(shut, 1e-9) > 6.0);

    // ...and it is monotonic in the control, which the smoothstep curve of the
    // original sketch was not once depth pushed the control past 1.
    double prev = -1.0;
    bool mono = true;
    for (int i = 0; i <= 8; i++) {
        const double g = staticGain(-1.f + 2.f * i / 8.f, false, 400.f);
        if (g < prev - 1e-4) mono = false;
        prev = g;
    }
    report("cartilago", "vca_monotonic", mono ? 1 : 0, mono);
}

// Tremolo: the envelope must actually move, and at the trough it must not be
// silent.
static void testTremolo() {
    Cartilago m; long fr = 0;
    m.params[Cartilago::DEPTH_PARAM].setValue(1.f);
    m.params[Cartilago::RATE_PARAM].setValue(2.f);       // 4 Hz
    m.params[Cartilago::DRIVE_PARAM].setValue(0.5f);
    m.feedthrough = false;
    m.inputs[Cartilago::AUDIO_INPUT].channels = 1;

    double ph = 0.0, w = 2.0 * M_PI * 700.0 / SR;
    for (int i = 0; i < (int)(0.5 * SR); i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(5.f * std::sin(ph));
        ph += w;
        m.process(makeArgs(fr++));
    }
    // track the envelope over one LFO period and find its extremes
    double env = 0.0, lo = 1e9, hi = 0.0;
    long nans = 0;
    for (int i = 0; i < (int)(0.75 * SR); i++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(5.f * std::sin(ph));
        ph += w;
        m.process(makeArgs(fr++));
        const float y = m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage();
        if (!std::isfinite(y)) nans++;
        env += (std::fabs(y) - env) * 0.002;
        if (i > (int)(0.25 * SR)) { lo = std::min(lo, env); hi = std::max(hi, env); }
    }
    report("cartilago", "trem_nans", nans, nans == 0);
    report("cartilago", "trem_depth", hi / std::max(lo, 1e-9), hi / std::max(lo, 1e-9) > 4.0);
    report("cartilago", "trem_trough_alive", lo, lo > 0.01);
}

// Filter mode: the same control sweeps a bandpass, so the two ends of the bias
// knob must favour different parts of the spectrum.
static void testVcf() {
    const double lowBias60 = staticGain(-1.f, true, 60.f);
    const double lowBias3k = staticGain(-1.f, true, 3000.f);
    const double hiBias60 = staticGain(1.f, true, 60.f);
    const double hiBias3k = staticGain(1.f, true, 3000.f);
    report("cartilago", "vcf_low_prefers_low", lowBias60 / std::max(lowBias3k, 1e-9),
           lowBias60 > lowBias3k);
    report("cartilago", "vcf_high_prefers_high", hiBias3k / std::max(hiBias60, 1e-9),
           hiBias3k > hiBias60);
}

// Control feedthrough: the pedal ticks with nothing plugged in. It must do so
// only when the menu says it should, and only when the control is moving.
static void testFeedthrough() {
    auto run = [](bool on, float rateKnob) {
        Cartilago m; long fr = 0;
        m.feedthrough = on;
        m.params[Cartilago::WAVE_PARAM].setValue(3.f);      // pulse
        m.params[Cartilago::RATE_PARAM].setValue(rateKnob);
        m.params[Cartilago::DEPTH_PARAM].setValue(1.f);
        for (int i = 0; i < (int)(0.5 * SR); i++) m.process(makeArgs(fr++));
        Stats s;
        for (int i = 0; i < (int)SR; i++) {
            m.process(makeArgs(fr++));
            s.add(m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage());
        }
        return s;
    };
    Stats on = run(true, 2.f), off = run(false, 2.f);
    // present but modest: a thump under the audio, not a signal of its own
    report("cartilago", "feed_on_ticks", on.peak, on.peak > 0.05);
    report("cartilago", "feed_off_silent", off.peak, off.peak < 1e-4);
    report("cartilago", "feed_bounded", on.peak, on.peak < 1.f);
    // the gate one-pole caps dV/dt, so audio-rate modulation cannot make the
    // tick grow without bound
    Stats fast = run(true, 9.f);                            // 512 Hz
    report("cartilago", "feed_audio_rate_bounded", fast.peak, fast.peak < 10.f);
}

// Sync resets the phase: two modules started a quarter period apart must line
// up again after a trigger.
static void testSync() {
    Cartilago a, b; long fa = 0, fb = 0;
    a.params[Cartilago::RATE_PARAM].setValue(3.f);   // 8 Hz
    b.params[Cartilago::RATE_PARAM].setValue(3.f);
    a.inputs[Cartilago::SYNC_INPUT].channels = 1;
    b.inputs[Cartilago::SYNC_INPUT].channels = 1;
    for (int i = 0; i < (int)(0.031 * SR); i++) a.process(makeArgs(fa++));  // offset
    for (int i = 0; i < (int)(0.2 * SR); i++) {
        a.process(makeArgs(fa++));
        b.process(makeArgs(fb++));
    }
    const double before = std::fabs(a.outputs[Cartilago::LFO_OUTPUT].getVoltage()
                                    - b.outputs[Cartilago::LFO_OUTPUT].getVoltage());
    a.inputs[Cartilago::SYNC_INPUT].setVoltage(5.f);
    b.inputs[Cartilago::SYNC_INPUT].setVoltage(5.f);
    a.process(makeArgs(fa++));
    b.process(makeArgs(fb++));
    a.inputs[Cartilago::SYNC_INPUT].setVoltage(0.f);
    b.inputs[Cartilago::SYNC_INPUT].setVoltage(0.f);
    for (int i = 0; i < (int)(0.05 * SR); i++) {
        a.process(makeArgs(fa++));
        b.process(makeArgs(fb++));
    }
    const double after = std::fabs(a.outputs[Cartilago::LFO_OUTPUT].getVoltage()
                                   - b.outputs[Cartilago::LFO_OUTPUT].getVoltage());
    report("cartilago", "sync_was_apart", before, before > 0.2);
    report("cartilago", "sync_locks", after, after < 0.01);
}

// Polyphony: one LFO, but the FET and the filter must not be shared.
static void testPoly() {
    Cartilago m; long fr = 0;
    m.params[Cartilago::DEPTH_PARAM].setValue(0.f);
    m.params[Cartilago::BIAS_PARAM].setValue(1.f);      // wide open
    m.params[Cartilago::DRIVE_PARAM].setValue(0.5f);
    m.feedthrough = false;
    m.inputs[Cartilago::AUDIO_INPUT].channels = 4;
    // Port::setChannels() is a no-op on a port the harness never connects
    m.outputs[Cartilago::AUDIO_OUTPUT].channels = 1;
    Stats s[4];
    double ph = 0.0;
    for (int i = 0; i < (int)(1.0 * SR); i++) {
        ph += 2.0 * M_PI * 300.0 / SR;
        for (int c = 0; c < 4; c++)
            m.inputs[Cartilago::AUDIO_INPUT].setVoltage((c + 1) * 0.5f * std::sin(ph), c);
        m.process(makeArgs(fr++));
        if (i > (int)(0.3 * SR))
            for (int c = 0; c < 4; c++) s[c].add(m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage(c));
    }
    report("cartilago", "poly_channels", m.outputs[Cartilago::AUDIO_OUTPUT].getChannels(),
           m.outputs[Cartilago::AUDIO_OUTPUT].getChannels() == 4);
    // amplitudes must stay in the 1:2:3:4 ratio they went in with
    bool ok = true;
    for (int c = 1; c < 4; c++) {
        const double want = (double)(c + 1) / 1.0;
        const double got = s[c].rms() / std::max(s[0].rms(), 1e-9);
        if (std::fabs(got - want) / want > 0.12) ok = false;
    }
    report("cartilago", "poly_no_crosstalk", ok ? 1 : 0, ok);
    for (int c = 0; c < 4; c++)
        report("cartilago", "poly_nans", s[c].nans, s[c].nans == 0);
}

// Every oversampling setting must be stable, and band-limiting must not change
// the level.
static void testOversampling() {
    double rms[5];
    for (int os = 0; os <= 4; os++) {
        Cartilago m; long fr = 0;
        m.osIndex = os;
        m.params[Cartilago::RATE_PARAM].setValue(8.f);    // 256 Hz, ring mod
        m.params[Cartilago::DEPTH_PARAM].setValue(1.f);
        m.params[Cartilago::WAVE_PARAM].setValue(3.f);
        Stats s = feedSine(m, fr, 330.f, 5.f, 0.4, 0.5);
        rms[os] = s.rms();
        char name[64];
        snprintf(name, sizeof name, "os%dx_nans", 1 << os);
        report("cartilago", name, s.nans, s.nans == 0);
        snprintf(name, sizeof name, "os%dx_peak", 1 << os);
        report("cartilago", name, s.peak, s.peak < 10.5f);
    }
    double lo = rms[0], hi = rms[0];
    for (int i = 1; i <= 4; i++) { lo = std::min(lo, rms[i]); hi = std::max(hi, rms[i]); }
    report("cartilago", "os_level_spread", hi / std::max(lo, 1e-9),
           hi / std::max(lo, 1e-9) < 1.5);
}

// Hostile CV and knob motion, both modes, with the shape at its extremes.
static void testStress() {
    for (int mode = 0; mode < 2; mode++) {
        Cartilago m; long fr = 0;
        m.params[Cartilago::MODE_PARAM].setValue((float)mode);
        m.params[Cartilago::DRIVE_PARAM].setValue(1.f);
        m.params[Cartilago::RES_PARAM].setValue(1.f);
        m.params[Cartilago::LEVEL_PARAM].setValue(2.f);
        m.inputs[Cartilago::AUDIO_INPUT].channels = 1;
        m.inputs[Cartilago::VOCT_INPUT].channels = 1;
        m.inputs[Cartilago::DEPTH_INPUT].channels = 1;
        m.inputs[Cartilago::BIAS_INPUT].channels = 1;
        m.inputs[Cartilago::MOD_INPUT].channels = 1;
        m.inputs[Cartilago::SYNC_INPUT].channels = 1;
        Stats s, l;
        for (int i = 0; i < (int)(10 * SR); i++) {
            const float t = (float)i / SR;
            m.inputs[Cartilago::AUDIO_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 137.f * t));
            m.inputs[Cartilago::VOCT_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.4f * t));
            m.inputs[Cartilago::DEPTH_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 3.1f * t));
            m.inputs[Cartilago::BIAS_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.7f * t));
            m.inputs[Cartilago::MOD_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 611.f * t));
            m.inputs[Cartilago::SYNC_INPUT].setVoltage((std::fmod(t, 0.37f) < 0.01f) ? 5.f : 0.f);
            m.params[Cartilago::SHAPE_PARAM].setValue(0.02f + 0.96f * (0.5f + 0.5f * std::sin(2.f * M_PI * 0.19f * t)));
            m.params[Cartilago::WAVE_PARAM].setValue(std::floor(std::fmod(t, 4.f)));
            m.process(makeArgs(fr++));
            s.add(m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage());
            l.add(m.outputs[Cartilago::LFO_OUTPUT].getVoltage());
        }
        const char* nm = mode ? "vcf" : "vca";
        char name[64];
        snprintf(name, sizeof name, "stress_%s_nans", nm);
        report("cartilago", name, s.nans, s.nans == 0);
        snprintf(name, sizeof name, "stress_%s_peak", nm);
        report("cartilago", name, s.peak, s.peak <= 10.f);
        snprintf(name, sizeof name, "stress_%s_lfo_peak", nm);
        report("cartilago", name, l.peak, l.peak <= 7.6f);
        snprintf(name, sizeof name, "stress_%s_lfo_nans", nm);
        report("cartilago", name, l.nans, l.nans == 0);
    }
}

SMOKE_MAIN(testVca, testTremolo, testVcf, testFeedthrough, testSync, testPoly,
           testOversampling, testStress)
