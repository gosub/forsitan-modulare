// materiae_random — what does Ctrl-R actually give you?
//
//   ./materiae_random silence [n]   uniform randomizations, how many are
//                                   inaudible, and which knobs are to blame
//   ./materiae_random gain          level and crest against the GAIN knob,
//                                   over a spread of random patches
//
// Rack's randomize is uniform over every parameter's whole range, which for a
// voice with a filter in it is not the same as uniform over useful sounds.
// This measures the gap.

#include "smoke_harness.hpp"
#include "../src/materiae.cpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

static const char* kParamNames[] = {
    "pitch", "ratio", "shape", "grid", "div", "xmod", "tilt", "dest",
    "relation", "blend", "cutoff", "reso", "filter", "gain", "decay2",
    "curve2", "attack", "decay", "curve", "e2pitch", "e2rel", "e2cut", "hit"
};

// Rack's own default: uniform over [min, max], snapping where the param snaps.
static void randomizeLikeRack(Materiae& m) {
    for (int i = 0; i < Materiae::PARAMS_LEN; i++) {
        ParamQuantity* q = m.getParamQuantity(i);
        if (i == Materiae::HIT_PARAM) { m.params[i].setValue(0.f); continue; }
        float v = q->getMinValue()
                + (q->getMaxValue() - q->getMinValue()) * random::uniform();
        if (q->snapEnabled) v = std::round(v);
        m.params[i].setValue(v);
    }
}

static Stats strike(Materiae& m, float seconds) {
    Stats s;
    long frame = 0;
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
    for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
    m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
    int n = (int)(seconds * SR);
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(frame++));
        s.add(m.outputs[Materiae::AUDIO_OUTPUT].getVoltage());
    }
    return s;
}

// A voice is "inaudible" when it never gets within 30 dB of the swing it
// declares. That is quiet enough to be useless in a patch, not merely soft.
static const float kAudibleRms = 5.f * 0.02f;

// The module's own randomize, the one Ctrl-R actually calls.
static void randomizeLikeModule(Materiae& m) {
    Module::RandomizeEvent e;
    m.onRandomize(e);
}

static bool useModule = true;

static int modeSilence(int n) {
    std::vector<double> quietSum(Materiae::PARAMS_LEN, 0.0);
    std::vector<double> loudSum(Materiae::PARAMS_LEN, 0.0);
    std::vector<float> saved(Materiae::PARAMS_LEN);
    std::vector<float> peaks;
    int quiet = 0;

    for (int k = 0; k < n; k++) {
        Materiae m;
        if (useModule) randomizeLikeModule(m); else randomizeLikeRack(m);
        for (int i = 0; i < Materiae::PARAMS_LEN; i++)
            saved[i] = m.params[i].getValue();
        Stats s = strike(m, 1.2f);
        // RMS, not peak: a patch can hit the rails once and then be nothing,
        // and that is exactly what reads as inaudible
        peaks.push_back((float)s.rms());
        bool isQuiet = s.rms() < kAudibleRms;
        if (isQuiet) quiet++;
        for (int i = 0; i < Materiae::PARAMS_LEN; i++) {
            ParamQuantity* q = m.getParamQuantity(i);
            float lo = q->getMinValue(), hi = q->getMaxValue();
            float norm = (hi > lo) ? (saved[i] - lo) / (hi - lo) : 0.f;
            (isQuiet ? quietSum : loudSum)[i] += norm;
        }
    }

    printf("%d randomizations, %d too quiet to use (%.1f%%)\n\n", n, quiet,
           100.0 * quiet / n);
    std::sort(peaks.begin(), peaks.end());
    auto pct = [&](double f) { return peaks[(size_t)(f * (peaks.size() - 1))]; };
    printf("RMS distribution, dB relative to the 5 V swing\n");
    const double qs[] = {0.05, 0.10, 0.25, 0.50, 0.75, 0.90};
    for (double q : qs)
        printf("  p%-3.0f  %6.1f dB\n", q * 100, 20.0 * std::log10(pct(q) / 5.0 + 1e-12));
    int below20 = 0, below12 = 0;
    for (float v : peaks) {
        if (20.0 * std::log10(v / 5.0 + 1e-12) < -20.0) below20++;
        if (20.0 * std::log10(v / 5.0 + 1e-12) < -12.0) below12++;
    }
    printf("  under -20 dB: %.1f%%   under -12 dB: %.1f%%\n\n",
           100.0 * below20 / n, 100.0 * below12 / n);
    printf("mean knob position, 0-1, in the inaudible set vs the rest\n");
    printf("param,quiet,loud,shift\n");
    struct Row { std::string name; double q, l, d; };
    std::vector<Row> rows;
    for (int i = 0; i < Materiae::PARAMS_LEN; i++) {
        if (i == Materiae::HIT_PARAM) continue;
        double qm = quiet ? quietSum[i] / quiet : 0.0;
        double lm = (n - quiet) ? loudSum[i] / (n - quiet) : 0.0;
        rows.push_back({kParamNames[i], qm, lm, qm - lm});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return std::fabs(a.d) > std::fabs(b.d); });
    for (const Row& r : rows)
        printf("%s,%.3f,%.3f,%+.3f\n", r.name.c_str(), r.q, r.l, r.d);
    return 0;
}

static int modeGain() {
    printf("patch,rms_0dB,rms_12dB,rms_24dB,gain_12_to_24_dB,worst_backstep_dB,"
           "slew_over_rms_0dB,slew_over_rms_24dB\n");
    int backwards = 0;
    for (int p = 0; p < 24; p++) {
        Materiae seedM;
        randomizeLikeRack(seedM);
        std::vector<float> knobs(Materiae::PARAMS_LEN);
        for (int i = 0; i < Materiae::PARAMS_LEN; i++)
            knobs[i] = seedM.params[i].getValue();
        double rms[9], slew[9];
        for (int g = 0; g <= 8; g++) {
            Materiae m;
            for (int i = 0; i < Materiae::PARAMS_LEN; i++)
                m.params[i].setValue(knobs[i]);
            m.params[Materiae::GAIN_PARAM].setValue(g / 8.f);
            m.params[Materiae::DECAY_PARAM].setValue(0.55f);
            m.params[Materiae::ATTACK_PARAM].setValue(0.f);
            // render, keeping the samples so slew can be measured
            long frame = 0;
            m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
            m.process(makeArgs(frame++));
            m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
            for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
            m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
            Stats s;
            float prev = 0.f, maxd = 0.f;
            for (int i = 0; i < (int)(0.8f * SR); i++) {
                m.process(makeArgs(frame++));
                float v = m.outputs[Materiae::AUDIO_OUTPUT].getVoltage();
                s.add(v);
                if (i) maxd = std::max(maxd, std::fabs(v - prev));
                prev = v;
            }
            rms[g] = s.rms();
            slew[g] = maxd / (s.rms() + 1e-9);
        }
        double worst = 0.0;
        for (int g = 1; g <= 8; g++) {
            double step = 20.0 * std::log10((rms[g] + 1e-12) / (rms[g - 1] + 1e-12));
            worst = std::min(worst, step);
        }
        if (worst < -0.5) backwards++;
        printf("%d,%.4f,%.4f,%.4f,%+.2f,%+.2f,%.1f,%.1f\n", p, rms[0], rms[4],
               rms[8], 20.0 * std::log10((rms[8] + 1e-12) / (rms[4] + 1e-12)),
               worst, slew[0], slew[8]);
    }
    printf("\npatches where more gain gives less level: %d of 24\n", backwards);
    return 0;
}

int main(int argc, char** argv) {
    random::init();
    const char* mode = argc > 1 ? argv[1] : "silence";
    for (int i = 2; i < argc; i++)
        if (!std::strcmp(argv[i], "--uniform")) useModule = false;
    if (!std::strcmp(mode, "silence"))
        return modeSilence(argc > 2 ? atoi(argv[2]) : 400);
    if (!std::strcmp(mode, "gain")) return modeGain();
    fprintf(stderr, "usage: %s silence [n] | gain\n", argv[0]);
    return 2;
}
