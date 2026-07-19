// smoke_imber — offline sanity checks for the imber module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/imber.cpp"

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

SMOKE_MAIN(testImber)
