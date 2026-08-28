// materiae_probe - measurement and audition harness for the square-relation
// percussion voice. It drives materiae_dsp.hpp directly and needs no Rack.
//
//   ./materiae_probe ops      per-operator spectrum, DC and pairwise distance
//   ./materiae_probe grid     alias content vs. the GRID control
//   ./materiae_probe xmod     stability and level across the feedback range
//   ./materiae_probe ratio    period structure vs. the ratio table
//   ./materiae_probe render [dir]   WAVs of the target sound families
//
// The `ops` distance matrix is the one that answers the design question: an
// operator set is only worth its knob positions if no two of them land in the
// same place.

#include "../src/materiae_dsp.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
#include <string>
#include <vector>

using namespace materiae_dsp;

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

// magnitude spectrum of the first kFFT samples, Hann windowed, normalized
static std::vector<float> spectrum(const std::vector<float>& x) {
    std::vector<std::complex<float>> a(kFFT, std::complex<float>(0.f, 0.f));
    for (int i = 0; i < kFFT && i < (int)x.size(); i++) {
        float w = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * i / (kFFT - 1));
        a[i] = std::complex<float>(x[i] * w, 0.f);
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
        float f = (float)i * SR / (float)kFFT;
        num += f * m[i]; den += m[i];
    }
    return num / den;
}

// energy above 8 kHz, as a fraction: with naive squares on a coarse grid this
// is where the fold-down lives
static float highFrac(const std::vector<float>& m) {
    float hi = 0.f, all = 1e-12f;
    for (int i = 0; i < (int)m.size(); i++) {
        float f = (float)i * SR / (float)kFFT;
        all += m[i];
        if (f > 8000.f) hi += m[i];
    }
    return hi / all;
}

// L1 distance between two normalized spectra, 0 = identical, 2 = disjoint
static float specDist(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.f;
    for (size_t i = 0; i < a.size(); i++) d += std::fabs(a[i] - b[i]);
    return d;
}

struct Stats {
    float rms = 0.f, peak = 0.f, dc = 0.f, decayMs = 0.f;
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
    s.rms = (float)std::sqrt(acc / std::max<size_t>(1, x.size()));
    s.dc = (float)(mean / std::max<size_t>(1, x.size()));
    // time from the peak down to -60 dB of it, held for 5 ms
    float thr = s.peak * 0.001f;
    int hold = (int)(0.005f * SR), run = 0, last = 0;
    for (int i = 0; i < (int)x.size(); i++) {
        if (std::fabs(x[i]) > thr) { run = 0; last = i; }
        else if (++run > hold) break;
    }
    s.decayMs = (float)last * 1000.f / SR;
    return s;
}

// -------------------------------------------------------------------- render

struct Setting {
    const char* name;
    Params p;
    float e2Pitch, e2Rel, e2Cut;
    bool trackCut;
};

static Params base() {
    Params p;
    p.f0 = 110.f; p.ratio = 1.5f; p.shape = 0.5f;
    p.xmod = 0.f; p.tilt = 0.f; p.divShift = 0; p.modDest = 0;
    p.gridRate = SR * kGridMaxMult;
    p.relation = 1.f; p.blend = 1.f;
    p.cutoff = 2000.f; p.reso = 0.f; p.filterMode = 0;
    p.attack = 0.0005f; p.decay = 0.3f; p.curve = 0.5f;
    p.decay2 = 0.06f; p.curve2 = 0.6f;
    return p;
}

static std::vector<float> run(const Params& p, float seconds,
                              float e2Pitch = 0.f, float e2Rel = 0.f,
                              float e2Cut = 0.f, bool trackCut = false,
                              bool freeRun = false) {
    Engine e;
    e.setSampleRate(SR);
    e.reset();
    e.freeRun = freeRun;
    e.trackCutoff = trackCut;
    e.trigger(p, 1.f);
    int n = (int)(seconds * SR);
    std::vector<float> out(n);
    float dt = 1.f / SR, a, drone, e2;
    for (int i = 0; i < n; i++) {
        e.process(p, dt, a, drone, e2, e2Pitch, e2Rel, e2Cut, 0.f);
        out[i] = a;
    }
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
    for (uint32_t i = 0; i < n; i++) {
        float v = clampf(x[i], -1.f, 1.f);
        u16((uint16_t)(int16_t)std::lrint(v * 30000.f));
    }
    fclose(f);
}

// the seven families the design brief asks the module to cover
static std::vector<Setting> families() {
    std::vector<Setting> v;
    Params p;

    p = base();  // kick: the filter is the body, the square is the transient
    p.f0 = 48.f; p.ratio = 1.f; p.relation = 1.f; p.blend = 0.35f;
    p.cutoff = 62.f; p.reso = 0.93f; p.decay = 0.55f; p.curve = 0.75f;
    p.decay2 = 0.045f; p.curve2 = 0.9f;
    v.push_back({"kick", p, -1.4f, 0.f, 0.35f, false});

    p = base();  // tom
    p.f0 = 120.f; p.ratio = 1.5f; p.relation = 1.f; p.blend = 0.5f;
    p.cutoff = 420.f; p.reso = 0.72f; p.decay = 0.45f; p.curve = 0.6f;
    p.decay2 = 0.12f; p.curve2 = 0.5f;
    v.push_back({"tom", p, -0.8f, 0.f, 0.2f, false});

    p = base();  // metal: irrational ratio into ring
    p.f0 = 620.f; p.ratio = 3.14159265f; p.relation = 2.f; p.blend = 1.f;
    p.xmod = 0.35f; p.tilt = -0.4f;
    p.cutoff = 3800.f; p.reso = 0.55f; p.filterMode = 1;
    p.decay = 1.1f; p.curve = 0.8f; p.decay2 = 0.2f;
    v.push_back({"metal", p, 0.f, 0.6f, 0.f, false});

    p = base();  // digital: sparse AND on a coarse grid, divided
    p.f0 = 260.f; p.ratio = 1.41421356f; p.relation = 0.f; p.blend = 1.f;
    p.divShift = 2; p.xmod = 0.5f; p.gridRate = 6200.f;
    p.cutoff = 5200.f; p.reso = 0.4f; p.decay = 0.18f; p.curve = 0.4f;
    v.push_back({"digital", p, 0.5f, 0.f, 0.f, false});

    p = base();  // click
    p.f0 = 2400.f; p.ratio = 1.75f; p.relation = 2.f; p.blend = 1.f;
    p.gridRate = 21000.f;
    p.cutoff = 7000.f; p.reso = 0.3f;
    p.attack = 0.0002f; p.decay = 0.012f; p.curve = 0.9f;
    v.push_back({"click", p, 0.f, 0.f, 0.f, false});

    p = base();  // cymbal: shift register pseudo-noise
    p.f0 = 1500.f; p.ratio = 2.71828183f; p.relation = 4.f; p.blend = 1.f;
    p.xmod = 0.6f; p.tilt = 0.3f;
    p.cutoff = 6500.f; p.reso = 0.25f; p.filterMode = 1;
    p.decay = 0.9f; p.curve = 0.85f; p.decay2 = 0.3f;
    v.push_back({"cymbal", p, 0.f, 0.f, 0.5f, false});

    p = base();  // strange: asymmetric feedback, operator swept by env 2
    p.f0 = 74.f; p.ratio = 1.41421356f; p.shape = 0.78f;
    p.relation = 0.4f; p.blend = 0.9f;
    p.xmod = 0.85f; p.tilt = -0.7f; p.divShift = 1; p.modDest = 2;
    p.gridRate = 11000.f;
    p.cutoff = 900.f; p.reso = 0.8f;
    p.decay = 1.4f; p.curve = 0.2f; p.decay2 = 0.55f; p.curve2 = -0.3f;
    v.push_back({"strange", p, -0.5f, 3.2f, 0.6f, false});

    return v;
}

// --------------------------------------------------------------------- modes

static int modeOps() {
    const char* names[kNumOps] = {"and", "sum", "ring", "flip", "noise"};
    std::vector<std::vector<float>> spec;
    printf("op,rms,peak,dc,centroid_hz,high_frac\n");
    for (int i = 0; i < kNumOps; i++) {
        Params p = base();
        p.relation = (float)i;
        p.f0 = 220.f; p.ratio = 1.5f; p.blend = 1.f;
        p.cutoff = 12000.f; p.reso = 0.f; p.decay = 2.f; p.curve = 0.f;
        std::vector<float> x = run(p, 0.4f);
        Stats s = stats(x);
        std::vector<float> m = spectrum(x);
        spec.push_back(m);
        printf("%s,%.4f,%.4f,%+.5f,%.1f,%.4f\n", names[i], s.rms, s.peak, s.dc,
               centroid(m), highFrac(m));
        if (!s.finite) { printf("NON-FINITE in %s\n", names[i]); return 1; }
    }
    printf("\npairwise spectral L1 distance (0 = same sound)\n");
    printf("op");
    for (int j = 0; j < kNumOps; j++) printf(",%s", names[j]);
    printf("\n");
    float worst = 9.f;
    for (int i = 0; i < kNumOps; i++) {
        printf("%s", names[i]);
        for (int j = 0; j < kNumOps; j++) {
            float d = specDist(spec[i], spec[j]);
            printf(",%.3f", d);
            if (i != j) worst = std::min(worst, d);
        }
        printf("\n");
    }
    printf("\nclosest pair: %.3f (want >> 0)\n", worst);

    // the identity the operator set is built on
    printf("\nidentity check: ring == -(xor), for bipolar squares\n");
    double err = 0.0;
    for (int i = 0; i < 10000; i++) {
        float a = (i * 7919 % 2) ? 1.f : -1.f;
        float b = (i * 104729 % 3) ? 1.f : -1.f;
        float ring = a * b;
        float x = ((a > 0.f) != (b > 0.f)) ? 1.f : -1.f;   // xor, bipolar
        err += std::fabs(ring + x);
    }
    printf("max abs(ring + xor) over 10000 states: %.1f\n", err);
    return worst > 0.05f ? 0 : 1;
}

// energy that does not sit on a harmonic of f0. A square wave quantized onto
// a coarse time grid folds its edges into partials that are not multiples of
// the fundamental, and that -- not brightness -- is what a listener hears as
// "digital". Centroid cannot see it: the fold-down lands both above and below.
static float inharmFrac(const std::vector<float>& m, float f0) {
    float off = 0.f, all = 1e-12f;
    for (int i = 1; i < (int)m.size(); i++) {
        float f = (float)i * SR / (float)kFFT;
        all += m[i];
        float h = f / f0;
        float d = std::fabs(h - std::floor(h + 0.5f));
        if (d > 0.12f) off += m[i];       // > ~12% of the way between partials
    }
    return off / all;
}

static int modeGrid() {
    // sweep the knob, through the module's own mapping, rather than a list of
    // rates: what matters is how the change is distributed over the travel
    const int kSteps = 16;
    const float f0 = 220.f;
    std::vector<float> ref, prev;
    printf("knob,grid_hz,rms,peak,centroid_hz,inharm_frac,dist_from_clean,step\n");
    for (int i = 0; i <= kSteps; i++) {
        float k = (float)i / (float)kSteps;
        float top = std::log2(SR * kGridMaxMult), bot = std::log2(kGridMin);
        float g = std::pow(2.f, top + (bot - top) * k);
        Params p = base();
        p.gridRate = g;
        p.f0 = f0; p.ratio = 1.5f; p.relation = 2.f; p.blend = 1.f;
        p.cutoff = 12000.f; p.decay = 2.f; p.curve = 0.f;
        std::vector<float> x = run(p, 0.4f);
        Stats s = stats(x);
        std::vector<float> m = spectrum(x);
        if (ref.empty()) ref = m;
        float step = prev.empty() ? 0.f : specDist(m, prev);
        prev = m;
        printf("%.3f,%.0f,%.4f,%.4f,%.1f,%.4f,%.3f,%.3f\n", k, g, s.rms,
               s.peak, centroid(m), inharmFrac(m, f0), specDist(m, ref), step);
        if (!s.finite) { printf("NON-FINITE at grid %.0f\n", g); return 1; }
    }

    // the corner the range change opens up: the highest pitch on the coarsest
    // grid, where dt runs to tens of cycles a step
    printf("\npitch extremes at the bottom of the knob\n");
    printf("f0_hz,rms,peak,finite\n");
    const float pitches[] = {20.f, 110.f, 880.f, 4000.f, 12000.f};
    int bad = 0;
    for (float f : pitches) {
        Params p = base();
        p.gridRate = kGridMin;
        p.f0 = f; p.ratio = 4.f; p.relation = 2.f; p.blend = 1.f;
        p.xmod = 1.f; p.cutoff = 12000.f; p.reso = 0.8f; p.decay = 1.f;
        std::vector<float> x = run(p, 0.5f);
        Stats s = stats(x);
        printf("%.0f,%.4f,%.4f,%d\n", f, s.rms, s.peak, s.finite ? 1 : 0);
        if (!s.finite || s.peak > 1.01f) bad++;
    }
    printf("\nunstable corners: %d (want 0)\n", bad);
    return bad ? 1 : 0;
}

// The whole feedback range, both destinations, every division: nothing here
// may go non-finite or push past the output swing.
static int modeXmod() {
    printf("xmod,tilt,dest,div,rms,peak,centroid_hz,finite\n");
    int bad = 0;
    const int dests[] = {0, 1, 2};
    for (float xm = 0.f; xm <= 1.001f; xm += 0.25f) {
        for (float tilt = -1.f; tilt <= 1.001f; tilt += 1.f) {
            for (int d : dests) {
                for (int sh = 0; sh < kNumDiv; sh += 2) {
                    Params p = base();
                    p.xmod = xm; p.tilt = tilt; p.modDest = d; p.divShift = sh;
                    p.f0 = 180.f; p.ratio = 1.41421356f; p.relation = 3.f;
                    p.cutoff = 9000.f; p.reso = 0.6f; p.decay = 1.f;
                    std::vector<float> x = run(p, 0.6f);
                    Stats s = stats(x);
                    std::vector<float> m = spectrum(x);
                    printf("%.2f,%+.0f,%d,%d,%.4f,%.4f,%.1f,%d\n", xm, tilt, d,
                           1 << sh, s.rms, s.peak, centroid(m), s.finite ? 1 : 0);
                    if (!s.finite || s.peak > 1.01f) bad++;
                }
            }
        }
    }
    printf("\nunstable settings: %d (want 0)\n", bad);
    return bad ? 1 : 0;
}

// How much does DIV actually change the sound, and under what conditions?
// It only reaches the cross-modulation cells, so at XMOD 0 it can do nothing
// at all; this prints the spectral distance from /1 across the cases.
static int modeDiv() {
    const char* opName[kNumOps] = {"and", "sum", "ring", "flip", "noise"};
    const float xmods[] = {0.f, 0.35f, 0.7f, 1.f};
    printf("op,xmod,div,rms,centroid_hz,dist_from_div1\n");
    float best = 0.f;
    for (int op = 0; op < kNumOps; op++) {
        for (float xm : xmods) {
            std::vector<float> ref;
            for (int sh = 0; sh < kNumDiv; sh++) {
                Params p = base();
                p.relation = (float)op;
                p.xmod = xm; p.divShift = sh;
                p.f0 = 180.f; p.ratio = 1.5f; p.blend = 1.f;
                p.cutoff = 12000.f; p.reso = 0.f; p.decay = 2.f; p.curve = 0.f;
                std::vector<float> x = run(p, 0.4f);
                Stats st = stats(x);
                std::vector<float> m = spectrum(x);
                if (sh == 0) ref = m;
                float d = specDist(m, ref);
                if (sh > 0) best = std::max(best, d);
                printf("%s,%.2f,%d,%.4f,%.1f,%.3f\n", opName[op], xm, 1 << sh,
                       st.rms, centroid(m), d);
            }
        }
    }
    printf("\nlargest distance any division reaches: %.3f\n", best);
    return 0;
}

// Is the output saturator aliasing? Drive a pure sine into it and measure the
// energy that lands on neither the fundamental nor any of its harmonics. A
// memoryless nonlinearity at the host rate folds everything it makes above
// Nyquist back down, and that fold is inharmonic -- which is what turns a
// saturator into a source of clicks and grit rather than warmth.
static int modeSat() {
    const float f0 = 2350.f;      // not a divisor of SR, so folds are obvious
    printf("gain_db,rms,alias_frac,worst_slew\n");
    for (int g = 0; g <= 8; g++) {
        float gain = std::pow(2.f, (g / 8.f) * 4.f);
        std::vector<float> x(kFFT);
        SoftClipper sc;
        sc.reset();
        float prev = 0.f, slew = 0.f;
        for (int i = 0; i < kFFT; i++) {
            float in = 0.7f * std::sin(2.f * 3.14159265f * f0 * i / SR);
            x[i] = sc.process(in * gain);
            if (i) slew = std::max(slew, std::fabs(x[i] - prev));
            prev = x[i];
        }
        std::vector<float> m = spectrum(x);
        float alias = 0.f, all = 1e-12f;
        for (int i = 1; i < (int)m.size(); i++) {
            float f = (float)i * SR / (float)kFFT;
            all += m[i];
            float h = f / f0;
            if (std::fabs(h - std::floor(h + 0.5f)) > 0.08f) alias += m[i];
        }
        Stats st = stats(x);
        printf("%.1f,%.4f,%.5f,%.4f\n", (g / 8.f) * 4.f * 6.0206f, st.rms,
               alias / all, slew);
    }
    return 0;
}

static int modeRatio() {
    printf("ratio,rms,peak,centroid_hz,high_frac\n");
    for (int i = 0; i < kNumRatios; i++) {
        Params p = base();
        p.ratio = kRatios[i];
        p.f0 = 200.f; p.relation = 0.f; p.blend = 1.f;
        p.cutoff = 12000.f; p.decay = 2.f; p.curve = 0.f;
        std::vector<float> x = run(p, 0.4f);
        Stats s = stats(x);
        std::vector<float> m = spectrum(x);
        printf("%.5f,%.4f,%.4f,%.1f,%.4f\n", kRatios[i], s.rms, s.peak,
               centroid(m), highFrac(m));
    }
    return 0;
}

static int modeRender(const char* dir) {
    std::string d = dir ? dir : ".";
    printf("family,rms,peak,dc,decay_ms,centroid_hz,file\n");
    int bad = 0;
    for (const Setting& s : families()) {
        std::vector<float> x = run(s.p, 2.5f, s.e2Pitch, s.e2Rel, s.e2Cut,
                                   s.trackCut);
        Stats st = stats(x);
        std::vector<float> m = spectrum(x);
        std::string path = d + "/materiae_" + s.name + ".wav";
        writeWav(path, x);
        printf("%s,%.4f,%.4f,%+.5f,%.0f,%.1f,%s\n", s.name, st.rms, st.peak,
               st.dc, st.decayMs, centroid(m), path.c_str());
        if (!st.finite) bad++;
    }
    printf("\nnon-finite renders: %d\n", bad);
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "ops";
    if (!strcmp(mode, "ops")) return modeOps();
    if (!strcmp(mode, "grid")) return modeGrid();
    if (!strcmp(mode, "xmod")) return modeXmod();
    if (!strcmp(mode, "div")) return modeDiv();
    if (!strcmp(mode, "sat")) return modeSat();
    if (!strcmp(mode, "ratio")) return modeRatio();
    if (!strcmp(mode, "render")) return modeRender(argc > 2 ? argv[2] : ".");
    fprintf(stderr, "usage: %s ops|grid|xmod|div|sat|ratio|render [dir]\n", argv[0]);
    return 2;
}
