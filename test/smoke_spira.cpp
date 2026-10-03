// smoke_spira - offline sanity checks for the spira module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by spira_probe, which needs no Rack. These are the
// module's own checks: that the defaults grow circles off an input, the
// direction switch reads the right way up, BIRTH and HOLD (buttons, jacks,
// the latch surviving a save), the right input normalled from the left, the
// CV scaling, NaN at every input, the buffer swap after a sample rate
// change, including a module deleted in the middle of one, and the output
// stage, LIMIT or SATURATE, and no click from a MIX step or a quick HOLD.

#include "smoke_harness.hpp"
#include "../src/spira.cpp"

#include <chrono>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

static void connect(Spira& m, int input, float v) {
    m.inputs[input].channels = 1;
    m.inputs[input].setVoltage(v);
}

struct Counter {
    bool prev = false;
    int n = 0;
    void add(float v) {
        bool h = v > 5.f;
        if (h && !prev) n++;
        prev = h;
    }
};

static float sine(long fr, float hz = 220.f) { return 5.f * std::sin(2.f * (float)M_PI * hz * fr / SR); }

static int sounding(Spira& m) {
    int n = 0;
    for (const spira::Circle& c : m.engine.circle) n += c.on && !c.dying;
    return n;
}

// Out of the box: RATE 0.5 Hz grows a circle every two seconds off the input.
static void testDefaults() {
    Spira m; long fr = 0;
    // patched, as a cable would: Rack sets no channel count on an empty jack
    m.outputs[Spira::TURN_OUTPUT].channels = 1;
    m.outputs[Spira::VOCT_OUTPUT].channels = 1;
    Stats s;
    Counter turns[spira::kCircles];
    int most = 0;
    for (long i = 0; i < (long)(6.f * SR); i++) {
        connect(m, Spira::IN_L_INPUT, sine(fr));
        m.process(makeArgs(fr++));
        s.add(m.outputs[Spira::OUT_L_OUTPUT].getVoltage());
        for (int c = 0; c < spira::kCircles; c++) turns[c].add(m.outputs[Spira::TURN_OUTPUT].getVoltage(c));
        most = std::max(most, sounding(m));
    }
    report("spira", "defaults_nans", s.nans, s.nans == 0);
    report("spira", "defaults_peak_v", s.peak, s.peak > 3.f && s.peak <= 12.f);
    report("spira", "defaults_circles", most, most >= 2 && most <= spira::kCircles);
    // three circles, born 2 s apart, each on its own channel: the first turns
    // four times a second for its whole life
    int total = 0;
    for (const Counter& c : turns) total += c.n;
    report("spira", "defaults_turns_on_channel_1", turns[0].n, turns[0].n > 16);
    report("spira", "defaults_turns_all_channels", total, total > turns[0].n + 12);
    report("spira", "turn_voct_eight_channels", m.outputs[Spira::TURN_OUTPUT].getChannels(),
           m.outputs[Spira::TURN_OUTPUT].getChannels() == 8 && m.outputs[Spira::VOCT_OUTPUT].getChannels() == 8);
    report("spira", "defaults_buffer", (double)m.live.n,
           m.live.n == spira::Engine::bufferSamples(SR) && !m.swapFailed);
}

// Up is forward, as the panel labels it; the widget numbers from the bottom.
static void testDirection() {
    Spira m; long fr = 0;
    m.params[Spira::DIRECTION_PARAM].setValue(2.f);
    m.process(makeArgs(fr++));
    bool top = m.ctl.direction == spira::DIR_FORWARD;
    m.params[Spira::DIRECTION_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    bool mid = m.ctl.direction == spira::DIR_PINGPONG;
    m.params[Spira::DIRECTION_PARAM].setValue(0.f);
    m.process(makeArgs(fr++));
    bool bottom = m.ctl.direction == spira::DIR_REVERSE;
    report("spira", "direction_switch_reads_up_as_forward", top && mid && bottom, top && mid && bottom);
}

// BIRTH, with RATE off: the button and the jack each grow one circle.
static void testBirth() {
    Spira m; long fr = 0;
    m.params[Spira::RATE_PARAM].setValue(0.f);
    for (long i = 0; i < (long)SR; i++) {
        connect(m, Spira::IN_L_INPUT, sine(fr));
        m.process(makeArgs(fr++));
    }
    report("spira", "rate_off_no_circles", sounding(m), sounding(m) == 0);
    m.params[Spira::BIRTH_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    m.params[Spira::BIRTH_PARAM].setValue(0.f);
    m.process(makeArgs(fr++));
    report("spira", "birth_button", sounding(m), sounding(m) == 1);
    // a trigger at the jack, offset: a SchmittTrigger starts high
    connect(m, Spira::BIRTH_INPUT, 0.f);
    for (int i = 0; i < 10; i++) m.process(makeArgs(fr++));
    connect(m, Spira::BIRTH_INPUT, 10.f);
    m.process(makeArgs(fr++));
    report("spira", "birth_jack", sounding(m), sounding(m) == 2);
    // SKIPS full: the jack's births are all skipped, the button's never
    m.params[Spira::SKIPS_PARAM].setValue(1.f);
    for (int k = 0; k < 5; k++) {
        connect(m, Spira::BIRTH_INPUT, 0.f);
        for (int i = 0; i < 10; i++) m.process(makeArgs(fr++));
        connect(m, Spira::BIRTH_INPUT, 10.f);
        m.process(makeArgs(fr++));
    }
    int before = sounding(m);
    m.params[Spira::BIRTH_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    m.params[Spira::BIRTH_PARAM].setValue(0.f);
    m.process(makeArgs(fr++));
    report("spira", "skips_jack_not_button", before * 10 + sounding(m), before == 2 && sounding(m) == 3);
}

// The BIRTH light flashes for births only, not for every lap; the ring
// lights the new circle's place, in yellow for a plain circle and orange-red
// for an inward spiral.
static void testLights() {
    Spira m; long fr = 0;
    m.params[Spira::RATE_PARAM].setValue(0.f);
    m.params[Spira::SIZE_PARAM].setValue(spira::sizeKnob(0.1f));
    m.params[Spira::FADE_PARAM].setValue(0.f);
    auto run = [&](float seconds, float& birthMax, float& ringMax) {
        birthMax = ringMax = 0.f;
        for (long i = 0; i < (long)(seconds * SR); i++) {
            connect(m, Spira::IN_L_INPUT, sine(fr));
            m.process(makeArgs(fr++));
            birthMax = std::max(birthMax, m.lights[Spira::BIRTH_LIGHT].getBrightness());
            ringMax = std::max(ringMax, m.lights[Spira::RING1_LIGHT].getBrightness());
        }
    };
    float b, r;
    run(1.f, b, r);
    m.params[Spira::BIRTH_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    m.params[Spira::BIRTH_PARAM].setValue(0.f);
    run(0.1f, b, r);
    report("spira", "birth_light_on_birth", b, b > 0.5f);
    report("spira", "ring_light_up", r, r > 0.3f);
    // ten more laps of 100 ms: the circle turns, the BIRTH light stays dark
    run(1.f, b, r);
    report("spira", "birth_light_not_on_laps", b, b == 0.f);
    float red = m.lights[Spira::RING1_LIGHT].getBrightness();
    float green = m.lights[Spira::RING1_LIGHT_G].getBrightness();
    float blue = m.lights[Spira::RING1_LIGHT_B].getBrightness();
    report("spira", "ring_plain_circle_yellow", green / std::max(red, 1e-6f),
           red > 0.3f && green > 0.6f * red && blue < 0.05f * red);

    // an inward spiral heats toward orange-red, on the next place round
    m.params[Spira::SPIRAL_PARAM].setValue(-1.f);
    m.params[Spira::SIZE_PARAM].setValue(spira::sizeKnob(2.f));
    m.params[Spira::BIRTH_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    m.params[Spira::BIRTH_PARAM].setValue(0.f);
    run(0.3f, b, r);
    red = m.lights[Spira::RING2_LIGHT].getBrightness();
    green = m.lights[Spira::RING2_LIGHT_G].getBrightness();
    report("spira", "ring_inward_orange_red", green / std::max(red, 1e-6f), red > 0.3f && green < 0.35f * red);
    // V/OCT: a short inward circle, on channel 3 as the third birth, climbs
    // an octave a lap (x0.5 on tape)
    m.params[Spira::SIZE_PARAM].setValue(spira::sizeKnob(0.1f));
    m.params[Spira::BIRTH_PARAM].setValue(1.f);
    m.process(makeArgs(fr++));
    m.params[Spira::BIRTH_PARAM].setValue(0.f);
    run(0.12f, b, r);
    float v3 = m.outputs[Spira::VOCT_OUTPUT].getVoltage(2);
    report("spira", "voct_per_circle", v3, std::fabs(v3 - 1.f) < 1e-3f);
}

// HOLD: the button latches, the gate holds while high, the latch is saved.
static void testHold() {
    Spira m; long fr = 0;
    m.process(makeArgs(fr++));   // a BooleanTrigger starts high
    auto press = [&]() {
        m.params[Spira::HOLD_PARAM].setValue(1.f);
        m.process(makeArgs(fr++));
        m.params[Spira::HOLD_PARAM].setValue(0.f);
        m.process(makeArgs(fr++));
    };
    press();
    bool on = m.holdLatched && m.engine.held;
    press();
    bool off = !m.holdLatched && !m.engine.held;
    report("spira", "hold_button_latches", on && off, on && off);
    connect(m, Spira::HOLD_INPUT, 10.f);
    m.process(makeArgs(fr++));
    bool gate = m.engine.held;
    connect(m, Spira::HOLD_INPUT, 0.f);
    m.process(makeArgs(fr++));
    report("spira", "hold_gate", gate && !m.engine.held, gate && !m.engine.held);

    press();
    m.keepBirth = true;
    json_t* j = m.dataToJson();
    Spira n;
    n.dataFromJson(j);
    json_decref(j);
    report("spira", "json_hold_and_menu", n.holdLatched && n.keepBirth, n.holdLatched && n.keepBirth);
}

// The right input follows the left until patched: centred circles and the
// line come out the same on both sides.
static void testNormal() {
    Spira m; long fr = 0;
    m.params[Spira::SPREAD_PARAM].setValue(0.f);
    float worst = 0.f;
    for (long i = 0; i < (long)(3.f * SR); i++) {
        connect(m, Spira::IN_L_INPUT, sine(fr));
        m.process(makeArgs(fr++));
        worst = std::max(worst, std::fabs(m.outputs[Spira::OUT_L_OUTPUT].getVoltage()
                                          - m.outputs[Spira::OUT_R_OUTPUT].getVoltage()));
    }
    report("spira", "right_normalled_from_left", worst, worst < 1e-4f);
}

// CV: SIZE and RATE 1 V/oct, SPIRAL and SHAPE +-5 V over the knob, V/OCT,
// MIX and LEVEL.
static void testCv() {
    Spira m; long fr = 0;
    m.params[Spira::SIZE_PARAM].setValue(spira::sizeKnob(0.25f));
    connect(m, Spira::SIZE_INPUT, 1.f);
    connect(m, Spira::RATE_INPUT, 1.f);
    connect(m, Spira::SPIRAL_INPUT, -5.f);
    connect(m, Spira::VOCT_INPUT, 1.f);
    m.process(makeArgs(fr++));
    report("spira", "size_cv_doubles", m.ctl.size, std::fabs(m.ctl.size - 0.5f) < 1e-3f);
    report("spira", "rate_cv_doubles", m.ctl.rate, std::fabs(m.ctl.rate - 1.f) < 1e-3f);
    report("spira", "spiral_cv_covers_knob", m.ctl.spiral, std::fabs(m.ctl.spiral - 0.5f) < 1e-5f);
    report("spira", "voct_is_12_semitones", m.ctl.pitch, std::fabs(m.ctl.pitch - 12.f) < 1e-4f);
    m.params[Spira::RATE_PARAM].setValue(0.f);
    m.process(makeArgs(fr++));
    report("spira", "rate_cv_leaves_off_off", m.ctl.rate, m.ctl.rate == 0.f);
    // MIX: 0..10 V over the knob; LEVEL: 2.4 dB/V, below the knob to -72 dB
    connect(m, Spira::MIX_INPUT, 5.f);
    connect(m, Spira::LEVEL_INPUT, -5.f);
    m.process(makeArgs(fr++));
    report("spira", "mix_cv_5v_adds_half", m.ctl.mix, std::fabs(m.ctl.mix - 1.f) < 1e-5f);
    report("spira", "level_cv_minus_5v_is_minus_12db", m.ctl.level, std::fabs(m.ctl.level + 12.f) < 1e-4f);
    connect(m, Spira::LEVEL_INPUT, -10.f);
    m.params[Spira::LEVEL_PARAM].setValue(-12.f);
    m.process(makeArgs(fr++));
    report("spira", "level_cv_below_the_knob", m.ctl.level, std::fabs(m.ctl.level + 36.f) < 1e-4f);
    connect(m, Spira::SKIPS_INPUT, 2.5f);
    m.process(makeArgs(fr++));
    report("spira", "skips_cv_2v5_is_quarter", m.ctl.skips, std::fabs(m.ctl.skips - 0.25f) < 1e-5f);
}

// NaN at every input, with circles sounding: every output stays finite.
static void testNan() {
    Spira m; long fr = 0;
    m.params[Spira::RATE_PARAM].setValue(1.f);
    for (long i = 0; i < (long)SR; i++) {
        connect(m, Spira::IN_L_INPUT, sine(fr));
        m.process(makeArgs(fr++));
    }
    for (int in = 0; in < Spira::INPUTS_LEN; in++) connect(m, in, NAN);
    Stats s;
    for (long i = 0; i < (long)SR; i++) {
        m.process(makeArgs(fr++));
        for (int o = 0; o < Spira::OUTPUTS_LEN; o++) s.add(m.outputs[o].getVoltage());
    }
    report("spira", "nan_inputs", s.nans, s.nans == 0);
}

static void settleSwap(Spira& m, long& fr) {
    for (int i = 0; i < 4000 && (m.job || m.swapGain < 1.f); i++) {
        m.process(makeArgs(fr++));
        if (m.job) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A sample rate change swaps the buffer for one of the new size, faded.
static void testSampleRate() {
    Spira m; long fr = 0;
    m.engine.setSampleRate(96000.f);
    float lowest = 10.f;
    for (int i = 0; i < 960; i++) {
        connect(m, Spira::IN_L_INPUT, 1.f);
        m.process(makeArgs(fr++));
        lowest = std::min(lowest, std::fabs(m.outputs[Spira::OUT_L_OUTPUT].getVoltage()));
    }
    settleSwap(m, fr);
    report("spira", "rate_change_buffer", (double)m.live.n,
           m.live.n == spira::Engine::bufferSamples(96000.f) && !m.swapFailed);
    report("spira", "rate_change_fades", lowest, lowest < 0.05f);
    report("spira", "rate_change_back_in", m.swapGain, m.swapGain == 1.f);
}

// Deleted while the worker is still allocating: the job outlives the module.
static void testDeleteMidSwap() {
    for (int k = 0; k < 10; k++) {
        Spira* m = new Spira; long fr = 0;
        m->engine.setSampleRate(k % 2 ? 96000.f : 44100.f);
        while (!m->job) m->process(makeArgs(fr++));
        delete m;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    report("spira", "delete_mid_swap", 0, true);
}

// Every factory preset loads, grows circles off a sine and stays finite and
// under the limiter.
static void testPresets() {
    const std::string dir = "../presets/spira/";
    std::vector<std::string> files;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".vcvm") files.push_back(n);
        }
        closedir(d);
    }
    int bad = 0;
    for (const std::string& f : files) {
        json_t* root = json_load_file((dir + f).c_str(), 0, nullptr);
        if (!root) { bad++; continue; }
        Spira m; long fr = 0;
        json_t* ps = json_object_get(root, "params");
        for (size_t i = 0; i < json_array_size(ps); i++) {
            json_t* p = json_array_get(ps, i);
            int id = (int)json_integer_value(json_object_get(p, "id"));
            if (id >= 0 && id < Spira::PARAMS_LEN)
                m.params[id].setValue((float)json_number_value(json_object_get(p, "value")));
        }
        m.dataFromJson(json_object_get(root, "data"));
        json_decref(root);
        Stats s;
        int most = 0;
        for (long i = 0; i < (long)(8.f * SR); i++) {
            connect(m, Spira::IN_L_INPUT, sine(fr, 110.f + 50.f * (i / 24000)));
            m.process(makeArgs(fr++));
            s.add(m.outputs[Spira::OUT_L_OUTPUT].getVoltage());
            most = std::max(most, sounding(m));
        }
        printf("# %-14s rms %.2f V, peak %.2f V, up to %d circles\n", f.c_str(), s.rms(), s.peak, most);
        if (s.nans || s.peak > 12.f || most == 0) bad++;
    }
    report("spira", "presets_found", files.size(), files.size() == 8);
    report("spira", "presets_run", bad, bad == 0);
}

// The output stage. LIMIT (the default) holds a pile of loud circles at 8 V
// and leaves a 7 V line alone; SATURATE bends the same pile toward 10 V. The
// menu survives a save, and a patch from before it loads as LIMIT.
static float outputPeak(bool saturate, float levelDb, float amp, float mix, float* rms = nullptr) {
    Spira m; long fr = 0;
    m.saturate = saturate;
    m.params[Spira::RATE_PARAM].setValue(spira::rateKnob(2.f));
    m.params[Spira::FADE_PARAM].setValue(0.f);
    m.params[Spira::SIZE_PARAM].setValue(spira::sizeKnob(1.3f));
    m.params[Spira::LEVEL_PARAM].setValue(levelDb);
    m.params[Spira::MIX_PARAM].setValue(mix);
    Stats st;
    for (long i = 0; i < (long)(6.f * SR); i++) {
        connect(m, Spira::IN_L_INPUT, amp / 5.f * sine(fr, 110.f));
        m.process(makeArgs(fr++));
        if (i > (long)SR) st.add(m.outputs[Spira::OUT_L_OUTPUT].getVoltage());
    }
    if (rms) *rms = st.rms();
    return st.peak;
}

static void testOutput() {
    float lim = outputPeak(false, 12.f, 10.f, 0.5f);   // hot enough that the pile always overdrives
    float sat = outputPeak(true, 12.f, 5.f, 0.5f);
    float line = outputPeak(false, 0.f, 7.f, 0.f);
    printf("# a pile at +12 dB: limit peak %.3f V, saturate peak %.3f V; a 7 V line: %.4f V\n", lim, sat, line);
    report("spira", "limit_holds_8v", lim, lim <= 8.001f && lim > 7.9f);
    report("spira", "saturate_bends_past_8v", sat, sat > 9.f && sat <= 10.f);
    report("spira", "limit_leaves_a_7v_line", line, std::fabs(line - 7.f) < 0.01f);

    Spira m;
    m.saturate = true;
    json_t* j = m.dataToJson();
    Spira n;
    n.dataFromJson(j);
    json_decref(j);
    json_t* old = json_pack("{s:b, s:b}", "hold", 0, "keepBirth", 0);
    Spira o;
    o.saturate = true;
    o.dataFromJson(old);
    json_decref(old);
    report("spira", "json_output_stage", n.saturate && !o.saturate, n.saturate && !o.saturate);
}

// A click is a step: the largest second difference of the output in the
// 30 ms after a change, against the 30 ms before it, on a 220 Hz sine with
// circles playing. Unsmoothed, a 5 V MIX step measured about 300 times the
// baseline, and HOLD back on 4 ms after going off about 70 times.
static float maxD2(Spira& m, long& fr, double sec, float* last) {
    float worst = 0.f;
    for (long i = 0; i < (long)(sec * SR); i++) {
        connect(m, Spira::IN_L_INPUT, sine(fr));
        m.process(makeArgs(fr++));
        float y = m.outputs[Spira::OUT_L_OUTPUT].getVoltage();
        worst = std::max(worst, std::fabs(y - 2.f * last[1] + last[0]));
        last[0] = last[1];
        last[1] = y;
    }
    return worst;
}

static void testClicks() {
    float mixWorst = 0.f, holdWorst = 0.f;
    for (int t = 0; t < 4; t++) {
        for (int which = 0; which < 2; which++) {
            Spira m; long fr = 0;
            float last[2] = {0.f, 0.f};
            connect(m, Spira::MIX_INPUT, 0.f);
            connect(m, Spira::HOLD_INPUT, 0.f);
            maxD2(m, fr, 5.0 + 0.37 * t, last);
            float before = std::max(maxD2(m, fr, 0.03, last), 1e-3f);
            float after;
            if (which == 0) {
                connect(m, Spira::MIX_INPUT, 5.f);
                after = maxD2(m, fr, 0.03, last);
                mixWorst = std::max(mixWorst, after / before);
            } else {
                connect(m, Spira::HOLD_INPUT, 10.f);
                maxD2(m, fr, 0.2, last);
                connect(m, Spira::HOLD_INPUT, 0.f);
                after = maxD2(m, fr, 0.004, last);
                connect(m, Spira::HOLD_INPUT, 10.f);
                after = std::max(after, maxD2(m, fr, 0.03, last));
                holdWorst = std::max(holdWorst, after / before);
            }
        }
    }
    report("spira", "mix_step_no_click", mixWorst, mixWorst < 3.f);
    report("spira", "hold_back_on_no_click", holdWorst, holdWorst < 8.f);
}

SMOKE_MAIN(testDefaults, testDirection, testBirth, testLights, testHold, testNormal, testCv, testNan,
           testSampleRate, testDeleteMidSwap, testPresets, testOutput, testClicks)
