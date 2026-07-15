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
    // sweep the dist/decay/tone CVs over their full range while howling
    m.inputs[Ululo::DIST_CV_INPUT].channels = 1;
    m.inputs[Ululo::DECAY_CV_INPUT].channels = 1;
    m.inputs[Ululo::TONE_CV_INPUT].channels = 1;
    Stats sc;
    for (int i = 0; i < (int)(4 * SR); i++) {
        float ph = (float)i / SR;
        m.inputs[Ululo::DIST_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 0.5f * ph));
        m.inputs[Ululo::DECAY_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 0.7f * ph));
        m.inputs[Ululo::TONE_CV_INPUT].setVoltage(5.f + 5.f * std::sin(2.f * M_PI * 1.3f * ph));
        m.process(makeArgs(frame++));
        sc.add(m.outputs[Ululo::AUDIO_OUTPUT].getVoltage());
    }
    report("ululo", "cv_sweep_nans", sc.nans, sc.nans == 0);
    report("ululo", "cv_sweep_peak", sc.peak, sc.peak < 6.f);
    m.inputs[Ululo::DIST_CV_INPUT].channels = 0;
    m.inputs[Ululo::DECAY_CV_INPUT].channels = 0;
    m.inputs[Ululo::TONE_CV_INPUT].channels = 0;
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

    // same rec press, but with overlap up and the head chain out of phase
    // with the write head: the playback->monitor snapshot must continue the
    // heard two-head blend, not the write head's position
    Tabes m4o; long f4o = 0;
    m4o.params[Tabes::WOW_PARAM].setValue(0.f);
    m4o.params[Tabes::OVERLAP_PARAM].setValue(1.f);
    m4o.inputs[Tabes::AUDIO_INPUT].channels = 1;
    m4o.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    float ph4o = 0.f;
    auto sine4o = [&]() {
        ph4o += 330.f / SR; if (ph4o >= 1.f) ph4o -= 1.f;
        return 5.f * std::sin(2.f * M_PI * ph4o);
    };
    // record 0.7 s, then play 0.55 s: playPos is at 0.55 s but the head
    // chain (hop = 0.35 s) is at 0.2 s, mid-crossfade
    m4o.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(0.7f * SR); i++) {
        m4o.inputs[Tabes::AUDIO_INPUT].setVoltage(sine4o());
        m4o.process(makeArgs(f4o++));
    }
    m4o.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    // silent input from here on: the loop is all we hear, so a snapshot
    // taken from the wrong head shows up as a raw step instead of being
    // masked by the monitored input
    m4o.inputs[Tabes::AUDIO_INPUT].setVoltage(0.f);
    for (int i = 0; i < (int)(0.55f * SR); i++)
        m4o.process(makeArgs(f4o++));
    float prev4o = m4o.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
    float startStepOvl = 0.f;
    m4o.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(0.05f * SR); i++) {
        m4o.process(makeArgs(f4o++));
        float v = m4o.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
        startStepOvl = std::max(startStepOvl, std::fabs(v - prev4o));
        prev4o = v;
    }
    report("tabes", "rec_start_ovl_step", startStepOvl, startStepOvl < 1.f);

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

    // no click at SPLICE: once the tape has aged, the aged read differs from
    // the pristine one in level and timbre, so restoring the pristine tape is
    // a source switch that must be crossfaded, not stepped. Sweep the splice
    // phase and, over the 20 ms after the splice, take the worst sample step
    // and 5 ms window-mean. The old 2 ms additive bridge only cancelled the
    // amplitude step (leaving a slope/timbre transient, and clippable by the
    // output clamp); the ~10 ms crossfade keeps step and local mean small.
    // Swept at overlap 0 and overlap max: the snapshot fed to the splice
    // crossfade must retrace the two-head chain (headAge + hop wrap), not the
    // write head, or a splice with overlap up blends against material from the
    // wrong position and still steps. The signal mixes two incommensurate
    // sines so a position offset can't hide as a whole number of periods (a
    // half loop offset is ~22.0 periods of a plain 220 Hz sine at 44.1 kHz);
    // the overlap variant ages gently so the aged read is still loud enough
    // that a wrong-position snapshot would step visibly.
    auto spliceSweep = [&](float ovlSet, float decaySet, int passes,
                           float& worstSpliceStep, float& worstSpliceDC) {
    for (int pad = 0; pad < period; pad += period / 6) {
        Tabes msp;
        long fsp = 0;
        msp.params[Tabes::DECAY_PARAM].setValue(decaySet);
        msp.params[Tabes::WOW_PARAM].setValue(0.3f);
        msp.params[Tabes::OVERLAP_PARAM].setValue(ovlSet);
        msp.monitorMode = Tabes::MONITOR_NEVER;          // isolate the loop
        msp.inputs[Tabes::AUDIO_INPUT].channels = 1;
        msp.inputs[Tabes::REC_GATE_INPUT].channels = 1;
        float phs = 0.f, phs2 = 0.f;
        auto sns = [&]() { phs += 220.f / SR; if (phs >= 1.f) phs -= 1.f;
                           phs2 += 173.3f / SR; if (phs2 >= 1.f) phs2 -= 1.f;
                           return 5.f * (0.7f * std::sin(2.f * M_PI * phs)
                                       + 0.3f * std::sin(2.f * M_PI * phs2)); };
        int rec = (int)(0.2f * SR) + pad;                // varied splice phase
        for (int i = 0; i < rec; i++) {
            msp.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
            msp.inputs[Tabes::AUDIO_INPUT].setVoltage(sns());
            msp.process(makeArgs(fsp++));
        }
        msp.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
        for (int i = 0; i < (int)(passes * 0.2f * SR); i++) {
            msp.inputs[Tabes::AUDIO_INPUT].setVoltage(sns());
            msp.process(makeArgs(fsp++));
        }
        float prevS = msp.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
        msp.params[Tabes::SPLICE_PARAM].setValue(1.f);   // splice on this sample
        msp.inputs[Tabes::AUDIO_INPUT].setVoltage(sns());
        msp.process(makeArgs(fsp++));
        msp.params[Tabes::SPLICE_PARAM].setValue(0.f);
        double sum = 0; std::vector<float> ring(WIN, 0.f); int head = 0, filled = 0;
        {
            float v = msp.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
            worstSpliceStep = std::max(worstSpliceStep, std::fabs(v - prevS));
            prevS = v; sum += v - ring[head]; ring[head] = v; head = (head + 1) % WIN;
            filled++;
        }
        for (int i = 0; i < (int)(0.02f * SR); i++) {
            msp.inputs[Tabes::AUDIO_INPUT].setVoltage(sns());
            msp.process(makeArgs(fsp++));
            float v = msp.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
            worstSpliceStep = std::max(worstSpliceStep, std::fabs(v - prevS));
            prevS = v;
            sum += v - ring[head]; ring[head] = v; head = (head + 1) % WIN;
            if (filled < WIN) filled++;
            else worstSpliceDC = std::max(worstSpliceDC, (float)std::fabs(sum / WIN));
        }
    }
    };
    // a 220 Hz 5 V sine steps ~0.14 V/sample and leaves ~0.5 V over 5 ms; a
    // real splice click/thump is several volts. Thresholds sit between.
    float worstSpliceStep = 0.f, worstSpliceDC = 0.f;
    spliceSweep(0.f, 0.9f, 40, worstSpliceStep, worstSpliceDC);   // age hard
    report("tabes", "splice_max_step", worstSpliceStep, worstSpliceStep < 1.f);
    report("tabes", "splice_dc", worstSpliceDC, worstSpliceDC < 1.2f);
    // intermediate overlap, not max: at max the heads sit exactly half a loop
    // apart, so even a snapshot based at the write head lands back on the head
    // chain after one hop wrap and the bug would be invisible
    float worstOvlStep = 0.f, worstOvlDC = 0.f;
    spliceSweep(0.6f, 0.5f, 8, worstOvlStep, worstOvlDC);         // age gently
    report("tabes", "splice_ovl_step", worstOvlStep, worstOvlStep < 1.f);
    report("tabes", "splice_ovl_dc", worstOvlDC, worstOvlDC < 1.2f);

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

// record `secs` of a 220 Hz sine into a fresh Tabes, leaving it playing
static void tabesRecord(Tabes& m, long& frame, float secs) {
    m.inputs[Tabes::AUDIO_INPUT].channels = 1;
    m.inputs[Tabes::REC_GATE_INPUT].channels = 1;
    float ph = 0.f;
    m.inputs[Tabes::REC_GATE_INPUT].setVoltage(10.f);
    for (int i = 0; i < (int)(secs * SR); i++) {
        ph += 220.f / SR; if (ph >= 1.f) ph -= 1.f;
        m.inputs[Tabes::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * ph));
        m.process(makeArgs(frame++));
    }
    m.inputs[Tabes::REC_GATE_INPUT].setVoltage(0.f);
    m.inputs[Tabes::AUDIO_INPUT].setVoltage(0.f);
}

// mk2 features: wow CV, ramp out, loop overlap, fx send/return
static void testTabesMk2() {
    // wow CV: with the knob at 0, feeding the CV must introduce read-head
    // wander (so the loop is no longer bit-identical pass to pass) yet stay
    // finite and level-sane
    {
        Tabes m; long frame = 0;
        m.params[Tabes::WOW_PARAM].setValue(0.f);
        m.inputs[Tabes::WOW_CV_INPUT].channels = 1;
        m.inputs[Tabes::WOW_CV_INPUT].setVoltage(5.f);   // full wow via CV
        tabesRecord(m, frame, 1.0f);
        Stats s;
        for (int i = 0; i < (int)(2 * SR); i++) {
            m.process(makeArgs(frame++));
            s.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
        }
        report("tabes", "wowcv_nans", s.nans, s.nans == 0);
        report("tabes", "wowcv_rms", s.rms(), s.rms() > 1.0 && s.peak < 12.f);
    }

    // ramp out: a loop-locked 0..10V saw that resets at the loop point
    {
        Tabes m; long frame = 0;
        m.params[Tabes::WOW_PARAM].setValue(0.f);
        tabesRecord(m, frame, 1.0f);
        float mn = 1e9f, mx = -1e9f, worstDrop = 0.f, prev = -1.f;
        int resets = 0;
        for (int i = 0; i < (int)(2.2f * SR); i++) {   // ~2 loops
            m.process(makeArgs(frame++));
            float r = m.outputs[Tabes::RAMP_OUTPUT].getVoltage();
            mn = std::min(mn, r); mx = std::max(mx, r);
            if (prev >= 0.f && r - prev < -5.f) { resets++; worstDrop = std::min(worstDrop, r - prev); }
            prev = r;
        }
        report("tabes", "ramp_min", mn, mn < 0.2f);
        report("tabes", "ramp_max", mx, mx > 9.5f && mx <= 10.f);
        report("tabes", "ramp_resets", resets, resets == 2);   // one per loop
    }

    // loop overlap: fully open (knob=1) it blurs the loop but must stay finite,
    // bounded, non-silent, and clickless across the wrap
    {
        Tabes m; long frame = 0;
        m.params[Tabes::WOW_PARAM].setValue(0.f);
        m.params[Tabes::OVERLAP_PARAM].setValue(1.f);
        tabesRecord(m, frame, 1.0f);
        Stats s; float prev = m.outputs[Tabes::AUDIO_OUTPUT].getVoltage(), maxStep = 0.f;
        float rampPrev = -1.f, eocPrev = 0.f;
        int rampResets = 0, eocPulses = 0;
        for (int i = 0; i < (int)(4 * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Tabes::AUDIO_OUTPUT].getVoltage();
            maxStep = std::max(maxStep, std::fabs(v - prev)); prev = v;
            s.add(v);
            float r = m.outputs[Tabes::RAMP_OUTPUT].getVoltage();
            if (rampPrev >= 0.f && r - rampPrev < -5.f) rampResets++;
            rampPrev = r;
            float e = m.outputs[Tabes::EOC_OUTPUT].getVoltage();
            if (e > 5.f && eocPrev <= 5.f) eocPulses++;
            eocPrev = e;
        }
        report("tabes", "overlap_nans", s.nans, s.nans == 0);
        report("tabes", "overlap_rms", s.rms(), s.rms() > 0.5 && s.peak < 12.f);
        report("tabes", "overlap_max_step", maxStep, maxStep < 1.f);
        // at max overlap the heard repeat is hop = loopLen/2, so a 1 s loop
        // repeats every 0.5 s: eoc and ramp must follow the heads, two per
        // tape rotation, not the write head's one
        report("tabes", "overlap_eoc_per_hop", eocPulses,
               eocPulses >= 7 && eocPulses <= 9);
        report("tabes", "overlap_ramp_per_hop", rampResets,
               rampResets >= 7 && rampResets <= 9);
    }

    // fx send/return: patch RETURN = gain * SEND (a 1-sample-delayed external
    // effect). A hot gain (>1) with full mix compounds every pass; the tape
    // clamp must keep it bounded and non-silent rather than exploding.
    // channels=1 on both ports simulates the patched send/return cables.
    {
        Tabes m; long frame = 0;
        m.params[Tabes::WOW_PARAM].setValue(0.f);
        m.params[Tabes::DECAY_PARAM].setValue(0.2f);
        m.params[Tabes::SEND_MIX_PARAM].setValue(1.f);
        m.inputs[Tabes::RETURN_INPUT].channels = 1;
        m.outputs[Tabes::SEND_OUTPUT].channels = 1;
        tabesRecord(m, frame, 0.5f);
        const float gain = 1.05f;
        float sendPrev = 0.f;
        Stats s;
        for (int i = 0; i < (int)(20 * SR); i++) {   // ~40 passes of buildup
            m.inputs[Tabes::RETURN_INPUT].setVoltage(gain * sendPrev);
            m.process(makeArgs(frame++));
            sendPrev = m.outputs[Tabes::SEND_OUTPUT].getVoltage();
            if (i >= (int)(19 * SR)) s.add(m.outputs[Tabes::AUDIO_OUTPUT].getVoltage());
        }
        report("tabes", "fx_nans", s.nans, s.nans == 0);
        report("tabes", "fx_bounded", s.peak, s.peak <= 10.01f);
        report("tabes", "fx_alive", s.rms(), s.rms() > 0.2);   // not collapsed to silence

        // sendMix = 0 ignores the return entirely (identical to no send), even
        // with the loop fully patched
        Tabes a, b; long fa = 0, fb = 0;
        a.params[Tabes::WOW_PARAM].setValue(0.f); b.params[Tabes::WOW_PARAM].setValue(0.f);
        a.params[Tabes::SEND_MIX_PARAM].setValue(0.f);
        b.params[Tabes::SEND_MIX_PARAM].setValue(0.f);
        a.inputs[Tabes::RETURN_INPUT].channels = 1;
        a.outputs[Tabes::SEND_OUTPUT].channels = 1;
        tabesRecord(a, fa, 0.5f); tabesRecord(b, fb, 0.5f);
        float maxDiff = 0.f, sp = 0.f;
        for (int i = 0; i < (int)(2 * SR); i++) {
            a.inputs[Tabes::RETURN_INPUT].setVoltage(3.f * sp);   // hot junk into a
            a.process(makeArgs(fa++)); b.process(makeArgs(fb++));
            sp = a.outputs[Tabes::SEND_OUTPUT].getVoltage();
            maxDiff = std::max(maxDiff, std::fabs(
                a.outputs[Tabes::AUDIO_OUTPUT].getVoltage()
                - b.outputs[Tabes::AUDIO_OUTPUT].getVoltage()));
        }
        report("tabes", "fx_mix0_bypass", maxDiff, maxDiff < 1e-4f);

        // return with NO send patched is ignored: the fx loop needs both ends.
        // c has a hot return but SEND left disconnected (channels stays 0);
        // even at mix = 1 it must be identical to the untouched reference d.
        Tabes c, d; long fc = 0, fd = 0;
        c.params[Tabes::WOW_PARAM].setValue(0.f); d.params[Tabes::WOW_PARAM].setValue(0.f);
        c.params[Tabes::SEND_MIX_PARAM].setValue(1.f);
        d.params[Tabes::SEND_MIX_PARAM].setValue(1.f);
        c.inputs[Tabes::RETURN_INPUT].channels = 1;   // return patched, send is not
        tabesRecord(c, fc, 0.5f); tabesRecord(d, fd, 0.5f);
        float maxDiff2 = 0.f;
        for (int i = 0; i < (int)(2 * SR); i++) {
            c.inputs[Tabes::RETURN_INPUT].setVoltage(4.f);   // hot DC into return
            c.process(makeArgs(fc++)); d.process(makeArgs(fd++));
            maxDiff2 = std::max(maxDiff2, std::fabs(
                c.outputs[Tabes::AUDIO_OUTPUT].getVoltage()
                - d.outputs[Tabes::AUDIO_OUTPUT].getVoltage()));
        }
        report("tabes", "fx_needs_send", maxDiff2, maxDiff2 < 1e-4f);
    }
}

int main() {
    rack::random::init();
    printf("module,check,value,pass\n");
    testRete();
    testUlulo();
    testTabes();
    testTabesMk2();
    testLustro();
    testBulla();
    return failures ? 1 : 0;
}
