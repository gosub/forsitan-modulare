// smoke_aether - offline sanity checks for the aether module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The two laws the engine is built on are measured by aether_probe, which
// does not involve Rack. These are the module's own checks: the jacks, the
// normalling, the knob mappings, the clock outputs and the oversampling.

#include "smoke_harness.hpp"
#include "../src/aether.cpp"

#include <vector>

static float knobFor(double hz) {
    return (float)aether::clampd(std::log2(hz / aether::kClkMin)
                                 / aether::kClkOctaves, 0.0, 1.0);
}

static void setKnobs(Aether& m, double carrierHz, double demodHz,
                     float level = 1.f, float tone = 0.7f, int type = 0) {
    m.params[Aether::LEVEL_PARAM].setValue(level);
    m.params[Aether::CARRIER_PARAM].setValue(knobFor(carrierHz));
    m.params[Aether::DEMOD_PARAM].setValue(knobFor(demodHz));
    m.params[Aether::TONE_PARAM].setValue(tone);
    m.params[Aether::TYPE_PARAM].setValue((float)type);
}

// A sine into SIGNAL IN, measuring one output.
static Stats feedSine(Aether& m, long& frame, float f, float amp,
                      double settleS, double measS, int outId) {
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    double ph = 0.0;
    const double w = 2.0 * M_PI * f / SR;
    for (long i = 0; i < (long)(settleS * SR); i++) {
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    Stats s;
    for (long i = 0; i < (long)(measS * SR); i++) {
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
        s.add(m.outputs[outId].getVoltage());
    }
    return s;
}

// Correlation between the recovered output and the input that made it: how
// much of the signal survived the trip, whatever else came along.
static double surviving(Aether& m, long& frame, float f, float amp,
                        double settleS, double measS) {
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    double ph = 0.0;
    const double w = 2.0 * M_PI * f / SR;
    for (long i = 0; i < (long)(settleS * SR); i++) {
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    double re = 0.0, im = 0.0;
    const long n = (long)(measS * SR);
    for (long i = 0; i < n; i++) {
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        m.process(makeArgs(frame++));
        const double y = m.outputs[Aether::SIGNAL_OUTPUT].getVoltage();
        re += y * std::cos(ph);
        im += y * std::sin(ph);
        ph += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

// ── the trip, made properly ─────────────────────────────────────────────────
// Clocks matched and well above the signal: the manual's own claim is that
// the output matches the input. It comes back battered, but it comes back.
static void testRecovery() {
    Aether m; long fr = 0;
    setKnobs(m, 96000.0, 96000.0, 1.f, 0.7f, aether::PD_PFD);
    const double got = surviving(m, fr, 220.f, 4.f, 0.3, 0.5);
    Stats s = feedSine(m, fr, 220.f, 4.f, 0.1, 0.3, Aether::SIGNAL_OUTPUT);
    report("aether", "recovery_nans", s.nans, s.nans == 0);
    report("aether", "recovery_gain", got / 4.0, got > 2.4 && got < 5.0);
    report("aether", "recovery_bounded", s.peak, s.peak <= 10.f);
}

// A demodulator clock slower than the carrier asks the loop for a control
// voltage past its rails: it never locks, and the manual says so.
static void testNoLock() {
    Aether ok; long fo = 0;
    setKnobs(ok, 96000.0, 96000.0, 1.f, 0.7f, aether::PD_PFD);
    const double locked = surviving(ok, fo, 220.f, 4.f, 0.3, 0.5);

    Aether slow; long fs = 0;
    setKnobs(slow, 96000.0, 6000.0, 1.f, 0.7f, aether::PD_PFD);
    const double lost = surviving(slow, fs, 220.f, 4.f, 0.3, 0.5);
    // 16:1 apart, what leaks through is about a fifth of what a locked loop
    // hands back, and it is not the signal any more.
    report("aether", "nolock_kills_signal", lost / std::max(locked, 1e-9),
           lost < 0.2 * locked);
}

// Turning the carrier down is turning the sample rate down: the same signal
// comes back with far more of everything that is not the signal.
static void testCarrierCrush() {
    Aether hi; long fh = 0;
    setKnobs(hi, 192000.0, 192000.0, 1.f, 0.7f, aether::PD_PFD);
    const double fundHi = surviving(hi, fh, 220.f, 4.f, 0.3, 0.5);
    Stats sHi = feedSine(hi, fh, 220.f, 4.f, 0.1, 0.3, Aether::SIGNAL_OUTPUT);

    Aether lo; long fl = 0;
    setKnobs(lo, 6000.0, 6000.0, 1.f, 0.7f, aether::PD_PFD);
    const double fundLo = surviving(lo, fl, 220.f, 4.f, 0.3, 0.5);
    Stats sLo = feedSine(lo, fl, 220.f, 4.f, 0.1, 0.3, Aether::SIGNAL_OUTPUT);

    // junk = whatever is in the output beyond the fundamental
    const double junkHi = sHi.rms() - fundHi / std::sqrt(2.0);
    const double junkLo = sLo.rms() - fundLo / std::sqrt(2.0);
    report("aether", "crush_nans", sHi.nans + sLo.nans, sHi.nans + sLo.nans == 0);
    report("aether", "crush_adds_junk", junkLo / std::max(junkHi, 1e-6),
           junkLo > 1.5 * junkHi);
}

// Nothing patched at all: the input jack is a DC bias, the transmitter is an
// oscillator and the receiver chases it. The module has to make a noise.
static void testStandalone() {
    Aether m; long fr = 0;
    setKnobs(m, 12000.0, 5000.0, 0.7f, 1.0f, aether::PD_XOR);
    Stats s;
    for (long i = 0; i < (long)(1.5 * SR); i++) {
        m.process(makeArgs(fr++));
        if (i > (long)(0.5 * SR)) s.add(m.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
    }
    report("aether", "standalone_nans", s.nans, s.nans == 0);
    report("aether", "standalone_alive", s.rms(), s.rms() > 0.2);
    report("aether", "standalone_bounded", s.peak, s.peak <= 10.f);

    // and it moves when the input level knob moves, because that knob is the
    // bias setting the transmitter's rate
    Aether q; long fq = 0;
    setKnobs(q, 12000.0, 5000.0, 0.05f, 1.0f, aether::PD_XOR);
    Stats sq;
    for (long i = 0; i < (long)(1.5 * SR); i++) {
        q.process(makeArgs(fq++));
        if (i > (long)(0.5 * SR)) sq.add(q.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
    }
    report("aether", "standalone_level_matters",
           std::fabs(s.rms() - sq.rms()), std::fabs(s.rms() - sq.rms()) > 0.05);
}

// The two clock outputs are square waves at the frequency the knobs ask for.
static void testClockOuts() {
    Aether m; long fr = 0;
    setKnobs(m, 800.0, 300.0);
    for (long i = 0; i < (long)(0.2 * SR); i++) m.process(makeArgs(fr++));
    int zc[2] = {0, 0};
    float prev[2] = {0.f, 0.f};
    long nans = 0;
    float peak = 0.f;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(fr++));
        const float y[2] = {m.outputs[Aether::CARRIER_OUTPUT].getVoltage(),
                            m.outputs[Aether::DEMOD_OUTPUT].getVoltage()};
        for (int c = 0; c < 2; c++) {
            if (!std::isfinite(y[c])) nans++;
            if (prev[c] <= 0.f && y[c] > 0.f) zc[c]++;
            prev[c] = y[c];
            peak = std::max(peak, std::fabs(y[c]));
        }
    }
    report("aether", "clk_nans", nans, nans == 0);
    report("aether", "clk_tx_hz", zc[0], zc[0] > 780 && zc[0] < 820);
    report("aether", "clk_rx_hz", zc[1], zc[1] > 290 && zc[1] < 310);
    report("aether", "clk_level", peak, peak > 4.f && peak <= 10.f);
}

// A clock patched into CARRIER IN takes over the transmitter's ticks. The
// oscillator behind CARRIER OUT keeps running regardless.
static void testExternalClock() {
    Aether m; long fr = 0;
    setKnobs(m, 800.0, 24000.0, 1.f, 0.7f, aether::PD_XOR);
    m.inputs[Aether::CARRIER_CLK_INPUT].channels = 1;
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    double ph = 0.0, sph = 0.0;
    Stats out, clk;
    for (long i = 0; i < (long)(1.2 * SR); i++) {
        m.inputs[Aether::CARRIER_CLK_INPUT].setVoltage(
            std::sin(ph) > 0.0 ? 5.f : -5.f);
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(3.f * std::sin(sph));
        ph += 2.0 * M_PI * 9000.0 / SR;
        sph += 2.0 * M_PI * 220.0 / SR;
        m.process(makeArgs(fr++));
        if (i > (long)(0.4 * SR)) {
            out.add(m.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
            clk.add(m.outputs[Aether::CARRIER_OUTPUT].getVoltage());
        }
    }
    report("aether", "extclk_nans", out.nans + clk.nans, out.nans + clk.nans == 0);
    report("aether", "extclk_alive", out.rms(), out.rms() > 0.05);
    report("aether", "extclk_osc_still_runs", clk.rms(), clk.rms() > 1.0);
}

// ERROR is a comparator: a square at the rails that keeps switching, and the
// threshold knob has to change what it does.
static void testError() {
    Aether m; long fr = 0;
    setKnobs(m, 24000.0, 24000.0, 1.f, 0.7f, aether::PD_XOR);
    Stats s = feedSine(m, fr, 220.f, 4.f, 0.2, 0.4, Aether::ERROR_OUTPUT);
    report("aether", "error_nans", s.nans, s.nans == 0);
    report("aether", "error_swings", s.rms(), s.rms() > 1.0);
    report("aether", "error_bounded", s.peak, s.peak <= 10.f);

    Aether hi; long fh = 0;
    setKnobs(hi, 24000.0, 24000.0, 1.f, 0.7f, aether::PD_XOR);
    hi.params[Aether::ERROR_PARAM].setValue(1.f);
    Stats sh = feedSine(hi, fh, 220.f, 4.f, 0.2, 0.4, Aether::ERROR_OUTPUT);
    report("aether", "error_threshold_matters",
           std::fabs(sh.sum / sh.n - s.sum / s.n),
           std::fabs(sh.sum / sh.n - s.sum / s.n) > 0.5);
}

// All three loop types have to produce something with the clocks matched.
static void testTypes() {
    for (int t = 0; t < 3; t++) {
        Aether m; long fr = 0;
        setKnobs(m, 48000.0, 48000.0, 1.f, 0.7f, t);
        Stats s = feedSine(m, fr, 220.f, 4.f, 0.3, 0.4, Aether::SIGNAL_OUTPUT);
        char name[32];
        std::snprintf(name, sizeof(name), "type%d_alive", t + 1);
        report("aether", name, s.rms(), s.nans == 0 && s.rms() > 0.2);
    }
}

// Hot input, both CV attenuators wide open with audio-rate CV, clocks swept:
// nothing may go non-finite or run away.
static void testAbuse() {
    Aether m; long fr = 0;
    setKnobs(m, 48000.0, 48000.0, 1.f, 1.0f, aether::PD_RS);
    m.params[Aether::CARRIER_CV_PARAM].setValue(1.f);
    m.params[Aether::DEMOD_CV_PARAM].setValue(1.f);
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    m.inputs[Aether::CARRIER_CV_INPUT].channels = 1;
    m.inputs[Aether::DEMOD_CV_INPUT].channels = 1;
    Stats s, e;
    for (long i = 0; i < (long)(3.0 * SR); i++) {
        const double t = i / (double)SR;
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 110.0 * t));
        m.inputs[Aether::CARRIER_CV_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 3000.0 * t));
        m.inputs[Aether::DEMOD_CV_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 0.5 * t));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
        e.add(m.outputs[Aether::ERROR_OUTPUT].getVoltage());
    }
    report("aether", "abuse_nans", s.nans + e.nans, s.nans + e.nans == 0);
    report("aether", "abuse_bounded", std::max(s.peak, e.peak),
           std::max(s.peak, e.peak) <= 10.f);
    report("aether", "abuse_alive", s.rms(), s.rms() > 0.05);
}

// Every oversampling setting must work, and none may change what the module
// is doing: the engine is scheduled in continuous time, so the recovered
// level is the oversampler's business only.
static void testOversampling() {
    double got[5];
    long nans = 0;
    for (int idx = 0; idx < 5; idx++) {
        Aether m; long fr = 0;
        m.osIndex = idx;
        setKnobs(m, 96000.0, 96000.0, 1.f, 0.7f, aether::PD_PFD);
        got[idx] = surviving(m, fr, 220.f, 4.f, 0.3, 0.4);
        Stats s = feedSine(m, fr, 220.f, 4.f, 0.05, 0.2, Aether::SIGNAL_OUTPUT);
        nans += s.nans;
    }
    double lo = got[1], hi = got[1];
    for (int i = 1; i < 5; i++) {       // 1x cannot represent a 96 kHz clock
        lo = std::min(lo, got[i]);
        hi = std::max(hi, got[i]);
    }
    report("aether", "os_nans", nans, nans == 0);
    report("aether", "os_consistent", hi / std::max(lo, 1e-9), hi / lo < 1.3);
}


// ── the dry/wet mixes ───────────────────────────────────────────────────────
// Both signal outputs cross-fade against the signal at the jack, so that the
// module can sit in an effect send. The engine is deterministic, so the same
// input gives the same wet run every time and the mix can be checked against
// arithmetic rather than against a threshold.
struct MixRun {
    std::vector<float> in, out, err;
};

static MixRun runMix(float knob, float cv, bool cvPatched) {
    Aether m; long fr = 0;
    setKnobs(m, 96000.0, 96000.0, 1.f, 0.7f, aether::PD_PFD);
    m.params[Aether::OUT_MIX_PARAM].setValue(knob);
    m.params[Aether::ERROR_MIX_PARAM].setValue(knob);
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    if (cvPatched) {
        m.inputs[Aether::OUT_MIX_INPUT].channels = 1;
        m.inputs[Aether::ERROR_MIX_INPUT].channels = 1;
        m.inputs[Aether::OUT_MIX_INPUT].setVoltage(cv);
        m.inputs[Aether::ERROR_MIX_INPUT].setVoltage(cv);
    }
    MixRun r;
    double ph = 0.0;
    const double w = 2.0 * M_PI * 220.0 / SR;
    const long settle = (long)(0.3 * SR), meas = (long)(0.2 * SR);
    for (long i = 0; i < settle + meas; i++) {
        const float x = 4.f * (float)std::sin(ph);
        ph += w;
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(x);
        m.process(makeArgs(fr++));
        if (i >= settle) {
            r.in.push_back(x);
            r.out.push_back(m.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
            r.err.push_back(m.outputs[Aether::ERROR_OUTPUT].getVoltage());
        }
    }
    return r;
}

// biggest sample-by-sample distance between a run and what it should be
static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0.0;
    for (size_t i = 0; i < a.size(); i++)
        d = std::max(d, (double)std::fabs(a[i] - b[i]));
    return d;
}

static void testMix() {
    const MixRun dry = runMix(0.f, 0.f, false);
    const MixRun wet = runMix(1.f, 0.f, false);
    const MixRun half = runMix(0.5f, 0.f, false);

    // fully dry is the input jack itself, on both outputs
    report("aether", "mix_dry_is_input", maxDiff(dry.out, dry.in),
           maxDiff(dry.out, dry.in) < 1e-4);
    report("aether", "mix_dry_error_is_input", maxDiff(dry.err, dry.in),
           maxDiff(dry.err, dry.in) < 1e-4);
    // fully wet is what the module did before the mix existed
    report("aether", "mix_wet_differs", maxDiff(wet.out, wet.in),
           maxDiff(wet.out, wet.in) > 0.5);

    // and noon is exactly halfway between the two
    std::vector<float> mid(half.out.size());
    for (size_t i = 0; i < mid.size(); i++)
        mid[i] = 0.5f * (dry.in[i] + wet.out[i]);
    report("aether", "mix_half_is_midpoint", maxDiff(half.out, mid),
           maxDiff(half.out, mid) < 1e-4);

    // patched, the knob attenuates the CV: 10 V at a full knob is fully wet,
    // 10 V at noon is what noon was, and no volts is dry however the knob sits
    const MixRun cvFull = runMix(1.f, 10.f, true);
    const MixRun cvHalf = runMix(0.5f, 10.f, true);
    const MixRun cvZero = runMix(1.f, 0.f, true);
    report("aether", "mixcv_full_is_wet", maxDiff(cvFull.out, wet.out),
           maxDiff(cvFull.out, wet.out) < 1e-4);
    report("aether", "mixcv_knob_attenuates", maxDiff(cvHalf.out, half.out),
           maxDiff(cvHalf.out, half.out) < 1e-4);
    report("aether", "mixcv_zero_is_dry", maxDiff(cvZero.out, cvZero.in),
           maxDiff(cvZero.out, cvZero.in) < 1e-4);
    // negative CV cannot push past dry
    const MixRun cvNeg = runMix(1.f, -5.f, true);
    report("aether", "mixcv_negative_clamps", maxDiff(cvNeg.out, cvNeg.in),
           maxDiff(cvNeg.out, cvNeg.in) < 1e-4);
}

// ── TONE CV ─────────────────────────────────────────────────────────────────
// The trimpot is an attenuverter and the CV is in octaves per volt, on the
// same span the knob covers. So the arithmetic is checkable: a knob at t with
// v volts through a fully open trimpot has to be the knob alone at
// (kToneOctaves*t + v)/kToneOctaves, sample for sample.
static std::vector<float> runTone(float knob, float amt, float cv,
                                  bool cvPatched) {
    Aether m; long fr = 0;
    setKnobs(m, 96000.0, 96000.0, 1.f, knob, aether::PD_PFD);
    m.params[Aether::TONE_CV_PARAM].setValue(amt);
    m.inputs[Aether::SIGNAL_INPUT].channels = 1;
    if (cvPatched) {
        m.inputs[Aether::TONE_CV_INPUT].channels = 1;
        m.inputs[Aether::TONE_CV_INPUT].setVoltage(cv);
    }
    std::vector<float> out;
    double ph = 0.0;
    const double w = 2.0 * M_PI * 2000.0 / SR;
    const long settle = (long)(0.3 * SR), meas = (long)(0.2 * SR);
    for (long i = 0; i < settle + meas; i++) {
        m.inputs[Aether::SIGNAL_INPUT].setVoltage(4.f * (float)std::sin(ph));
        ph += w;
        m.process(makeArgs(fr++));
        if (i >= settle) out.push_back(m.outputs[Aether::SIGNAL_OUTPUT].getVoltage());
    }
    return out;
}

static double peakOf(const std::vector<float>& v) {
    double p = 0.0;
    for (float x : v) p = std::max(p, (double)std::fabs(x));
    return p;
}

static void testToneCv() {
    const float t0 = 0.3f;
    const float cv = 3.f;
    const float equiv = (float)(((double)aether::kToneOctaves * t0 + cv)
                                / aether::kToneOctaves);

    const std::vector<float> plain = runTone(t0, 0.f, 0.f, false);
    const std::vector<float> up = runTone(t0, 1.f, cv, true);
    const std::vector<float> knobUp = runTone(equiv, 0.f, 0.f, false);

    // +3 V through an open trimpot is the knob three octaves higher
    report("aether", "tonecv_is_octaves", maxDiff(up, knobUp),
           maxDiff(up, knobUp) < 1e-4);
    // and it did something: 2 kHz gets through the wider pole
    report("aether", "tonecv_opens", peakOf(up) / std::max(peakOf(plain), 1e-9),
           peakOf(up) > peakOf(plain) * 1.5);

    // a shut trimpot is no CV at all, whatever is patched
    const std::vector<float> shut = runTone(t0, 0.f, 5.f, true);
    report("aether", "tonecv_amt_zero_is_knob", maxDiff(shut, plain),
           maxDiff(shut, plain) < 1e-4);

    // the trimpot inverts: the same volts darken instead
    const std::vector<float> down = runTone(0.7f, -1.f, cv, true);
    const std::vector<float> plainHi = runTone(0.7f, 0.f, 0.f, false);
    report("aether", "tonecv_inverts", peakOf(down) / std::max(peakOf(plainHi), 1e-9),
           peakOf(down) < peakOf(plainHi));

    // and it cannot be driven past either end of the knob's own span
    const std::vector<float> top = runTone(1.f, 1.f, 10.f, true);
    const std::vector<float> knobTop = runTone(1.f, 0.f, 0.f, false);
    report("aether", "tonecv_clamps_high", maxDiff(top, knobTop),
           maxDiff(top, knobTop) < 1e-4);
    const std::vector<float> bot = runTone(0.f, -1.f, 10.f, true);
    const std::vector<float> knobBot = runTone(0.f, 0.f, 0.f, false);
    report("aether", "tonecv_clamps_low", maxDiff(bot, knobBot),
           maxDiff(bot, knobBot) < 1e-4);
}

SMOKE_MAIN(testRecovery, testNoLock, testCarrierCrush, testStandalone,
           testClockOuts, testExternalClock, testError, testTypes,
           testAbuse, testOversampling, testMix, testToneCv)
