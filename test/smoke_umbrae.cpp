// smoke_umbrae — offline sanity checks for the umbrae module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The loop's own physics is measured by umbrae_probe, which does not involve
// Rack. These are the module's own checks: the jacks, the normalling, the
// switches, the external loop, and the one property the whole port is built
// around — the howl's pitch is a property of the circuit, so it may not move
// when the oversampling ratio does.

#include "smoke_harness.hpp"
#include "../src/umbrae.cpp"

static void setFaders(Umbrae& m, float drive, float bass, float treble,
                      float fbk, float xfade) {
    m.params[Umbrae::DRIVE_PARAM].setValue(drive);
    m.params[Umbrae::BASS_PARAM].setValue(bass);
    m.params[Umbrae::TREBLE_PARAM].setValue(treble);
    m.params[Umbrae::FBK_PARAM].setValue(fbk);
    m.params[Umbrae::XFADE_PARAM].setValue(xfade);
}

// Pitch by hysteresis crossings, as in the probe: count the fundamental
// rather than every wiggle a harmonic puts on it.
static double pitchOf(const std::vector<float>& y) {
    float peak = 0.f;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak < 1e-3f) return 0.0;
    const float th = 0.2f * peak;
    int cycles = 0;
    bool armed = false;
    long first = -1, last = -1;
    for (size_t i = 0; i < y.size(); i++) {
        if (y[i] < -th) armed = true;
        else if (armed && y[i] > th) {
            armed = false;
            if (first < 0) first = (long)i; else last = (long)i;
            cycles++;
        }
    }
    if (cycles < 3 || last <= first) return 0.0;
    return (cycles - 1) * SR / (double)(last - first);
}

// Let a patch run with nothing in the input and collect the output.
static std::vector<float> freeRun(Umbrae& m, long& frame, double settleS,
                                  double measS, Stats* stats = nullptr) {
    for (long i = 0; i < (long)(settleS * SR); i++) m.process(makeArgs(frame++));
    std::vector<float> ys;
    ys.reserve((size_t)(measS * SR));
    for (long i = 0; i < (long)(measS * SR); i++) {
        m.process(makeArgs(frame++));
        const float y = m.outputs[Umbrae::XFADE_OUTPUT].getVoltage();
        ys.push_back(y);
        if (stats) stats->add(y);
    }
    return ys;
}

static Stats feedSine(Umbrae& m, long& frame, float f, float amp,
                      double settleS, double measS, int outId) {
    m.inputs[Umbrae::SIGNAL_INPUT].channels = 1;
    double ph = 0.0;
    const double w = 2.0 * M_PI * f / SR;
    for (long i = 0; i < (long)(settleS * SR); i++) {
        m.inputs[Umbrae::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
    }
    Stats s;
    for (long i = 0; i < (long)(measS * SR); i++) {
        m.inputs[Umbrae::SIGNAL_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        m.process(makeArgs(frame++));
        s.add(m.outputs[outId].getVoltage());
    }
    return s;
}

// ── the drive stage ─────────────────────────────────────────────────────────
// x3 gain and soft clipping at +-5 V is the one piece of the signal path the
// hardware's manual gives numbers for.
static void testDrive() {
    Umbrae m; long fr = 0;
    setFaders(m, 1.f, 0.f, 0.f, 0.f, 0.f);
    m.params[Umbrae::SRC_PARAM].setValue(1.f);      // clean side = post drive
    Stats small = feedSine(m, fr, 200.f, 0.2f, 0.1, 0.2, Umbrae::XFADE_OUTPUT);
    const double gain = small.rms() / (0.2 / std::sqrt(2.0));
    report("umbrae", "drive_x3", gain, gain > 2.85 && gain < 3.05);

    Umbrae hot; long fh = 0;
    setFaders(hot, 1.f, 0.f, 0.f, 0.f, 0.f);
    hot.params[Umbrae::SRC_PARAM].setValue(1.f);
    Stats big = feedSine(hot, fh, 200.f, 9.f, 0.1, 0.2, Umbrae::XFADE_OUTPUT);
    report("umbrae", "drive_clips_at_5v", big.peak,
           big.peak > 4.5f && big.peak <= 5.2f);
    report("umbrae", "drive_nans", small.nans + big.nans,
           small.nans + big.nans == 0);
}

// ── the loop ────────────────────────────────────────────────────────────────
// Below unity the module is quiet with nothing patched; above it, it howls,
// and which band is up decides the register.
static void testHowl() {
    Umbrae quiet; long fq = 0;
    setFaders(quiet, 0.f, 1.f, 1.f, 0.2f, 1.f);
    Stats sq;
    freeRun(quiet, fq, 1.5, 0.5, &sq);
    report("umbrae", "quiet_below_unity", sq.rms(), sq.rms() < 1e-3);

    Umbrae low; long fl = 0;
    setFaders(low, 0.f, 1.f, 0.f, 1.f, 1.f);
    Stats sl;
    std::vector<float> yl = freeRun(low, fl, 2.0, 1.0, &sl);
    const double hzLow = pitchOf(yl);
    report("umbrae", "howl_nans", sl.nans, sl.nans == 0);
    report("umbrae", "howl_alive", sl.rms(), sl.rms() > 0.5);
    report("umbrae", "howl_bounded", sl.peak, sl.peak <= 10.f);
    report("umbrae", "howl_bass_low", hzLow, hzLow > 60.0 && hzLow < 200.0);

    Umbrae high; long fh = 0;
    setFaders(high, 0.f, 0.f, 1.f, 1.f, 1.f);
    Stats sh;
    std::vector<float> yh = freeRun(high, fh, 2.0, 1.0, &sh);
    const double hzHigh = pitchOf(yh);
    report("umbrae", "howl_treble_high", hzHigh,
           hzHigh > 1500.0 && hzHigh < 6000.0);
    report("umbrae", "howl_registers_apart", hzHigh / std::max(hzLow, 1.0),
           hzHigh > 8.0 * hzLow);
}

// ── the property the port exists for ────────────────────────────────────────
// The howl's pitch is the circuit's, so changing how finely the circuit is
// simulated may not change it.
static void testOversamplingPitch() {
    double hz[3] = {0, 0, 0};
    long nans = 0;
    for (int i = 0; i < 3; i++) {
        Umbrae m; long fr = 0;
        m.osIndex = Umbrae::kMinOsIndex + i;      // 4x, 8x, 16x
        setFaders(m, 0.f, 1.f, 0.f, 1.f, 1.f);
        m.params[Umbrae::BASS_BOOST_PARAM].setValue(0.5f);
        Stats s;
        std::vector<float> y = freeRun(m, fr, 2.0, 1.0, &s);
        hz[i] = pitchOf(y);
        nans += s.nans;
    }
    double lo = hz[0], hi = hz[0];
    for (int i = 1; i < 3; i++) { lo = std::min(lo, hz[i]); hi = std::max(hi, hz[i]); }
    report("umbrae", "os_nans", nans, nans == 0);
    report("umbrae", "os_pitch_stable", hi / std::max(lo, 1e-9),
           lo > 0.0 && hi / lo < 1.05);
}

// ── the envelope follower ───────────────────────────────────────────────────
static void testDynamics() {
    for (int decay = 0; decay < 2; decay++) {
        Umbrae m; long fr = 0;
        setFaders(m, 1.f, 0.f, 0.f, 0.f, 0.f);
        m.params[Umbrae::DECAY_PARAM].setValue((float)decay);
        Stats s = feedSine(m, fr, 200.f, 5.f, 0.2, 0.2, Umbrae::DYNAMICS_OUTPUT);
        const double held = m.outputs[Umbrae::DYNAMICS_OUTPUT].getVoltage();
        m.inputs[Umbrae::SIGNAL_INPUT].setVoltage(0.f);
        long fell = -1;
        for (long i = 0; i < (long)(2.0 * SR); i++) {
            m.process(makeArgs(fr++));
            if (m.outputs[Umbrae::DYNAMICS_OUTPUT].getVoltage() < 0.368 * held) {
                fell = i;
                break;
            }
        }
        const double ms = fell < 0 ? 1e9 : 1000.0 * fell / SR;
        char name[40];
        std::snprintf(name, sizeof(name), "env_%s_decay_ms",
                      decay ? "long" : "short");
        report("umbrae", name, ms,
               decay ? (ms > 400 && ms < 900) : (ms > 30 && ms < 120));
        std::snprintf(name, sizeof(name), "env_%s_level", decay ? "long" : "short");
        report("umbrae", name, held, held > 3.f && held <= 5.f && s.nans == 0);
    }
}

// DYNAMICS is normalled to the feedback CV, so a signal going in gates the
// feedback it causes. With the attenuverter up, an input has to make the loop
// louder than it is with none.
static void testNormalling() {
    Umbrae idle; long fi = 0;
    setFaders(idle, 1.f, 1.f, 1.f, 0.f, 1.f);
    idle.params[Umbrae::FBK_CV_PARAM].setValue(1.f);
    Stats si;
    freeRun(idle, fi, 1.0, 0.5, &si);

    Umbrae driven; long fd = 0;
    setFaders(driven, 1.f, 1.f, 1.f, 0.f, 1.f);
    driven.params[Umbrae::FBK_CV_PARAM].setValue(1.f);
    Stats sd = feedSine(driven, fd, 90.f, 4.f, 1.0, 0.5, Umbrae::XFADE_OUTPUT);
    report("umbrae", "env_normalled_to_fbk", sd.rms() / std::max(si.rms(), 1e-6),
           sd.rms() > 10.0 * si.rms());
}

// ── the loop taken outside ──────────────────────────────────────────────────
// Patching the return breaks the internal loop: with nothing coming back the
// howl stops. Feed the send back by hand and it starts again.
static void testExternalLoop() {
    Umbrae open; long fo = 0;
    setFaders(open, 0.f, 1.f, 1.f, 1.f, 1.f);
    open.inputs[Umbrae::RETURN_INPUT].channels = 1;
    open.inputs[Umbrae::RETURN_INPUT].setVoltage(0.f);
    Stats so;
    freeRun(open, fo, 1.5, 0.5, &so);
    report("umbrae", "return_breaks_loop", so.rms(), so.rms() < 1e-3);

    Umbrae closed; long fc = 0;
    setFaders(closed, 0.f, 1.f, 1.f, 1.f, 1.f);
    closed.inputs[Umbrae::RETURN_INPUT].channels = 1;
    Stats sc;
    long nans = 0;
    float peak = 0.f;
    double s2 = 0.0;
    long n = 0;
    for (long i = 0; i < (long)(2.5 * SR); i++) {
        // the send, straight back into the return: the loop closed outside
        closed.inputs[Umbrae::RETURN_INPUT].setVoltage(
            closed.outputs[Umbrae::SEND_OUTPUT].getVoltage());
        closed.process(makeArgs(fc++));
        const float y = closed.outputs[Umbrae::XFADE_OUTPUT].getVoltage();
        if (!std::isfinite(y)) nans++;
        if (i > (long)(1.5 * SR)) { s2 += y * y; n++; peak = std::max(peak, std::fabs(y)); }
    }
    const double rms = n ? std::sqrt(s2 / n) : 0.0;
    report("umbrae", "extloop_nans", nans, nans == 0);
    report("umbrae", "extloop_howls", rms, rms > 0.3);
    report("umbrae", "extloop_bounded", peak, peak <= 10.f);
    (void)sc;
}

// The polarity switch inverts the send, which is what saves a loop that runs
// through something that inverts.
static void testPolarity() {
    Umbrae a; long fa = 0;
    setFaders(a, 1.f, 1.f, 1.f, 0.5f, 1.f);
    Stats sa = feedSine(a, fa, 200.f, 2.f, 0.2, 0.2, Umbrae::SEND_OUTPUT);

    Umbrae b; long fb = 0;
    setFaders(b, 1.f, 1.f, 1.f, 0.5f, 1.f);
    b.params[Umbrae::POLARITY_PARAM].setValue(1.f);
    Stats sb = feedSine(b, fb, 200.f, 2.f, 0.2, 0.2, Umbrae::SEND_OUTPUT);
    report("umbrae", "polarity_same_level",
           sb.rms() / std::max(sa.rms(), 1e-9),
           sb.rms() > 0.9 * sa.rms() && sb.rms() < 1.1 * sa.rms());
}

// ── the crossfader ──────────────────────────────────────────────────────────
static void testXfade() {
    Umbrae clean; long fc = 0;
    setFaders(clean, 1.f / 3.f, 0.f, 0.f, 0.f, 0.f);
    clean.params[Umbrae::SRC_PARAM].setValue(1.f);
    Stats sc = feedSine(clean, fc, 200.f, 2.f, 0.1, 0.2, Umbrae::XFADE_OUTPUT);

    Umbrae looped; long fl = 0;
    setFaders(looped, 1.f / 3.f, 0.f, 0.f, 0.f, 1.f);   // tone bands shut
    Stats sl = feedSine(looped, fl, 200.f, 2.f, 0.1, 0.2, Umbrae::XFADE_OUTPUT);
    report("umbrae", "xfade_clean_side", sc.rms(), sc.rms() > 1.2 && sc.rms() < 1.6);
    report("umbrae", "xfade_other_side", sl.rms(), sl.rms() < 0.05);
}

// The HF warning watches the loop, so it should light on a treble howl and
// stay dark on a bass one.
static void testHfLight() {
    Umbrae low; long fl = 0;
    setFaders(low, 0.f, 1.f, 0.f, 1.f, 1.f);
    freeRun(low, fl, 2.0, 0.5);
    const float hfLow = low.lights[Umbrae::HF_LIGHT].getBrightness();

    Umbrae high; long fh = 0;
    setFaders(high, 0.f, 0.f, 1.f, 1.f, 1.f);
    freeRun(high, fh, 2.0, 0.5);
    const float hfHigh = high.lights[Umbrae::HF_LIGHT].getBrightness();
    report("umbrae", "hf_light_on_treble", hfHigh, hfHigh > 0.3f);
    report("umbrae", "hf_light_off_bass", hfLow, hfLow < hfHigh * 0.5f);
}

// Everything up, hyper drive on, hot input, all three CV inputs driven at
// audio rate: nothing may go non-finite or run away.
static void testAbuse() {
    Umbrae m; long fr = 0;
    setFaders(m, 1.f, 1.f, 1.f, 1.f, 1.f);
    m.params[Umbrae::BASS_BOOST_PARAM].setValue(1.f);
    m.params[Umbrae::TREBLE_BOOST_PARAM].setValue(1.f);
    m.params[Umbrae::HYPER_PARAM].setValue(1.f);
    m.params[Umbrae::DRIVE_CV_PARAM].setValue(1.f);
    m.params[Umbrae::FBK_CV_PARAM].setValue(-1.f);
    m.params[Umbrae::XFADE_CV_PARAM].setValue(1.f);
    m.inputs[Umbrae::SIGNAL_INPUT].channels = 1;
    m.inputs[Umbrae::DRIVE_CV_INPUT].channels = 1;
    m.inputs[Umbrae::FBK_CV_INPUT].channels = 1;
    m.inputs[Umbrae::BASS_CV_INPUT].channels = 1;
    Stats out, send, env;
    for (long i = 0; i < (long)(3.0 * SR); i++) {
        const double t = i / (double)SR;
        m.inputs[Umbrae::SIGNAL_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 110.0 * t));
        m.inputs[Umbrae::DRIVE_CV_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 700.0 * t));
        m.inputs[Umbrae::FBK_CV_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 3.0 * t));
        m.inputs[Umbrae::BASS_CV_INPUT].setVoltage(
            10.f * std::sin(2.0 * M_PI * 0.5 * t));
        m.process(makeArgs(fr++));
        out.add(m.outputs[Umbrae::XFADE_OUTPUT].getVoltage());
        send.add(m.outputs[Umbrae::SEND_OUTPUT].getVoltage());
        env.add(m.outputs[Umbrae::DYNAMICS_OUTPUT].getVoltage());
    }
    report("umbrae", "abuse_nans", out.nans + send.nans + env.nans,
           out.nans + send.nans + env.nans == 0);
    report("umbrae", "abuse_bounded", std::max(out.peak, send.peak),
           std::max(out.peak, send.peak) <= 10.f);
    report("umbrae", "abuse_env_unipolar", env.peak,
           env.peak <= 5.05f && env.sum >= 0.0);
    report("umbrae", "abuse_alive", out.rms(), out.rms() > 0.1);
}

SMOKE_MAIN(testDrive, testHowl, testOversamplingPitch, testDynamics,
           testNormalling, testExternalLoop, testPolarity, testXfade,
           testHfLight, testAbuse)
