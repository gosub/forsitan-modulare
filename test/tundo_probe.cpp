// tundo_probe - measurement harness for the tundo drum voice.
// Renders single hits and reports the numbers a percussive voice is judged
// by: level, decay time, partial frequencies against the spread law, spectral
// centroid against HARM, high-order energy against FOLD, and pitch tracking.
// Also dumps a hit to a WAV file for listening.
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/tundo.cpp"

#include <vector>
#include <string>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef std::vector<float> Buf;

// ------------------------------------------------------------------- driving

static void defaults(Tundo& m) {
    m.params[Tundo::PITCH_PARAM].setValue(0.f);
    m.params[Tundo::HARM_PARAM].setValue(0.3f);
    m.params[Tundo::SPREAD_PARAM].setValue(0.f);
    m.params[Tundo::MORPH_PARAM].setValue(0.f);
    m.params[Tundo::FOLD_PARAM].setValue(0.f);
    m.params[Tundo::ATTACK_PARAM].setValue(0.5f);
    m.params[Tundo::DECAY_PARAM].setValue(0.35f);
}

// settle the smoothers, strike once, capture `secs` of audio
static Buf hit(Tundo& m, double secs, double settle = 0.05) {
    long frame = 0;
    for (int i = 0; i < (int)(settle * SR); i++)
        m.process(makeArgs(frame++));
    Buf out((size_t)(secs * SR));
    for (size_t i = 0; i < out.size(); i++) {
        m.inputs[Tundo::TRIG_INPUT].setVoltage(i < 50 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        out[i] = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
    }
    return out;
}

// ---------------------------------------------------------------- measuring

static double peakOf(const Buf& x) {
    double p = 0.0;
    for (size_t i = 0; i < x.size(); i++) p = std::max(p, (double)std::fabs(x[i]));
    return p;
}

// time from the peak to the point where a 5 ms sliding RMS falls 60 dB below
// its own maximum, in seconds; -1 if it never does inside the window
static double decayTime(const Buf& x) {
    int w = (int)(0.005 * SR);
    double acc = 0.0;
    Buf env(x.size(), 0.f);
    for (size_t i = 0; i < x.size(); i++) {
        acc += (double)x[i] * x[i];
        if ((int)i >= w) acc -= (double)x[i - w] * x[i - w];
        env[i] = (float)std::sqrt(acc / w);
    }
    size_t at = 0;
    float best = 0.f;
    for (size_t i = 0; i < env.size(); i++)
        if (env[i] > best) { best = env[i]; at = i; }
    if (best <= 0.f) return -1.0;
    for (size_t i = at; i < env.size(); i++)
        if (env[i] < best * 0.001f) return (double)(i - at) / SR;
    return -1.0;
}

// in-place radix-2 FFT
static void fft(std::vector<std::complex<double> >& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; k++) {
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// magnitude spectrum of `n` samples starting at `off`, Hann windowed
static std::vector<double> spectrum(const Buf& x, size_t off, size_t n) {
    std::vector<std::complex<double> > a(n);
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
        double v = (off + i < x.size()) ? x[off + i] : 0.f;
        a[i] = std::complex<double>(v * w, 0.0);
    }
    fft(a);
    std::vector<double> mag(n / 2);
    for (size_t i = 0; i < n / 2; i++) mag[i] = std::abs(a[i]);
    return mag;
}

// power-weighted, so the broadband floor of the ZOH images and the 16-bit
// quantization does not drown the partials
static double centroid(const std::vector<double>& mag, double binHz) {
    double num = 0.0, den = 0.0;
    for (size_t i = 1; i < mag.size(); i++) {
        double e = mag[i] * mag[i];
        num += e * i * binHz;
        den += e;
    }
    return den > 0.0 ? num / den : 0.0;
}

// fraction of the spectral energy above `hz`
static double energyAbove(const std::vector<double>& mag, double binHz, double hz) {
    double hi = 0.0, all = 0.0;
    for (size_t i = 1; i < mag.size(); i++) {
        double e = mag[i] * mag[i];
        all += e;
        if (i * binHz > hz) hi += e;
    }
    return all > 0.0 ? hi / all : 0.0;
}

// the strongest peak near `hz`, refined by parabolic interpolation
static double peakNear(const std::vector<double>& mag, double binHz, double hz,
                       double tolFrac) {
    int lo = std::max(1, (int)((hz * (1.0 - tolFrac)) / binHz));
    int hi = std::min((int)mag.size() - 2, (int)((hz * (1.0 + tolFrac)) / binHz));
    if (lo >= hi) return 0.0;
    int at = lo;
    for (int i = lo; i <= hi; i++)
        if (mag[i] > mag[at]) at = i;
    double a = mag[at - 1], b = mag[at], c = mag[at + 1];
    double d = (a - c) / (2.0 * (a - 2.0 * b + c) + 1e-30);
    return (at + d) * binHz;
}

// fundamental from interpolated rising zero crossings
static double zcFreq(const Buf& x, size_t off, size_t n) {
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
    return SR * count / (last - first);
}

// -------------------------------------------------------------------- output

static void writeWav(const char* path, const Buf& x) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    int n = (int)x.size();
    int rate = (int)SR;
    int dataBytes = n * 2;
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    int riff = 36 + dataBytes;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    int fmtLen = 16, byteRate = rate * 2, blockAlign = 2, bits = 16;
    short one = 1, chans = 1;
    std::memcpy(h + 16, &fmtLen, 4);
    std::memcpy(h + 20, &one, 2);
    std::memcpy(h + 22, &chans, 2);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &byteRate, 4);
    std::memcpy(h + 32, &blockAlign, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &dataBytes, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < n; i++) {
        int v = (int)std::lround(clamp(x[i] / 10.f, -1.f, 1.f) * 32767.f);
        short s = (short)v;
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("wrote %s (%.2f s)\n", path, n / SR);
}

// --------------------------------------------------------------------- tests

static void probeLevels() {
    printf("\n== level and decay (default patch, output 10 Vpp)\n");
    printf("decay_knob,decay_ms,peak_V,decay_60dB_s\n");
    const float knobs[] = {0.f, 0.2f, 0.35f, 0.6f, 0.8f, 1.f};
    for (int i = 0; i < 6; i++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::DECAY_PARAM].setValue(knobs[i]);
        Buf x = hit(m, std::min(6.0, 0.2 + 2.0 * knobs[i] * 4.0));
        printf("%.2f,%.1f,%.3f,%.4f\n", knobs[i], m.p.decayMs, peakOf(x), decayTime(x));
    }
}

static void probeHarm() {
    printf("\n== harm staging (centroid should climb monotonically)\n");
    printf("harm,centroid_hz,peak_V\n");
    for (int i = 0; i <= 10; i++) {
        float h = i / 10.f;
        Tundo m;
        defaults(m);
        m.params[Tundo::HARM_PARAM].setValue(h);
        m.params[Tundo::DECAY_PARAM].setValue(0.6f);
        Buf x = hit(m, 0.5);
        std::vector<double> mag = spectrum(x, (size_t)(0.005 * SR), 16384);
        printf("%.1f,%.1f,%.3f\n", h, centroid(mag, SR / 16384.0), peakOf(x));
    }
}

static void probeSpread() {
    printf("\n== spread (partial frequencies vs. the law)\n");
    printf("spread,f1,f2,f3,f4,f5,f6\n");
    const float sv[] = {0.f, 0.5f, 1.f};
    for (int s = 0; s < 3; s++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::HARM_PARAM].setValue(1.f);
        m.params[Tundo::SPREAD_PARAM].setValue(sv[s]);
        m.params[Tundo::DECAY_PARAM].setValue(0.8f);
        m.params[Tundo::PITCH_PARAM].setValue(2.f);     // ~131 Hz
        Buf x = hit(m, 1.0);
        std::vector<double> mag = spectrum(x, (size_t)(0.02 * SR), 32768);
        double bin = SR / 32768.0;
        printf("%.1f", sv[s]);
        for (int i = 0; i < 6; i++) {
            float want = m.engine.ratio[i] * m.p.f0;
            printf(",%.1f", peakNear(mag, bin, want, 0.06));
        }
        printf("\n");
    }
}

static void probeFold() {
    printf("\n== fold (energy above the 12th partial should climb)\n");
    printf("fold,thresh,hi_energy_frac,peak_V\n");
    for (int i = 0; i <= 10; i++) {
        float f = i / 10.f;
        Tundo m;
        defaults(m);
        m.params[Tundo::FOLD_PARAM].setValue(f);
        m.params[Tundo::DECAY_PARAM].setValue(0.6f);
        Buf x = hit(m, 0.5);
        std::vector<double> mag = spectrum(x, (size_t)(0.005 * SR), 16384);
        double bin = SR / 16384.0;
        printf("%.1f,%.4f,%.4f,%.3f\n", f, m.engine.foldThresh,
               energyAbove(mag, bin, 12.0 * m.p.f0), peakOf(x));
    }
}

static void probePitch() {
    printf("\n== pitch tracking (skin, harm 0, sine)\n");
    printf("range,volts,want_hz,got_hz,cents\n");
    for (int r = 0; r < 3; r++) {
        for (int v = 0; v <= 3; v++) {
            Tundo m;
            defaults(m);
            m.params[Tundo::HARM_PARAM].setValue(0.f);
            m.params[Tundo::DECAY_PARAM].setValue(1.f);
            m.params[Tundo::PITCH_PARAM].setValue((float)v);
            m.params[Tundo::RANGE_PARAM].setValue((float)r);
            Buf x = hit(m, 0.6);
            double want = tundo_dsp::kBaseHz * std::exp2((double)v + 2.0 * r);
            double got = zcFreq(x, (size_t)(0.15 * SR), (size_t)(0.4 * SR));
            double cents = got > 0.0 ? 1200.0 * std::log2(got / want) : 0.0;
            printf("%d,%d,%.2f,%.3f,%.2f\n", r, v, want, got, cents);
        }
    }
}

static void probeModes() {
    printf("\n== modes\n");
    printf("mode,peak_V,rms_V,centroid_hz\n");
    const char* names[3] = {"skin", "liquid", "metal"};
    for (int mo = 0; mo < 3; mo++) {
        Tundo m;
        defaults(m);
        m.params[Tundo::MODE_PARAM].setValue((float)mo);
        m.params[Tundo::HARM_PARAM].setValue(0.7f);
        m.params[Tundo::DECAY_PARAM].setValue(0.5f);
        Buf x = hit(m, 1.0);
        Stats s;
        for (size_t i = 0; i < x.size(); i++) s.add(x[i]);
        // the first 43 ms, where the modes actually differ; over a full
        // second every mode is just its surviving carrier
        std::vector<double> mag = spectrum(x, (size_t)(0.002 * SR), 2048);
        printf("%s,%.3f,%.3f,%.1f\n", names[mo], peakOf(x), s.rms(),
               centroid(mag, SR / 2048.0));
    }
}

static void probeRateModes() {
    printf("\n== sample-rate modes (hardware vs. clean)\n");
    printf("mode,internal_rate_hz,mult,peak_V,hi_energy_frac\n");
    for (int c = 0; c < 2; c++) {
        Tundo m;
        defaults(m);
        m.cleanRate = (c == 1);
        m.params[Tundo::HARM_PARAM].setValue(0.8f);
        m.params[Tundo::MORPH_PARAM].setValue(1.f);     // square: worst case
        m.params[Tundo::DECAY_PARAM].setValue(0.6f);
        Buf x = hit(m, 0.5);
        std::vector<double> mag = spectrum(x, (size_t)(0.005 * SR), 16384);
        printf("%s,%.0f,%.0f,%.3f,%.4f\n", c ? "clean" : "hardware",
               m.engine.rate, m.engine.mult, peakOf(x),
               energyAbove(mag, SR / 16384.0, 8000.0));
    }
}

static void probeCpu() {
    printf("\n== cpu (one voice, 60 s of audio at 48 kHz)\n");
    Tundo m;
    defaults(m);
    m.params[Tundo::HARM_PARAM].setValue(1.f);
    m.params[Tundo::FOLD_PARAM].setValue(0.9f);
    m.params[Tundo::DECAY_PARAM].setValue(0.7f);
    long frame = 0;
    long n = (long)(60.0 * SR);
    clock_t t0 = clock();
    double sink = 0.0;
    for (long i = 0; i < n; i++) {
        m.inputs[Tundo::TRIG_INPUT].setVoltage((i % (long)(SR / 4)) < 50 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        sink += m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("cpu_percent_of_one_core,%.3f\n", 100.0 * secs / 60.0);
    printf("(sink %.3f)\n", sink);
}

static void dumpWavs(const char* dir) {
    struct Patch { const char* name; float pitch, harm, spread, morph, fold, att, dec; int mode; };
    static const Patch patches[] = {
        {"kick",   -1.f, 0.15f, 0.f,  0.f,  0.15f, 0.5f,  0.30f, 1},
        {"snare",   1.f, 0.65f, 0.6f, 0.4f, 0.35f, 0.35f, 0.25f, 0},
        {"hat",     3.f, 0.9f,  1.f,  0.8f, 0.5f,  0.45f, 0.10f, 2},
        {"stab",    1.f, 0.8f,  0.8f, 0.6f, 0.8f,  0.55f, 0.45f, 2},
        {"bass",   -1.f, 0.5f,  0.1f, 0.7f, 0.6f,  0.6f,  0.55f, 0},
    };
    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
        const Patch& q = patches[i];
        Tundo m;
        defaults(m);
        m.params[Tundo::PITCH_PARAM].setValue(q.pitch);
        m.params[Tundo::HARM_PARAM].setValue(q.harm);
        m.params[Tundo::SPREAD_PARAM].setValue(q.spread);
        m.params[Tundo::MORPH_PARAM].setValue(q.morph);
        m.params[Tundo::FOLD_PARAM].setValue(q.fold);
        m.params[Tundo::ATTACK_PARAM].setValue(q.att);
        m.params[Tundo::DECAY_PARAM].setValue(q.dec);
        m.params[Tundo::MODE_PARAM].setValue((float)q.mode);
        // four strikes at 2 Hz
        long frame = 0;
        for (int j = 0; j < (int)(0.05 * SR); j++) m.process(makeArgs(frame++));
        Buf x((size_t)(2.0 * SR));
        int period = (int)(SR / 2);
        for (size_t j = 0; j < x.size(); j++) {
            m.inputs[Tundo::TRIG_INPUT].setVoltage((j % period) < 50 ? 5.f : 0.f);
            m.process(makeArgs(frame++));
            x[j] = m.outputs[Tundo::AUDIO_OUTPUT].getVoltage();
        }
        std::string path = std::string(dir) + "/tundo_" + q.name + ".wav";
        writeWav(path.c_str(), x);
    }
}

int main(int argc, char** argv) {
    rack::random::init();
    const char* wavDir = nullptr;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavDir = argv[++i];

    if (wavDir) {
        dumpWavs(wavDir);
        return 0;
    }
    probeLevels();
    probeHarm();
    probeSpread();
    probeFold();
    probePitch();
    probeModes();
    probeRateModes();
    probeCpu();
    return 0;
}
