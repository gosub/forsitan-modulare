// cartilago_probe - measurements that characterise the module, not a pass/fail
// harness. Built by `make all`, not run by `make check`.
//
//   1. alias floor of each LFO shape, band-limited and not, and the sweep of
//      the polyBLAMP scale that fixes kBlampScale in src/cartilago.cpp;
//   2. the FET attenuator's transfer curve and its harmonic content;
//   3. the alias floor of the whole module with the LFO in the audio band.
//
// Usage: ./cartilago_probe [blamp|fet|alias]

#include "smoke_harness.hpp"
#include "../src/cartilago.cpp"

#include <complex>
#include <vector>

static const int kN = 4096;

// One bin of the DFT of x, at bin b.
static double bin(const std::vector<double>& x, int b) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * b / (double)x.size();
    for (size_t i = 0; i < x.size(); i++) {
        re += x[i] * std::cos(w * i);
        im -= x[i] * std::sin(w * i);
    }
    return std::sqrt(re * re + im * im);
}

// Energy off the harmonic grid, relative to energy on it, in dB. The
// oscillator frequency is bin0 * fs / N so its harmonics land exactly on bins
// and a rectangular window leaks nothing.
static double aliasDb(const std::vector<double>& x, int bin0) {
    double harm = 0.0, alias = 0.0;
    for (int b = 1; b < (int)x.size() / 2; b++) {
        const double m = bin(x, b);
        ((b % bin0 == 0) ? harm : alias) += m * m;
    }
    return 10.0 * std::log10(alias / std::max(harm, 1e-30));
}

// Render the variable-symmetry triangle with an arbitrary polyBLAMP scale, so
// the constant in the module can be re-derived rather than trusted.
static std::vector<double> tri(float f, float fs, float s, float k, bool bl) {
    std::vector<double> out(kN);
    const float dt = f / fs;
    float ph = 0.f;
    for (int i = 0; i < kN; i++) {
        float y = (ph < s) ? (-1.f + 2.f * ph / s) : (1.f - 2.f * (ph - s) / (1.f - s));
        if (bl) {
            const float ds = 2.f / s + 2.f / (1.f - s);
            float q = ph - s;
            if (q < 0.f) q += 1.f;
            const float g = k * dt * ds;
            y += g * cartilago::blamp(ph, dt) - g * cartilago::blamp(q, dt);
        }
        out[i] = y;
        ph += dt;
        if (ph >= 1.f) ph -= 1.f;
    }
    return out;
}

static std::vector<double> shape(int wave, float f, float fs, float s, bool bl) {
    std::vector<double> out(kN);
    cartilago::Lfo lfo;
    for (int i = 0; i < kN; i++) out[i] = lfo.process(f / fs, s, wave, bl);
    return out;
}

static void probeBlamp() {
    const float fs = 48000.f;
    printf("# polyBLAMP scale sweep: alias energy relative to harmonic, dB\n");
    printf("bin0,shape,k,alias_db\n");
    for (int bin0 : {101, 37}) {
        const float f = bin0 * fs / kN;
        for (float s : {0.5f, 0.25f, 0.1f}) {
            printf("%d,%.2f,naive,%.2f\n", bin0, s, aliasDb(tri(f, fs, s, 0.f, false), bin0));
            for (float k = 0.40f; k <= 0.75f; k += 0.05f)
                printf("%d,%.2f,%.2f,%.2f\n", bin0, s, k,
                       aliasDb(tri(f, fs, s, k, true), bin0));
        }
    }
    printf("\n# the other three shapes, band-limited vs not\n");
    printf("wave,naive_db,blep_db\n");
    const int bin0 = 101;
    const float f = bin0 * fs / kN;
    const char* nm[4] = {"triangle", "ramp up", "ramp down", "pulse"};
    for (int w = 0; w < 4; w++)
        printf("%s,%.2f,%.2f\n", nm[w],
               aliasDb(shape(w, f, fs, 0.5f, false), bin0),
               aliasDb(shape(w, f, fs, 0.5f, true), bin0));
}

// The attenuator's gain and its harmonic content across the sweep.
static void probeFet() {
    printf("# FET attenuator: gain and harmonics vs control, 300 Hz at 5 V\n");
    printf("ctl01,gain,h2_db,h3_db\n");
    for (int i = 0; i <= 10; i++) {
        const float ctl = i / 10.f;
        cartilago::Voice v;
        std::vector<double> out(kN);
        const int bin0 = 32;                      // 375 Hz at 48 kHz
        for (int n = 0; n < kN; n++) {
            const float x = std::sin(2.f * M_PI * bin0 * n / (float)kN);
            out[n] = v.vca(x, ctl);
        }
        const double h1 = bin(out, bin0);
        printf("%.2f,%.4f,%.2f,%.2f\n", ctl, h1 * 2.0 / kN,
               20.0 * std::log10(bin(out, 2 * bin0) / std::max(h1, 1e-30)),
               20.0 * std::log10(bin(out, 3 * bin0) / std::max(h1, 1e-30)));
    }
}

// The module as a whole, ring-modulating. Classifying bins as harmonic or
// aliased does not work here: the FET is itself a distortion, so the spectrum
// is legitimately dense, and at the modulation rates that matter the products
// n*carrier +- m*rate cover nearly every bin. Instead measure how far each
// setting sits from the same patch rendered at 16x, comparing magnitude
// spectra so the oversamplers' different group delays do not count.
static std::vector<double> render(int os, bool bandLimit, float rateHz, int cbin) {
    Cartilago m;
    long fr = 0;
    m.osIndex = os;
    m.bandLimit = bandLimit;
    m.feedthrough = false;
    m.params[Cartilago::WAVE_PARAM].setValue(3.f);              // pulse
    m.params[Cartilago::RATE_PARAM].setValue(std::log2(rateHz));
    m.params[Cartilago::DEPTH_PARAM].setValue(1.f);
    m.params[Cartilago::DRIVE_PARAM].setValue(0.5f);
    m.inputs[Cartilago::AUDIO_INPUT].channels = 1;
    std::vector<double> out(kN);
    for (int n = 0; n < (int)(0.3 * SR) + kN; n++) {
        m.inputs[Cartilago::AUDIO_INPUT].setVoltage(
            5.f * std::sin(2.f * M_PI * cbin * n / (float)kN));
        m.process(makeArgs(fr++));
        const int k = n - (int)(0.3 * SR);
        if (k >= 0) out[k] = m.outputs[Cartilago::AUDIO_OUTPUT].getVoltage();
    }
    return out;
}

static void probeAlias() {
    const int cbin = 33;                        // 386.7 Hz at 48 kHz, N = 4096
    printf("# spectral distance from the same patch at 16x, dB\n");
    printf("os,band_limit,rate_hz,err_db\n");
    for (float rateHz : {58.6f, 1183.6f}) {
        std::vector<double> ref = render(4, true, rateHz, cbin);
        std::vector<double> refMag(kN / 2);
        double refE = 0.0;
        for (int b = 1; b < kN / 2; b++) {
            refMag[b] = bin(ref, b);
            refE += refMag[b] * refMag[b];
        }
        for (int os = 0; os <= 3; os++) {
            for (int bl = 0; bl < 2; bl++) {
                std::vector<double> x = render(os, bl != 0, rateHz, cbin);
                double err = 0.0;
                for (int b = 1; b < kN / 2; b++) {
                    const double d = bin(x, b) - refMag[b];
                    err += d * d;
                }
                printf("%d,%d,%.1f,%.2f\n", 1 << os, bl, rateHz,
                       10.0 * std::log10(err / std::max(refE, 1e-30)));
            }
        }
    }
}

int main(int argc, char** argv) {
    rack::random::init();
    const char* what = (argc > 1) ? argv[1] : "all";
    if (!std::strcmp(what, "all") || !std::strcmp(what, "blamp")) probeBlamp();
    if (!std::strcmp(what, "all") || !std::strcmp(what, "fet")) probeFet();
    if (!std::strcmp(what, "all") || !std::strcmp(what, "alias")) probeAlias();
    return 0;
}
