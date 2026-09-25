// smoke_olim - offline sanity checks for the olim module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
//
// The engine is measured by olim_probe, which needs no Rack. These are the
// module's own checks: where an echo lands and its sign, the VCA normalling,
// R normalled from L, NaN at the input, the clock cable, the Memory swap and
// its fade, TIME clamped to the memory, and a module deleted while its
// buffer is still being allocated.

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
    Olim m; long fr = 0;
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
    report("olim", "echo_nans", s.nans, s.nans == 0);
    report("olim", "echo_at_samples", at, at == 12000);
    report("olim", "echo_inverted", v, v < -3.9f);
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

SMOKE_MAIN(testEcho, testVca, testNormal, testNan, testClock, testMemory, testDeleteMidSwap)
