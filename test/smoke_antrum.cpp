// smoke_antrum — offline sanity checks for the antrum reverb.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/antrum.cpp"

static uint32_t rngState = 0x12345678u;
static float noise() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return ((float)(rngState >> 8) * (1.f / 16777216.f) - 0.5f) * 10.f;
}

// run `secs` of audio, optionally feeding noise, collecting both outputs
static void run(Antrum& m, long& frame, double secs, bool feed, Stats* s = nullptr,
                Stats* cv = nullptr) {
    for (int i = 0; i < (int)(secs * SR); i++) {
        m.inputs[Antrum::LEFT_INPUT].setVoltage(feed ? noise() : 0.f);
        m.process(makeArgs(frame++));
        if (s) {
            s->add(m.outputs[Antrum::LEFT_OUTPUT].getVoltage());
            s->add(m.outputs[Antrum::RIGHT_OUTPUT].getVoltage());
        }
        if (cv) cv->add(m.outputs[Antrum::CV_OUTPUT].getVoltage());
    }
}

static void testQuiet() {
    Antrum m;
    long frame = 0;
    Stats s;
    run(m, frame, 2.0, false, &s);
    report("antrum", "silent_without_input", s.rms(), s.rms() < 1e-4);
    report("antrum", "silent_nans", s.nans, s.nans == 0);
}

static void testDryPath() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(0.f);
    run(m, frame, 0.2, true);   // let the smoothers settle
    double err = 0.0, ref = 0.0;
    for (int i = 0; i < (int)(0.5 * SR); i++) {
        float in = noise();
        m.inputs[Antrum::LEFT_INPUT].setVoltage(in);
        m.process(makeArgs(frame++));
        err = std::max(err, (double)std::fabs(m.outputs[Antrum::LEFT_OUTPUT].getVoltage() - in));
        ref = std::max(ref, (double)std::fabs(in));
    }
    report("antrum", "dry_passthrough_err", err, err < 1e-3 * ref);
}

static void testTail() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::DECAY_PARAM].setValue(0.6f);
    run(m, frame, 1.0, true);            // excite the network
    Stats wet;
    run(m, frame, 0.2, false, &wet);     // just after the input stops
    Stats late;
    run(m, frame, 8.0, false);           // let it ring out
    run(m, frame, 1.0, false, &late);
    report("antrum", "tail_alive", wet.rms(), wet.rms() > 0.05);
    report("antrum", "tail_decays", late.rms(), late.rms() < wet.rms() * 0.2);
}

static void testCvOut() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    Stats cvQuiet;
    run(m, frame, 0.5, false, nullptr, &cvQuiet);
    Stats cvLoud;
    run(m, frame, 1.0, true);
    run(m, frame, 0.5, true, nullptr, &cvLoud);
    report("antrum", "cv_quiet", cvQuiet.peak, cvQuiet.peak < 0.01);
    report("antrum", "cv_follows_energy", cvLoud.rms(), cvLoud.rms() > 0.2);
    report("antrum", "cv_bounded", cvLoud.peak, cvLoud.peak <= 10.001f);
}

// infinite decay, no damping, wide open: the saturation and the level
// compensation have to keep the network from running away
static void testInfinite() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::DECAY_PARAM].setValue(1.2f);
    m.params[Antrum::ABSORB_PARAM].setValue(0.f);
    run(m, frame, 1.0, true);
    Stats s;
    run(m, frame, 20.0, false, &s);
    report("antrum", "infinite_nans", s.nans, s.nans == 0);
    report("antrum", "infinite_sustains", s.rms(), s.rms() > 0.05);
    report("antrum", "infinite_bounded", s.peak, s.peak <= 10.001f);
}

// everything hostile at once: infinite decay, shimmer, reverse, and size
// swept at audio rate while the pre-delay is modulated too
static void testHostile() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::DECAY_PARAM].setValue(1.2f);
    m.params[Antrum::ABSORB_PARAM].setValue(0.f);
    m.params[Antrum::DEPTH_PARAM].setValue(1.f);      // ergodic + shimmer
    m.params[Antrum::SPEED_PARAM].setValue(0.9f);
    m.params[Antrum::TILT_PARAM].setValue(-1.f);
    m.params[Antrum::REVERSE_PARAM].setValue(1.f);    // latch reverse on
    m.process(makeArgs(frame++));
    m.params[Antrum::REVERSE_PARAM].setValue(0.f);
    Stats s;
    for (int i = 0; i < (int)(20 * SR); i++) {
        float t = (float)i / SR;
        m.params[Antrum::SIZE_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 300.f * t));
        m.params[Antrum::PREDELAY_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 3.f * t));
        m.inputs[Antrum::LEFT_INPUT].setVoltage(t < 4.f ? noise() : 0.f);
        m.inputs[Antrum::RIGHT_INPUT].setVoltage(t < 4.f ? noise() : 0.f);
        m.process(makeArgs(frame++));
        s.add(m.outputs[Antrum::LEFT_OUTPUT].getVoltage());
        s.add(m.outputs[Antrum::RIGHT_OUTPUT].getVoltage());
    }
    report("antrum", "hostile_nans", s.nans, s.nans == 0);
    report("antrum", "hostile_alive", s.rms(), s.rms() > 0.01);
    report("antrum", "hostile_bounded", s.peak, s.peak <= 10.001f);
}

// decay is the loop gain only, so at 0 the network still passes one
// diffused pass and the early reflections: short, but not silence
static void testZeroDecay() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::DECAY_PARAM].setValue(0.f);
    Stats wet;
    run(m, frame, 1.0, true, &wet);
    Stats tail;
    run(m, frame, 0.1, false, &tail);   // the single pass still has to arrive
    run(m, frame, 0.9, false);          // the allpass diffusers ring on a while
    Stats gone;
    run(m, frame, 1.0, false, &gone);   // and then there is nothing left
    report("antrum", "zero_decay_passes_signal", wet.rms(), wet.rms() > 0.5);
    report("antrum", "zero_decay_short_tail", tail.rms(), tail.rms() > 0.05);
    report("antrum", "zero_decay_no_sustain", gone.rms(), gone.rms() < wet.rms() * 0.001);
}

// unlinked shimmer: with depth at noon nothing else modulates, so the
// octave-up voice is the only thing the menu setting can add. At full
// shimmer it replaces the direct injection into the network, so what moves
// is the tail's spectrum, not its level: the fundamental collapses and the
// octave (and the octave above that, shimmer feeding on itself) takes over.
static double binMag(const std::vector<float>& x, double freq) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < x.size(); i++) {
        double a = 2.0 * M_PI * freq * i / SR;
        re += x[i] * std::cos(a);
        im += x[i] * std::sin(a);
    }
    return std::sqrt(re * re + im * im) / x.size();
}

// excite with a 200 Hz sine, then capture the tail
static void tailOf(Antrum& m, long& frame, std::vector<float>& tail) {
    for (int i = 0; i < (int)(1.5 * SR); i++) {
        m.inputs[Antrum::LEFT_INPUT].setVoltage(
            5.f * std::sin(2.f * M_PI * 200.f * i / SR));
        m.process(makeArgs(frame++));
    }
    tail.resize((int)(0.5 * SR));
    for (size_t i = 0; i < tail.size(); i++) {
        m.inputs[Antrum::LEFT_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        tail[i] = m.outputs[Antrum::LEFT_OUTPUT].getVoltage();
    }
}

static void setShimmerCase(Antrum& m, int mode) {
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::DECAY_PARAM].setValue(0.9f);
    m.params[Antrum::ABSORB_PARAM].setValue(0.34f);   // diffusion, no damping
    m.shimmerMode = mode;
}

static void testShimmerUnlinked() {
    long frame = 0;
    Antrum off;
    setShimmerCase(off, 0);          // folded into depth, depth at noon = none
    std::vector<float> offTail;
    tailOf(off, frame, offTail);

    frame = 0;
    Antrum on;
    setShimmerCase(on, 4);           // unlinked at 100%, depth still at noon
    std::vector<float> onTail;
    tailOf(on, frame, onTail);

    double offRatio = binMag(offTail, 400.) / binMag(offTail, 200.);
    double onRatio = binMag(onTail, 400.) / binMag(onTail, 200.);
    Stats s;
    for (float v : onTail) s.add(v);
    report("antrum", "shimmer_off_keeps_fundamental", offRatio, offRatio < 0.5);
    report("antrum", "shimmer_unlinked_octave_up", onRatio, onRatio > 5.0);
    report("antrum", "shimmer_unlinked_nans", s.nans, s.nans == 0);
    report("antrum", "shimmer_unlinked_bounded", s.peak, s.peak <= 10.001f);
}

// clock sync: a 2 Hz clock with the pre-delay knob at noon should land on
// one clock period of pre-delay
static void testClockSync() {
    Antrum m;
    long frame = 0;
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::PREDELAY_PARAM].setValue(0.5f);
    // isConnected() is channel-count based and setChannels() is a no-op on a
    // disconnected port, so fake the cable the only way an offline test can
    m.inputs[Antrum::CLOCK_INPUT].channels = 1;
    int period = (int)(SR / 2.f);
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.inputs[Antrum::CLOCK_INPUT].setVoltage((i % period) < 100 ? 5.f : 0.f);
        m.inputs[Antrum::LEFT_INPUT].setVoltage(noise());
        m.process(makeArgs(frame++));
    }
    float want = 500.f;   // ms
    float got = m.p.predelayMs;
    report("antrum", "clock_sync_predelay_ms", got, std::fabs(got - want) < 5.f);
}

SMOKE_MAIN(testQuiet, testDryPath, testTail, testCvOut, testZeroDecay, testInfinite,
           testHostile, testShimmerUnlinked, testClockSync)
