// textor_probe - measures discontinuities ("clicks") in textor's output.
//
// The cloth is a sine, so everything textor can play is that sine at a
// semitone-quantized rate (at most +36, i.e. under 2 kHz) beneath a slow
// raised-cosine window, plus its own decaying delay repeats. All of that
// lives well below 8 kHz. A discontinuity does not: a step in the waveform
// spreads energy across the whole spectrum. So the probe highpasses the
// output at 8 kHz (4th-order Butterworth) and reports what comes through -
// residual level relative to the signal, the loudest single transient, and
// how many separate transients cross an audibility threshold.
//
// Two things the measurement has to get right:
//   - the cloth frequency must NOT fit a whole number of cycles into the
//     two-second buffer, or the buffer wraps continuously by accident and
//     the loudest click in the module hides itself
//   - the element levels are turned down, because the output soft limiter
//     is a cubic: driven hard by seven voices at 0.8 it generates harmonics
//     above 8 kHz on its own, and they swamp the clicks being looked for
//
// Peak sample-to-sample step is NOT a usable metric here: sixteen voices at
// 2 kHz sum their slews, so a clean output already steps by volts/sample.
//
// Not run by `make check` (it measures, it does not assert). Build with
// `make textor_probe` and run.

#include "smoke_harness.hpp"
#include "../src/textor.cpp"

static const float kClothHz = 220.3f;   // 440.6 cycles per 2 s buffer
static const float kLevel = 0.3f;       // keep the output limiter out of it
static const float kHpHz = 8000.f;
static const float kEventV = 0.05f;      // audible transient, at 5 V full scale
static const float kRefractoryS = 0.005f;

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void highpass(float f0, float fs, float q) {
        float w0 = 2.f * (float)M_PI * f0 / fs;
        float c = std::cos(w0), s = std::sin(w0);
        float alpha = s / (2.f * q);
        float a0 = 1.f + alpha;
        b0 = (1.f + c) * 0.5f / a0;
        b1 = -(1.f + c) / a0;
        b2 = (1.f + c) * 0.5f / a0;
        a1 = -2.f * c / a0;
        a2 = (1.f - alpha) / a0;
    }
    float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// 4th-order Butterworth highpass + transient statistics of what it passes
struct ClickMeter {
    Biquad s1, s2;
    double hf2 = 0, all2 = 0;
    long n = 0;
    float peak = 0.f;
    long events = 0;
    int refractory = 0;
    float settle = 0.f;

    ClickMeter() {
        s1.highpass(kHpHz, SR, 0.5412f);
        s2.highpass(kHpHz, SR, 1.3066f);
        settle = 0.01f * SR;   // ignore the filters' own startup transient
    }
    void add(float v) {
        float h = s2.process(s1.process(v));
        if (settle > 0.f) { settle -= 1.f; return; }
        hf2 += (double)h * h;
        all2 += (double)v * v;
        n++;
        float a = std::fabs(h);
        peak = std::max(peak, a);
        if (refractory > 0)
            refractory--;
        else if (a > kEventV) {
            events++;
            refractory = (int)(kRefractoryS * SR);
        }
    }
    // fold another scenario's meter in: each module instance needs its own
    // filter, or the step between one run's last sample and the next run's
    // first reads as a click that belongs to neither
    void merge(const ClickMeter& o) {
        hf2 += o.hf2; all2 += o.all2; n += o.n;
        peak = std::max(peak, o.peak);
        events += o.events;
    }
    double hfRms() const { return n ? std::sqrt(hf2 / n) : 0.0; }
    double rms() const { return n ? std::sqrt(all2 / n) : 0.0; }
    double hfDb() const {
        double r = rms(), h = hfRms();
        return (r > 1e-9 && h > 1e-12) ? 20.0 * std::log10(h / r) : -200.0;
    }
};

static void setLevels(Textor& m) {
    m.params[Textor::WARP_LEVEL_PARAM].setValue(kLevel);
    m.params[Textor::WEFT_LEVEL_PARAM].setValue(kLevel);
    m.params[Textor::FLECK_LEVEL_PARAM].setValue(kLevel);
}

static void captureSine(Textor& m, long& frame, float hz = kClothHz) {
    m.inputs[Textor::AUDIO_INPUT].channels = 1;
    m.inputs[Textor::REC_INPUT].channels = 1;
    float phase = 0.f;
    for (int i = 0; i < (int)(2.3f * SR); i++) {
        phase += hz / SR;
        if (phase >= 1.f) phase -= 1.f;
        m.inputs[Textor::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * (float)M_PI * phase));
        m.inputs[Textor::REC_INPUT].setVoltage(i >= 1000 && i < 1100 ? 10.f : 0.f);
        m.process(makeArgs(frame++));
    }
    m.inputs[Textor::AUDIO_INPUT].setVoltage(0.f);
    m.inputs[Textor::REC_INPUT].setVoltage(0.f);
}

// settle on one weave, then measure `seconds` of it looping
static void run(const char* tag, bool declick, bool texture, int weaves, float seconds) {
    ClickMeter total;
    double totalSeconds = 0.0;
    for (int k = 0; k < weaves; k++) {
        ClickMeter c;
        Textor m;
        long frame = 0;
        m.declick = declick;
        setLevels(m);
        m.params[Textor::MODE_PARAM].setValue(texture ? 1.f : 0.f);
        captureSine(m, frame);
        // the auto-play weave, then one deliberate reroll per k
        m.params[Textor::WEAVE_PARAM].setValue(0.5f);
        for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
        m.params[Textor::WEAVE_PARAM].setValue(0.25f + 0.05f * k);
        for (int i = 0; i < (int)(0.2f * SR); i++) m.process(makeArgs(frame++));
        for (int i = 0; i < (int)(seconds * SR); i++) {
            m.process(makeArgs(frame++));
            c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
        }
        totalSeconds += seconds;
        total.merge(c);
    }
    printf("%-24s %-8s hf=%6.1f dB  hf_peak=%6.3f V  clicks=%4ld (%5.1f/s)  rms=%.3f\n",
           tag, declick ? "declick" : "raw", total.hfDb(), total.peak, total.events,
           total.events / totalSeconds, total.rms());
}

// swap the cloth under a running loom: record again while it plays
static void runRerecord(const char* tag, bool declick) {
    Textor m;
    long frame = 0;
    m.declick = declick;
    setLevels(m);
    captureSine(m, frame);
    m.params[Textor::WEAVE_PARAM].setValue(0.5f);
    ClickMeter c;
    for (int i = 0; i < (int)(1.f * SR); i++) {
        m.process(makeArgs(frame++));
        c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    float phase = 0.f;
    for (int i = 0; i < (int)(4.f * SR); i++) {
        phase += 330.f / SR;
        if (phase >= 1.f) phase -= 1.f;
        m.inputs[Textor::AUDIO_INPUT].setVoltage(5.f * std::sin(2.f * (float)M_PI * phase));
        m.inputs[Textor::REC_INPUT].setVoltage(i >= 1000 && i < 1100 ? 10.f : 0.f);
        m.process(makeArgs(frame++));
        c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    printf("%-24s %-8s hf=%6.1f dB  hf_peak=%6.3f V  clicks=%4ld\n",
           tag, declick ? "declick" : "raw", c.hfDb(), c.peak, c.events);
}

// RESET: the sample is erased under a sounding loom
static void runReset(const char* tag, bool declick) {
    Textor m;
    long frame = 0;
    m.declick = declick;
    setLevels(m);
    captureSine(m, frame);
    m.params[Textor::WEAVE_PARAM].setValue(0.5f);
    ClickMeter c;
    for (int i = 0; i < (int)(1.5f * SR); i++) {
        m.process(makeArgs(frame++));
        c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    m.params[Textor::WEAVE_PARAM].setValue(0.f);
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        m.process(makeArgs(frame++));
        c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    printf("%-24s %-8s hf=%6.1f dB  hf_peak=%6.3f V  clicks=%4ld\n",
           tag, declick ? "declick" : "raw", c.hfDb(), c.peak, c.events);
}

// a knob sweep: every reroll swaps the weave (and its delay tap) underneath
static void runSweep(const char* tag, bool declick, bool restart) {
    Textor m;
    long frame = 0;
    m.declick = declick;
    m.sweepRestart = restart;
    setLevels(m);
    captureSine(m, frame);
    m.params[Textor::WEAVE_PARAM].setValue(0.3f);
    ClickMeter c;
    for (int i = 0; i < (int)(0.5f * SR); i++) {
        m.process(makeArgs(frame++));
        c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
    }
    for (int k = 0; k < 12; k++) {
        m.params[Textor::WEAVE_PARAM].setValue(0.3f + 0.05f * k);
        for (int i = 0; i < (int)(0.35f * SR); i++) {
            m.process(makeArgs(frame++));
            c.add(m.outputs[Textor::LEFT_OUTPUT].getVoltage());
        }
    }
    printf("%-24s %-8s hf=%6.1f dB  hf_peak=%6.3f V  clicks=%4ld\n",
           tag, declick ? "declick" : "raw", c.hfDb(), c.peak, c.events);
}

int main() {
    printf("cloth: %.1f Hz sine, so all musical content sits below 2 kHz.\n"
           "hf = RMS above %.0f Hz relative to the output, hf_peak = loudest\n"
           "transient there, clicks = separate transients above %.2f V.\n\n",
           kClothHz, kHpHz, kEventV);
    for (int d = 0; d < 2; d++) {
        bool dc = d != 0;
        run("texture, 10 weaves", dc, true, 10, 8.f);
        run("rhythm, 10 weaves", dc, false, 10, 8.f);
        runRerecord("rerecord under loom", dc);
        runReset("reset under loom", dc);
        runSweep("knob sweep", dc, false);
        runSweep("knob sweep (restart)", dc, true);
        printf("\n");
    }
    return 0;
}
