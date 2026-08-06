// tomentum_probe — measurements that characterise the fuzz, not a pass/fail
// harness. Built by `make all`, not run by `make check`.
//
//   1. the tone stack's response against ElectroSmash's published figures
//      (notch at 1 kHz, -13.5 dB there, 7 dB insertion loss);
//   2. the diode clipper's static transfer curve and the table's error
//      against a Newton solve of the same equation;
//   3. level, harmonic spectrum and gain structure through the whole pedal;
//   4. spectral distance from a 16x rendering, per oversampling setting.
//
// Usage: ./tomentum_probe [tone|diode|level|alias]

#include "smoke_harness.hpp"
#include "../src/tomentum.cpp"

#include <vector>

static const int kN = 4096;

static double bin(const std::vector<double>& x, int b) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * b / (double)x.size();
    for (size_t i = 0; i < x.size(); i++) {
        re += x[i] * std::cos(w * i);
        im -= x[i] * std::sin(w * i);
    }
    return std::sqrt(re * re + im * im);
}

// The tone stack alone, driven with an impulse and measured with a DFT, so
// what is checked is the discrete filter the module actually runs.
static void probeTone() {
    const float sr = 192000.f;                  // 48 kHz at the default 4x
    printf("# tone stack response, dB, as the module runs it at %.0f kHz\n", sr / 1000);
    printf("tone,60Hz,200Hz,1kHz,5kHz,12kHz,notch_db,notch_hz\n");
    for (float t = 0.f; t <= 1.001f; t += 0.25f) {
        tomentum::ToneStack ts;
        ts.set(t, sr);
        std::vector<double> h(1 << 15);
        for (size_t i = 0; i < h.size(); i++) h[i] = ts.process(i == 0 ? 1.f : 0.f);
        auto at = [&](double f) {
            const double b = f * h.size() / sr;
            const int lo = (int)b;
            const double a = bin(h, lo), c = bin(h, lo + 1);
            return 20.0 * std::log10(a + (c - a) * (b - lo));
        };
        double best = 1e9, bestF = 0.0;
        for (double f = 100.0; f < 6000.0; f *= 1.01) {
            const double v = at(f);
            if (v < best) { best = v; bestF = f; }
        }
        printf("%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f\n", t,
               at(60), at(200), at(1000), at(5000), at(12000), best, bestF);
    }
    printf("# reference (ElectroSmash, V3, ideal source and light load):\n");
    printf("#   notch 1 kHz at -13.5 dB, 7 dB overall insertion loss\n");
}

// The table against a Newton solve of the same equation, and the curve itself.
static void probeDiode() {
    const tomentum::DiodeTable* t = tomentum::diodeTables();
    const char* nm[3] = {"silicon", "germanium", "led"};
    printf("# diode clipper: table error vs a Newton solve of v + Rf Id(v) = w\n");
    printf("diode,max_abs_err_mV,max_rel_err_pct\n");
    for (int d = 0; d < 3; d++) {
        const float Is = tomentum::kDiodes[d].Is, nVt = tomentum::kDiodes[d].nVt;
        double maxAbs = 0.0, maxRel = 0.0;
        for (double w = -40.0; w <= 40.0; w += 0.0137) {
            // reference: bisection, immune to the table's own guesses
            double lo = -20.0, hi = 20.0;
            for (int i = 0; i < 200; i++) {
                const double m = 0.5 * (lo + hi);
                const double a = std::max(-60.0, std::min(m / nVt, 60.0));
                const double f = m + tomentum::kRf * Is * (std::exp(a) - std::exp(-a)) - w;
                (f > 0.0 ? hi : lo) = m;
            }
            const double ref = 0.5 * (lo + hi);
            const double got = t[d].lookup((float)w);
            maxAbs = std::max(maxAbs, std::fabs(got - ref));
            if (std::fabs(ref) > 0.05)
                maxRel = std::max(maxRel, std::fabs(got - ref) / std::fabs(ref));
        }
        printf("%s,%.4f,%.4f\n", nm[d], maxAbs * 1000.0, maxRel * 100.0);
    }
    printf("\n# transfer curve, volts out per volt of would-be output\n");
    printf("w,silicon,germanium,led\n");
    for (double w = -3.0; w <= 3.0001; w += 0.25)
        printf("%.2f,%.4f,%.4f,%.4f\n", w, t[0].lookup((float)w),
               t[1].lookup((float)w), t[2].lookup((float)w));
}

// Gain structure and harmonic content through the whole pedal.
static void probeLevel() {
    printf("# output level and harmonics, 220 Hz sine in, tone and volume centred\n");
    printf("in_vpp,sustain,out_vpp,out_rms,h2_db,h3_db,h5_db\n");
    for (float inV : {0.2f, 1.f, 5.f, 10.f}) {
        for (float sus : {0.f, 0.5f, 1.f}) {
            Tomentum m;
            long fr = 0;
            m.params[Tomentum::SUSTAIN_PARAM].setValue(sus);
            m.params[Tomentum::VOLUME_PARAM].setValue(1.f);
            m.inputs[Tomentum::AUDIO_INPUT].channels = 1;
            const int cbin = 19;                  // 222.7 Hz at 48 kHz, N = 4096
            std::vector<double> out(kN);
            for (int n = 0; n < (int)(0.5 * SR) + kN; n++) {
                m.inputs[Tomentum::AUDIO_INPUT].setVoltage(
                    inV * std::sin(2.f * M_PI * cbin * n / (float)kN));
                m.process(makeArgs(fr++));
                const int k = n - (int)(0.5 * SR);
                if (k >= 0) out[k] = m.outputs[Tomentum::AUDIO_OUTPUT].getVoltage();
            }
            double peak = 0.0, sum2 = 0.0;
            for (double v : out) { peak = std::max(peak, std::fabs(v)); sum2 += v * v; }
            const double h1 = bin(out, cbin);
            printf("%.1f,%.1f,%.2f,%.3f,%.1f,%.1f,%.1f\n", inV, sus, peak,
                   std::sqrt(sum2 / kN),
                   20.0 * std::log10(bin(out, 2 * cbin) / std::max(h1, 1e-30)),
                   20.0 * std::log10(bin(out, 3 * cbin) / std::max(h1, 1e-30)),
                   20.0 * std::log10(bin(out, 5 * cbin) / std::max(h1, 1e-30)));
        }
    }
}

// How far each oversampling setting sits from the same patch at 16x. Compared
// as magnitude spectra so the oversamplers' group delays do not count.
static void probeAlias() {
    const int cbin = 19;
    printf("# spectral distance from the same patch at 16x, dB\n");
    printf("os,tone_in_hz,err_db\n");
    for (int cb : {19, 191}) {                    // 222.7 Hz and 2238 Hz
        std::vector<std::vector<double>> got(5);
        for (int os = 4; os >= 0; os--) {
            Tomentum m;
            long fr = 0;
            m.osIndex = os;
            m.params[Tomentum::SUSTAIN_PARAM].setValue(1.f);
            m.params[Tomentum::VOLUME_PARAM].setValue(1.f);
            m.inputs[Tomentum::AUDIO_INPUT].channels = 1;
            std::vector<double> out(kN);
            for (int n = 0; n < (int)(0.5 * SR) + kN; n++) {
                m.inputs[Tomentum::AUDIO_INPUT].setVoltage(
                    5.f * std::sin(2.f * M_PI * cb * n / (float)kN));
                m.process(makeArgs(fr++));
                const int k = n - (int)(0.5 * SR);
                if (k >= 0) out[k] = m.outputs[Tomentum::AUDIO_OUTPUT].getVoltage();
            }
            got[os] = out;
        }
        std::vector<double> refMag(kN / 2);
        double refE = 0.0;
        for (int b = 1; b < kN / 2; b++) {
            refMag[b] = bin(got[4], b);
            refE += refMag[b] * refMag[b];
        }
        for (int os = 0; os <= 3; os++) {
            double err = 0.0;
            for (int b = 1; b < kN / 2; b++) {
                const double d = bin(got[os], b) - refMag[b];
                err += d * d;
            }
            printf("%d,%.0f,%.2f\n", 1 << os, cb * SR / kN,
                   10.0 * std::log10(err / std::max(refE, 1e-30)));
        }
    }
    (void)cbin;
}

int main(int argc, char** argv) {
    rack::random::init();
    const char* what = (argc > 1) ? argv[1] : "all";
    if (!std::strcmp(what, "all") || !std::strcmp(what, "tone")) probeTone();
    if (!std::strcmp(what, "all") || !std::strcmp(what, "diode")) probeDiode();
    if (!std::strcmp(what, "all") || !std::strcmp(what, "level")) probeLevel();
    if (!std::strcmp(what, "all") || !std::strcmp(what, "alias")) probeAlias();
    return 0;
}
