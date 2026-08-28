// smoke_raucus - offline sanity checks for the raucus module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.
// The measurements that characterise the pedal (tone stack response against
// the published figures, transfer curves, gain structure) live in
// raucus_probe.cpp, which is not run here.

#include "smoke_harness.hpp"
#include "../src/raucus.cpp"

#include <vector>

// 222.7 Hz at 48 kHz with a 4096-point window: the fundamental and its
// harmonics land exactly on bins, so a rectangular window leaks nothing.
static const int kN = 4096;
static const int kBin = 19;

static double bin(const std::vector<double>& x, int b) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * b / (double)x.size();
    for (size_t i = 0; i < x.size(); i++) {
        re += x[i] * std::cos(w * i);
        im -= x[i] * std::sin(w * i);
    }
    return std::sqrt(re * re + im * im);
}

struct Run {
    std::vector<double> x;
    double peak = 0.0, rms = 0.0, dc = 0.0;
    long nans = 0;
    double h(int n) const { return bin(x, n * kBin); }
    double hdb(int n) const { return 20.0 * std::log10(h(n) / std::max(h(1), 1e-30)); }
};

// Drive one module with a sine and capture a clean analysis window.
static Run feed(Raucus& m, float amp, double settleS = 0.4) {
    Run r;
    r.x.resize(kN);
    long fr = 0;
    m.inputs[Raucus::AUDIO_INPUT].channels = 1;
    double sum = 0.0, sum2 = 0.0;
    for (int n = 0; n < (int)(settleS * SR) + kN; n++) {
        m.inputs[Raucus::AUDIO_INPUT].setVoltage(
            amp * std::sin(2.f * M_PI * kBin * n / (float)kN));
        m.process(makeArgs(fr++));
        const int k = n - (int)(settleS * SR);
        if (k < 0) continue;
        float v = m.outputs[Raucus::AUDIO_OUTPUT].getVoltage();
        if (!std::isfinite(v)) { r.nans++; v = 0.f; }
        r.x[k] = v;
        r.peak = std::max(r.peak, (double)std::fabs(v));
        sum += v;
        sum2 += (double)v * v;
    }
    r.rms = std::sqrt(sum2 / kN);
    r.dc = sum / kN;
    return r;
}

static Raucus* make(float sustain, float tone, float volume) {
    Raucus* m = new Raucus();
    m->params[Raucus::SUSTAIN_PARAM].setValue(sustain);
    m->params[Raucus::TONE_PARAM].setValue(tone);
    m->params[Raucus::VOLUME_PARAM].setValue(volume);
    return m;
}

// It has to be loud, clipped and free of DC, and it must not run away.
static void testLevel() {
    for (float amp : {0.5f, 5.f, 10.f}) {
        Raucus* m = make(0.65f, 0.5f, 0.7f);
        Run r = feed(*m, amp);
        char name[64];
        snprintf(name, sizeof name, "level_nans_%.0fv", amp);
        report("raucus", name, r.nans, r.nans == 0);
        snprintf(name, sizeof name, "level_peak_%.0fv", amp);
        report("raucus", name, r.peak, r.peak > 1.0 && r.peak <= 10.0);
        snprintf(name, sizeof name, "level_dc_%.0fv", amp);
        report("raucus", name, r.dc, std::fabs(r.dc) < 0.02);
        // square-ish clipping: odd harmonics, and plenty of them
        snprintf(name, sizeof name, "level_h3_%.0fv", amp);
        report("raucus", name, r.hdb(3), r.hdb(3) > -30.0);
        delete m;
    }

    // volume at zero is silence, whatever else is going on
    Raucus* q = make(1.f, 0.5f, 0.f);
    Run r = feed(*q, 5.f);
    report("raucus", "volume_zero_silent", r.peak, r.peak < 1e-3);
    delete q;
}

// Sustain drives the clipping pair harder rather than moving a threshold, so
// it behaves differently at the two ends of the input range. Quiet in, it is
// the difference between clean and fuzz; loud in, both stages are clipping
// whatever it is set to and it barely changes the level at all - which is the
// pedal's reputation for having no clean setting.
static void testSustain() {
    // ~2 mV at the pedal's input, a fifth of what a guitar delivers
    Raucus* lo = make(0.f, 0.5f, 0.7f);
    Raucus* hi = make(1.f, 0.5f, 0.7f);
    lo->params[Raucus::GAIN_PARAM].setValue(0.f);
    hi->params[Raucus::GAIN_PARAM].setValue(0.f);
    Run a = feed(*lo, 1.f), b = feed(*hi, 1.f);
    report("raucus", "sustain_min_alive", a.rms, a.rms > 1e-3);
    report("raucus", "sustain_min_is_cleaner", a.hdb(3), a.hdb(3) < -25.0);
    report("raucus", "sustain_adds_harmonics", b.hdb(3) - a.hdb(3),
           b.hdb(3) > a.hdb(3) + 6.0);
    delete lo;
    delete hi;

    // driven hard, it is not a fader
    Raucus* lo2 = make(0.f, 0.5f, 0.7f);
    Raucus* hi2 = make(1.f, 0.5f, 0.7f);
    Run c = feed(*lo2, 5.f), d = feed(*hi2, 5.f);
    report("raucus", "sustain_not_a_fader", d.rms / std::max(c.rms, 1e-9),
           d.rms / std::max(c.rms, 1e-9) < 1.6);
    delete lo2;
    delete hi2;
}

// The tone stack, checked on its own: the notch has to be where the analysis
// says it is, move the right way, and cost the right amount of level.
static void testToneStack() {
    auto response = [](float t, double f) {
        raucus::ToneStack ts;
        ts.set(t, 192000.f);
        const int n = 1 << 15;
        double re = 0.0, im = 0.0;
        const double w = 2.0 * M_PI * f / 192000.0;
        for (int i = 0; i < n; i++) {
            const double h = ts.process(i == 0 ? 1.f : 0.f);
            re += h * std::cos(w * i);
            im -= h * std::sin(w * i);
        }
        return 20.0 * std::log10(std::sqrt(re * re + im * im));
    };
    auto notch = [&](float t) {
        double best = 1e9, bestF = 0.0;
        for (double f = 100.0; f < 6000.0; f *= 1.01) {
            const double v = response(t, f);
            if (v < best) { best = v; bestF = f; }
        }
        return std::make_pair(bestF, best);
    };

    // centred: ElectroSmash puts the notch at 1 kHz
    auto mid = notch(0.5f);
    report("raucus", "notch_hz_centred", mid.first,
           mid.first > 850.0 && mid.first < 1350.0);
    // and it is a real scoop, not a gentle tilt
    report("raucus", "notch_depth_centred", response(0.5f, 60.0) - mid.second,
           response(0.5f, 60.0) - mid.second > 5.0);
    // treble-ward the notch walks down
    auto treble = notch(1.f);
    report("raucus", "notch_moves_down", treble.first, treble.first < 400.0);
    // the two ends tilt the way the labels say
    report("raucus", "bass_end_favours_lows",
           response(0.f, 100.0) - response(0.f, 5000.0),
           response(0.f, 100.0) > response(0.f, 5000.0) + 10.0);
    report("raucus", "treble_end_favours_highs",
           response(1.f, 5000.0) - response(1.f, 100.0),
           response(1.f, 5000.0) > response(1.f, 100.0) + 10.0);
    // a passive network cannot have gain
    double worst = -1e9;
    for (float t = 0.f; t <= 1.f; t += 0.1f)
        for (double f = 20.0; f < 20000.0; f *= 1.2)
            worst = std::max(worst, response(t, f));
    report("raucus", "tone_stack_passive", worst, worst < 0.1);
}

// mids fills the scoop back in.
static void testMids() {
    Raucus* off = make(0.8f, 0.5f, 0.7f);
    Raucus* on = make(0.8f, 0.5f, 0.7f);
    on->params[Raucus::MIDS_PARAM].setValue(1.f);
    Run a = feed(*off, 5.f), b = feed(*on, 5.f);
    report("raucus", "mids_adds_level", b.rms / std::max(a.rms, 1e-9),
           b.rms > a.rms * 1.05);
    report("raucus", "mids_nans", b.nans, b.nans == 0);
    delete off;
    delete on;
}

// Bias starves the stages: symmetric clipping has no even harmonics at all,
// and an offset operating point is the only thing here that makes any.
static void testBias() {
    Raucus* sym = make(0.9f, 0.5f, 0.7f);
    Raucus* asym = make(0.9f, 0.5f, 0.7f);
    asym->params[Raucus::BIAS_PARAM].setValue(1.f);
    Run a = feed(*sym, 5.f), b = feed(*asym, 5.f);
    report("raucus", "bias_zero_is_symmetric", a.hdb(2), a.hdb(2) < -60.0);
    report("raucus", "bias_makes_even_harmonics", b.hdb(2), b.hdb(2) > -40.0);
    report("raucus", "bias_no_dc", b.dc, std::fabs(b.dc) < 0.02);
    delete sym;
    delete asym;
}

// The diode menu: the table has to be an accurate solution of the circuit's
// node equation, and the four choices have to be audibly different.
static void testDiodes() {
    const raucus::DiodeTable* t = raucus::diodeTables();
    double worst = 0.0;
    for (int d = 0; d < raucus::kDiodeTypes; d++) {
        const float Is = raucus::kDiodes[d].Is, nVt = raucus::kDiodes[d].nVt;
        for (double w = -40.0; w <= 40.0; w += 0.37) {
            double lo = -20.0, hi = 20.0;
            for (int i = 0; i < 200; i++) {
                const double m = 0.5 * (lo + hi);
                const double a = std::max(-60.0, std::min(m / nVt, 60.0));
                const double f = m + raucus::kRf * Is * (std::exp(a) - std::exp(-a)) - w;
                (f > 0.0 ? hi : lo) = m;
            }
            worst = std::max(worst, std::fabs(t[d].lookup((float)w) - 0.5 * (lo + hi)));
        }
    }
    report("raucus", "diode_table_err_mv", worst * 1000.0, worst < 1e-4);

    double rms[4];
    for (int d = 0; d <= raucus::DIODE_LIFTED; d++) {
        Raucus* m = make(0.8f, 0.5f, 0.7f);
        m->diode = d;
        Run r = feed(*m, 5.f);
        rms[d] = r.rms;
        char name[64];
        snprintf(name, sizeof name, "diode%d_nans", d);
        report("raucus", name, r.nans, r.nans == 0);
        snprintf(name, sizeof name, "diode%d_peak", d);
        report("raucus", name, r.peak, r.peak <= 10.0);
        delete m;
    }
    // higher forward voltage lets the stage swing further before it clamps
    report("raucus", "germanium_quieter_than_silicon",
           rms[raucus::DIODE_GERMANIUM] / rms[raucus::DIODE_SILICON],
           rms[raucus::DIODE_GERMANIUM] < rms[raucus::DIODE_SILICON]);
    report("raucus", "led_louder_than_silicon",
           rms[raucus::DIODE_LED] / rms[raucus::DIODE_SILICON],
           rms[raucus::DIODE_LED] > rms[raucus::DIODE_SILICON]);
    report("raucus", "lifted_loudest",
           rms[raucus::DIODE_LIFTED] / rms[raucus::DIODE_SILICON],
           rms[raucus::DIODE_LIFTED] > rms[raucus::DIODE_LED]);
}

// One pedal per channel: no shared filter state, no shared tone stack.
static void testPoly() {
    Raucus m;
    long fr = 0;
    m.params[Raucus::GAIN_PARAM].setValue(0.2f);
    m.inputs[Raucus::AUDIO_INPUT].channels = 4;
    m.inputs[Raucus::TONE_INPUT].channels = 4;
    m.outputs[Raucus::AUDIO_OUTPUT].channels = 1;
    // channel 0 gets signal, the rest get silence; and each channel gets a
    // different tone, so a shared biquad would show up as crosstalk
    for (int c = 0; c < 4; c++)
        m.inputs[Raucus::TONE_INPUT].setVoltage(c * 3.f, c);
    Stats s[4];
    for (int n = 0; n < (int)(1.0 * SR); n++) {
        m.inputs[Raucus::AUDIO_INPUT].setVoltage(
            5.f * std::sin(2.f * M_PI * 220.f * n / SR), 0);
        for (int c = 1; c < 4; c++) m.inputs[Raucus::AUDIO_INPUT].setVoltage(0.f, c);
        m.process(makeArgs(fr++));
        if (n > (int)(0.5 * SR))
            for (int c = 0; c < 4; c++) s[c].add(m.outputs[Raucus::AUDIO_OUTPUT].getVoltage(c));
    }
    report("raucus", "poly_channels", m.outputs[Raucus::AUDIO_OUTPUT].getChannels(),
           m.outputs[Raucus::AUDIO_OUTPUT].getChannels() == 4);
    report("raucus", "poly_ch0_loud", s[0].rms(), s[0].rms() > 0.5);
    for (int c = 1; c < 4; c++) {
        char name[64];
        snprintf(name, sizeof name, "poly_ch%d_silent", c);
        report("raucus", name, s[c].rms(), s[c].rms() < 1e-3);
    }
}

// Every oversampling setting must be stable and roughly level-matched.
static void testOversampling() {
    double rms[5];
    for (int os = 0; os <= 4; os++) {
        Raucus* m = make(0.9f, 0.5f, 0.7f);
        m->osIndex = os;
        Run r = feed(*m, 5.f);
        rms[os] = r.rms;
        char name[64];
        snprintf(name, sizeof name, "os%dx_nans", 1 << os);
        report("raucus", name, r.nans, r.nans == 0);
        snprintf(name, sizeof name, "os%dx_peak", 1 << os);
        report("raucus", name, r.peak, r.peak <= 10.0);
        delete m;
    }
    double lo = rms[0], hi = rms[0];
    for (int i = 1; i <= 4; i++) { lo = std::min(lo, rms[i]); hi = std::max(hi, rms[i]); }
    report("raucus", "os_level_spread", hi / std::max(lo, 1e-9),
           hi / std::max(lo, 1e-9) < 1.3);
}

// Hostile input and CV, with every setting swept underneath it.
static void testStress() {
    for (int d = 0; d <= raucus::DIODE_LIFTED; d++) {
        Raucus m;
        long fr = 0;
        m.diode = d;
        m.inputs[Raucus::AUDIO_INPUT].channels = 1;
        m.inputs[Raucus::SUSTAIN_INPUT].channels = 1;
        m.inputs[Raucus::TONE_INPUT].channels = 1;
        m.inputs[Raucus::VOLUME_INPUT].channels = 1;
        Stats s;
        for (int i = 0; i < (int)(8 * SR); i++) {
            const float t = (float)i / SR;
            m.inputs[Raucus::AUDIO_INPUT].setVoltage(
                12.f * std::sin(2.f * M_PI * 91.f * t) * std::sin(2.f * M_PI * 0.3f * t));
            m.inputs[Raucus::SUSTAIN_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 3.1f * t));
            m.inputs[Raucus::TONE_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 7.7f * t));
            m.inputs[Raucus::VOLUME_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 1.3f * t));
            m.params[Raucus::GAIN_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.21f * t));
            m.params[Raucus::BIAS_PARAM].setValue(std::sin(2.f * M_PI * 0.17f * t));
            m.params[Raucus::MIDS_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.11f * t));
            m.process(makeArgs(fr++));
            s.add(m.outputs[Raucus::AUDIO_OUTPUT].getVoltage());
        }
        char name[64];
        snprintf(name, sizeof name, "stress_diode%d_nans", d);
        report("raucus", name, s.nans, s.nans == 0);
        snprintf(name, sizeof name, "stress_diode%d_peak", d);
        report("raucus", name, s.peak, s.peak <= 10.f);
    }

    // silence in, silence out: no self-oscillation, no idle noise
    Raucus* q = make(1.f, 0.5f, 1.f);
    long fr = 0;
    Stats s;
    for (int i = 0; i < (int)(2 * SR); i++) {
        q->process(makeArgs(fr++));
        if (i > (int)SR) s.add(q->outputs[Raucus::AUDIO_OUTPUT].getVoltage());
    }
    report("raucus", "silence_in_silence_out", s.peak, s.peak < 1e-4);
    delete q;
}

SMOKE_MAIN(testLevel, testSustain, testToneStack, testMids, testBias,
           testDiodes, testPoly, testOversampling, testStress)
