// olim_probe - measurement and audition harness for the olim engine. It
// drives src/olim/olim.hpp directly and needs no Rack.
//
//   ./olim_probe heads         head positions across SPREAD and TIME, free and
//                              clocked, and the FEEDBACK knob's law; checks
//                              that the last head is TIME, noon is even and
//                              the clocked steps are powers of two
//   ./olim_probe loop          level per pass against FEEDBACK, one head and
//                              all eight: the arc must hold, below it decays,
//                              above it stays under full scale
//   ./olim_probe clicks        the #22 measure (max second difference over
//                              peak, 220 Hz sine) under TIME, SPREAD and CV
//                              sweeps; clean ~0.001, fails above 0.01
//   ./olim_probe cpu           ns per sample, all heads up
//   ./olim_probe wav <dir>     stereo scenes to listen to
//
// `heads`, `loop` and `clicks` exit nonzero on a failed check.

#include "../src/olim/olim.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace olim;

static const float SR = 48000.f;
static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

// An engine with its own 150 s of memory.
struct Rig {
    Engine e;
    std::vector<float> l, r;
    Controls c;
    explicit Rig(float seconds = 150.f, float sr = SR) {
        e.setSampleRate(sr);
        size_t n = Engine::bufferSamples(seconds, sr);
        l.assign(n, 0.f);
        r.assign(n, 0.f);
        e.attach(l.data(), r.data(), n);
        c.dry = 0.f;
    }
    void run(float in, float& ol, float& orr, bool clk = false) {
        e.process(c, clk, in, in, ol, orr);
    }
};

static float dB(float x) { return 20.f * std::log10(std::max(x, 1e-9f)); }

// ------------------------------------------------------------------ heads

static int cmdHeads() {
    printf("SPREAD knob -> head positions, fraction of TIME\n");
    const float knobs[] = {0.f, 0.2f, 0.405f, 0.5f, 0.595f, 0.8f, 1.f};
    for (float k : knobs) {
        float s = spreadAmount(k, 0.f);
        printf("  %.3f (s %.3f):", k, s);
        for (int i = 1; i <= kHeads; i++) printf(" %.3f", spreadWeight(i / 8.f, s));
        printf("\n");
        check(std::fabs(spreadWeight(1.f, s) - 1.f) < 1e-6f, "last head is TIME");
        if (k >= kArcLow && k <= kArcHigh)
            for (int i = 1; i <= kHeads; i++)
                check(std::fabs(spreadWeight(i / 8.f, s) - i / 8.f) < 1e-6f,
                      "even spacing across the noon flat");
    }
    for (int i = 1; i < kHeads; i++) {
        check(spreadWeight(i / 8.f, 0.f) < i / 8.f, "left crowds toward now");
        check(spreadWeight(i / 8.f, 1.f) > i / 8.f, "right crowds toward TIME");
    }

    printf("\nTIME knob, free: seconds (CV 0, +1, -1 V)\n");
    for (float k = 0.f; k <= 1.001f; k += 0.125f)
        printf("  %.3f  %8.4f %8.4f %8.4f\n", k, freeTime(k, 0.f),
               freeTime(k, 1.f), freeTime(k, -1.f));
    check(std::fabs(freeTime(1.f, 0.f) - 8.f) < 1e-4f, "knob top is 8 s");
    check(std::fabs(freeTime(0.5f, 1.f) * 2.f - freeTime(0.5f, 0.f)) < 1e-5f,
          "+1 V halves TIME");

    printf("\nTIME, clocked at 0.5 s: seconds (knob across, CV 0 / +1 V)\n");
    for (float k = 0.f; k <= 1.001f; k += 1.f / 12.f) {
        float t[2];
        for (int j = 0; j < 2; j++) {
            Rig g;
            g.c.time = k;
            g.c.timeCv = j ? 1.f : 0.f;
            float a, b;
            for (int n = 0; n < (int)(2.2f * SR); n++)
                g.run(0.f, a, b, (n % (int)(0.5f * SR)) < 100);
            t[j] = g.e.timeSec;
            float ratio = std::log2(t[j] / 0.5f);
            check(std::fabs(ratio - std::round(ratio)) < 1e-3f, "clocked TIME is a power of two");
        }
        printf("  %.3f  %9.5f %9.5f  (2^%+.0f)\n", k, t[0], t[1], std::log2(t[0] / 0.5f));
        check(std::fabs(std::log2(t[0] / t[1]) - 2.f) < 1e-3f, "+1 V is two steps clocked");
    }

    printf("\nclock gone after 2 s\n");
    {
        Rig g;
        g.c.time = 0.5f;
        float a, b;
        for (int n = 0; n < (int)(2.f * SR); n++) g.run(0.f, a, b, (n % 12000) < 100);
        bool on = g.e.clocked();
        for (int n = 0; n < (int)(2.1f * SR); n++) g.run(0.f, a, b, false);
        printf("  clocked %d, then %d; TIME %.4f s (free %.4f)\n", on, g.e.clocked(),
               g.e.timeSec, freeTime(0.5f, 0.f));
        check(on && !g.e.clocked(), "clock arrives and goes");
        check(std::fabs(g.e.timeSec - freeTime(0.5f, 0.f)) < 1e-4f, "free TIME after the clock");
    }

    printf("\nFEEDBACK knob -> loop gain\n");
    for (float k = 0.f; k <= 1.001f; k += 0.05f)
        printf("  %.2f  %.3f%s\n", k, feedbackGain(k, 0.f),
               (k >= kArcLow && k <= kArcHigh) ? "  arc" : "");
    check(feedbackGain(kArcLow, 0.f) == 1.f && feedbackGain(kArcHigh, 0.f) == 1.f,
          "arc is exactly 1");
    check(feedbackGain(1.f, 5.f) == kFeedbackMax, "CV reaches the ceiling");

    printf("\n%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// ------------------------------------------------------------------- loop

// A 20 ms noise burst at 1 V rms, then silence; RMS in windows of TIME.
static std::vector<float> passes(float fbKnob, bool allHeads, int count, float* peak) {
    Rig g;
    const float T = 0.25f;
    g.c.time = std::sqrt(T / kTimeMax);
    g.c.feedback = fbKnob;
    for (int i = 0; i < kHeads; i++) g.c.gain[i] = allHeads || i == kHeads - 1 ? 1.f : 0.f;
    Rng rng(7);
    const int win = (int)(T * SR);
    std::vector<float> out;
    float pk = 0.f;
    // let the heads reach their places before the burst
    float a, b;
    for (int n = 0; n < (int)(0.5f * SR); n++) g.run(0.f, a, b);
    for (int p = 0; p <= count; p++) {
        double acc = 0.0;
        for (int n = 0; n < win; n++) {
            float in = (p == 0 && n < (int)(0.02f * SR)) ? 1.732f * rng.uniform() : 0.f;
            g.run(in, a, b);
            if (p > 0) { acc += (double)a * a; pk = std::max(pk, std::fabs(a)); }
        }
        if (p > 0) out.push_back((float)std::sqrt(acc / win));
    }
    if (peak) *peak = pk;
    return out;
}

static int cmdLoop() {
    const float knobs[] = {0.25f, 0.405f, 0.5f, 0.595f, 0.7f, 0.85f, 1.f};
    for (int all = 0; all < 2; all++) {
        printf("%s, TIME 0.25 s: dB per pass relative to pass 1\n",
               all ? "eight heads" : "last head only");
        printf("  knob  gain   p2     p5     p10    p20    p40    peak V\n");
        for (float k : knobs) {
            float pk;
            std::vector<float> v = passes(k, all, 40, &pk);
            float ref = std::max(v[0], 1e-9f);
            printf("  %.3f %.2f", k, feedbackGain(k, 0.f));
            const int at[] = {1, 4, 9, 19, 39};
            for (int i : at) printf(" %6.1f", dB(v[i] / ref));
            printf("  %6.2f\n", pk);
            float drift = dB(v[39] / ref);
            if (k >= kArcLow && k <= kArcHigh && !all)
                check(std::fabs(drift) < 3.f, "the arc holds one head within 3 dB over 40 passes");
            if (k < kArcLow) check(drift < -20.f, "below the arc decays");
            check(pk <= kFullScale * 1.0001f, "output stays under full scale");
        }
        printf("\n");
    }
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// ----------------------------------------------------------------- clicks

// max |x[n] - 2x[n-1] + x[n-2]| over the peak
static float roughness(const std::vector<float>& x) {
    float d2 = 0.f, pk = 1e-9f;
    for (size_t n = 2; n < x.size(); n++) {
        d2 = std::max(d2, std::fabs(x[n] - 2.f * x[n - 1] + x[n - 2]));
        pk = std::max(pk, std::fabs(x[n]));
    }
    return d2 / pk;
}

typedef void (*Mover)(Controls&, float t);   // t = seconds

static float clickRun(Mover move, float seconds, bool allHeads) {
    Rig g;
    g.c.time = 0.4f;
    // Eight heads at a quarter each on a 1.25 V sine peak at 2.5 V, and the
    // compressor's key at 0.75 of full scale: nothing limits, so what is
    // measured is the heads and not the limiter doing its job.
    for (int i = 0; i < kHeads; i++) g.c.gain[i] = allHeads ? 0.25f : i == kHeads - 1 ? 1.f : 0.f;
    std::vector<float> out;
    float a, b;
    // The sine has to be in the whole buffer before anything is measured, or
    // each head's first echo starts abruptly and reads as a click. 45 s is
    // past the longest TIME any case reaches (0.8 knob at -5 V).
    const int pre = (int)(45.f * SR);
    move(g.c, 0.f);
    for (int n = 0; n < pre + (int)(seconds * SR); n++) {
        // in double: 45 s back, a float's time jitters the phase audibly
        double t = (n - pre) / (double)SR;
        if (n >= pre) move(g.c, (float)t);
        g.run((float)(1.25 * std::sin(2.0 * M_PI * 220.0 * t)), a, b);
        if (n >= pre) out.push_back(a);
    }
    return roughness(out);
}

static int cmdClicks() {
    struct Case { const char* name; Mover m; bool all; };
    const Case cases[] = {
        {"still",               [](Controls&, float) {}, true},
        {"TIME knob sweep",     [](Controls& c, float t) { c.time = 0.2f + 0.6f * std::fmod(t / 4.f, 1.f); }, true},
        {"TIME knob, one head", [](Controls& c, float t) { c.time = 0.2f + 0.6f * std::fmod(t / 4.f, 1.f); }, false},
        {"TIME CV 0.3 Hz +-5V", [](Controls& c, float t) { c.timeCv = 5.f * std::sin(2.f * 3.14159265f * 0.3f * t); }, true},
        {"SPREAD knob sweep",   [](Controls& c, float t) { c.spread = std::fmod(t / 3.f, 1.f); }, true},
        {"SPREAD CV 2 Hz",      [](Controls& c, float t) { c.spreadCv = 5.f * std::sin(2.f * 3.14159265f * 2.f * t); }, true},
        {"sliders stepping",    [](Controls& c, float t) { for (int i = 0; i < kHeads; i++) c.gain[i] = 0.25f * (((int)(t * 7.f) + i) % 2); }, true},
        {"TIME jump to zero",   [](Controls& c, float t) { c.time = t > 2.f ? 0.f : 0.4f; }, false},
    };
    printf("roughness (max 2nd difference / peak), clean ~0.001, limit 0.01\n");
    for (const Case& k : cases) {
        float r = clickRun(k.m, 8.f, k.all);
        printf("  %-22s %.4f%s\n", k.name, r, r > 0.01f ? "  CLICK" : "");
        check(r <= 0.01f, k.name);
    }
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// -------------------------------------------------------------------- cpu

static int cmdCpu() {
    Rig g;
    g.c.feedback = 0.9f;
    for (int i = 0; i < kHeads; i++) g.c.gain[i] = 1.f;
    const int n = (int)(10.f * SR);
    Rng rng(3);
    float a, b, sink = 0.f;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) {
        g.run(0.5f * rng.uniform(), a, b);
        sink += a;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    printf("%.1f ns per sample (%.2f%% of one core at 48 kHz)  [%g]\n",
           ns, ns * SR * 1e-7, sink * 0.f);
    return 0;
}

// -------------------------------------------------------------------- wav

static void writeWav(const std::string& path, const std::vector<float>& l,
                     const std::vector<float>& r) {
    const uint32_t n = (uint32_t)l.size();
    const uint32_t rate = (uint32_t)SR;
    const uint32_t dataBytes = n * 4;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2);
    u32(rate); u32(rate * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(dataBytes);
    for (uint32_t i = 0; i < n; i++) {
        for (float v : {l[i], r[i]}) {
            v = std::min(std::max(v / kFullScale, -1.f), 1.f);
            u16((uint16_t)(int16_t)std::lrint(v * 32767.f));
        }
    }
    fclose(f);
}

// Plucks: decaying sines at random pitches from a pentatonic set, every
// 0.7 s for the first `playFor` seconds.
struct Plucks {
    Rng rng{11};
    float phase = 0.f, freq = 220.f, env = 0.f;
    int n = 0;
    float next(float playFor) {
        if (n % (int)(0.7f * SR) == 0 && n < playFor * SR) {
            const float st[] = {0, 3, 5, 7, 10, 12, 15};
            freq = 220.f * std::exp2(st[(int)((rng.uniform() * 0.5f + 0.5f) * 6.99f)] / 12.f);
            env = 1.f;
        }
        n++;
        env *= 0.99985f;
        phase += freq / SR;
        phase -= std::floor(phase);
        return 3.f * env * std::sin(2.f * 3.14159265f * phase);
    }
};

static int cmdWav(std::string dir) {
    if (!dir.empty() && dir.back() != '/') dir += '/';
    struct Scene {
        const char* name;
        float seconds, playFor, clockHz;
        void (*set)(Controls&, float t);
    };
    const Scene scenes[] = {
        {"even", 14, 5, 0, [](Controls& c, float) { c.time = 0.5f; c.dry = 1; }},
        {"now", 14, 5, 0, [](Controls& c, float) { c.time = 0.5f; c.spread = 0; c.dry = 1; }},
        {"then", 14, 5, 0, [](Controls& c, float) { c.time = 0.5f; c.spread = 1; c.dry = 1; }},
        {"sound-on-sound", 30, 8, 0, [](Controls& c, float) { c.time = 0.5f; c.feedback = 0.5f; c.dry = 1; }},
        {"howl", 30, 4, 0, [](Controls& c, float) { c.time = 0.3f; c.feedback = 0.9f; }},
        {"sweep", 20, 20, 0, [](Controls& c, float t) { c.time = 0.15f + 0.4f * (0.5f - 0.5f * std::cos(t * 0.6f)); c.feedback = 0.45f; }},
        {"comb", 12, 12, 0, [](Controls& c, float t) { c.time = 0.03f; c.timeCv = std::fmod(t, 4.f) - 2.f; c.feedback = 0.5f; for (int i = 0; i < kHeads; i++) c.gain[i] = i == 7; }},
        {"clocked", 16, 6, 2, [](Controls& c, float) { c.time = 0.6f; c.feedback = 0.35f; c.dry = 1; }},
    };
    for (const Scene& s : scenes) {
        Rig g;
        for (int i = 0; i < kHeads; i++) g.c.gain[i] = 0.7f;
        Plucks p;
        std::vector<float> l, r;
        const int period = s.clockHz > 0 ? (int)(SR / s.clockHz) : 0;
        for (int n = 0; n < (int)(s.seconds * SR); n++) {
            s.set(g.c, n / SR);
            float a, b;
            bool clk = period && (n % period) < 200;
            g.run(p.next(s.playFor), a, b, clk);
            l.push_back(a);
            r.push_back(b);
        }
        writeWav(dir + "olim_" + s.name + ".wav", l, r);
        printf("  %s\n", s.name);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "heads") return cmdHeads();
    if (cmd == "loop") return cmdLoop();
    if (cmd == "clicks") return cmdClicks();
    if (cmd == "cpu") return cmdCpu();
    if (cmd == "wav" && argc > 2) return cmdWav(argv[2]);
    fprintf(stderr, "usage: olim_probe heads | loop | clicks | cpu | wav <dir>\n");
    return 2;
}
