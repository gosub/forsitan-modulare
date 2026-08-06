// smoke_caligo — offline sanity checks for the caligo Greyhole port.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"

#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
#include "../src/caligo.cpp"

static uint32_t rngState = 0x12345678u;
static float noise() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return ((float)(rngState >> 8) * (1.f / 16777216.f) - 0.5f) * 10.f;
}

// run `secs` of audio, optionally feeding noise, collecting the outputs.
// `loopGain` >= 0 patches the send back into the return at that gain, which is
// the only way an offline test can exercise the feedback-loop break.
static void run(Caligo& m, long& frame, double secs, bool feed, Stats* s = nullptr,
                float loopGain = -1.f) {
    if (loopGain >= 0.f) {
        m.inputs[Caligo::RTN_L_INPUT].channels = 1;
        m.inputs[Caligo::RTN_R_INPUT].channels = 1;
    }
    for (int i = 0; i < (int)(secs * SR); i++) {
        m.inputs[Caligo::IN_L_INPUT].setVoltage(feed ? noise() : 0.f);
        m.process(makeArgs(frame++));
        if (loopGain >= 0.f) {
            m.inputs[Caligo::RTN_L_INPUT].setVoltage(
                m.outputs[Caligo::SND_L_OUTPUT].getVoltage() * loopGain);
            m.inputs[Caligo::RTN_R_INPUT].setVoltage(
                m.outputs[Caligo::SND_R_OUTPUT].getVoltage() * loopGain);
        }
        if (s) {
            s->add(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage());
            s->add(m.outputs[Caligo::OUT_R_OUTPUT].getVoltage());
        }
    }
}

// largest sample-to-sample jump on either output over `secs`
static double maxSlew(Caligo& m, long& frame, double secs, bool feed) {
    double worst = 0.0;
    float pl = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
    float pr = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
    for (int i = 0; i < (int)(secs * SR); i++) {
        m.inputs[Caligo::IN_L_INPUT].setVoltage(feed ? noise() : 0.f);
        m.process(makeArgs(frame++));
        float l = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        float r = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
        worst = std::max(worst, (double)std::fabs(l - pl));
        worst = std::max(worst, (double)std::fabs(r - pr));
        pl = l;
        pr = r;
    }
    return worst;
}

static void setWet(Caligo& m) { m.params[Caligo::MIX_PARAM].setValue(1.f); }

static void testQuiet() {
    Caligo m;
    long frame = 0;
    Stats s;
    run(m, frame, 2.0, false, &s);
    report("caligo", "silent_without_input", s.rms(), s.rms() < 1e-4);
    report("caligo", "silent_nans", s.nans, s.nans == 0);
}

static void testDryPath() {
    Caligo m;
    long frame = 0;
    m.params[Caligo::MIX_PARAM].setValue(0.f);
    run(m, frame, 0.2, true);           // let the smoothers settle
    double err = 0.0, ref = 0.0;
    for (int i = 0; i < (int)(0.5 * SR); i++) {
        float in = noise();
        m.inputs[Caligo::IN_L_INPUT].setVoltage(in);
        m.process(makeArgs(frame++));
        err = std::max(err,
            (double)std::fabs(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage() - in));
        ref = std::max(ref, (double)std::fabs(in));
    }
    report("caligo", "dry_passthrough_err", err, err < 1e-3 * ref);
}

// the defaults: 400 ms of delay at 90% feedback, so the repeats have to be
// audible after the input stops and gone a long while later
static void testTail() {
    Caligo m;
    long frame = 0;
    setWet(m);
    run(m, frame, 1.0, true);
    Stats wet;
    run(m, frame, 0.5, false, &wet);
    run(m, frame, 20.0, false);
    Stats late;
    run(m, frame, 1.0, false, &late);
    report("caligo", "tail_alive", wet.rms(), wet.rms() > 0.05);
    report("caligo", "tail_decays", late.rms(), late.rms() < wet.rms() * 0.05);
}

// diff at zero collapses the diffuser to one plain delay line: 24 lines in
// series, ~71 ms at 48 kHz, and nothing else. So the burst comes out once at
// that delay, then again every 400 ms, with real silence in between — which a
// diffused setting does not have.
static void testPlainDelay() {
    Caligo m;
    long frame = 0;
    setWet(m);
    m.params[Caligo::DIFF_PARAM].setValue(0.f);
    m.params[Caligo::MOD_PARAM].setValue(0.f);
    run(m, frame, 1.5, false);           // let the long delay's crossfade settle
    run(m, frame, 0.05, true);           // a 50 ms burst
    Stats direct, gap, echo;
    run(m, frame, 0.10, false, &direct); // +50..150 ms: the chain's own output
    run(m, frame, 0.33, false, &gap);    // +150..480 ms: dead
    run(m, frame, 0.17, false, &echo);   // +480..650 ms: the first repeat
    report("caligo", "plain_delay_direct", direct.rms(), direct.rms() > 0.05);
    report("caligo", "plain_delay_gap", gap.rms(), gap.rms() < direct.rms() * 0.05);
    report("caligo", "plain_delay_echo", echo.rms(), echo.rms() > direct.rms() * 0.2);
}

// spin fully CCW leaves the two channels unconnected, so a mono input must
// come out decorrelated (different prime lengths per channel) but never
// correlated the way the interleaved default is not
static void testSpin() {
    auto corrOf = [](float spin) {
        Caligo m;
        long frame = 0;
        setWet(m);
        m.params[Caligo::SPIN_PARAM].setValue(spin);
        m.params[Caligo::MOD_PARAM].setValue(0.f);
        for (int i = 0; i < (int)(1.0 * SR); i++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
            m.process(makeArgs(frame++));
        }
        double sa = 0, sb = 0, sab = 0;
        for (int i = 0; i < (int)(2.0 * SR); i++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
            m.process(makeArgs(frame++));
            double l = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
            double r = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
            sa += l * l; sb += r * r; sab += l * r;
        }
        return sab / (std::sqrt(sa * sb) + 1e-30);
    };
    double wide = corrOf(0.f);
    double churn = corrOf(1.f);
    report("caligo", "spin_zero_decorrelated", std::fabs(wide), std::fabs(wide) < 0.6);
    report("caligo", "spin_full_decorrelated", std::fabs(churn), std::fabs(churn) < 0.3);
}

// freeze: unity feedback, input shut out. The cloud must hold its level for a
// long time and must not click on the way in.
static void testFreeze() {
    Caligo m;
    long frame = 0;
    setWet(m);
    run(m, frame, 1.5, true);
    Stats before;
    run(m, frame, 0.2, true, &before);
    double steady = maxSlew(m, frame, 0.2, true);

    m.params[Caligo::FRZ_PARAM].setValue(1.f);
    m.process(makeArgs(frame++));
    m.params[Caligo::FRZ_PARAM].setValue(0.f);
    double onFreeze = maxSlew(m, frame, 0.1, false);

    Stats held;
    run(m, frame, 1.0, false, &held);
    run(m, frame, 15.0, false);
    Stats late;
    run(m, frame, 1.0, false, &late);
    report("caligo", "freeze_holds", late.rms(),
           late.rms() > held.rms() * 0.3 && late.rms() < held.rms() * 3.0);
    report("caligo", "freeze_no_click", onFreeze, onFreeze < steady * 3.0 + 0.05);
    report("caligo", "freeze_bounded", late.peak, late.peak <= 10.001f);
    report("caligo", "freeze_nans", late.nans, late.nans == 0);
}

// scatter slides the whole network into a new configuration over ~0.23 s, so
// it must change the sound without a discontinuity
static void testScatter() {
    Caligo m;
    long frame = 0;
    setWet(m);
    run(m, frame, 1.5, true);
    double steady = maxSlew(m, frame, 0.2, true);
    uint32_t before = m.scatterSeed;

    m.params[Caligo::SCT_PARAM].setValue(1.f);
    m.process(makeArgs(frame++));
    m.params[Caligo::SCT_PARAM].setValue(0.f);
    double onScatter = maxSlew(m, frame, 0.5, true);

    Stats s;
    run(m, frame, 2.0, true, &s);
    report("caligo", "scatter_reseeds", (double)m.scatterSeed, m.scatterSeed != before);
    report("caligo", "scatter_no_click", onScatter, onScatter < steady * 3.0 + 0.05);
    report("caligo", "scatter_nans", s.nans, s.nans == 0);
    report("caligo", "scatter_alive", s.rms(), s.rms() > 0.05);

    // the seed walk is deterministic, so the same number of presses from the
    // same start has to land in the same room
    Caligo a, b;
    a.scatterSeed = b.scatterSeed = 0;
    for (int i = 0; i < 5; i++) { a.nextScatterSeed(); b.nextScatterSeed(); }
    report("caligo", "scatter_deterministic", (double)a.scatterSeed,
           a.scatterSeed == b.scatterSeed && a.scatterSeed != 0);
}

// Moving `size` must not add broadband noise. The network is linear, so with a
// sine in and only size changing the output has to stay near the input's band;
// any HF energy is interpolation artifact. This is the regression test for two
// bugs that made sweeping size (and drift, which sweeps it for you) crackle:
// an allpass-interpolated delay line, discontinuous every time a glide crossed
// an integer, and a float32 glide that stalled short of its target and parked
// all 24 lengths off-integer for ever.
static void testSizeMoveIsQuiet() {
    auto hfOf = [](int mode) {          // 0 static, 1 sweep, 2 drift
        Caligo m;
        setWet(m);
        m.params[Caligo::MOD_PARAM].setValue(0.f);
        m.params[Caligo::FEEDBACK_PARAM].setValue(0.9f);
        if (mode == 2) m.params[Caligo::DRIFT_PARAM].setValue(1.f);
        long frame = 0;
        double hp[4] = {0, 0, 0, 0};
        const double a = std::exp(-2.0 * M_PI * 5000.0 / SR);
        double eTot = 0, eHf = 0;
        for (int i = 0; i < (int)(20.0 * SR); i++) {
            float t = (float)i / SR;
            if (mode == 1)      // size 0.8..2.0 at 0.1 Hz, in knob terms
                m.params[Caligo::SIZE_PARAM].setValue(
                    0.226f + 0.215f * (1.f - std::cos(2.f * M_PI * 0.1f * t)));
            m.inputs[Caligo::IN_L_INPUT].setVoltage(
                2.f * std::sin(2.0 * M_PI * 200.0 * i / SR));
            m.process(makeArgs(frame++));
            double x = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
            double y = x;
            for (int k = 0; k < 4; k++) { hp[k] = y * (1 - a) + hp[k] * a; y -= hp[k]; }
            if (t > 3.f) { eTot += x * x; eHf += y * y; }
        }
        return 10.0 * std::log10((eHf + 1e-30) / (eTot + 1e-30));
    };
    double stat = hfOf(0), sweep = hfOf(1), drift = hfOf(2);
    report("caligo", "size_static_hf_dBr", stat, stat < -70.0);
    report("caligo", "size_sweep_hf_dBr", sweep, sweep < -70.0);
    report("caligo", "size_drift_hf_dBr", drift, drift < -70.0);
}

// The lengths must actually arrive on their integers, and from the first
// control block: the glide is primed at its target the way the original's
// smooth_init is, so a patch does not load with 24 lines gliding in.
static void testGlideConverges() {
    for (float sizeKnob : {0.f, 1.f / 3.f, 1.f}) {
        Caligo m;
        setWet(m);
        m.params[Caligo::SIZE_PARAM].setValue(sizeKnob);
        long frame = 0;
        for (int i = 0; i < (int)(0.5 * SR); i++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
            m.process(makeArgs(frame++));
        }
        int off = 0;
        double worst = 0.0;
        for (int i = 0; i < caligo_dsp::kStages; i++)
            for (int j = 0; j < caligo_dsp::kNest; j++)
                for (float d : {m.engine.level[i][j].dL, m.engine.level[i][j].dR}) {
                    double f = d - std::floor(d);
                    if (f > 0.5) f = 1.0 - f;
                    if (f > 1e-7) off++;
                    worst = std::max(worst, f);
                }
        char name[64];
        std::snprintf(name, sizeof(name), "glide_exact_at_size_knob_%.2f", sizeKnob);
        report("caligo", name, worst, off == 0);
    }
}

// tape mode Doppler-shifts instead of dissolving: sweeping the time knob has
// to stay bounded and clean in both modes
static void testTimeModes() {
    for (int mode = 0; mode < 2; mode++) {
        Caligo m;
        long frame = 0;
        setWet(m);
        m.p.tape = (mode == 1);
        m.params[Caligo::FEEDBACK_PARAM].setValue(0.95f);
        Stats s;
        for (int i = 0; i < (int)(8.0 * SR); i++) {
            float t = (float)i / SR;
            m.params[Caligo::TIME_PARAM].setValue(0.35f + 0.25f * std::sin(2.f * M_PI * 0.5f * t));
            m.inputs[Caligo::IN_L_INPUT].setVoltage(t < 4.f ? noise() : 0.f);
            m.process(makeArgs(frame++));
            s.add(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage());
            s.add(m.outputs[Caligo::OUT_R_OUTPUT].getVoltage());
        }
        const char* tag = mode ? "tape" : "dissolve";
        report("caligo", (std::string("sweep_") + tag + "_nans").c_str(), s.nans, s.nans == 0);
        report("caligo", (std::string("sweep_") + tag + "_alive").c_str(), s.rms(), s.rms() > 0.02);
        report("caligo", (std::string("sweep_") + tag + "_bounded").c_str(), s.peak,
               s.peak <= 10.001f);
    }
}

// feedback at 120% with the loop patched through at unity gain on top: the
// saturator is the only thing standing between this and infinity
static void testRunaway() {
    Caligo m;
    long frame = 0;
    setWet(m);
    m.params[Caligo::FEEDBACK_PARAM].setValue(1.2f);
    m.params[Caligo::DAMP_PARAM].setValue(0.f);
    run(m, frame, 1.0, true, nullptr, 1.f);
    Stats s;
    run(m, frame, 20.0, false, &s, 1.f);
    report("caligo", "runaway_nans", s.nans, s.nans == 0);
    report("caligo", "runaway_sustains", s.rms(), s.rms() > 0.05);
    report("caligo", "runaway_bounded", s.peak, s.peak <= 10.001f);

    // and with the loop patched at 4x, which no internal knob can reach
    Caligo hot;
    long f2 = 0;
    setWet(hot);
    hot.params[Caligo::FEEDBACK_PARAM].setValue(1.2f);
    run(hot, f2, 1.0, true, nullptr, 4.f);
    Stats h;
    run(hot, f2, 10.0, false, &h, 4.f);
    report("caligo", "hot_loop_nans", h.nans, h.nans == 0);
    report("caligo", "hot_loop_bounded", h.peak, h.peak <= 10.001f);
}

// The send/return break must be transparent: patching the send straight back
// into the return has to give what the unpatched module gives.
//
// Only the first pass can be compared sample by sample. The jacks round-trip
// through volts (x5 then x0.2, and 0.2f is not exactly 1/5), so a closed loop
// starts about 1e-7 away from the normalled one, and this network is chaotic
// enough to grow that. So: exact until the feedback arrives, then equal in
// level.
static void testLoopBreakTransparent() {
    Caligo open_, closed;
    long f1 = 0, f2 = 0;
    setWet(open_);
    setWet(closed);
    closed.inputs[Caligo::RTN_L_INPUT].channels = 1;
    closed.inputs[Caligo::RTN_R_INPUT].channels = 1;

    double err = 0.0, ref = 0.0;
    Stats so, sc;
    rngState = 0x777u;
    // 0.4 s is the default delay time, so the first 0.35 s is pre-feedback
    int firstPass = (int)(0.35 * SR);
    for (int i = 0; i < (int)(4.0 * SR); i++) {
        float in = noise();
        open_.inputs[Caligo::IN_L_INPUT].setVoltage(in);
        closed.inputs[Caligo::IN_L_INPUT].setVoltage(in);
        open_.process(makeArgs(f1++));
        closed.process(makeArgs(f2++));
        closed.inputs[Caligo::RTN_L_INPUT].setVoltage(
            closed.outputs[Caligo::SND_L_OUTPUT].getVoltage());
        closed.inputs[Caligo::RTN_R_INPUT].setVoltage(
            closed.outputs[Caligo::SND_R_OUTPUT].getVoltage());
        float a = open_.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        float b = closed.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        if (i < firstPass) {
            err = std::max(err, (double)std::fabs(a - b));
            ref = std::max(ref, (double)std::fabs(a));
        }
        else {
            so.add(a);
            sc.add(b);
        }
    }
    double dB = 20.0 * std::log10((sc.rms() + 1e-12) / (so.rms() + 1e-12));
    report("caligo", "loop_break_first_pass_exact", err, err < 1e-4 * (ref + 1e-9));
    report("caligo", "loop_break_level_match_dB", dB, std::fabs(dB) < 0.5);
}

// clock sync: a 2 Hz clock with the time knob at noon should land on one
// clock period of delay
static void testClockSync() {
    Caligo m;
    long frame = 0;
    setWet(m);
    m.params[Caligo::TIME_PARAM].setValue(0.5f);
    m.inputs[Caligo::CLK_INPUT].channels = 1;
    int period = (int)(SR / 2.f);
    for (int i = 0; i < (int)(4 * SR); i++) {
        m.inputs[Caligo::CLK_INPUT].setVoltage((i % period) < 100 ? 5.f : 0.f);
        m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
        m.process(makeArgs(frame++));
    }
    report("caligo", "clock_sync_time_ms", m.effTimeSec * 1000.f,
           std::fabs(m.effTimeSec - 0.5f) < 0.005f);
    report("caligo", "clock_sync_ratio", m.appliedRatio, m.appliedRatio == 7);
}

// everything hostile at once: infinite feedback, drift, spin and size swept
// at audio rate, scatter firing, freeze toggling
static void testHostile() {
    Caligo m;
    long frame = 0;
    setWet(m);
    m.params[Caligo::FEEDBACK_PARAM].setValue(1.2f);
    m.params[Caligo::DAMP_PARAM].setValue(0.f);
    m.params[Caligo::MOD_PARAM].setValue(1.f);
    m.params[Caligo::RATE_PARAM].setValue(1.f);
    m.params[Caligo::DRIFT_PARAM].setValue(1.f);
    Stats s;
    for (int i = 0; i < (int)(20 * SR); i++) {
        float t = (float)i / SR;
        m.params[Caligo::SIZE_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 300.f * t));
        m.params[Caligo::SPIN_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 700.f * t));
        m.params[Caligo::DIFF_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 90.f * t));
        m.params[Caligo::SCT_PARAM].setValue((i % (int)(1.3 * SR)) < 200 ? 1.f : 0.f);
        m.params[Caligo::FRZ_PARAM].setValue((i % (int)(3.1 * SR)) < 200 ? 1.f : 0.f);
        m.inputs[Caligo::IN_L_INPUT].setVoltage(t < 6.f ? noise() : 0.f);
        m.inputs[Caligo::IN_R_INPUT].setVoltage(t < 6.f ? noise() : 0.f);
        m.process(makeArgs(frame++));
        s.add(m.outputs[Caligo::OUT_L_OUTPUT].getVoltage());
        s.add(m.outputs[Caligo::OUT_R_OUTPUT].getVoltage());
    }
    report("caligo", "hostile_nans", s.nans, s.nans == 0);
    report("caligo", "hostile_alive", s.rms(), s.rms() > 0.01);
    report("caligo", "hostile_bounded", s.peak, s.peak <= 10.001f);
}

// every factory preset has to load, make sound and stay bounded
static void testPresets() {
    const char* dir = "../presets/caligo";
    DIR* d = opendir(dir);
    if (!d) {
        report("caligo", "presets_dir", 0, false);
        return;
    }
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".vcvm") == 0)
            files.push_back(std::string(dir) + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    report("caligo", "presets_found", (double)files.size(), files.size() == 7);

    for (const std::string& f : files) {
        json_error_t err;
        json_t* root = json_load_file(f.c_str(), 0, &err);
        std::string name = f.substr(f.rfind('/') + 1);
        name = name.substr(0, name.size() - 5);
        if (!root) {
            report("caligo", ("preset_" + name).c_str(), 0, false);
            continue;
        }
        Caligo m;
        size_t i;
        json_t* v;
        json_t* ps = json_object_get(root, "params");
        json_array_foreach(ps, i, v) {
            int id = (int)json_integer_value(json_object_get(v, "id"));
            float val = (float)json_number_value(json_object_get(v, "value"));
            if (id >= 0 && id < Caligo::PARAMS_LEN)
                m.params[id].setValue(val);
        }
        if (json_t* data = json_object_get(root, "data"))
            m.dataFromJson(data);
        json_decref(root);

        long frame = 0;
        Stats all;
        run(m, frame, 2.5, true, &all);      // excite
        Stats wet;
        run(m, frame, 0.5, false, &wet);     // the echo on its own
        run(m, frame, 0.2, false, &all);
        bool ok = all.nans == 0 && wet.nans == 0 && all.peak <= 10.001f
                  && wet.rms() > 0.01;
        report("caligo", ("preset_" + name).c_str(), wet.rms(), ok);
    }
}

SMOKE_MAIN(testQuiet, testDryPath, testTail, testPlainDelay, testSpin,
           testSizeMoveIsQuiet, testGlideConverges, testFreeze, testScatter,
           testTimeModes, testRunaway, testLoopBreakTransparent, testClockSync,
           testHostile, testPresets)
