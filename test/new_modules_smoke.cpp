// new_modules_smoke — offline sanity checks for the v2.7.0 modules
// (rete, ululo, tabes, lustro, bulla). Drives each module's process()
// directly and checks for non-finite samples, runaway levels and basic
// expected behavior (self-oscillation, loop decay, pluck response).
//
// Prints one CSV row per check: module,check,value,pass
// Exits nonzero if any check fails.

#include <rack.hpp>

// pluginInstance is normally defined in forsitan.cpp; the harness never
// loads assets so a null plugin is fine.
rack::plugin::Plugin* pluginInstance = nullptr;

#include "../src/rete.cpp"
#include "../src/ululo.cpp"
#include "../src/tabes.cpp"
#include "../src/lustro.cpp"
#include "../src/bulla.cpp"

#include <cstdio>
#include <cmath>

static const float SR = 48000.f;
static int failures = 0;

static void report(const char* mod, const char* check, double value, bool pass) {
    printf("%s,%s,%g,%s\n", mod, check, value, pass ? "PASS" : "FAIL");
    if (!pass) failures++;
}

struct Stats {
    double sum = 0, sum2 = 0;
    float peak = 0;
    long nans = 0;
    long n = 0;
    void add(float v) {
        if (!std::isfinite(v)) { nans++; v = 0.f; }
        sum += v; sum2 += v * v;
        peak = std::max(peak, std::fabs(v));
        n++;
    }
    double rms() const { return n ? std::sqrt(sum2 / n) : 0.0; }
};

static Module::ProcessArgs makeArgs(long frame) {
    Module::ProcessArgs args;
    args.sampleRate = SR;
    args.sampleTime = 1.f / SR;
    args.frame = frame;
    return args;
}

static void testRete() {
    Rete m;
    long frame = 0;
    // simulate a connected poly cable (engine keeps disconnected outs at 0)
    m.outputs[Rete::POLY_OUTPUT].channels = 1;
    // defaults; 2 s warmup, 8 s measure
    for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
    Stats s;
    for (int i = 0; i < (int)(8 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Rete::LEFT_OUTPUT].getVoltage());
        s.add(m.outputs[Rete::RIGHT_OUTPUT].getVoltage());
    }
    report("rete", "nans", s.nans, s.nans == 0);
    report("rete", "self_osc_rms", s.rms(), s.rms() > 0.05);
    report("rete", "peak", s.peak, s.peak < 12.f);
    report("rete", "poly_channels", m.outputs[Rete::POLY_OUTPUT].getChannels(),
           m.outputs[Rete::POLY_OUTPUT].getChannels() == 8);
    // reseed via trigger input must not blow up
    m.inputs[Rete::RND_INPUT].channels = 1;
    Stats s2;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.inputs[Rete::RND_INPUT].setVoltage((i % 24000) < 100 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        s2.add(m.outputs[Rete::LEFT_OUTPUT].getVoltage());
    }
    report("rete", "reseed_nans", s2.nans, s2.nans == 0);
}

static void testUlulo() {
    Ululo m;
    long frame = 0;
    m.params[Ululo::GAIN_PARAM].setValue(1.5f);   // well into howling range
    for (int i = 0; i < (int)(4 * SR); i++) m.process(makeArgs(frame++));
    Stats s;
    for (int i = 0; i < (int)(6 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "nans", s.nans, s.nans == 0);
    report("ululo", "howl_rms", s.rms(), s.rms() > 0.05);
    report("ululo", "peak", s.peak, s.peak < 6.f);   // tanh-bounded * 5V
    // gain at zero must ring down to silence (decay knob still allows several
    // seconds of legitimate string ring-out)
    m.params[Ululo::GAIN_PARAM].setValue(0.f);
    m.params[Ululo::DECAY_PARAM].setValue(0.5f);
    for (int i = 0; i < (int)(8 * SR); i++) m.process(makeArgs(frame++));
    Stats s3;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        s3.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "silent_at_zero_gain", s3.rms(), s3.rms() < 0.01);
}

static void testTabes() {
    Tabes m;
    long frame = 0;
    m.params[Tabes::DECAY_PARAM].setValue(0.8f);
    m.monitorMode = Tabes::MONITOR_NEVER;
    m.inputs[Tabes::AUDIO_INPUT].channels = 1;
    m.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    // record 2 s of a 220 Hz sine at +-5 V
    float phase = 0.f;
    m.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(2 * SR); i++) {
        phase += 220.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Tabes::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.process(makeArgs(frame++));
    }
    m.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    m.inputs[Tabes::AUDIO_INPUT].setVoltage(0.f);
    // first pass
    Stats p1;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        p1.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
    }
    // let it age 28 more passes, then measure again
    Stats mid;
    for (int i = 0; i < (int)(2 * 28 * SR); i++) {
        m.process(makeArgs(frame++));
        mid.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
    }
    Stats p30;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        p30.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
    }
    report("tabes", "nans", p1.nans + mid.nans + p30.nans,
           p1.nans + mid.nans + p30.nans == 0);
    report("tabes", "pass1_rms", p1.rms(), p1.rms() > 1.0);
    report("tabes", "pass30_rms", p30.rms(), p30.rms() < 0.7 * p1.rms());
    float age = m.outputs[Tabes::AGE_OUTPUT].getVoltage();
    report("tabes", "age_cv", age, age > 2.5f && age <= 10.f);
    // splice restores the original level
    m.params[Tabes::SPLICE_PARAM].setValue(1.f);
    m.process(makeArgs(frame++));
    m.params[Tabes::SPLICE_PARAM].setValue(0.f);
    Stats ps;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        ps.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
    }
    report("tabes", "splice_rms", ps.rms(), ps.rms() > 0.8 * p1.rms());

    // clickless: record a sine whose end doesn't align with its beginning
    // (a quarter period off), keep the input running, and check the output
    // never steps across the stop handoff or the loop seam
    Tabes m2;
    long f2 = 0;
    m2.params[Tabes::WOW_PARAM].setValue(0.f);
    m2.inputs[Tabes::AUDIO_INPUT].channels = 1;
    m2.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    float ph = 0.f;
    auto sine = [&]() {
        ph += 220.f / SR; if (ph >= 1.f) ph -= 1.f;
        return 5.f * std::sin(2.f * M_PI * ph);
    };
    m2.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    int n = (int)SR + 55;
    for (int i = 0; i < n; i++) {
        m2.inputs[Tabes::AUDIO_INPUT].setVoltage(sine());
        m2.process(makeArgs(f2++));
    }
    m2.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    float prev = m2.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
    float maxStep = 0.f;
    for (int i = 0; i < 3 * n; i++) {
        m2.inputs[Tabes::AUDIO_INPUT].setVoltage(sine());
        m2.process(makeArgs(f2++));
        float v = m2.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
        maxStep = std::max(maxStep, std::fabs(v - prev));
        prev = v;
    }
    report("tabes", "seam_max_step", maxStep, maxStep < 1.f);

    // empty tape in the default monitor mode passes the input through
    Tabes m3;
    long f3 = 0;
    m3.inputs[Tabes::AUDIO_INPUT].channels = 1;
    float thruMin = 10.f;
    for (int i = 0; i < 100; i++) {
        m3.inputs[Tabes::AUDIO_INPUT].setVoltage(4.f);
        m3.process(makeArgs(f3++));
        if (i > 0)   // first sample initializes sr / buffers
            thruMin = std::min(thruMin, m3.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
    }
    report("tabes", "empty_monitor_thru", thruMin, std::fabs(thruMin - 4.f) < 1e-3f);

    // clickless rec START: press rec while a loop plays (wow displaces the
    // play head, so the loop-cut point is arbitrary) and the output must not
    // step as monitoring crossfades in over the loop
    Tabes m4;
    long f4 = 0;
    m4.params[Tabes::WOW_PARAM].setValue(0.5f);   // head well off the grid
    m4.inputs[Tabes::AUDIO_INPUT].channels = 1;
    m4.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    float ph4 = 0.f;
    auto sine4 = [&]() {
        ph4 += 330.f / SR; if (ph4 >= 1.f) ph4 -= 1.f;
        return 5.f * std::sin(2.f * M_PI * ph4);
    };
    // record ~0.7 s, then play ~0.4 s so wow has swung the head off zero
    m4.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(0.7f * SR); i++) {
        m4.inputs[Tabes::AUDIO_INPUT].setVoltage(sine4());
        m4.process(makeArgs(f4++));
    }
    m4.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    for (int i = 0; i < (int)(0.4f * SR); i++) {
        m4.inputs[Tabes::AUDIO_INPUT].setVoltage(sine4());
        m4.process(makeArgs(f4++));
    }
    // now press rec again; measure the output step over the transition
    float prev4 = m4.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
    float startStep = 0.f;
    m4.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(0.05f * SR); i++) {
        m4.inputs[Tabes::AUDIO_INPUT].setVoltage(sine4());
        m4.process(makeArgs(f4++));
        float v = m4.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
        startStep = std::max(startStep, std::fabs(v - prev4));
        prev4 = v;
    }
    report("tabes", "rec_start_max_step", startStep, startStep < 1.f);

    // no thump at rec STOP: the transition must not inject a DC pulse. A click
    // is a step (caught above); a thump is a net displacement of the local
    // mean, which a sine averages away but a DC bridge does not. The thump is
    // worst when the loop start and the input at stop are in antiphase, so
    // sweep the recording length across a full period and take the worst 5 ms
    // window-mean over the stop. (The old additive bridge hit ~2.3 V here
    // against a ~0.5 V sine baseline; the crossfade stays near baseline.)
    const int WIN = (int)(0.005f * SR);
    const int period = (int)(SR / 220.f);
    float worstStopDC = 0.f;
    for (int pad = 0; pad < period; pad += period / 12) {
        Tabes ms;
        long fs = 0;
        ms.params[Tabes::WOW_PARAM].setValue(0.3f);
        ms.inputs[Tabes::AUDIO_INPUT].channels = 1;
        ms.inputs[Tabes::REC_GATE_INPUT].channels = 1;
        float php = 0.f;
        auto sn = [&]() { php += 220.f / SR; if (php >= 1.f) php -= 1.f;
                          return 5.f * std::sin(2.f * M_PI * php); };
        int rec = (int)(0.2f * SR) + pad;              // varied stop phase
        for (int i = 0; i < rec; i++) {
            ms.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
            ms.inputs[Tabes::AUDIO_INPUT].setVoltage(sn());
            ms.process(makeArgs(fs++));
        }
        // measure the worst window-mean over the 20 ms after stop
        double sum = 0; std::vector<float> ring(WIN, 0.f); int head = 0, filled = 0;
        for (int i = 0; i < (int)(0.02f * SR); i++) {
            ms.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
            ms.inputs[Tabes::AUDIO_INPUT].setVoltage(sn());
            ms.process(makeArgs(fs++));
            float v = ms.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
            sum += v - ring[head]; ring[head] = v; head = (head + 1) % WIN;
            if (filled < WIN) filled++;
            else worstStopDC = std::max(worstStopDC, (float)std::fabs(sum / WIN));
        }
    }
    // a 220 Hz sine over a 5 ms window leaves ~0.5 V residual mean; a real
    // thump is several volts. 1.2 V sits well between the two.
    report("tabes", "rec_stop_dc", worstStopDC, worstStopDC < 1.2f);

    // polyphonic: a 2-channel input records as a stereo tape and plays back
    // two distinct channels (one shared transport, two tracks)
    Tabes mp;
    long fp = 0;
    mp.params[Tabes::WOW_PARAM].setValue(0.f);
    mp.inputs[Tabes::AUDIO_INPUT].channels = 2;
    mp.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    mp.outputs[Tabes::AUDIO_OUTPUT].channels = 1;   // simulate a patched cable
    float pa = 0.f, pb = 0.f;
    mp.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        pa += 220.f / SR; if (pa >= 1.f) pa -= 1.f;
        pb += 330.f / SR; if (pb >= 1.f) pb -= 1.f;
        mp.inputs[Tabes::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * pa), 0);
        mp.inputs[Tabes::AUDIO_INPUT].setVoltage(3.f * std::sin(2.f * M_PI * pb), 1);
        mp.process(makeArgs(fp++));
    }
    mp.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    mp.inputs[Tabes::AUDIO_INPUT].setVoltage(0.f, 0);
    mp.inputs[Tabes::AUDIO_INPUT].setVoltage(0.f, 1);
    Stats pl, pr;
    int outChans = 0;
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        mp.process(makeArgs(fp++));
        outChans = mp.outputs[Tabes::AUDIO_OUTPUT].getChannels();
        pl.add(mp.outputs[Tabes::AUDIO_OUTPUT].getVoltage(0));
        pr.add(mp.outputs[Tabes::AUDIO_OUTPUT].getVoltage(1));
    }
    report("tabes", "poly_channels", outChans, outChans == 2);
    report("tabes", "poly_left_rms", pl.rms(), pl.rms() > 1.0);
    report("tabes", "poly_right_rms", pr.rms(), pr.rms() > 0.5);
    // the two tracks must be distinct, not a mono copy of one channel
    report("tabes", "poly_distinct", std::fabs(pl.rms() - pr.rms()),
           std::fabs(pl.rms() - pr.rms()) > 0.3);
}

static void testLustro() {
    Lustro m;
    long frame = 0;
    uint32_t rng = 0x12345u;
    auto white = [&]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return ((rng >> 8) * (2.f / 16777216.f) - 1.f) * 5.f;
    };
    m.inputs[Lustro::AUDIO_INPUT].channels = 1;
    m.inputs[Lustro::EXCITE_INPUT].channels = 1;
    // string at rest: wet-only output should be (near) silent
    Stats rest;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.inputs[Lustro::AUDIO_INPUT].setVoltage(white());
        m.process(makeArgs(frame++));
        rest.add(m.outputs[Lustro::AUDIO_OUTPUT].getVoltage());
    }
    // pluck, then measure the bloom
    Stats bloom;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.inputs[Lustro::EXCITE_INPUT].setVoltage(i < 100 ? 5.f : 0.f);
        m.inputs[Lustro::AUDIO_INPUT].setVoltage(white());
        m.process(makeArgs(frame++));
        bloom.add(m.outputs[Lustro::AUDIO_OUTPUT].getVoltage());
    }
    report("lustro", "nans", rest.nans + bloom.nans, rest.nans + bloom.nans == 0);
    report("lustro", "rest_rms", rest.rms(), rest.rms() < 0.05);
    report("lustro", "bloom_rms", bloom.rms(), bloom.rms() > 5.0 * (rest.rms() + 1e-4));
    report("lustro", "peak", bloom.peak, bloom.peak < 12.f);
}

static void testBulla() {
    Bulla m;
    long frame = 0;
    for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
    Stats s, r;
    for (int i = 0; i < (int)(8 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Bulla::AUDIO_OUTPUT].getVoltage());
        r.add(m.outputs[Bulla::RUNGLER_OUTPUT].getVoltage());
    }
    report("bulla", "nans", s.nans + r.nans, s.nans + r.nans == 0);
    report("bulla", "audio_rms", s.rms(), s.rms() > 0.05);
    report("bulla", "peak", s.peak, s.peak < 12.f);
    report("bulla", "rungler_range", r.peak, r.peak <= 10.01f);
    report("bulla", "rungler_moves", r.rms(), r.rms() > 0.01);
}

int main() {
    rack::random::init();
    printf("module,check,value,pass\n");
    testRete();
    testUlulo();
    testTabes();
    testLustro();
    testBulla();
    return failures ? 1 : 0;
}
