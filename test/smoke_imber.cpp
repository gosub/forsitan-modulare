// smoke_imber — offline sanity checks for the imber module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/imber.cpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// The bank worker is started from process(), i.e. from the audio thread,
// which is SCHED_FIFO under JACK/PipeWire. A worker that inherits that
// policy runs a 400 ms render at realtime priority and is killed by the
// 200 ms RLIMIT_RTTIME that RTKit installs, taking Rack with it. This is
// the crash reported on Ubuntu 24.04 (community thread 26023).
//
// Realtime priority needs privileges the test cannot assume, so it checks
// the inheritance itself from SCHED_BATCH, which any user may set and, like
// SCHED_FIFO, may be left again: whatever the caller's policy, the worker
// must come up SCHED_OTHER. (SCHED_IDLE would not do: leaving it is a
// promotion, so it needs CAP_SYS_NICE, while leaving SCHED_FIFO does not.)
static void testWorkerScheduling() {
#if defined(__linux__)
    static std::atomic<int> workerPolicy(-2);
    static std::atomic<bool> workerRan(false);
    static std::atomic<int> callerPolicy(-2);
    std::thread caller([]() {
        sched_param sp;
        std::memset(&sp, 0, sizeof(sp));
        if (pthread_setschedparam(pthread_self(), SCHED_BATCH, &sp) != 0)
            return;   // leaves callerPolicy at -2: reported as skipped
        int pol;
        pthread_getschedparam(pthread_self(), &pol, &sp);
        callerPolicy.store(pol);
        imber_worker::startDetached([]() {
            int p;
            sched_param s;
            pthread_getschedparam(pthread_self(), &p, &s);
            workerPolicy.store(p);
            workerRan.store(true);
        });
    });
    caller.join();
    for (int i = 0; i < 500 && !workerRan.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (callerPolicy.load() != SCHED_BATCH) {
        report("imber", "worker_not_realtime", -1, true);   // could not set up
        return;
    }
    report("imber", "worker_not_realtime", workerPolicy.load(),
           workerRan.load() && workerPolicy.load() == SCHED_OTHER);
#endif
}

static void testImber() {
    Imber m;
    long frame = 0;
    // The constructor seeds constRng from the clock, and the first process
    // draws both the engine seed and the bank seed from it. Pin both before
    // any process() runs: the very first frame fires every clock division at
    // once (nominal[] is still 0) and rolls real musical state off that seed,
    // so the eng.init() below is too late to be the only pin.
    m.constRng.seed(42);
    m.bankSeed = 42;
    // first process initializes the engine at SR and kicks off the bank
    m.process(makeArgs(frame++));

    // Wait on the worker by sleeping, NOT by spinning process(): the bank
    // takes a wall-clock-dependent time to build, so engine frames burned
    // while waiting would start the checks from a different phase every run.
    // Sleeping keeps the cost at exactly one process() to install the bank
    // (and stops the spin from starving the worker: ~0.4 s here, was ~9 s).
    int waitedMs = 0;
    while (m.bankJob && !m.bankJob->done.load() && waitedMs < 30000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waitedMs += 20;
    }
    m.process(makeArgs(frame++));   // installs the finished bank
    report("imber", "bank_lands", waitedMs / 1000.0, m.eng.bank != nullptr);
    if (!m.eng.bank)
        return;

    // Now pin the engine for the checks: fixed timing seed, all clocks
    // reachable, FX parked far away. init() leaves the landed bank alone,
    // so re-running it here is safe.
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

    // mutes are performance state: no randomize path may touch them.
    // Runs last: bigRandom() rerolls the constellations and every fader,
    // which would contaminate any check placed after it.
    m.params[Imber::VON3_PARAM].setValue(0.f);
    m.bigRandom();
    report("imber", "rnd_spares_mutes",
           m.params[Imber::VON3_PARAM].getValue(),
           m.params[Imber::VON3_PARAM].getValue() < 0.5f);
    report("imber", "mute_not_randomizable",
           m.getParamQuantity(Imber::VON3_PARAM)->randomizeEnabled ? 1 : 0,
           !m.getParamQuantity(Imber::VON3_PARAM)->randomizeEnabled);
}

// The tuning is a setting rather than rolled material, so it must survive
// ephemeral mode, default to the library tuning on patches that predate it,
// and rebuild the bank from the same seed when it changes.
static void testImberTuning() {
    {
        Imber m;
        report("imber", "default_tuning", m.scaleIndex,
               m.rootNote == imber_dsp::kDefaultRoot
               && m.scaleIndex == imber_dsp::kDefaultScale);
    }
    {
        Imber m;
        m.rootNote = 7;
        m.scaleIndex = 0;
        json_t* j = m.dataToJson();
        Imber n;
        n.dataFromJson(j);
        json_decref(j);
        report("imber", "tuning_round_trips", n.rootNote,
               n.rootNote == 7 && n.scaleIndex == 0);
        // ephemeral rerolls the bank and constellations but not the tuning
        m.ephemeral = true;
        json_t* j2 = m.dataToJson();
        Imber e;
        e.dataFromJson(j2);
        json_decref(j2);
        report("imber", "tuning_survives_ephemeral", e.rootNote,
               e.rootNote == 7 && e.scaleIndex == 0);
    }
    {
        Imber o;
        json_t* j = json_object();
        json_object_set_new(j, "bankSeed", json_integer(5));
        o.dataFromJson(j);
        json_decref(j);
        report("imber", "old_patch_default_tuning", o.scaleIndex,
               o.rootNote == imber_dsp::kDefaultRoot
               && o.scaleIndex == imber_dsp::kDefaultScale);
    }
    // a retune rebuilds all 192 buffers from the same seed, and the old bank
    // keeps playing until the new one lands
    Imber m;
    long frame = 0;
    m.constRng.seed(7);
    m.bankSeed = 7;
    m.process(makeArgs(frame++));
    int waitedMs = 0;
    while (m.bankJob && !m.bankJob->done.load() && waitedMs < 60000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waitedMs += 20;
    }
    m.process(makeArgs(frame++));
    if (!m.eng.bank) {
        report("imber", "retune_bank_lands", 0, false);
        return;
    }
    std::vector<float> before = m.eng.bank->loops[0];
    uint64_t seedBefore = m.bankSeed;
    for (int i = 0; i < (int)(0.5f * SR); i++) m.process(makeArgs(frame++));
    m.rootNote = 7;
    m.scaleIndex = 0;
    m.bankDirty = true;
    m.process(makeArgs(frame++));
    report("imber", "retune_rebuilds", m.bankJob ? 1 : 0, m.bankJob != nullptr);
    Stats during;
    for (int i = 0; i < (int)(0.2f * SR); i++) {
        m.process(makeArgs(frame++));
        during.add(m.outputs[Imber::LEFT_OUTPUT].getVoltage());
    }
    report("imber", "retune_keeps_sounding", during.rms(),
           during.rms() > 0.001 && during.nans == 0);
    waitedMs = 0;
    while (m.bankJob && !m.bankJob->done.load() && waitedMs < 60000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waitedMs += 20;
    }
    m.process(makeArgs(frame++));
    report("imber", "retune_new_material", m.eng.bank ? 1 : 0,
           m.eng.bank && m.eng.bank->loops[0] != before);
    report("imber", "retune_keeps_seed", (double)m.bankSeed,
           m.bankSeed == seedBefore);
}

SMOKE_MAIN(testWorkerScheduling, testImber, testImberTuning)
