// new_modules_smoke — offline sanity checks for the v2.7.0 modules
// (rete, ululo, tabes, lustro, bulla) plus perge, vorax, textor,
// imber and sylla.
// Drives each module's process() directly and checks for non-finite
// samples, runaway levels and basic expected behavior (self-oscillation,
// loop decay, pluck response).
//
// Prints one CSV row per check: module,check,value,pass
// Exits nonzero if any check fails.

#include <rack.hpp>

// pluginInstance is normally defined in forsitan.cpp; the harness never
// loads assets so a null plugin is fine.
rack::plugin::Plugin* pluginInstance = nullptr;

#include "../src/rete.cpp"
#include "../src/imber.cpp"
#include "../src/sylla.cpp"
#include "../src/ululo.cpp"
#include "../src/tabes.cpp"
#include "../src/lustro.cpp"
#include "../src/bulla.cpp"
#include "../src/perge.cpp"
#include "../src/vorax.cpp"
#include "../src/textor.cpp"
#include "../src/guttur.cpp"

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

static void testPerge() {
    Perge m;
    long frame = 0;
    m.inputs[Perge::IN_L_INPUT].channels = 1;
    m.params[Perge::MIX_PARAM].setValue(1.f);      // wet only
    m.params[Perge::TEMPO_PARAM].setValue(0.7f);   // fast-ish repeats
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    // feed 0.4 s of a 330 Hz burst at +-5 V, then silence
    float phase = 0.f;
    for (int i = 0; i < (int)(0.4f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Perge::IN_L_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.process(makeArgs(frame++));
    }
    m.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    // repeats must appear after the input stops
    Stats rep;
    for (int i = 0; i < (int)(3 * SR); i++) {
        m.process(makeArgs(frame++));
        rep.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
        rep.add(m.outputs[Perge::OUT_R_OUTPUT].getVoltage());
    }
    report("perge", "nans", rep.nans, rep.nans == 0);
    report("perge", "repeats_rms", rep.rms(), rep.rms() > 0.02);
    report("perge", "peak", rep.peak, rep.peak < 12.f);
    // low sustain: repeats must die out
    m.params[Perge::SUSTAIN_PARAM].setValue(0.05f);
    for (int i = 0; i < (int)(6 * SR); i++) m.process(makeArgs(frame++));
    Stats dead;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        dead.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "repeats_decay", dead.rms(), dead.rms() < 0.01);
    // freeze: repeats persist over 8 s with everything cranked
    m.onReset();
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    m.params[Perge::GLITCH_PARAM].setValue(1.f);    // full dimension
    m.params[Perge::LOFI_PARAM].setValue(-1.f);
    m.params[Perge::RVRB_PARAM].setValue(-1.f);
    m.params[Perge::PITCH_PARAM].setValue(0.7f);
    phase = 0.f;
    for (int i = 0; i < (int)(0.4f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Perge::IN_L_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.process(makeArgs(frame++));
    }
    m.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    // let the capture commit and the repeats get going, then freeze for 7 s
    // (the wait must outlast the envelope release, or the still-open capture
    // is what freezes and slot ages never come into play)
    for (int i = 0; i < (int)(2.0f * SR); i++) m.process(makeArgs(frame++));
    m.params[Perge::SUSTAIN_PARAM].setValue(1.f);   // freeze zone
    for (int i = 0; i < (int)(7 * SR); i++) m.process(makeArgs(frame++));
    Stats froz;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        froz.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "freeze_nans", froz.nans, froz.nans == 0);
    report("perge", "freeze_persists", froz.rms(), froz.rms() > 0.02);
    report("perge", "freeze_peak", froz.peak, froz.peak < 12.f);
    // unfreeze: the repeat train must resume decaying, not vanish
    // (skip 1.5 s so the freeze-era reverb tail doesn't mask the repeats)
    m.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    for (int i = 0; i < (int)(1.5f * SR); i++) m.process(makeArgs(frame++));
    Stats unfr;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        unfr.add(m.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "unfreeze_repeats", unfr.rms(), unfr.rms() > 0.05);
    // capture gate: forces a capture of material below the threshold
    Perge m2;
    long f2 = 0;
    m2.inputs[Perge::IN_L_INPUT].channels = 1;
    m2.inputs[Perge::CAPTURE_GATE_INPUT].channels = 1;
    m2.params[Perge::MIX_PARAM].setValue(1.f);
    m2.params[Perge::TEMPO_PARAM].setValue(0.7f);
    m2.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
    m2.params[Perge::THRESH_PARAM].setValue(1.f);   // way above the input
    phase = 0.f;
    for (int i = 0; i < (int)(0.6f * SR); i++) {
        phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
        m2.inputs[Perge::IN_L_INPUT].setVoltage(0.5f * std::sin(2.f * M_PI * phase));
        bool gate = i >= (int)(0.1f * SR) && i < (int)(0.4f * SR);
        m2.inputs[Perge::CAPTURE_GATE_INPUT].setVoltage(gate ? 10.f : 0.f);
        m2.process(makeArgs(f2++));
    }
    m2.inputs[Perge::IN_L_INPUT].setVoltage(0.f);
    Stats forced;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m2.process(makeArgs(f2++));
        forced.add(m2.outputs[Perge::OUT_L_OUTPUT].getVoltage());
    }
    report("perge", "capture_gate", forced.rms(), forced.rms() > 0.005);
    // dimension: the previous captures must come back as audible layers
    // (they decay with the current train's age, not their own, or the
    // slots are inaudible by the time they are "previous")
    auto dimRun = [](float dimKnob) {
        Perge p;
        long f = 0;
        p.inputs[Perge::IN_L_INPUT].channels = 1;
        p.params[Perge::MIX_PARAM].setValue(1.f);
        p.params[Perge::TEMPO_PARAM].setValue(0.7f);
        p.params[Perge::SUSTAIN_PARAM].setValue(0.6f);
        p.params[Perge::GLITCH_PARAM].setValue(dimKnob);
        float ph = 0.f;
        Stats s;
        for (long i = 0; i < (long)(9.f * SR); i++) {
            float t = i / SR;
            float v = 0.f;
            for (int k = 0; k < 3; k++) {   // three distinct phrases
                float t0 = 0.5f + 2.f * k;
                if (t >= t0 && t < t0 + 0.4f) {
                    ph += (200.f + 90.f * k) / SR;
                    if (ph >= 1.f) ph -= 1.f;
                    v = std::sin(2.f * (float)M_PI * ph);
                }
            }
            p.inputs[Perge::IN_L_INPUT].setVoltage(5.f * v);
            p.process(makeArgs(f++));
            if (t >= 5.5f)
                s.add(p.outputs[Perge::OUT_L_OUTPUT].getVoltage());
        }
        return s.rms();
    };
    double dimOff = dimRun(0.f);
    double dimOn = dimRun(1.f);
    report("perge", "dimension_layers", dimOn / std::max(dimOff, 1e-9),
           dimOn > 1.08 * dimOff);
    // captures own their audio: a slot must survive the 8 s ring
    // lapping the tape it was cut from
    Perge m4;
    long f4 = 0;
    m4.inputs[Perge::IN_L_INPUT].channels = 1;
    phase = 0.f;
    for (long i = 0; i < (long)(11.f * SR); i++) {
        float t = i / SR;
        // one phrase at 0.5 s, a second at 9.5 s (first tape lapped ~8.5 s)
        float v = 0.f;
        if ((t >= 0.5f && t < 0.9f) || (t >= 9.5f && t < 9.9f)) {
            phase += 330.f / SR; if (phase >= 1.f) phase -= 1.f;
            v = std::sin(2.f * (float)M_PI * phase);
        }
        m4.inputs[Perge::IN_L_INPUT].setVoltage(5.f * v);
        m4.process(makeArgs(f4++));
    }
    report("perge", "slot_survives_lap", m4.slots[1].len, m4.slots[1].len > 0);
}

static void testVorax() {
    Vorax m;
    long frame = 0;
    // feedback fully down: the loop must stay essentially silent
    Stats quiet;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        quiet.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
    }
    report("vorax", "silent_at_no_fb", quiet.rms(), quiet.rms() < 0.01);
    // raise feedback past unity: the loop must self-excite from the
    // -90 dBFS noise seed alone
    m.params[Vorax::FEEDBACK_PARAM].setValue(6.f);
    for (int i = 0; i < (int)(6 * SR); i++) m.process(makeArgs(frame++));
    Stats s, sr_;
    for (int i = 0; i < (int)(6 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
        sr_.add(m.outputs[Vorax::RIGHT_OUTPUT].getVoltage());
    }
    report("vorax", "nans", s.nans + sr_.nans, s.nans + sr_.nans == 0);
    report("vorax", "self_osc_rms", s.rms(), s.rms() > 0.05);
    report("vorax", "peak", std::max(s.peak, sr_.peak),
           std::max(s.peak, sr_.peak) < 12.f);
    // stereo: the 4-sample body offset must decorrelate the channels
    report("vorax", "right_alive", sr_.rms(), sr_.rms() > 0.05);
    // everything hostile at once: reverb wet + max echo feedback + short
    // time + body sweep, must stay bounded (each echo repeat is clipped)
    m.params[Vorax::VERB_MIX_PARAM].setValue(1.f);
    m.params[Vorax::VERB_DECAY_PARAM].setValue(1.f);
    m.params[Vorax::ECHO_SEND_PARAM].setValue(1.f);
    m.params[Vorax::ECHO_FB_PARAM].setValue(1.5f);
    m.params[Vorax::ECHO_TIME_PARAM].setValue(0.1f);
    m.params[Vorax::FEEDBACK_PARAM].setValue(12.f);
    Stats h;
    for (int i = 0; i < (int)(10 * SR); i++) {
        m.params[Vorax::BODY_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.2f * i / SR));
        m.process(makeArgs(frame++));
        h.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
        h.add(m.outputs[Vorax::RIGHT_OUTPUT].getVoltage());
    }
    report("vorax", "hostile_nans", h.nans, h.nans == 0);
    report("vorax", "hostile_bounded", h.peak, h.peak <= 10.01f);
    report("vorax", "hostile_alive", h.rms(), h.rms() > 0.05);
    // feedback back down: the drone must die away
    m.params[Vorax::FEEDBACK_PARAM].setValue(-60.f);
    m.params[Vorax::ECHO_FB_PARAM].setValue(0.f);
    m.params[Vorax::ECHO_SEND_PARAM].setValue(0.f);
    m.params[Vorax::VERB_MIX_PARAM].setValue(0.f);
    for (int i = 0; i < (int)(10 * SR); i++) m.process(makeArgs(frame++));
    Stats dead;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        dead.add(m.outputs[Vorax::LEFT_OUTPUT].getVoltage());
    }
    report("vorax", "dies_at_no_fb", dead.rms(), dead.rms() < 0.01);
}

static void testTextor() {
    Textor m;
    long frame = 0;
    m.inputs[Textor::AUDIO_INPUT].channels = 1;
    m.inputs[Textor::REC_INPUT].channels = 1;
    // capture 2 s of a 220 Hz sine via the rec trigger (the Schmitt
    // trigger needs to see low before the pulse)
    float phase = 0.f;
    for (int i = 0; i < (int)(2.3f * SR); i++) {
        phase += 220.f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Textor::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * phase));
        m.inputs[Textor::REC_INPUT].setVoltage(i >= 1000 && i < 1100 ? 10.f : 0.f);
        m.process(makeArgs(frame++));
    }
    m.inputs[Textor::AUDIO_INPUT].setVoltage(0.f);
    // a capture landing on a silent loom starts playback by itself
    Stats pre;
    for (int i = 0; i < (int)(2 * SR); i++) {
        m.process(makeArgs(frame++));
        pre.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    report("textor", "autoplay_after_rec", pre.rms(), pre.rms() > 0.02);
    // move the knob into the weave field (baseline), then nudge it: reroll
    m.params[Textor::WEAVE_PARAM].setValue(0.5f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.params[Textor::WEAVE_PARAM].setValue(0.6f);
    Stats s;
    int gates = 0;
    float prevGate = 0.f;
    for (int i = 0; i < (int)(6 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
        s.add(m.outputs[Textor::RIGHT_OUTPUT].getVoltage());
        float g = m.outputs[Textor::WARP_GATE_OUTPUT].getVoltage()
                + m.outputs[Textor::WEFT_GATE_OUTPUT].getVoltage()
                + m.outputs[Textor::FLECK_GATE_OUTPUT].getVoltage();
        if (g > 5.f && prevGate <= 5.f) gates++;
        prevGate = g;
    }
    report("textor", "nans", s.nans, s.nans == 0);
    report("textor", "loop_rms", s.rms(), s.rms() > 0.02);
    report("textor", "peak", s.peak, s.peak <= 5.01f);   // soft-limited * 5V
    report("textor", "gates_fire", gates, gates > 5);
    // reweave changes the loop: different seeds should give a different
    // strand pattern (coarse fingerprint over divisions/phases + tempo)
    auto fingerprint = [](Textor& t) {
        long fp = (long)(t.rollStepS * 1e6f);
        for (int e = 0; e < Textor::kElements; e++)
            for (int i = 0; i < t.strandCount[e]; i++)
                fp += (e + 1) * (t.strands[e][i].div * 31 + t.strands[e][i].phase + 1);
        return fp;
    };
    long fp1 = fingerprint(m);
    float tempo1 = m.rollStepS;
    m.params[Textor::WEAVE_PARAM].setValue(0.8f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    long fp2 = fingerprint(m);
    float tempo2 = m.rollStepS;
    report("textor", "reweave_changes_loop", std::labs(fp1 - fp2), fp1 != fp2);
    // per-roll tempo: the two rolls should not share a step period
    report("textor", "tempo_per_roll", std::fabs(tempo1 - tempo2),
           std::fabs(tempo1 - tempo2) > 1e-4f);
    // pitch shifts are semitone-quantized even in random mode
    m.params[Textor::PITCH_PARAM].setValue(0.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    bool semiOk = true;
    for (int e = 0; e < Textor::kElements; e++)
        for (int i = 0; i < m.strandCount[e]; i++) {
            float s = m.strands[e][i].semi;
            if (std::fabs(s - std::round(s)) > 1e-4f) semiOk = false;
        }
    report("textor", "random_pitch_semitones", semiOk, semiOk);
    m.params[Textor::PITCH_PARAM].setValue(1.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    // rhythm mode re-renders and keeps playing
    m.params[Textor::MODE_PARAM].setValue(0.f);
    Stats rh;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.process(makeArgs(frame++));
        rh.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    report("textor", "rhythm_alive", rh.rms(), rh.rms() > 0.005);
    report("textor", "rhythm_nans", rh.nans, rh.nans == 0);
    // external clock paces the steps: fast clock, count gate onsets rise
    m.inputs[Textor::CLOCK_INPUT].channels = 1;
    int fastGates = 0;
    prevGate = 0.f;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.inputs[Textor::CLOCK_INPUT].setVoltage((i % 1500) < 200 ? 10.f : 0.f);
        m.process(makeArgs(frame++));
        float g = m.outputs[Textor::WARP_GATE_OUTPUT].getVoltage()
                + m.outputs[Textor::WEFT_GATE_OUTPUT].getVoltage()
                + m.outputs[Textor::FLECK_GATE_OUTPUT].getVoltage();
        if (g > 5.f && prevGate <= 5.f) fastGates++;
        prevGate = g;
    }
    report("textor", "clocked_gates", fastGates, fastGates > 0);
    m.inputs[Textor::CLOCK_INPUT].channels = 0;
    m.inputs[Textor::CLOCK_INPUT].setVoltage(0.f);
    // hardware sweep behavior: rerolls with restart must stay clean
    m.sweepRestart = true;
    Stats sw;
    for (int k = 0; k < 8; k++) {
        m.params[Textor::WEAVE_PARAM].setValue(0.3f + 0.05f * k);
        for (int i = 0; i < (int)(0.25f * SR); i++) {
            m.process(makeArgs(frame++));
            sw.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
        }
    }
    report("textor", "sweep_restart_nans", sw.nans, sw.nans == 0);
    report("textor", "sweep_restart_alive", sw.rms(), sw.rms() > 0.005);
    m.sweepRestart = false;
    // reset zone: erases the sample and stops the loom
    m.params[Textor::WEAVE_PARAM].setValue(0.f);
    for (int i = 0; i < (int)(2 * SR); i++) m.process(makeArgs(frame++));
    Stats off;
    int offGates = 0;
    prevGate = 0.f;
    for (int i = 0; i < (int)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        off.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
        float g = m.outputs[Textor::WARP_GATE_OUTPUT].getVoltage();
        if (g > 5.f && prevGate <= 5.f) offGates++;
        prevGate = g;
    }
    report("textor", "reset_silences", off.rms(), off.rms() < 1e-4);
    report("textor", "reset_stops_gates", offGates, offGates == 0);
}

static void testImber() {
    Imber m;
    long frame = 0;
    // first process initializes the engine at SR; then pin everything
    // deterministic: fixed timing seed, all clocks reachable, FX far away
    m.process(makeArgs(frame++));
    m.eng.init(SR, 42);
    for (int i = 0; i < imber_engine::DIV_COUNT; i++) {
        m.eng.clk[i].ax = m.eng.clk[i].bx = 0.45f + 0.025f * i;
        m.eng.clk[i].ay = m.eng.clk[i].by = 0.5f;
    }
    for (int i = 0; i < imber_engine::kFxObjs; i++) {
        m.eng.fxo[i].ax = m.eng.fxo[i].bx = 0.95f;
        m.eng.fxo[i].ay = m.eng.fxo[i].by = 0.95f;
    }
    m.params[Imber::REACH_PARAM].setValue(0.7f);

    // the worker-thread bank should land while the engine idles
    // (simulated samples run faster than wall time; yield to the worker)
    long waited = 0;
    while (!m.eng.bank && waited < (long)(30 * SR)) {
        m.process(makeArgs(frame++));
        if (++waited % (long)SR == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    report("imber", "bank_lands_s", waited / SR, m.eng.bank != nullptr);
    if (!m.eng.bank)
        return;
    report("imber", "bank_sizes",
           m.eng.bank->loops.size(),
           m.eng.bank->loops.size() == (size_t)imber_gen::kBankLoops
           && m.eng.bank->skips.size() == (size_t)imber_gen::kBankSkips
           && m.eng.bank->micros.size() == (size_t)imber_gen::kBankMicros);

    // main render window: audio, gates, skip/micro activity, couplings
    Stats s, sk, mi;
    int g16 = 0;
    float prevG = 0.f;
    for (long i = 0; i < (long)(8 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
        s.add(m.outputs[Imber::RIGHT_OUTPUT].getVoltage());
        sk.add(m.outputs[Imber::SKIP_OUTPUT].getVoltage());
        mi.add(m.outputs[Imber::MICRO_OUTPUT].getVoltage());
        float g = m.outputs[Imber::G16N_OUTPUT].getVoltage();
        if (g > 5.f && prevG <= 5.f) g16++;
        prevG = g;
    }
    report("imber", "nans", s.nans, s.nans == 0);
    report("imber", "rms", s.rms(), s.rms() > 0.01);
    report("imber", "peak", s.peak, s.peak <= 5.01f);
    // 16n at 100 bpm is ~150 ms; drunk jitter still lands near 53 in 8 s
    report("imber", "gate16_count", g16, g16 > 25 && g16 < 90);
    report("imber", "skip_voice_alive", sk.rms(), sk.peak > 0.01f);
    report("imber", "micro_voice_alive", mi.rms(), mi.peak > 0.01f);
    int bound = 0;
    for (int i = 0; i < Imber::kPlayers; i++)
        bound += m.eng.pl[i].boundClock >= 0;
    report("imber", "players_bound", bound, bound == Imber::kPlayers);
    // default spread has 4 vertically aligned pairs -> sync couplings
    report("imber", "pairs_detected", m.eng.pairCount, m.eng.pairCount >= 4);

    // FX assignment: park an fx cluster on player 1
    for (int i = 0; i < imber_engine::kFxObjs; i++) {
        m.eng.fxo[i].ax = m.eng.fxo[i].bx =
            m.params[Imber::X1_PARAM].getValue();
        m.eng.fxo[i].ay = m.eng.fxo[i].by =
            m.params[Imber::Y1_PARAM].getValue();
    }
    Stats fx;
    for (long i = 0; i < (long)(3 * SR); i++) {
        m.process(makeArgs(frame++));
        fx.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
    }
    report("imber", "fx_stack_nans", fx.nans, fx.nans == 0);
    report("imber", "fx_stack_mask", m.eng.fxMask[0], m.eng.fxMask[0] == 0xff);

    // per-voice mute + force-new-sample, and the randomize exemption
    {
        m.params[Imber::SCV_PARAM].setValue(0.f);   // isolate the 8 players
        m.params[Imber::PLV_PARAM].setValue(0.f);
        m.params[Imber::SKP_PARAM].setValue(0.f);
        m.params[Imber::PLS_PARAM].setValue(0.f);
        for (int i = 0; i < Imber::kPlayers; i++)
            m.params[Imber::VON1_PARAM + i].setValue(0.f);
        for (long i = 0; i < (long)(2 * SR); i++) m.process(makeArgs(frame++));
        Stats mute;
        for (long i = 0; i < (long)(2 * SR); i++) {
            m.process(makeArgs(frame++));
            mute.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
        }
        report("imber", "all_voices_muted_silent", mute.rms(), mute.rms() < 1e-4);
        // muting must not unbind: the panel still shows the would-be clock
        int stillBound = 0;
        for (int i = 0; i < Imber::kPlayers; i++)
            stillBound += m.eng.pl[i].boundClock >= 0;
        report("imber", "muted_keeps_binding", stillBound,
               stillBound == Imber::kPlayers);

        for (int i = 0; i < Imber::kPlayers; i++)
            m.params[Imber::VON1_PARAM + i].setValue(1.f);
        for (long i = 0; i < (long)(2 * SR); i++) m.process(makeArgs(frame++));
        Stats back;
        for (long i = 0; i < (long)(2 * SR); i++) {
            m.process(makeArgs(frame++));
            back.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
        }
        report("imber", "unmute_restores", back.rms(), back.rms() > 0.01);

        // the new-sample button loads a different buffer on every press
        int changed = 0, prevBuf = m.eng.pl[0].cur.buf;
        for (int k = 0; k < 6; k++) {
            m.params[Imber::NEW1_PARAM].setValue(1.f);
            for (long i = 0; i < (long)(0.05f * SR); i++) m.process(makeArgs(frame++));
            m.params[Imber::NEW1_PARAM].setValue(0.f);
            for (long i = 0; i < (long)(0.05f * SR); i++) m.process(makeArgs(frame++));
            if (m.eng.pl[0].cur.buf != prevBuf) {
                changed++;
                prevBuf = m.eng.pl[0].cur.buf;
            }
        }
        report("imber", "new_sample_button", changed, changed == 6);

        // mutes are performance state: no randomize path may touch them
        m.params[Imber::VON3_PARAM].setValue(0.f);
        m.bigRandom();
        report("imber", "rnd_spares_mutes",
               m.params[Imber::VON3_PARAM].getValue(),
               m.params[Imber::VON3_PARAM].getValue() < 0.5f);
        report("imber", "mute_not_randomizable",
               m.getParamQuantity(Imber::VON3_PARAM)->randomizeEnabled ? 1 : 0,
               !m.getParamQuantity(Imber::VON3_PARAM)->randomizeEnabled);
        m.params[Imber::VON3_PARAM].setValue(1.f);
    }

    // sparse engine: a slow clock with short windows must open real gaps,
    // and the same mode at speed must fall back to the continuous bed
    {
        m.engineMode = 1;
        m.params[Imber::BPM_PARAM].setValue(std::log2(1.f));
        m.params[Imber::LPM_PARAM].setValue(0.3f);
        for (long i = 0; i < (long)(3 * SR); i++) m.process(makeArgs(frame++));
        Stats sp;
        long quiet = 0, gap = 0, maxGap = 0, n = 0;
        for (long i = 0; i < (long)(20 * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Imber::LEFT_OUTPUT].getVoltage();
            sp.add(v);
            n++;
            if (std::fabs(v) < 0.02f) {
                quiet++;
                if (++gap > maxGap) maxGap = gap;
            }
            else gap = 0;
        }
        report("imber", "sparse_nans", sp.nans, sp.nans == 0);
        report("imber", "sparse_silence_pct", 100.0 * quiet / n,
               100.0 * quiet / n > 60.0);
        report("imber", "sparse_longest_gap_s", maxGap / (double)SR,
               maxGap / (double)SR > 1.0);
        report("imber", "sparse_drops_full_level", sp.peak, sp.peak > 0.5f);

        // fast clock: edges outrun the material, gate never closes
        m.params[Imber::BPM_PARAM].setValue(std::log2(180.f));
        m.params[Imber::LPM_PARAM].setValue(2.f);
        for (long i = 0; i < (long)(3 * SR); i++) m.process(makeArgs(frame++));
        long fastQuiet = 0, fastN = 0;
        Stats fs;
        for (long i = 0; i < (long)(5 * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Imber::LEFT_OUTPUT].getVoltage();
            fs.add(v);
            fastN++;
            if (std::fabs(v) < 0.02f) fastQuiet++;
        }
        report("imber", "sparse_fast_is_continuous", 100.0 * fastQuiet / fastN,
               100.0 * fastQuiet / fastN < 10.0);
        report("imber", "sparse_fast_nans", fs.nans, fs.nans == 0);
        m.engineMode = 0;
        m.params[Imber::BPM_PARAM].setValue(std::log2(100.f));
        m.params[Imber::LPM_PARAM].setValue(2.f);
        for (long i = 0; i < (long)(2 * SR); i++) m.process(makeArgs(frame++));
    }

    // ON off freezes and silences the engine (and its gates)
    m.params[Imber::ON_PARAM].setValue(0.f);
    for (long i = 0; i < (long)(0.5f * SR); i++)
        m.process(makeArgs(frame++));
    Stats off;
    int offGates = 0;
    prevG = 0.f;
    for (long i = 0; i < (long)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        off.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
        float g = m.outputs[Imber::G16N_OUTPUT].getVoltage();
        if (g > 5.f && prevG <= 5.f) offGates++;
        prevG = g;
    }
    report("imber", "off_silences", off.rms(), off.rms() < 1e-3);
    report("imber", "off_stops_gates", offGates, offGates == 0);
}

static void testSylla() {
    Sylla m;
    long frame = 0;
    // default: loop on, nothing patched -> generates itself and free-runs
    long waited = 0;
    while (m.buffer.empty() && waited < (long)(30 * SR)) {
        m.process(makeArgs(frame++));
        if (++waited % (long)SR == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report("sylla", "render_lands_s", waited / SR, !m.buffer.empty());
    if (m.buffer.empty())
        return;
    Stats s;
    int eoc = 0;
    float prevE = 0.f;
    for (long i = 0; i < (long)(9 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Sylla::OUT_OUTPUT].getVoltage());
        float e = m.outputs[Sylla::EOC_OUTPUT].getVoltage();
        if (e > 5.f && prevE <= 5.f) eoc++;
        prevE = e;
    }
    report("sylla", "nans", s.nans, s.nans == 0);
    report("sylla", "freerun_rms", s.rms(), s.rms() > 0.02);
    report("sylla", "eoc_fires", eoc, eoc >= 1);

    // regenerating as micro swaps in a much shorter buffer
    size_t loopLen = m.buffer.size();
    m.params[Sylla::FAMILY_PARAM].setValue(8.f);   // micro
    m.params[Sylla::GEN_PARAM].setValue(1.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.params[Sylla::GEN_PARAM].setValue(0.f);
    waited = 0;
    while (m.buffer.size() == loopLen && waited < (long)(30 * SR)) {
        m.process(makeArgs(frame++));
        if (++waited % (long)SR == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report("sylla", "gen_swaps_buffer", m.buffer.size(),
           m.buffer.size() != loopLen && m.buffer.size() < SR);

    // one-shot trigger mode: plays once, then falls silent
    m.params[Sylla::LOOP_PARAM].setValue(0.f);
    m.inputs[Sylla::TRIG_INPUT].channels = 1;
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.inputs[Sylla::TRIG_INPUT].setVoltage(10.f);
    for (int i = 0; i < 16; i++) m.process(makeArgs(frame++));
    m.inputs[Sylla::TRIG_INPUT].setVoltage(0.f);
    Stats shot;
    for (long i = 0; i < (long)(1 * SR); i++) {
        m.process(makeArgs(frame++));
        shot.add(m.outputs[Sylla::OUT_OUTPUT].getVoltage());
    }
    Stats tail;
    for (long i = 0; i < (long)(0.5f * SR); i++) {
        m.process(makeArgs(frame++));
        tail.add(m.outputs[Sylla::OUT_OUTPUT].getVoltage());
    }
    report("sylla", "oneshot_plays", shot.peak, shot.peak > 0.05f);
    report("sylla", "oneshot_stops", tail.rms(), tail.rms() < 1e-4);
}

static void testGuttur() {
    Guttur m;
    long frame = 0;
    // defaults (drive .2, omega 2e-4, both banks on preset 6, level 1.4):
    // the chaos must self-start from the ignition kick, then breathe with
    // the ~24 s forcing cycle (surges near t=1 s and t=12 s, quiet between)
    // — so measure across one full cycle from the start
    Stats s, d;
    for (int i = 0; i < (int)(24 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        d.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
    }
    report("guttur", "nans", s.nans + d.nans, s.nans + d.nans == 0);
    report("guttur", "self_start_rms", s.rms(), s.rms() > 0.01);
    report("guttur", "peak", s.peak, s.peak <= 10.01f);
    report("guttur", "duff_alive", d.rms(), d.rms() > 0.01);
    report("guttur", "duff_bounded", d.peak, d.peak <= 10.01f);
    // reset trigger: state must clear and the sound re-ignite, no NaNs
    m.inputs[Guttur::RESET_INPUT].channels = 1;
    m.inputs[Guttur::RESET_INPUT].setVoltage(10.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.inputs[Guttur::RESET_INPUT].setVoltage(0.f);
    Stats r;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.process(makeArgs(frame++));
        r.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
    }
    report("guttur", "reset_reignites", r.rms(), r.nans == 0 && r.rms() > 0.01);
    // hostile: everything cranked + spread + fastest glide preset sweeps
    m.params[Guttur::DRIVE_PARAM].setValue(10.f);
    m.params[Guttur::RATE_PARAM].setValue(5.f);
    m.params[Guttur::Q_PARAM].setValue(1.f);
    m.params[Guttur::LEVEL_PARAM].setValue(3.5f);
    m.params[Guttur::SPREAD_PARAM].setValue(1.f);
    m.params[Guttur::GAINA_PARAM].setValue(2.f);
    m.params[Guttur::GAINB_PARAM].setValue(2.f);
    Stats h;
    for (int i = 0; i < (int)(10 * SR); i++) {
        if (i % (int)SR == 0) {
            // stomp through the factory banks while everything is hot
            m.params[Guttur::BANKA_PARAM].setValue(1.f + (i / (int)SR) * 2.f);
            m.params[Guttur::BANKB_PARAM].setValue(20.f - (i / (int)SR) * 2.f);
        }
        m.process(makeArgs(frame++));
        h.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        h.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
    }
    report("guttur", "hostile_nans", h.nans, h.nans == 0);
    report("guttur", "hostile_bounded", h.peak, h.peak <= 10.01f);
    // raw-Duffing mode ("snazzy clicks"): bounded by the unit clip, alive
    m.params[Guttur::FILT_PARAM].setValue(0.f);
    m.params[Guttur::LEVEL_PARAM].setValue(1.4f);
    for (int i = 0; i < (int)(1 * SR); i++) m.process(makeArgs(frame++));
    Stats raw;
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.process(makeArgs(frame++));
        raw.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
    }
    report("guttur", "raw_nans", raw.nans, raw.nans == 0);
    report("guttur", "raw_alive", raw.rms(), raw.rms() > 0.01);
    report("guttur", "raw_bounded", raw.peak, raw.peak <= 10.01f);
    // back to filters + distortion sweep at runtime: click-free-ish is by
    // ear, but at least every type must stay finite
    m.params[Guttur::FILT_PARAM].setValue(1.f);
    Stats dist;
    for (int type = 0; type <= 4; type++) {
        m.params[Guttur::DIST_PARAM].setValue((float)type);
        for (int i = 0; i < (int)(1 * SR); i++) {
            m.process(makeArgs(frame++));
            dist.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        }
    }
    report("guttur", "dist_sweep_nans", dist.nans, dist.nans == 0);
    report("guttur", "dist_sweep_bounded", dist.peak, dist.peak <= 10.01f);
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
    testPerge();
    testVorax();
    testTextor();
    testImber();
    testSylla();
    testGuttur();
    return failures ? 1 : 0;
}
