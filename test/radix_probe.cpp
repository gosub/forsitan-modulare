// radix_probe - measurement and audition harness for the radix engine. It
// drives src/radix/radix.hpp directly and needs no Rack.
//
//   ./radix_probe measure        every program: level, DC, centroid, motion,
//                                nearest neighbour; flags silent and stuck ones
//   ./radix_probe pitch          RATE's pitch against CLOCK, both menu settings
//   ./radix_probe param S L T    one program across the PARAM knob
//   ./radix_probe grit           what GRIT adds, in dB under the signal
//   ./radix_probe cpu            ns per host sample at the top of CLOCK
//   ./radix_probe wav <dir>      one WAV per program, PARAM swept 0..255
//   ./radix_probe tour <file>    all 150 programs in one WAV, a second each
//
// S L T are names or indices: `param self xor text`, `param 2 3 5`.
//
// `measure` is the one that answers the design question: 150 programs are
// only worth the three knobs if they do not collapse onto a handful of
// sounds, and a program that is silent or frozen at the defaults is a knob
// position that says nothing.

#include "../src/radix/radix.hpp"

#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace radix;

static const float SR = 48000.f;
static const int kFFT = 8192;

// ------------------------------------------------------------------ helpers

static void fft(std::vector<std::complex<float>>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.f * 3.14159265358979f / (float)len;
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (int k = 0; k < len / 2; k++) {
                std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// magnitude spectrum of kFFT samples from `start`, Hann windowed, DC removed,
// normalized to sum 1
static std::vector<float> spectrum(const std::vector<float>& x, int start = 0) {
    std::vector<std::complex<float>> a(kFFT, std::complex<float>(0.f, 0.f));
    double mean = 0.0;
    for (int i = 0; i < kFFT; i++) mean += x[start + i];
    mean /= kFFT;
    for (int i = 0; i < kFFT; i++) {
        float w = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * i / (kFFT - 1));
        a[i] = std::complex<float>((x[start + i] - (float)mean) * w, 0.f);
    }
    fft(a);
    std::vector<float> m(kFFT / 2);
    float sum = 1e-12f;
    for (int i = 0; i < kFFT / 2; i++) { m[i] = std::abs(a[i]); sum += m[i]; }
    for (int i = 0; i < kFFT / 2; i++) m[i] /= sum;
    return m;
}

static float centroid(const std::vector<float>& m) {
    float num = 0.f, den = 1e-12f;
    for (int i = 0; i < (int)m.size(); i++) {
        num += (float)i * SR / (float)kFFT * m[i];
        den += m[i];
    }
    return num / den;
}

// strongest bin, parabolically interpolated, in Hz
static float peakHz(const std::vector<float>& m) {
    int best = 1;
    for (int i = 2; i < (int)m.size() - 1; i++)
        if (m[i] > m[best]) best = i;
    float a = m[best - 1], b = m[best], c = m[best + 1];
    float d = (a - 2.f * b + c) != 0.f ? 0.5f * (a - c) / (a - 2.f * b + c) : 0.f;
    return ((float)best + d) * SR / (float)kFFT;
}

// L1 distance between two normalized spectra, 0 = identical, 2 = disjoint
static float specDist(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.f;
    for (size_t i = 0; i < a.size(); i++) d += std::fabs(a[i] - b[i]);
    return d;
}

struct Stats {
    float rms = 0.f, peak = 0.f, dc = 0.f;
    float motion = 0.f;   // spectral distance, first window against the last
    bool finite = true;
};

static Stats stats(const std::vector<float>& x) {
    Stats s;
    double acc = 0.0, mean = 0.0;
    for (float v : x) {
        if (!std::isfinite(v)) s.finite = false;
        acc += (double)v * v;
        mean += v;
        s.peak = std::max(s.peak, std::fabs(v));
    }
    s.dc = (float)(mean / x.size());
    s.rms = (float)std::sqrt(std::max(0.0, acc / x.size() - (double)s.dc * s.dc));
    s.motion = specDist(spectrum(x, 0), spectrum(x, (int)x.size() - kFFT));
    return s;
}

// ------------------------------------------------------------------- render

static Params base() {
    Params p;
    p.freq = 261.63f;
    p.clock = kRefClock;
    p.param = 64.f;
    p.bits = 16;
    p.grit = 0.f;
    return p;
}

// SRC = IN listens to a 110 Hz sine, the one outside signal radix is given
static std::vector<float> run(Params p, float seconds, bool sweepParam = false,
                              float* cvMean = nullptr) {
    Engine e;
    e.setSampleRate(SR);
    e.reset();
    int n = (int)(seconds * SR);
    std::vector<float> out(n);
    double cvSum = 0.0;
    for (int i = 0; i < n; i++) {
        if (sweepParam) p.param = 255.f * (float)i / (float)n;
        p.in = std::sin(6.2831853f * 110.f * (float)i / SR);
        float cv;
        out[i] = e.process(p, cv);
        cvSum += cv;
    }
    if (cvMean) *cvMean = (float)(cvSum / n);
    return out;
}

static void writeWav(const std::string& path, const std::vector<float>& x) {
    const uint32_t n = (uint32_t)x.size();
    const uint32_t rate = (uint32_t)SR;
    const uint32_t dataBytes = n * 2;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(rate); u32(rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(dataBytes);
    // -6 dB: the engine runs full scale, and a folder of these is for ears
    for (uint32_t i = 0; i < n; i++) {
        float v = std::min(std::max(x[i] * 0.5f, -1.f), 1.f);
        u16((uint16_t)(int16_t)std::lrint(v * 32767.f));
    }
    fclose(f);
}

static std::string progName(int s, int l, int t) {
    return std::string(srcName(s)) + "-" + lawName(l) + "-" + tableName(t);
}

static int parseIndex(const char* arg, int n, const char* (*name)(int)) {
    for (int i = 0; i < n; i++)
        if (!strcmp(arg, name(i))) return i;
    int v = atoi(arg);
    if (v < 0 || v >= n) { fprintf(stderr, "bad index %s\n", arg); exit(2); }
    return v;
}

// ----------------------------------------------------------------- commands

static int cmdMeasure() {
    struct Row { int s, l, t; Stats st; float cent, cv; std::vector<float> spec; };
    std::vector<Row> rows;
    for (int s = 0; s < NUM_SRC; s++)
        for (int l = 0; l < NUM_LAW; l++)
            for (int t = 0; t < NUM_TABLE; t++) {
                Params p = base();
                p.src = s; p.law = l; p.table = t;
                Row r{s, l, t, {}, 0.f, 0.f, {}};
                std::vector<float> x = run(p, 1.f, false, &r.cv);
                r.st = stats(x);
                r.spec = spectrum(x, (int)x.size() / 2);
                r.cent = centroid(r.spec);
                rows.push_back(r);
            }

    printf("%-20s %6s %6s %6s %7s %6s %5s  %-20s %5s  flags\n",
           "program", "rms", "dc", "peak", "cent", "motion", "cv",
           "nearest", "dist");
    int silent = 0, dupes = 0, bad = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        size_t near = i;
        float nd = 1e9f;
        for (size_t j = 0; j < rows.size(); j++) {
            if (j == i) continue;
            float d = specDist(r.spec, rows[j].spec);
            if (d < nd) { nd = d; near = j; }
        }
        std::string flags;
        if (!r.st.finite) { flags += " NONFINITE"; bad++; }
        if (r.st.rms < 0.01f) { flags += " SILENT"; silent++; }
        if (std::fabs(r.st.dc) > 0.25f) flags += " DC";
        if (nd < 0.05f) { flags += " DUPE"; dupes++; }
        const Row& o = rows[near];
        printf("%-20s %6.3f %+6.3f %6.3f %7.0f %6.3f %5.2f  %-20s %5.3f %s\n",
               progName(r.s, r.l, r.t).c_str(), r.st.rms, r.st.dc, r.st.peak,
               r.cent, r.st.motion, r.cv, progName(o.s, o.l, o.t).c_str(), nd,
               flags.c_str());
    }
    printf("\n%d programs, %d silent, %d near-duplicate (dist < 0.05), %d non-finite\n",
           (int)rows.size(), silent, dupes, bad);
    return bad ? 1 : 0;
}

static int cmdPitch() {
    const float clocks[] = {1000.f, 4000.f, 16000.f, 32000.f, 64000.f, 96000.f};
    const float freqs[] = {55.f, 261.63f, 1000.f};
    printf("SRC param, LAW add, PARAM 128, TABLE sine: the peak should be RATE\n");
    printf("unless the clock moves the pitch, where it is RATE * clock / %.0f\n\n",
           kRefClock);
    printf("%8s %8s %10s %10s\n", "rate", "clock", "measured", "clockMoves");
    for (float f : freqs)
        for (float c : clocks) {
            Params p = base();
            p.param = 128.f; p.freq = f; p.clock = c;
            float a = peakHz(spectrum(run(p, 0.5f), 4096));
            p.clockMovesPitch = true;
            float b = peakHz(spectrum(run(p, 0.5f), 4096));
            printf("%8.1f %8.0f %10.1f %10.1f\n", f, c, a, b);
        }
    return 0;
}

static int cmdParam(int s, int l, int t) {
    printf("%s across PARAM\n\n", progName(s, l, t).c_str());
    printf("%6s %6s %7s %7s %7s %6s\n", "param", "rms", "dc", "cent", "peak Hz", "motion");
    for (int v = 0; v <= 256; v += 16) {
        Params p = base();
        p.src = s; p.law = l; p.table = t;
        p.param = (float)std::min(v, 255);
        std::vector<float> x = run(p, 0.5f);
        Stats st = stats(x);
        std::vector<float> m = spectrum(x, (int)x.size() - kFFT);
        printf("%6d %6.3f %+7.3f %7.0f %7.1f %6.3f\n", (int)p.param, st.rms,
               st.dc, centroid(m), peakHz(m), st.motion);
    }
    return 0;
}

// GRIT against the clean program: how loud the corruption it adds is, in dB
// below the signal, and where it moves the centroid. The knob is only doing
// its job if every step of it is audible.
static int cmdGrit() {
    const int progs[][3] = {{SRC_PARAM, LAW_ADD, TAB_SINE}, {SRC_PARAM, LAW_ADD, TAB_SAW},
                            {SRC_SELF, LAW_XOR, TAB_TEXT}};
    for (auto& pr : progs) {
        printf("%s\n%6s %9s %7s\n", progName(pr[0], pr[1], pr[2]).c_str(),
               "grit", "added dB", "cent");
        Params p = base();
        p.src = pr[0]; p.law = pr[1]; p.table = pr[2];
        p.grit = 0.f;
        std::vector<float> clean = run(p, 0.5f);
        for (int g = 0; g <= 10; g++) {
            p.grit = g / 10.f;
            std::vector<float> x = run(p, 0.5f);
            double e = 0.0, s = 0.0;
            for (size_t i = 0; i < x.size(); i++) {
                e += (double)(x[i] - clean[i]) * (x[i] - clean[i]);
                s += (double)clean[i] * clean[i];
            }
            double db = e > 0.0 ? 10.0 * std::log10(e / s) : -999.0;
            printf("%6.1f %9.1f %7.0f\n", p.grit, db,
                   centroid(spectrum(x, (int)x.size() - kFFT)));
        }
        printf("\n");
    }
    return 0;
}

static int cmdCpu() {
    Params p = base();
    p.clock = kClockMax;
    p.src = SRC_SELF; p.law = LAW_XOR; p.table = TAB_TEXT; p.grit = 0.5f;
    Engine e;
    e.setSampleRate(SR);
    const int n = (int)SR * 10;
    float sink = 0.f, cv;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) sink += e.process(p, cv);
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    printf("%.1f ns per sample at a %.0f Hz clock (%.3f%% of one core at %.0f Hz)"
           " [%g]\n", ns, kClockMax, ns * SR * 1e-7, SR, sink * 0.f);
    return 0;
}

static int cmdWav(const std::string& dir) {
    for (int s = 0; s < NUM_SRC; s++)
        for (int l = 0; l < NUM_LAW; l++)
            for (int t = 0; t < NUM_TABLE; t++) {
                Params p = base();
                p.src = s; p.law = l; p.table = t;
                char name[64];
                snprintf(name, sizeof name, "/%d%d%d-", s, l, t);
                writeWav(dir + name + progName(s, l, t) + ".wav", run(p, 4.f, true));
            }
    printf("wrote %d files to %s\n", kNumPrograms, dir.c_str());
    return 0;
}

static int cmdTour(const std::string& path) {
    std::vector<float> all;
    for (int s = 0; s < NUM_SRC; s++)
        for (int l = 0; l < NUM_LAW; l++)
            for (int t = 0; t < NUM_TABLE; t++) {
                Params p = base();
                p.src = s; p.law = l; p.table = t;
                std::vector<float> x = run(p, 1.f);
                all.insert(all.end(), x.begin(), x.end());
            }
    writeWav(path, all);
    printf("wrote %s: programs in SRC, LAW, TABLE order, one second each\n",
           path.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "measure";
    if (cmd == "measure") return cmdMeasure();
    if (cmd == "pitch") return cmdPitch();
    if (cmd == "cpu") return cmdCpu();
    if (cmd == "grit") return cmdGrit();
    if (cmd == "param" && argc > 4)
        return cmdParam(parseIndex(argv[2], NUM_SRC, srcName),
                        parseIndex(argv[3], NUM_LAW, lawName),
                        parseIndex(argv[4], NUM_TABLE, tableName));
    if (cmd == "wav" && argc > 2) return cmdWav(argv[2]);
    if (cmd == "tour" && argc > 2) return cmdTour(argv[2]);
    fprintf(stderr, "usage: radix_probe measure | pitch | grit | cpu | param S L T | "
                    "wav <dir> | tour <file>\n");
    return 2;
}
