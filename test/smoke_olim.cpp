// smoke_olim - offline sanity checks for the olim module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by olim_probe, which needs no Rack. These are the
// module's own checks: where an echo lands and its sign, the VCA normalling,
// R normalled from L, NaN at the input, the clock cable, the Memory swap and
// its fade, TIME clamped to the memory, a module deleted while its buffer
// is still being allocated, the head presets and transforms, and a slider
// move riding the next crossfades, the output limiter in both modes and the
// input fades.

#include "smoke_harness.hpp"
#include "../src/olim.cpp"

#include <chrono>
#include <thread>

static void connect(Olim& m, int input, float v) {
    m.inputs[input].channels = 1;
    m.inputs[input].setVoltage(v);
}

static void soloHead(Olim& m, int head) {
    for (int i = 0; i < olim::kHeads; i++)
        m.params[Olim::HEAD1_PARAM + i].setValue(i == head ? 1.f : 0.f);
}

// Runs until any pending buffer swap has landed and faded back in.
static void settleSwap(Olim& m, long& frame) {
    for (int k = 0; k < 2000 && (m.job || m.swapGain < 1.f); k++) {
        for (int i = 0; i < 480; i++) m.process(makeArgs(frame++));
        if (m.job) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// An impulse into head 1 alone at TIME 2 s comes back 0.25 s later, inverted,
// as the only thing on the output.
static void testEcho() {
    for (int hardware = 0; hardware < 2; hardware++) {
        Olim m; long fr = 0;
        m.engine.softLimit = !hardware;
        m.params[Olim::DRY_PARAM].setValue(0.f);
        soloHead(m, 0);
        connect(m, Olim::IN_L_INPUT, 0.f);
        // let head 1 fade to its place (from delay 0 at construction)
        for (int i = 0; i < 24000; i++) m.process(makeArgs(fr++));
        m.inputs[Olim::IN_L_INPUT].setVoltage(4.f);
        m.process(makeArgs(fr++));
        m.inputs[Olim::IN_L_INPUT].setVoltage(0.f);
        long at = -1; float peak = 0.f, v = 0.f;
        Stats s;
        for (long i = 1; i < 24000; i++) {
            m.process(makeArgs(fr++));
            float y = m.outputs[Olim::OUT_L_OUTPUT].getVoltage();
            s.add(y);
            if (std::fabs(y) > peak) { peak = std::fabs(y); at = i; v = y; }
        }
        // the soft output limiter runs its look-ahead behind; the hardware's none
        long want = 12000 + m.engine.outputLatency();
        const char* tag = hardware ? "hardware_" : "";
        report("olim", (std::string(tag) + "echo_nans").c_str(), s.nans, s.nans == 0);
        report("olim", (std::string(tag) + "echo_at_samples").c_str(), at, at == want);
        report("olim", (std::string(tag) + "echo_inverted").c_str(), v, v < -3.9f);
    }
}

// A sine through head 1, level against the head's VCA: unpatched is unity,
// 0 V is silence, 2.5 V is half.
static float headRms(float vca, bool patch) {
    Olim m; long fr = 0;
    m.params[Olim::DRY_PARAM].setValue(0.f);
    m.params[Olim::TIME_PARAM].setValue(0.1f);   // 80 ms
    soloHead(m, 7);
    if (patch) connect(m, Olim::HEAD8_VCA_INPUT, vca);
    m.inputs[Olim::IN_L_INPUT].channels = 1;
    Stats s;
    for (long i = 0; i < 48000; i++) {
        m.inputs[Olim::IN_L_INPUT].setVoltage(2.f * std::sin(2.f * M_PI * 220.f * i / SR));
        m.process(makeArgs(fr++));
        if (i > 24000) s.add(m.outputs[Olim::OUT_L_OUTPUT].getVoltage());
    }
    return (float)s.rms();
}

static void testVca() {
    float unity = headRms(0.f, false);
    float off = headRms(0.f, true);
    float half = headRms(2.5f, true);
    float over = headRms(9.f, true);
    report("olim", "vca_unpatched_rms", unity, std::fabs(unity - 1.414f) < 0.02f);
    report("olim", "vca_0v_rms", off, off < 1e-4f);
    report("olim", "vca_half_ratio", half / unity, std::fabs(half / unity - 0.5f) < 0.01f);
    report("olim", "vca_clamped_ratio", over / unity, std::fabs(over / unity - 1.f) < 0.01f);
}

// Only IN L patched: OUT R is OUT L.
static void testNormal() {
    Olim m; long fr = 0;
    m.inputs[Olim::IN_L_INPUT].channels = 1;
    float diff = 0.f;
    for (long i = 0; i < 48000; i++) {
        m.inputs[Olim::IN_L_INPUT].setVoltage(3.f * std::sin(2.f * M_PI * 110.f * i / SR));
        m.process(makeArgs(fr++));
        diff = std::max(diff, std::fabs(m.outputs[Olim::OUT_L_OUTPUT].getVoltage()
                                         - m.outputs[Olim::OUT_R_OUTPUT].getVoltage()));
    }
    report("olim", "r_normalled_from_l", diff, diff < 1e-6f);
}

// A NaN at the input must not get into the buffer, where it would play back
// for as long as the memory is.
static void testNan() {
    Olim m; long fr = 0;
    m.params[Olim::TIME_PARAM].setValue(0.1f);
    m.params[Olim::FEEDBACK_PARAM].setValue(0.5f);
    connect(m, Olim::IN_L_INPUT, NAN);
    connect(m, Olim::IN_R_INPUT, INFINITY);
    Stats s;
    for (long i = 0; i < 48000; i++) {
        m.process(makeArgs(fr++));
        s.add(m.outputs[Olim::OUT_L_OUTPUT].getVoltage());
        s.add(m.outputs[Olim::OUT_R_OUTPUT].getVoltage());
    }
    report("olim", "nan_input_nans", s.nans, s.nans == 0);
}

// A clock makes TIME a power of two of its period; pulling the cable makes
// it free again at once, not two seconds later.
static void testClock() {
    Olim m; long fr = 0;
    m.inputs[Olim::CLOCK_INPUT].channels = 1;
    for (long i = 0; i < 48000; i++) {
        m.inputs[Olim::CLOCK_INPUT].setVoltage((i % 12000) < 240 ? 10.f : 0.f);
        m.process(makeArgs(fr++));
    }
    float clocked = m.engine.timeSec;
    bool on = m.engine.clocked();
    m.inputs[Olim::CLOCK_INPUT].channels = 0;
    for (long i = 0; i < 16; i++) m.process(makeArgs(fr++));
    report("olim", "clock_time_s", clocked, on && std::fabs(clocked - 0.25f) < 1e-3f);
    report("olim", "clock_unpatched_free", m.engine.timeSec,
           !m.engine.clocked() && std::fabs(m.engine.timeSec - 2.f) < 1e-3f);
}

// Memory 20 s: the buffer is swapped for one that size, the output fades out
// and back, and a TIME past 20 s clamps to it.
static void testMemory() {
    Olim m; long fr = 0;
    report("olim", "memory_default_samples", (double)m.live.n,
           m.live.n == olim::Engine::bufferSamples(150.f, SR));
    m.setMemory(20.f);
    connect(m, Olim::IN_L_INPUT, 1.f);
    m.params[Olim::DRY_PARAM].setValue(1.f);
    float lowest = 10.f;
    for (int i = 0; i < 480; i++) {
        m.process(makeArgs(fr++));
        lowest = std::min(lowest, std::fabs(m.outputs[Olim::OUT_L_OUTPUT].getVoltage()));
    }
    settleSwap(m, fr);
    report("olim", "memory_20_samples", (double)m.live.n,
           m.live.n == olim::Engine::bufferSamples(20.f, SR) && !m.swapFailed);
    report("olim", "memory_swap_fades", lowest, lowest < 0.05f);
    report("olim", "memory_back_in", m.swapGain, m.swapGain == 1.f);

    m.params[Olim::TIME_PARAM].setValue(1.f);
    connect(m, Olim::TIME_CV_INPUT, -5.f);   // asks for 256 s
    for (int i = 0; i < 64; i++) m.process(makeArgs(fr++));
    report("olim", "memory_clamps_time", m.engine.timeSec,
           m.engine.timeSec <= 20.f && m.engine.timeSec > 19.9f);

    json_t* j = m.dataToJson();
    Olim n;
    n.dataFromJson(j);
    json_decref(j);
    report("olim", "memory_json", n.memory, n.memory == 20.f);
}

// Deleted while the worker is still allocating: the job outlives the module.
static void testDeleteMidSwap() {
    for (int k = 0; k < 20; k++) {
        Olim* m = new Olim; long fr = 0;
        m->setMemory(k % 2 ? 60.f : 20.f);
        while (!m->job) m->process(makeArgs(fr++));
        delete m;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    report("olim", "delete_mid_swap", 0, true);
}

static void heads(Olim& m, float* v) {
    for (int i = 0; i < olim::kHeads; i++) v[i] = m.params[Olim::HEAD1_PARAM + i].getValue();
}

static bool same(const float* a, const float* b) {
    for (int i = 0; i < olim::kHeads; i++)
        if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
    return true;
}

// Every preset lands in range, peaks at 50% (All 100% and All 0% aside),
// leaves DRY where it was, and the patterns are the heads they say.
static void testPresets() {
    Olim m;
    m.params[Olim::DRY_PARAM].setValue(0.37f);
    int bad = 0;
    for (int p = 0; p < olim::NUM_PRESETS; p++) {
        m.applyPreset(p);
        float v[olim::kHeads];
        heads(m, v);
        float top = 0.f;
        for (float x : v) {
            bad += x < 0.f || x > 1.f;
            top = std::max(top, x);
        }
        float want = p == olim::PRESET_ALL_100 ? 1.f : p == olim::PRESET_ALL_0 ? 0.f : 0.5f;
        if (p == olim::PRESET_RANDOM) bad += top > 0.5f;
        else bad += std::fabs(top - want) > 1e-6f;
        bad += m.params[Olim::DRY_PARAM].getValue() != 0.37f;
    }
    report("olim", "presets_range_peak_dry", bad, bad == 0);

    auto on = [&](int p) {
        m.applyPreset(p);
        int mask = 0;
        for (int i = 0; i < olim::kHeads; i++)
            if (m.params[Olim::HEAD1_PARAM + i].getValue() > 0.f) mask |= 1 << i;
        return mask;
    };
    int wrong = 0;
    wrong += on(olim::PRESET_ODDS) != 0x55;
    wrong += on(olim::PRESET_EVENS) != 0xaa;
    wrong += on(olim::PRESET_LAST) != 0x80;
    wrong += on(olim::PRESET_FIRST) != 0x01;
    wrong += on(olim::PRESET_HALVES) != 0x88;
    wrong += on(olim::PRESET_DOTTED) != 0x24;
    wrong += on(olim::PRESET_TRESILLO) != 0xa4;
    report("olim", "preset_patterns", wrong, wrong == 0);
}

// Reverse turns each ascending shape into its descending twin, and each
// transform undoes with its opposite.
static void testTransforms() {
    Olim m;
    float a[olim::kHeads], b[olim::kHeads], c[olim::kHeads];
    int bad = 0;
    const int pairs[][2] = {{olim::PRESET_LINEAR_UP, olim::PRESET_LINEAR_DOWN},
                            {olim::PRESET_EXP_UP, olim::PRESET_EXP_DOWN}};
    for (auto& pr : pairs) {
        m.applyPreset(pr[1]); heads(m, a);
        m.applyPreset(pr[0]); m.applyTransform(olim::TRANSFORM_REVERSE); heads(m, b);
        bad += !same(a, b);
    }
    m.applyPreset(olim::PRESET_TRESILLO); heads(m, a);
    m.applyTransform(olim::TRANSFORM_ROTATE_LEFT); heads(m, b);
    bad += b[1] != 0.5f || b[4] != 0.5f || b[6] != 0.5f;   // one head earlier
    m.applyTransform(olim::TRANSFORM_ROTATE_RIGHT); heads(m, c);
    bad += !same(a, c);
    m.applyTransform(olim::TRANSFORM_INVERT);
    m.applyTransform(olim::TRANSFORM_INVERT); heads(m, c);
    bad += !same(a, c);
    m.applyTransform(olim::TRANSFORM_DOUBLE);
    m.applyTransform(olim::TRANSFORM_HALF); heads(m, c);
    bad += !same(a, c);
    m.applyPreset(olim::PRESET_ALL_100);
    m.applyTransform(olim::TRANSFORM_DOUBLE); heads(m, c);
    for (float x : c) bad += x != 1.f;
    report("olim", "transforms", bad, bad == 0);
}

// The mutations stay small: Mutate keeps silent heads silent and each level
// within 20%, Mutate wide within 10 points, Mutate pattern swaps exactly one
// pair of neighbours. Checked over many draws, each from the same start.
static void testMutations() {
    Olim m;
    float a[olim::kHeads], b[olim::kHeads];
    int bad = 0;
    for (int k = 0; k < 200; k++) {
        m.applyPreset(olim::PRESET_TRESILLO);
        m.params[Olim::HEAD1_PARAM].setValue(0.9f);   // near the top, to see the clamp
        heads(m, a);
        m.applyTransform(olim::TRANSFORM_MUTATE); heads(m, b);
        for (int i = 0; i < olim::kHeads; i++) {
            if (a[i] == 0.f) bad += b[i] != 0.f;
            else bad += b[i] < 0.8f * a[i] - 1e-6f || b[i] > std::min(1.2f * a[i], 1.f) + 1e-6f;
        }
        m.applyPreset(olim::PRESET_TRESILLO); heads(m, a);
        m.applyTransform(olim::TRANSFORM_MUTATE_WIDE); heads(m, b);
        for (int i = 0; i < olim::kHeads; i++)
            bad += std::fabs(b[i] - a[i]) > 0.1f + 1e-6f || b[i] < 0.f || b[i] > 1.f;
        m.applyPreset(olim::PRESET_TRESILLO); heads(m, a);
        m.applyTransform(olim::TRANSFORM_MUTATE_PATTERN); heads(m, b);
        int moved = 0, first = -1;
        for (int i = 0; i < olim::kHeads; i++)
            if (a[i] != b[i]) { moved++; if (first < 0) first = i; }
        bad += moved != 2 || a[first] != b[first + 1] || a[first + 1] != b[first];
    }
    // nothing to swap on a flat shape
    m.applyPreset(olim::PRESET_ALL_50); heads(m, a);
    m.applyTransform(olim::TRANSFORM_MUTATE_PATTERN); heads(m, b);
    bad += !same(a, b);
    report("olim", "mutations", bad, bad == 0);
}

// A slider move rides the head's next crossfade, as on the hardware: it is
// not heard at once, and it has landed within two fades (one to pick it up,
// one to ramp), 400 ms.
static void testSliderRamp() {
    Olim m; long fr = 0;
    m.params[Olim::DRY_PARAM].setValue(0.f);
    m.params[Olim::TIME_PARAM].setValue(0.1f);   // 80 ms
    soloHead(m, 7);
    m.params[Olim::HEAD8_PARAM].setValue(1.f);
    m.inputs[Olim::IN_L_INPUT].channels = 1;
    auto step = [&]() {
        m.inputs[Olim::IN_L_INPUT].setVoltage(2.f * std::sin(2.f * M_PI * 220.f * fr / SR));
        m.process(makeArgs(fr++));
        return std::fabs(m.outputs[Olim::OUT_L_OUTPUT].getVoltage());
    };
    for (int k = 0; k < 5; k++) {
        // land the move at a different point of the fade cycle each time
        for (long i = 0; i < 24000 + 1777 * k; i++) step();
        m.params[Olim::HEAD8_PARAM].setValue(0.f);
        float early = 0.f, late = 0.f;
        for (long i = 0; i < (long)(0.5f * SR); i++) {
            float y = step();
            if (i < (long)(0.01f * SR)) early = std::max(early, y);
            if (i > (long)(0.41f * SR)) late = std::max(late, y);
        }
        report("olim", "slider_not_instant_peak", early, early > 1.8f);
        report("olim", "slider_landed_by_400ms_peak", late, late < 1e-3f);
        m.params[Olim::HEAD8_PARAM].setValue(1.f);
    }
}

// A gain typed into FEEDBACK comes back as typed. Below the arc it came
// back halved: 0.8 set a knob that gave 0.4.
static void testFeedbackTyped() {
    const float gains[] = {0.f, 0.5f, 0.8f, 0.985f, 1.f, 1.5f, 2.f};
    float worst = 0.f;
    for (float g : gains)
        worst = std::max(worst, std::fabs(olim::feedbackGain(olim::feedbackKnob(g), 0.f) - g));
    report("olim", "feedback_typed_gain_round_trips", worst, worst < 1e-4f);
}

// A 5 V sine gliding in pitch, dry plus the last head at unity: the two
// drift in and out of phase and their sum crosses 5 V again and again. The
// hardware's instant limiter clips each crossing in one sample, which is a
// crackle; the soft one glides its gain down over its look-ahead. Largest
// second difference of the output against the sine's own (0.03 at 440 Hz).
static void limiterRun(bool hardware, float* worstD2, float* peak) {
    Olim m; long fr = 0;
    m.engine.softLimit = !hardware;
    m.inputs[Olim::IN_L_INPUT].channels = 1;
    double ph = 0.;
    float p1 = 0.f, p2 = 0.f;
    *worstD2 = *peak = 0.f;
    for (long i = 0; i < (long)(8 * SR); i++) {
        double t = (double)i / SR;
        ph += 233.0 * std::pow(2.0, 0.5 * std::sin(2.0 * M_PI * 0.3 * t)) / SR;
        m.inputs[Olim::IN_L_INPUT].setVoltage(5.f * (float)std::sin(2.0 * M_PI * ph));
        m.process(makeArgs(fr++));
        float y = m.outputs[Olim::OUT_L_OUTPUT].getVoltage();
        if (i > (long)(4 * SR)) {
            *worstD2 = std::max(*worstD2, std::fabs(y - 2.f * p1 + p2));
            *peak = std::max(*peak, std::fabs(y));
        }
        p2 = p1;
        p1 = y;
    }
}

static void testOutputLimiter() {
    float softD2, softPeak, hardD2, hardPeak;
    limiterRun(false, &softD2, &softPeak);
    limiterRun(true, &hardD2, &hardPeak);
    printf("# gliding 5 V sine: soft d2 %.4f peak %.3f V, hardware d2 %.4f peak %.3f V\n",
           softD2, softPeak, hardD2, hardPeak);
    report("olim", "soft_limiter_no_crackle", softD2, softD2 < 0.02f);
    report("olim", "soft_limiter_holds_5v", softPeak, softPeak <= 5.001f && softPeak > 4.5f);
    report("olim", "hardware_limiter_still_clips", hardD2, hardD2 > 0.05f);

    Olim a;
    a.engine.softLimit = false;
    a.inputFades = false;
    json_t* j = a.dataToJson();
    Olim b;
    b.dataFromJson(j);
    json_decref(j);
    Olim c;    // a patch from before the menu: soft and fading
    json_t* old = json_pack("{s:f}", "memory", 20.0);
    c.dataFromJson(old);
    json_decref(old);
    bool ok = !b.engine.softLimit && !b.inputFades && c.engine.softLimit && c.inputFades;
    report("olim", "json_limiter_and_fades", ok, ok);
}

// A cable plugged into a running 5 V source, then pulled: with the fades the
// output ramps over 5 ms (5 V / 240 samples, about 0.02 V a sample) instead
// of stepping 5 V. R takes over from the L normal the same way. Largest
// sample-to-sample change, dry only.
static float plugStep(bool fades, int which) {
    Olim m; long fr = 0;
    m.inputFades = fades;
    soloHead(m, -1);
    m.params[Olim::DRY_PARAM].setValue(1.f);
    for (int i = 0; i < 4800; i++) m.process(makeArgs(fr++));
    if (which == 2) { connect(m, Olim::IN_L_INPUT, 4.f); for (int i = 0; i < 4800; i++) m.process(makeArgs(fr++)); }
    float prev = m.outputs[Olim::OUT_R_OUTPUT].getVoltage(), worst = 0.f;
    if (which == 0) connect(m, Olim::IN_L_INPUT, 4.f);                  // plug
    if (which == 1) { connect(m, Olim::IN_L_INPUT, 4.f);
                      for (int i = 0; i < 4800; i++) m.process(makeArgs(fr++));
                      prev = m.outputs[Olim::OUT_R_OUTPUT].getVoltage();
                      m.inputs[Olim::IN_L_INPUT].channels = 0;           // pull
                      m.inputs[Olim::IN_L_INPUT].setVoltage(0.f); }
    if (which == 2) connect(m, Olim::IN_R_INPUT, -4.f);                 // R off the normal
    for (int i = 0; i < 2400; i++) {
        m.process(makeArgs(fr++));
        float y = m.outputs[Olim::OUT_R_OUTPUT].getVoltage();
        worst = std::max(worst, std::fabs(y - prev));
        prev = y;
    }
    return worst;
}

static void testInputFades() {
    float plug = plugStep(true, 0), pull = plugStep(true, 1), normal = plugStep(true, 2);
    float raw = plugStep(false, 0);
    report("olim", "fade_plug", plug, plug < 0.05f);
    report("olim", "fade_pull", pull, pull < 0.05f);
    report("olim", "fade_r_off_the_normal", normal, normal < 0.1f);
    report("olim", "no_fade_steps", raw, raw > 3.9f);
}

SMOKE_MAIN(testEcho, testVca, testNormal, testNan, testClock, testMemory, testDeleteMidSwap,
           testPresets, testTransforms, testMutations,
           testSliderRamp, testFeedbackTyped, testOutputLimiter, testInputFades)
