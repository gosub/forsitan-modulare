// smoke_tundo — offline sanity checks for the tundo drum voice.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// tundo_probe is the harness that measures character; this one only asks
// whether the voice is alive, in tune, in bounds and free of NaNs.

#include "smoke_harness.hpp"

#include <vector>
#include <algorithm>
#include "../src/tundo.cpp"

typedef std::vector<float> Buf;

static void defaults(Tundo& m) {
    m.params[Tundo::PITCH_PARAM].setValue(0.f);
    m.params[Tundo::HARM_PARAM].setValue(0.3f);
    m.params[Tundo::SPREAD_PARAM].setValue(0.f);
    m.params[Tundo::MORPH_PARAM].setValue(0.f);
    m.params[Tundo::FOLD_PARAM].setValue(0.f);
    m.params[Tundo::ATTACK_PARAM].setValue(0.5f);
    m.params[Tundo::DECAY_PARAM].setValue(0.35f);
}

static Module::ProcessArgs argsAt(long frame, float rate) {
    Module::ProcessArgs a;
    a.sampleRate = rate;
    a.sampleTime = 1.f / rate;
    a.frame = frame;
    return a;
}

// settle the smoothers, strike once, capture `secs` of audio (and the envelope)
static Buf strike(Tundo& m, double secs, float rate = SR, Buf* envOut = nullptr) {
    long frame = 0;
    for (int i = 0; i < (int)(0.05 * rate); i++)
        m.process(argsAt(frame++, rate));
    Buf out((size_t)(secs * rate));
    if (envOut) envOut->resize(out.size());
    for (size_t i = 0; i < out.size(); i++) {
        m.inputs[Tundo::TRIG_INPUT].setVoltage(i < 50 ? 5.f : 0.f);
        m.process(argsAt(frame++, rate));
        out[i] = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
        if (envOut) (*envOut)[i] = m.outputs[Tundo::ENV_OUTPUT].getVoltage();
    }
    return out;
}

static void collect(const Buf& x, Stats& s) {
    for (size_t i = 0; i < x.size(); i++) s.add(x[i]);
}

static double peakOf(const Buf& x, size_t from = 0, size_t to = (size_t)-1) {
    double p = 0.0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; i++) p = std::max(p, (double)std::fabs(x[i]));
    return p;
}

// fundamental from interpolated rising zero crossings
static double zcFreq(const Buf& x, size_t off, size_t n, float rate) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = off + 1; i < off + n && i < x.size(); i++) {
        if (x[i - 1] < 0.f && x[i] >= 0.f) {
            double t = (i - 1) + (double)(-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            else { last = t; count++; }
        }
    }
    if (count < 2) return 0.0;
    return rate * count / (last - first);
}

// ------------------------------------------------------------------- checks

static void testSilence() {
    Tundo m;
    defaults(m);
    long frame = 0;
    Stats s;
    for (int i = 0; i < (int)(2.0 * SR); i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Tundo::AUDIO_OUTPUT].getVoltage());
        s.add(m.outputs[Tundo::ENV_OUTPUT].getVoltage());
    }
    report("tundo", "silent_untriggered", s.rms(), s.rms() < 1e-6);
    report("tundo", "silent_nans", s.nans, s.nans == 0);
}

// one hit: inside the level cap, no NaNs, and the envelope reaches -60 dB
// within the decay time the knob asks for
static void testHit() {
    Tundo m;
    defaults(m);
    m.params[Tundo::DECAY_PARAM].setValue(0.5f);
    Buf env;
    Buf x = strike(m, 2.0, SR, &env);
    Stats s;
    collect(x, s);
    double pk = peakOf(x);
    report("tundo", "hit_alive", pk, pk > 1.0);
    report("tundo", "hit_within_cap", pk, pk <= 5.0001);
    report("tundo", "hit_nans", s.nans, s.nans == 0);

    double envPeak = peakOf(env);
    size_t want = (size_t)(1.2 * m.p.decayMs * 0.001 * SR);
    double envLate = peakOf(env, want);
    report("tundo", "env_decays_by_1.2D", envLate / envPeak, envLate < envPeak * 0.001);

    // and the audio itself is gone not long after
    double audioLate = peakOf(x, (size_t)(2.5 * m.p.decayMs * 0.001 * SR));
    report("tundo", "audio_decays", audioLate / pk, audioLate < pk * 0.01);
}

// 20 strikes at 8 Hz: every one has to sound, and at fixed settings they
// have to land within 1 dB of each other
static void testRetrigger() {
    Tundo m;
    defaults(m);
    m.params[Tundo::DECAY_PARAM].setValue(0.3f);
    long frame = 0;
    for (int i = 0; i < (int)(0.05 * SR); i++) m.process(makeArgs(frame++));
    int period = (int)(SR / 8.f);
    double lo = 1e9, hi = 0.0;
    long nans = 0;
    for (int k = 0; k < 20; k++) {
        double pk = 0.0;
        for (int i = 0; i < period; i++) {
            m.inputs[Tundo::TRIG_INPUT].setVoltage(i < 50 ? 5.f : 0.f);
            m.process(makeArgs(frame++));
            float v = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
            if (!std::isfinite(v)) { nans++; v = 0.f; }
            pk = std::max(pk, (double)std::fabs(v));
        }
        if (k == 0) continue;          // the first hit starts from silence
        lo = std::min(lo, pk);
        hi = std::max(hi, pk);
    }
    double spread = 20.0 * std::log10(hi / std::max(lo, 1e-9));
    report("tundo", "retrigger_all_sound", lo, lo > 1.0);
    report("tundo", "retrigger_spread_db", spread, spread < 1.0);
    report("tundo", "retrigger_nans", nans, nans == 0);
}

// 1 V/oct, and the range switch's two octaves per step, measured on a sine
static void testPitch() {
    double worst = 0.0;
    for (int r = 0; r < 3; r++) {
        for (int v = 0; v <= 3; v++) {
            Tundo m;
            defaults(m);
            m.params[Tundo::HARM_PARAM].setValue(0.f);
            m.params[Tundo::DECAY_PARAM].setValue(1.f);
            m.params[Tundo::PITCH_PARAM].setValue((float)v);
            m.params[Tundo::RANGE_PARAM].setValue((float)r);
            Buf x = strike(m, 0.6);
            double want = tundo_dsp::kBaseHz * std::exp2((double)v + 2.0 * r);
            double got = zcFreq(x, (size_t)(0.15 * SR), (size_t)(0.4 * SR), SR);
            double cents = got > 0.0 ? 1200.0 * std::log2(got / want) : 9999.0;
            worst = std::max(worst, std::fabs(cents));
        }
    }
    report("tundo", "pitch_worst_cents", worst, worst < 5.0);
}

// spread endpoints: the harmonic series at 0, the prime series at 1
static void testSpread() {
    static const float H[6] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    static const float P[6] = {1.f, 3.f, 5.f, 7.f, 11.f, 13.f};
    double worst = 0.0;
    for (int s = 0; s < 2; s++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::SPREAD_PARAM].setValue((float)s);
        long frame = 0;
        for (int i = 0; i < (int)(0.05 * SR); i++) m.process(makeArgs(frame++));
        for (int i = 0; i < 6; i++) {
            float want = s ? P[i] : H[i];
            worst = std::max(worst,
                             (double)std::fabs(m.engine.ratio[i] - want) / want);
        }
    }
    report("tundo", "spread_ratio_err", worst, worst < 0.005);
}

// HARM has to add partials, not just move level around
static void testHarmStaging() {
    double prevLive = -1.0;
    bool monotonic = true;
    double levelLo = 1e9, levelHi = 0.0;
    for (int k = 0; k <= 20; k++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::HARM_PARAM].setValue(k / 20.f);
        m.params[Tundo::DECAY_PARAM].setValue(0.6f);
        Buf x = strike(m, 0.4);
        // the engine's own staging: total partial gain and total decay length
        double live = 0.0;
        for (int i = 0; i < tundo_dsp::kNumOsc; i++)
            live += m.engine.gain[i] * m.engine.dmul[i];
        if (live < prevLive - 1e-6) monotonic = false;
        prevLive = live;
        double pk = peakOf(x);
        levelLo = std::min(levelLo, pk);
        levelHi = std::max(levelHi, pk);
    }
    report("tundo", "harm_staging_monotonic", prevLive, monotonic);
    // loudness compensation: sweeping HARM must not sweep the level
    double spread = 20.0 * std::log10(levelHi / levelLo);
    report("tundo", "harm_level_spread_db", spread, spread < 3.0);
}

// FOLD is two mechanisms in series and each gets its own check. The first
// three quarters lower the reflection threshold, which brightens the hit
// steadily. The top quarter crossfades in the pulse train, which replaces a
// dense fold with a sparse tuned one: brighter still, but peakier and lower
// in RMS rather than louder. They hand over at kFoldPulseSpan, and the metric
// is not monotonic across that seam.
static void testFold() {
    double prevHi = -1.0;
    bool monotonic = true;
    double worstPeak = 0.0;
    double hiPulseLo = 0.0, hiPulseHi = 0.0, rmsPulseLo = 0.0, rmsPulseHi = 0.0;
    for (int k = 0; k <= 10; k++) {
        float fold = k / 10.f;
        Tundo m;
        defaults(m);
        m.params[Tundo::FOLD_PARAM].setValue(fold);
        m.params[Tundo::DECAY_PARAM].setValue(0.6f);
        Buf x = strike(m, 0.3);
        // high-order energy without an FFT: energy in the sample difference,
        // which weights each partial by its frequency. Measured over the
        // strike only: the envelope drives the folder, so the folding lives
        // in the first tens of ms and the tail is clean by design.
        x.resize(std::min(x.size(), (size_t)(0.06 * SR)));
        double hi = 0.0, all = 0.0;
        for (size_t i = 1; i < x.size(); i++) {
            double d = x[i] - x[i - 1];
            hi += d * d;
            all += (double)x[i] * x[i];
        }
        double frac = all > 0.0 ? hi / all : 0.0;
        double rms = std::sqrt(all / std::max<size_t>(x.size(), 1));
        if (fold <= tundo_dsp::kFoldPulseSpan) {
            if (frac < prevHi * 0.98) monotonic = false;
            prevHi = frac;
        }
        if (k == 8) { hiPulseLo = frac; rmsPulseLo = rms; }
        if (k == 10) { hiPulseHi = frac; rmsPulseHi = rms; }
        worstPeak = std::max(worstPeak, peakOf(x));
    }
    report("tundo", "fold_brightens", prevHi, monotonic);
    // and the pulse train, across the top quarter: brighter and thinner
    report("tundo", "fold_pulse_brightens", hiPulseHi / hiPulseLo,
           hiPulseHi > hiPulseLo * 1.1);
    report("tundo", "fold_pulse_thins", rmsPulseHi / rmsPulseLo,
           rmsPulseHi < rmsPulseLo * 0.8);
    report("tundo", "fold_within_cap", worstPeak, worstPeak <= 5.0001);
}

// Skin and Metal have to be different sounds at every HARM setting, the
// default very much included. While Metal's modulation index shared Skin's
// amplitude staging the two modes rendered bit-identical output everywhere
// below kHarmAmpStart, which is nearly half the knob: an operator amplitude
// of zero is a legitimate Skin partial, but an index of zero is a bare
// carrier, and two bare carriers at ratios 1 and 2 are exactly Skin.
static void testSkinVsMetal() {
    double worst = 1e9;
    for (int k = 0; k <= 4; k++) {
        Buf out[2];
        for (int j = 0; j < 2; j++) {
            Tundo m;
            defaults(m);
            m.params[Tundo::MODE_PARAM].setValue(j == 0 ? 0.f : 2.f);
            m.params[Tundo::HARM_PARAM].setValue(k / 4.f);
            m.params[Tundo::DECAY_PARAM].setValue(0.6f);
            out[j] = strike(m, 0.2);
        }
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i < out[0].size(); i++) {
            double d = (double)out[0][i] - out[1][i];
            num += d * d;
            den += (double)out[0][i] * out[0][i];
        }
        worst = std::min(worst, den > 0.0 ? std::sqrt(num / den) : 0.0);
    }
    report("tundo", "skin_metal_differ", worst, worst > 0.15);
}

// A slow attack has to sound whatever DECAY says. While both envelopes ran
// from the strike they fought each other: by the time a slow attack was up, a
// short decay had already collapsed, and the product never got off the floor.
// Measured over the plane, the top of ATTACK was 39 dB down with DECAY at
// noon and 70 dB down in the corner.
static void testAttackDecay() {
    const float attacks[3] = {0.7f, 0.85f, 1.0f};
    const float decays[2] = {0.2f, 0.5f};
    double worst = 1e9;
    for (int a = 0; a < 3; a++) {
        for (int d = 0; d < 2; d++) {
            Tundo m;
            defaults(m);
            m.params[Tundo::ATTACK_PARAM].setValue(attacks[a]);
            m.params[Tundo::DECAY_PARAM].setValue(decays[d]);
            // long enough to contain the crest of the slowest attack
            worst = std::min(worst, peakOf(strike(m, 3.0)));
        }
    }
    report("tundo", "slow_attack_audible_V", worst, worst > 1.5);
}

// every mode sounds, and Liquid starts above its own steady pitch
static void testModes() {
    for (int mo = 0; mo < 3; mo++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::MODE_PARAM].setValue((float)mo);
        m.params[Tundo::HARM_PARAM].setValue(0.7f);
        m.params[Tundo::DECAY_PARAM].setValue(0.5f);
        Buf x = strike(m, 1.0);
        Stats s;
        collect(x, s);
        const char* names[3] = {"skin", "liquid", "metal"};
        char key[64];
        snprintf(key, sizeof(key), "mode_%s_alive", names[mo]);
        report("tundo", key, peakOf(x), peakOf(x) > 1.0 && s.nans == 0);
    }

    // Liquid's pitch envelope: the first millisecond has to sit a couple of
    // octaves above the steady pitch, within the configured depth
    Tundo m;
    defaults(m);
    m.params[Tundo::MODE_PARAM].setValue(1.f);
    m.params[Tundo::HARM_PARAM].setValue(0.f);
    m.params[Tundo::PITCH_PARAM].setValue(3.f);
    m.params[Tundo::DECAY_PARAM].setValue(1.f);
    Buf x = strike(m, 1.5);
    double early = zcFreq(x, 0, (size_t)(0.003 * SR), SR);
    double late = zcFreq(x, (size_t)(1.0 * SR), (size_t)(0.4 * SR), SR);
    double oct = (early > 0.0 && late > 0.0) ? std::log2(early / late) : 0.0;
    report("tundo", "liquid_pitch_octaves", oct, oct > 1.5 && oct < 2.2);
}

// both rate modes at three host rates: no NaNs, no DC, and the same level
static void testRateModes() {
    const float rates[3] = {44100.f, 48000.f, 96000.f};
    for (int c = 0; c < 2; c++) {
        double lo = 1e9, hi = 0.0, worstDc = 0.0;
        long nans = 0;
        for (int r = 0; r < 3; r++) {
            Tundo m;
            defaults(m);
            m.cleanRate = (c == 1);
            m.params[Tundo::HARM_PARAM].setValue(0.8f);
            m.params[Tundo::MORPH_PARAM].setValue(1.f);
            m.params[Tundo::FOLD_PARAM].setValue(0.5f);
            m.params[Tundo::DECAY_PARAM].setValue(0.6f);
            Buf x = strike(m, 0.5, rates[r]);
            Stats s;
            collect(x, s);
            nans += s.nans;
            lo = std::min(lo, peakOf(x));
            hi = std::max(hi, peakOf(x));
            worstDc = std::max(worstDc, std::fabs(s.sum / std::max(s.n, 1L)));
        }
        const char* tag = c ? "clean" : "hardware";
        char key[64];
        snprintf(key, sizeof(key), "rate_%s_nans", tag);
        report("tundo", key, nans, nans == 0);
        // not steady DC (the blocker's zero at 0 Hz removes that): the
        // envelope sweeping the fold thresholds leaves a transient sub-audio
        // wander during the hit, a few mV against a 5 V signal
        snprintf(key, sizeof(key), "rate_%s_dc_mv", tag);
        report("tundo", key, worstDc * 1000.0, worstDc < 0.01);
        snprintf(key, sizeof(key), "rate_%s_level_spread_db", tag);
        double spread = 20.0 * std::log10(hi / std::max(lo, 1e-9));
        report("tundo", key, spread, spread < 1.0);
    }
}

// free-run: DECAY fully CW with the menu option armed turns tundo into an
// oscillator that holds its level
static void testFreeRun() {
    Tundo m;
    defaults(m);
    m.freeRun = true;
    m.params[Tundo::DECAY_PARAM].setValue(1.f);
    m.params[Tundo::PITCH_PARAM].setValue(2.f);
    long frame = 0;
    for (int i = 0; i < (int)(0.2 * SR); i++) m.process(makeArgs(frame++));
    double early = 0.0, late = 0.0;
    Stats s;
    for (int i = 0; i < (int)(10.0 * SR); i++) {
        m.process(makeArgs(frame++));
        float v = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
        s.add(v);
        if (i < (int)(0.5 * SR)) early = std::max(early, (double)std::fabs(v));
        if (i > (int)(9.5 * SR)) late = std::max(late, (double)std::fabs(v));
    }
    report("tundo", "freerun_sustains", late, late > 1.0);
    report("tundo", "freerun_level_db", 20.0 * std::log10(late / std::max(early, 1e-9)),
           std::fabs(20.0 * std::log10(late / std::max(early, 1e-9))) < 1.0);
    report("tundo", "freerun_nans", s.nans, s.nans == 0);
}

// a patched MODE / RANGE CV overrides the switch, in thirds of 0..5 V
static void testSwitchCv() {
    bool ok = true;
    const float volts[3] = {0.5f, 2.5f, 4.5f};
    for (int want = 0; want < 3; want++) {
        Tundo m;
        defaults(m);
        // isConnected() is channel-count based and setChannels() is a no-op on
        // a disconnected port, so fake the cable the only way an offline test can
        m.inputs[Tundo::MODE_CV_INPUT].channels = 1;
        m.inputs[Tundo::RANGE_CV_INPUT].channels = 1;
        m.params[Tundo::MODE_PARAM].setValue(0.f);     // switch says Skin/Bass
        m.params[Tundo::RANGE_PARAM].setValue(0.f);
        m.params[Tundo::HARM_PARAM].setValue(0.f);
        m.params[Tundo::DECAY_PARAM].setValue(1.f);
        m.inputs[Tundo::MODE_CV_INPUT].setVoltage(volts[want]);
        m.inputs[Tundo::RANGE_CV_INPUT].setVoltage(volts[want]);
        Buf x = strike(m, 0.6);
        if (m.p.mode != want) ok = false;
        double got = zcFreq(x, (size_t)(0.15 * SR), (size_t)(0.3 * SR), SR);
        double wantHz = tundo_dsp::kBaseHz * std::exp2(2.0 * want);
        // Liquid and Metal do not track a plain sine, so only Skin's pitch
        // can be measured; the others just have to sound
        if (want == 0 && std::fabs(1200.0 * std::log2(got / wantHz)) > 5.0) ok = false;
        if (want != 0 && peakOf(x) < 1.0) ok = false;
    }
    report("tundo", "switch_cv_override", ok ? 1 : 0, ok);
}

// everything hostile at once, with every knob swept at audio rate
static void testHostile() {
    Tundo m;
    defaults(m);
    m.freeRun = true;
    m.extendedSpread = true;
    m.outputLevel = 2;                  // 14 Vpp
    long frame = 0;
    Stats s;
    int n = (int)(20.0 * SR);
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        m.params[Tundo::PITCH_PARAM].setValue(-3.f + 8.f * (0.5f + 0.5f * std::sin(t * 37.f)));
        m.params[Tundo::HARM_PARAM].setValue(0.5f + 0.5f * std::sin(t * 211.f));
        m.params[Tundo::SPREAD_PARAM].setValue(0.5f + 0.5f * std::sin(t * 97.f));
        m.params[Tundo::MORPH_PARAM].setValue(0.5f + 0.5f * std::sin(t * 149.f));
        m.params[Tundo::FOLD_PARAM].setValue(0.5f + 0.5f * std::sin(t * 71.f));
        m.params[Tundo::ATTACK_PARAM].setValue(0.5f + 0.5f * std::sin(t * 13.f));
        m.params[Tundo::DECAY_PARAM].setValue(0.5f + 0.5f * std::sin(t * 5.f));
        m.params[Tundo::MODE_PARAM].setValue((float)((i / (int)SR) % 3));
        m.params[Tundo::RANGE_PARAM].setValue((float)((i / (int)(SR / 2)) % 3));
        m.inputs[Tundo::TRIG_INPUT].setVoltage((i % 1000) < 50 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        s.add(m.outputs[Tundo::AUDIO_OUTPUT].getVoltage());
        s.add(m.outputs[Tundo::ENV_OUTPUT].getVoltage());
    }
    report("tundo", "hostile_nans", s.nans, s.nans == 0);
    report("tundo", "hostile_bounded", s.peak, s.peak <= 10.0001f);
    report("tundo", "hostile_alive", s.rms(), s.rms() > 0.05);
}

// one voice, quarter notes, everything expensive turned on. Not a hard budget
// — the number is machine-dependent — but a regression that doubles the cost
// of the engine will trip it.
static void testCpu() {
    Tundo m;
    defaults(m);
    m.params[Tundo::HARM_PARAM].setValue(1.f);
    m.params[Tundo::FOLD_PARAM].setValue(0.9f);
    m.params[Tundo::DECAY_PARAM].setValue(0.7f);
    long frame = 0;
    long n = (long)(30.0 * SR);
    double sink = 0.0;
    clock_t t0 = clock();
    for (long i = 0; i < n; i++) {
        m.inputs[Tundo::TRIG_INPUT].setVoltage((i % (long)(SR / 4)) < 50 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        sink += m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
    }
    double pct = 100.0 * ((double)(clock() - t0) / CLOCKS_PER_SEC) / 30.0;
    report("tundo", "cpu_percent_of_core", pct, pct < 5.0 && std::isfinite(sink));
}

SMOKE_MAIN(testSilence, testHit, testRetrigger, testPitch, testSpread,
           testHarmStaging, testFold, testSkinVsMetal, testAttackDecay,
           testModes, testRateModes, testFreeRun, testSwitchCv, testHostile,
           testCpu)
