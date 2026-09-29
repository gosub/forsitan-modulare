// spira_probe - measurement harness for the spira engine. It drives
// src/spira/spira.hpp directly and needs no Rack.
//
//   ./spira_probe laws       each knob law and its inverse, round trips
//   ./spira_probe spiral     one circle's laps: each r times the last, the
//                            converging one ending at the sum of its laps,
//                            TAPE's speed per lap, TAPE 0 holding the speed,
//                            the speed limits, an unwinding one stopping at
//                            its longest lap, FADE per first-lap length
//   ./spira_probe clicks     a 220 Hz sine through circles, spirals, both
//                            directions, SHAPE, stealing and HOLD: the worst
//                            second difference against a clean sine's
//   ./spira_probe line       HOLD loops the last LINE seconds, never more
//                            than was written; circles born under HOLD read
//                            the held loop; REACH stays on the line; RATE
//   ./spira_probe fuzz       random controls and events: finite and bounded
//   ./spira_probe cpu        ns per sample with every circle sounding
//   ./spira_probe wav <in.wav> <dir>   scenes to listen to, from a file
//
// Every command but `cpu` and `wav` exits nonzero on a failed check.

#include "../src/spira/spira.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spira;

static float SR = 48000.f;
static int failures = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

struct Rig {
    Engine e;
    Buffers b;
    Controls c;
    Events ev;
    Output o;
    explicit Rig(float sr = SR) {
        e.setSampleRate(sr);
        e.rng.seed(12345);
        b.allocate(Engine::bufferSamples(sr));
        e.attach(b.l, b.r, b.n);
        c.mix = 1.f;       // circles only
        c.spread = 0.f;
    }
    Output tick(float l, float r) {
        o = e.process(c, ev, l, r);
        ev = Events();
        return o;
    }
    Output tick(float x) { return tick(x, x); }
};

struct Sine {
    double ph = 0.;
    float hz, amp;
    Sine(float f = 220.f, float a = 5.f) : hz(f), amp(a) {}
    float next() {
        float v = amp * (float)std::sin(2. * M_PI * ph);
        ph += hz / SR;
        if (ph >= 1.) ph -= 1.;
        return v;
    }
};

// ---------------------------------------------------------------- laws

static int cmdLaws() {
    printf("laws\n");
    float worst = 0.f;
    for (int i = 0; i <= 100; i++) {
        float k = i / 100.f;
        worst = std::max(worst, std::fabs(sizeKnob(sizeSeconds(k)) - k));
        worst = std::max(worst, std::fabs(lineKnob(lineSeconds(k)) - k));
        if (k >= kRateOff) worst = std::max(worst, std::fabs(rateKnob(rateHz(k)) - k));
        float s = 2.f * k - 1.f;
        worst = std::max(worst, std::fabs(spiralKnob(spiralRatio(s)) - s));
    }
    printf("  worst round trip %.2e\n", worst);
    check(worst < 1e-4f, "every law round-trips through its inverse");
    check(std::fabs(sizeSeconds(0.f) - kSizeMin) < 1e-6f && std::fabs(sizeSeconds(1.f) - kSizeMax) < 1e-5f,
          "SIZE spans 10 ms .. 4 s");
    check(rateHz(0.f) == 0.f && rateHz(kRateOff * 0.5f) == 0.f, "RATE is off at the bottom");
    check(std::fabs(rateHz(1.f) - kRateMax) < 1e-3f, "RATE reaches 20 per second");
    check(std::fabs(spiralRatio(0.f) - 1.f) < 1e-6f, "SPIRAL centre is a circle");
    check(std::fabs(spiralRatio(1.f) - 2.f) < 1e-5f && std::fabs(spiralRatio(-1.f) - 0.5f) < 1e-6f,
          "SPIRAL ends are x2 and x0.5");
    check(std::fabs(spiralRatio(0.5f) - std::exp2(0.25f)) < 1e-5f, "SPIRAL is square-law: 0.5 is x1.19");
    check(std::fabs(sizeKnob(0.25f) - sizeKnob(sizeSeconds(sizeKnob(0.25f)))) < 1e-6f
          && std::fabs(sizeSeconds(sizeKnob(0.25f)) - 0.25f) < 1e-5f, "typing 250 ms into SIZE is 250 ms");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- spiral

struct Laps {
    std::vector<long> start;     // TURN times, samples
    std::vector<float> speed;    // V/oct at each
    long end = -1;               // when the circle stopped sounding
};

// One circle born at 1 s into a steady sine, followed until it ends or
// `seconds` pass.
static Laps follow(Rig& g, float seconds) {
    Laps L;
    Sine s;
    long total = (long)(seconds * SR);
    for (long i = 0; i < total; i++) {
        if (i == (long)SR) g.ev.birth = true;
        Output o = g.tick(s.next());
        if (i >= (long)SR) {
            if (o.turn) {
                L.start.push_back(i);
                L.speed.push_back(o.speedOct);
            }
            if (o.circles == 0 && L.end < 0 && !L.start.empty()) L.end = i;
        }
    }
    return L;
}

static int cmdSpiral() {
    printf("spiral\n");
    {
        // converging, TAPE full: each lap r times the last, faster by 1/r
        Rig g;
        g.c.size = 0.4f;
        g.c.spiral = 0.8f;
        g.c.tape = 1.f;
        g.c.fadeDb = 0.f;
        Laps L = follow(g, 6.f);
        double T0 = 0.4 * SR;
        bool ratios = L.start.size() > 6;
        double worst = 0.;
        for (size_t k = 2; k + 1 < L.start.size() && k < 12; k++) {
            double r = (double)(L.start[k + 1] - L.start[k]) / (double)(L.start[k] - L.start[k - 1]);
            worst = std::max(worst, std::fabs(r - 0.8));
        }
        printf("  %zu laps, worst lap ratio error %.4f\n", L.start.size(), worst);
        check(ratios && worst < 0.01, "SPIRAL 0.8: every lap 0.8 of the last");
        double predicted = 0.;
        for (double T = T0; T >= kLapMin * SR; T *= 0.8) predicted += T;
        double took = (double)(L.end - L.start[0]);
        printf("  converged after %.3f s, sum of laps %.3f s, T0/(1-r) %.3f s\n",
               took / SR, predicted / SR, T0 / 0.2 / SR);
        check(L.end > 0 && std::fabs(took - predicted) < 0.01 * SR, "the circle ends at the sum of its laps");
        check(L.speed.size() > 4 && std::fabs(L.speed[3] - 3.f * (float)-std::log2(0.8)) < 0.01f,
              "TAPE 1: three laps in, 3 x log2(1/0.8) octaves up");
        float top = 0.f;
        for (float v : L.speed) top = std::max(top, v);
        check(std::fabs(top - std::log2(kSpeedMax)) < 0.01f, "the speed stops at two octaves up");
    }
    {
        // TAPE 0: the laps shrink by cutting, and the speed stays
        Rig g;
        g.c.size = 0.4f;
        g.c.spiral = 0.8f;
        g.c.tape = 0.f;
        g.c.fadeDb = 0.f;
        Laps L = follow(g, 6.f);
        float worst = 0.f;
        for (float v : L.speed) worst = std::max(worst, std::fabs(v));
        check(L.start.size() > 6 && worst < 1e-4f, "TAPE 0: the speed never moves");
    }
    {
        // PITCH sets the speed at birth; the lap length stays SIZE
        Rig g;
        g.c.size = 0.3f;
        g.c.pitch = -12.f;
        g.c.fadeDb = 0.f;
        Laps L = follow(g, 3.f);
        check(L.start.size() > 3 && std::fabs(L.speed[0] + 1.f) < 1e-4f, "PITCH -12: one octave down");
        check(L.start.size() > 3 && std::labs(L.start[2] - L.start[1] - (long)(0.3f * SR)) <= 1,
              "PITCH leaves the lap SIZE long");
    }
    {
        // unwinding: r = 2, laps double until the longest lap
        Rig g;
        g.c.size = 1.f;
        g.c.spiral = 2.f;
        g.c.tape = 1.f;
        g.c.fadeDb = 0.f;
        g.c.line = 30.f;
        g.c.hold = true;     // or the line overwrites the window before 32 s
        Laps L = follow(g, 40.f);
        std::vector<double> T;
        for (size_t k = 1; k < L.start.size(); k++) T.push_back((double)(L.start[k] - L.start[k - 1]) / SR);
        printf("  unwinding laps:");
        for (double t : T) printf(" %.2f", t);
        printf(" s\n");
        check(T.size() >= 4 && std::fabs(T[0] - 1.) < 0.01 && std::fabs(T[1] - 2.) < 0.01
              && std::fabs(T[2] - 4.) < 0.01, "SPIRAL x2: 1, 2, 4 s");
        check(T.size() >= 5 && std::fabs(T[4] - kLapMax) < 0.01, "and it stops growing at 16 s");
    }
    {
        // FADE: -6 dB per first-lap length, so a lap half as long fades 3 dB
        Rig g;
        g.c.size = 0.5f;
        g.c.spiral = 0.5f;
        g.c.tape = 0.f;
        g.c.fadeDb = -6.f;
        g.c.soft = 0.f;
        Sine s;
        std::vector<float> y;
        std::vector<long> turns;
        for (long i = 0; i < (long)(3 * SR); i++) {
            if (i == (long)SR) g.ev.birth = true;
            Output o = g.tick(s.next());
            if (o.turn) turns.push_back(i);
            y.push_back(o.l);
        }
        // RMS over the middle half of each lap, clear of both seams
        std::vector<float> peak;
        for (size_t k = 0; k + 1 < turns.size() && k < 4; k++) {
            long a = turns[k], b = turns[k + 1], q = (b - a) / 4;
            double e = 0.;
            for (long i = a + q; i < b - q; i++) e += (double)y[i] * y[i];
            peak.push_back((float)std::sqrt(e / (b - a - 2 * q)));
        }
        bool ok = peak.size() >= 4;
        float d1 = ok ? 20.f * std::log10(peak[1] / peak[0]) : 0.f;
        float d2 = ok ? 20.f * std::log10(peak[2] / peak[1]) : 0.f;
        printf("  lap levels %.2f, %.2f dB\n", d1, d2);
        check(ok && std::fabs(d1 + 6.f) < 0.3f && std::fabs(d2 + 3.f) < 0.3f,
              "FADE: -6 dB after the first lap, -3 after one half as long");
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- clicks

// The worst second difference of the output over its peak, against a clean
// sine's (2 pi f / sr)^2, about 0.0008 at 220 Hz. A click is a step, and a
// step is a second difference of the order of the step itself.
struct ClickMeter {
    double prev1 = 0., prev2 = 0., worst = 0., peak = 0.;
    long n = 0;
    void add(double x) {
        if (n >= 2) worst = std::max(worst, std::fabs(x - 2. * prev1 + prev2));
        peak = std::max(peak, std::fabs(x));
        prev2 = prev1;
        prev1 = x;
        n++;
    }
    double ratio() const { return peak > 0. ? worst / peak : 0.; }
};

static double clicks(void (*set)(Controls&), int births, float seconds, bool holdHalfway = false,
                     float hz = 220.f) {
    Rig g;
    set(g.c);
    Sine s(hz, 1.f);     // eight circles of it stay clear of the limiter
    ClickMeter m;
    long total = (long)(seconds * SR);
    for (long i = 0; i < total; i++) {
        if (i >= (long)SR && births > 0 && (i - (long)SR) % (long)(SR / 4) == 0) {
            g.ev.birth = true;
            births--;
        }
        if (holdHalfway) g.c.hold = i > total / 2 && i < total * 3 / 4;
        Output o = g.tick(s.next());
        if (i > (long)SR) m.add(o.l);
    }
    return m.ratio();
}

static int cmdClicks() {
    printf("clicks (clean sine 0.0008 at 220 Hz, limit 0.01)\n");
    struct Case {
        const char* name;
        void (*set)(Controls&);
        int births;
        bool hold;
        float hz;
        float seconds;
    };
    Case cases[] = {
        {"circle, hard seam", [](Controls& c) { c.size = 0.1f; c.soft = 0.f; c.fadeDb = 0.f; }, 1, false, 220.f, 5.f},
        {"circle, soft", [](Controls& c) { c.size = 0.1f; c.soft = 1.f; c.fadeDb = 0.f; }, 1, false, 220.f, 5.f},
        {"ping-pong", [](Controls& c) { c.size = 0.1f; c.soft = 0.f; c.direction = DIR_PINGPONG; }, 1, false, 220.f, 5.f},
        {"reverse", [](Controls& c) { c.size = 0.1f; c.soft = 0.f; c.direction = DIR_REVERSE; }, 1, false, 220.f, 5.f},
        // at 55 Hz, since TAPE takes it two octaves up, and only while the
        // laps are longer than 20 ms: below that the seams are the tone
        {"converging tape", [](Controls& c) { c.size = 0.3f; c.spiral = 0.9f; c.fadeDb = 0.f; }, 1, false, 55.f, 3.f},
        {"converging cut", [](Controls& c) { c.size = 0.3f; c.spiral = 0.9f; c.tape = 0.f; c.fadeDb = 0.f; }, 1, false, 55.f, 3.f},
        {"unwinding", [](Controls& c) { c.size = 0.05f; c.spiral = 1.3f; c.fadeDb = 0.f; }, 1, false, 220.f, 5.f},
        {"decaying laps", [](Controls& c) { c.size = 0.1f; c.shape = -1.f; }, 1, false, 220.f, 5.f},
        {"swelling laps", [](Controls& c) { c.size = 0.1f; c.shape = 1.f; }, 1, false, 220.f, 5.f},
        {"stealing", [](Controls& c) { c.size = 1.f; c.fadeDb = 0.f; }, 16, false, 220.f, 5.f},
        {"jitter", [](Controls& c) { c.size = 0.1f; c.jitter = 1.f; c.fadeDb = 0.f; }, 1, false, 220.f, 5.f},
        {"tone", [](Controls& c) { c.size = 0.1f; c.tone = -2.f; c.fadeDb = 0.f; }, 1, false, 220.f, 5.f},
        {"hold, line only", [](Controls& c) { c.mix = 0.f; c.line = 1.f; }, 0, true, 220.f, 5.f},
    };
    for (auto& k : cases) {
        double r = clicks(k.set, k.births, k.seconds, k.hold, k.hz);
        printf("  %-18s %.4f\n", k.name, r);
        check(r < 0.01, k.name);
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- line

static int cmdLine() {
    printf("line\n");
    {
        // HOLD after 3 s of a ramp that counts seconds, LINE 2: the loop
        // plays what was written between 1 and 3 s, and repeats every 2 s
        Rig g;
        g.c.mix = 0.f;
        g.c.line = 2.f;
        long t = 0;
        std::vector<float> y;
        for (; t < (long)(3 * SR); t++) g.tick((float)t / SR);
        g.c.hold = true;
        for (long i = 0; i < (long)(5 * SR); i++, t++) y.push_back(g.tick((float)t / SR).l);
        // the loop starts at the start of the last two seconds (the read
        // sits two samples back, for the interpolator)
        float worst = 0.f;
        for (float at : {0.5f, 1.2f, 2.5f, 3.9f}) {
            size_t i = (size_t)(at * SR);
            float want = 1.f + std::fmod(at, 2.f) - 2.f / SR;
            worst = std::max(worst, std::fabs(y[i] - want));
        }
        printf("  held loop, worst error %.2e\n", worst);
        check(worst < 1e-3f, "HOLD, LINE 2 s: the loop is the last two seconds, again and again");
    }
    {
        // HOLD after half a second, LINE 8: never longer than what was written
        Rig g;
        g.c.mix = 0.f;
        g.c.line = 8.f;
        long t = 0;
        for (; t < (long)(0.5f * SR); t++) g.tick(1.f + (float)t / SR);
        g.c.hold = true;
        float lo = 1e9f;
        for (long i = 0; i < (long)(3 * SR); i++) {
            float v = g.tick(0.f).l;
            if (i > (long)(0.05f * SR)) lo = std::min(lo, v);
        }
        check(lo > 0.9f, "HOLD before LINE is filled loops what there is, no silence");
    }
    {
        // circles born under HOLD read the held loop, not the input: a
        // sine before HOLD, silence after
        Rig g;
        g.c.line = 1.f;
        g.c.size = 0.2f;
        g.c.fadeDb = 0.f;
        Sine s(110.f, 3.f);
        for (long t = 0; t < (long)(2 * SR); t++) g.tick(s.next());
        g.c.hold = true;
        double e = 0.;
        long count = 0;
        for (long i = 0; i < (long)(3 * SR); i++) {
            if (i == (long)(0.5f * SR)) g.ev.birth = true;
            Output o = g.tick(0.f);
            if (i > (long)(0.6f * SR)) {
                e += (double)o.l * o.l;
                count++;
            }
        }
        // the sine's RMS at the centre pan: 3 / sqrt 2 x sqrt 0.5 = 1.5
        double rms = std::sqrt(e / count);
        printf("  circle under HOLD: %.3f V RMS (the held sine 1.5, the input 0)\n", rms);
        check(std::fabs(rms - 1.5) < 0.15, "a circle born under HOLD plays the held loop");
    }
    {
        // RATE: 4 per second, births counted by the circles that sound
        Rig g;
        g.c.rate = 4.f;
        g.c.size = 0.05f;
        g.c.fadeDb = -24.f;
        int births = 0, prev = 0;
        Sine s;
        for (long i = 0; i < (long)(5 * SR); i++) {
            Output o = g.tick(s.next());
            if (o.circles > prev) births++;
            prev = o.circles;
        }
        printf("  RATE 4 Hz: %d births in 5 s\n", births);
        check(births >= 19 && births <= 21, "RATE 4: twenty circles in five seconds");
    }
    {
        // REACH: circles born at most REACH x LINE behind the playhead
        Rig g;
        g.c.reach = 1.f;
        g.c.line = 2.f;
        g.c.size = 0.1f;
        long t = 0;
        double worst = 0.;
        for (; t < (long)(6 * SR); t++) {
            g.tick(0.f);
            if (t > (long)(3 * SR) && t % 4800 == 0) {
                g.ev.birth = true;
                g.tick(0.f);
                t++;
                const Circle& c = g.e.circle[g.e.lead];
                worst = std::max(worst, ((double)g.e.w - c.cur.a) / SR);
            }
        }
        printf("  REACH 1, LINE 2: furthest back %.3f s\n", worst);
        check(worst > 1.5 && worst < 2.2, "REACH 1 reaches about LINE back, no further");
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- fuzz

static int cmdFuzz() {
    printf("fuzz\n");
    Rng r;
    r.seed(777);
    bool finite = true;
    float peak = 0.f;
    for (int round = 0; round < 60; round++) {
        Rig g(round % 3 == 0 ? 44100.f : round % 3 == 1 ? 48000.f : 96000.f);
        Sine s(80.f + 900.f * r.uniform(), 10.f * r.uniform());
        auto u = [&]() { return r.uniform(); };
        for (long i = 0; i < (long)(6 * g.e.sr); i++) {
            if (i % 4800 == 0) {
                Controls& c = g.c;
                c.size = sizeSeconds(u());
                c.spiral = spiralRatio(2.f * u() - 1.f);
                c.tape = u();
                c.pitch = 48.f * u() - 24.f;
                c.fadeDb = -24.f + 27.f * u();
                c.shape = 2.f * u() - 1.f;
                c.soft = u();
                c.tone = 4.f * u() - 2.f;
                c.jitter = u();
                c.rate = rateHz(u());
                c.reach = u();
                c.anchor = u();
                c.line = lineSeconds(u());
                c.mix = u();
                c.spread = u();
                c.direction = (int)(u() * 3.f) % 3;
                c.hold = u() < 0.3f;
                c.keepBirth = u() < 0.3f;
            }
            if (u() < 0.0005f) g.ev.birth = true;
            Output o = g.tick(s.next(), -s.next());
            if (!std::isfinite(o.l) || !std::isfinite(o.r) || !std::isfinite(o.speedOct)) finite = false;
            peak = std::max(peak, std::max(std::fabs(o.l), std::fabs(o.r)));
        }
    }
    printf("  peak %.2f V\n", peak);
    check(finite, "every output finite");
    check(peak <= 20.1f, "bounded: line and soft-limited circles");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- cpu

static int cmdCpu() {
    Rig g;
    g.c.size = 2.f;
    g.c.fadeDb = 0.f;
    g.c.soft = 0.5f;
    Sine s;
    for (long i = 0; i < (long)(3 * SR); i++) {
        if (i % 4800 == 0 && i > (long)SR) g.ev.birth = true;
        g.tick(s.next());
    }
    long n = (long)(10 * SR);
    auto t0 = std::chrono::steady_clock::now();
    float acc = 0.f;
    for (long i = 0; i < n; i++) acc += g.tick(s.next()).l;
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    printf("cpu: %d circles, %.1f ns/sample, %.2f%% of a core at 48 kHz (%g)\n",
           g.o.circles, ns, ns * SR * 1e-7, acc * 0.f);
    return 0;
}

// ---------------------------------------------------------------- wav

static bool readWav(const std::string& path, std::vector<float>& l, std::vector<float>& r, float& sr) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    unsigned char tmp[65536];
    size_t got;
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + got);
    fclose(f);
    if (d.size() < 12 || memcmp(&d[0], "RIFF", 4) || memcmp(&d[8], "WAVE", 4)) return false;
    auto u16 = [&](size_t i) { return (uint32_t)d[i] | (uint32_t)d[i + 1] << 8; };
    auto u32 = [&](size_t i) { return u16(i) | u16(i + 2) << 16; };
    int fmt = 0, ch = 0, bits = 0;
    size_t i = 12;
    while (i + 8 <= d.size()) {
        uint32_t len = u32(i + 4);
        if (!memcmp(&d[i], "fmt ", 4)) {
            fmt = (int)u16(i + 8);
            ch = (int)u16(i + 10);
            sr = (float)u32(i + 12);
            bits = (int)u16(i + 22);
            if (fmt == 0xFFFE) fmt = (int)u16(i + 32);
        } else if (!memcmp(&d[i], "data", 4)) {
            size_t bytes = bits / 8, frames = std::min((size_t)len, d.size() - i - 8) / (bytes * ch);
            for (size_t k = 0; k < frames; k++) {
                float s[2];
                for (int c = 0; c < std::min(ch, 2); c++) {
                    size_t p = i + 8 + (k * ch + c) * bytes;
                    if (fmt == 3 && bits == 32) { float v; memcpy(&v, &d[p], 4); s[c] = v; }
                    else if (bits == 16) s[c] = (int16_t)u16(p) / 32768.f;
                    else if (bits == 24) s[c] = (int32_t)((u32(p) & 0xffffff) << 8) / 2147483648.f;
                    else return false;
                }
                l.push_back(s[0]);
                r.push_back(ch > 1 ? s[1] : s[0]);
            }
            return !l.empty();
        }
        i += 8 + len + (len & 1);
    }
    return false;
}

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r,
                     float sr) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t n = l.size(), rate = (uint32_t)sr, bytes = n * 4;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(bytes);
    for (size_t k = 0; k < n; k++)
        for (float v : {l[k], r[k]}) {
            int s = (int)std::lround(std::max(-1.f, std::min(1.f, v * 0.1f)) * 32767);   // 10 V = full scale
            u16((uint16_t)(int16_t)s);
        }
    fclose(f);
}

static int cmdWav(const std::string& in, const std::string& dir) {
    std::vector<float> xl, xr;
    float sr = 48000.f;
    if (!readWav(in, xl, xr, sr)) {
        fprintf(stderr, "cannot read %s (PCM 16/24 or float 32)\n", in.c_str());
        return 1;
    }
    struct Scene {
        const char* name;
        void (*set)(Controls&);
        float hold;     // seconds in, 0 = never
    };
    Scene scenes[] = {
        // circles and nothing else: the stutter this module must get past
        {"1_circles", [](Controls& c) { c.size = 0.5f; c.rate = 0.7f; c.fadeDb = -3.f; }, 0.f},
        // converging on tape: every repeat shorter and higher, into a tone
        {"2_inward_tape", [](Controls& c) {
             c.size = 0.4f; c.spiral = spiralRatio(-0.3f); c.tape = 1.f; c.fadeDb = 0.f; c.rate = 0.4f;
             c.soft = 0.3f; c.spread = 0.6f; }, 0.f},
        // converging by cutting: a roll into a buzz at the same pitch
        {"3_inward_cut", [](Controls& c) {
             c.size = 0.4f; c.spiral = spiralRatio(-0.3f); c.tape = 0.f; c.fadeDb = 0.f; c.rate = 0.4f;
             c.anchor = 1.f; }, 0.f},
        // unwinding: slower and lower, darker every turn
        {"4_outward", [](Controls& c) {
             c.size = 0.15f; c.spiral = spiralRatio(0.45f); c.tape = 1.f; c.fadeDb = -1.f; c.rate = 0.3f;
             c.tone = -0.7f; c.spread = 0.8f; c.soft = 0.5f; }, 0.f},
        // a looper of grains: short soft laps, many of them, scattered
        {"5_grains", [](Controls& c) {
             c.size = 0.07f; c.soft = 1.f; c.rate = 12.f; c.reach = 0.3f; c.jitter = 0.6f; c.spread = 1.f;
             c.fadeDb = -6.f; c.line = 6.f; c.spiral = spiralRatio(-0.15f); }, 0.f},
        // plucked laps in ping-pong, closing in
        {"6_plucks", [](Controls& c) {
             c.size = 0.25f; c.shape = -0.6f; c.direction = DIR_PINGPONG; c.spiral = spiralRatio(-0.2f);
             c.fadeDb = -1.f; c.rate = 0.8f; c.spread = 0.8f; c.tape = 0.5f; }, 0.f},
        // swelling reversed laps going up and thin
        {"7_swells", [](Controls& c) {
             c.size = 0.6f; c.shape = 0.6f; c.soft = 0.6f; c.direction = DIR_REVERSE; c.tone = 0.8f;
             c.fadeDb = -2.f; c.rate = 0.5f; c.pitch = 7.f; c.spread = 0.7f; }, 0.f},
        // HOLD at 4 s on a 4 s line, circles from anywhere on it
        {"8_held", [](Controls& c) {
             c.line = 4.f; c.reach = 1.f; c.size = 0.3f; c.rate = 1.5f; c.jitter = 0.4f; c.soft = 0.5f;
             c.spiral = spiralRatio(-0.25f); c.fadeDb = -2.f; c.direction = DIR_PINGPONG; c.spread = 1.f;
             c.mix = 0.6f; }, 4.f},
    };
    float seconds = 24.f;
    for (auto& s : scenes) {
        SR = sr;
        Rig g(sr);
        s.set(g.c);
        if (s.name[0] != '8') g.c.mix = 0.5f;
        std::vector<float> yl, yr;
        long total = (long)(seconds * sr);
        for (long i = 0; i < total; i++) {
            size_t k = (size_t)i % xl.size();
            g.c.hold = s.hold > 0.f && i >= (long)(s.hold * sr);
            Output o = g.tick(5.f * xl[k], 5.f * xr[k]);
            yl.push_back(o.l);
            yr.push_back(o.r);
        }
        std::string path = dir + "/" + s.name + ".wav";
        writeWav(path, yl, yr, sr);
        printf("%s\n", path.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "laws") return cmdLaws();
    if (cmd == "spiral") return cmdSpiral();
    if (cmd == "clicks") return cmdClicks();
    if (cmd == "line") return cmdLine();
    if (cmd == "fuzz") return cmdFuzz();
    if (cmd == "cpu") return cmdCpu();
    if (cmd == "wav" && argc > 3) return cmdWav(argv[2], argv[3]);
    fprintf(stderr, "usage: spira_probe laws|spiral|clicks|line|fuzz|cpu|wav <in.wav> <dir>\n");
    return 2;
}
