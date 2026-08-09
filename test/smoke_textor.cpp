// smoke_textor — offline sanity checks for the textor module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/textor.cpp"

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

// The declick option: with it on, the woven output must hold no
// discontinuities. Everything textor plays here is a 220.3 Hz sine at a
// semitone-quantized rate, so nothing musical reaches 8 kHz; a step in the
// waveform does. The cloth frequency deliberately does not fit a whole
// number of cycles into the two-second buffer, or fragments would wrap
// continuously by accident and the check would pass on a bug. Element
// levels stay low so the output soft limiter, a cubic, does not generate
// harmonics up there itself. textor_probe measures the same thing in
// detail across more scenarios.
static float declickPeak(bool declick) {
    // 4th-order Butterworth highpass at 8 kHz, as two RBJ biquads
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        Biquad(float f0, float q) {
            float w0 = 2.f * (float)M_PI * f0 / SR;
            float c = std::cos(w0), alpha = std::sin(w0) / (2.f * q);
            float a0 = 1.f + alpha;
            b0 = (1.f + c) * 0.5f / a0;
            b1 = -(1.f + c) / a0;
            b2 = b0;
            a1 = -2.f * c / a0;
            a2 = (1.f - alpha) / a0;
        }
        float process(float x) {
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }
    } hp1(8000.f, 0.5412f), hp2(8000.f, 1.3066f);

    Textor m;
    long frame = 0;
    m.declick = declick;
    m.params[Textor::WARP_LEVEL_PARAM].setValue(0.3f);
    m.params[Textor::WEFT_LEVEL_PARAM].setValue(0.3f);
    m.params[Textor::FLECK_LEVEL_PARAM].setValue(0.3f);
    m.inputs[Textor::AUDIO_INPUT].channels = 1;
    m.inputs[Textor::REC_INPUT].channels = 1;
    float phase = 0.f;
    for (int i = 0; i < (int)(2.3f * SR); i++) {
        phase += 220.3f / SR; if (phase >= 1.f) phase -= 1.f;
        m.inputs[Textor::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * (float)M_PI * phase));
        m.inputs[Textor::REC_INPUT].setVoltage(i >= 1000 && i < 1100 ? 10.f : 0.f);
        m.process(makeArgs(frame++));
    }
    m.inputs[Textor::AUDIO_INPUT].setVoltage(0.f);
    m.inputs[Textor::REC_INPUT].setVoltage(0.f);

    float peak = 0.f;
    for (int k = 0; k < 4; k++) {
        m.params[Textor::WEAVE_PARAM].setValue(0.3f + 0.1f * k);
        for (int i = 0; i < (int)(3.f * SR); i++) {
            m.process(makeArgs(frame++));
            float h = hp2.process(hp1.process(m.outputs[Textor::LEFT_OUTPUT].getVoltage()));
            if (i > (int)(0.05f * SR))   // past the filters' own startup
                peak = std::max(peak, std::fabs(h));
        }
    }
    return peak;
}

static void testTextorDeclick() {
    float raw = declickPeak(false);
    float clean = declickPeak(true);
    // the raw engine clicks: if it did not, the check below proves nothing
    report("textor", "declick_off_clicks", raw, raw > 0.3f);
    report("textor", "declick_on_clean", clean, clean < 0.05f);
    report("textor", "declick_improves", raw / std::max(clean, 1e-6f),
           clean < 0.5f * raw);
}

SMOKE_MAIN(testTextor, testTextorDeclick)
