// rubigo_probe - measurement harness for the rubigo engine. It drives
// src/rubigo/rubigo.hpp directly and needs no Rack.
//
//   ./rubigo_probe seq       the STEPS lock: a loop repeats exactly, a
//                            lock from OFF keeps what was just heard,
//                            shortening and lengthening regenerate only the
//                            tail, SKIPS at 0 and 1, a skip holds the value
//   ./rubigo_probe clock     internal TEMPO, external x1 x2 x4 /2 /8, the
//                            return to the internal clock and the option
//                            that stops it, RESET, restart on play
//   ./rubigo_probe voice     silence untriggered, the three decay times,
//                            PITCH, the resonance ringing with the sources
//                            muted, the RUST rate, peak level per effect
//   ./rubigo_probe random    the reasoned random against a uniform draw:
//                            how many come out inaudible or clipped flat,
//                            level and brightness (RMS frequency) per archetype
//   ./rubigo_probe fuzz      random controls and events: finite and bounded
//   ./rubigo_probe cpu       ns per sample
//   ./rubigo_probe wav <dir> the manual's recipes, to listen to
//
// Every command but `cpu` and `wav` exits nonzero on a failed check.

#include "../src/rubigo/rubigo.hpp"
#include "../src/rubigo/reasoned.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rubigo;

static const float SR = 48000.f;
static int failures = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// The knob position of an exponential law for a value.
static float knobFor(float v, float lo, float hi) { return std::log(v / lo) / std::log(hi / lo); }

struct Rig {
    Engine e;
    Controls c;
    Events ev;
    Output o;
    Rig() {
        e.seed(12345);
        c.play = false;
    }
    Output tick() {
        o = e.process(c, ev, SR);
        ev = Events();
        return o;
    }
};

// ---------------------------------------------------------------- seq

struct Step {
    bool fire;
    float held;
};

static std::vector<Step> run(Sequencer& s, Rng& r, int n, float skips) {
    std::vector<Step> v;
    for (int i = 0; i < n; i++) {
        bool f = s.step(skips, r);
        v.push_back({f, s.held});
    }
    return v;
}

static bool same(const std::vector<Step>& a, size_t ia, const std::vector<Step>& b, size_t ib, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[ia + i].fire != b[ib + i].fire || a[ia + i].held != b[ib + i].held) return false;
    return true;
}

static int cmdSeq() {
    printf("seq\n");
    Rng r;
    r.seed(7);
    // skips 0.5: a mixed pattern, so fire and hold are both exercised.
    {
        Sequencer s;
        std::vector<Step> heard = run(s, r, 40, 0.5f);
        Slot last8[8];
        for (int i = 0; i < 8; i++) last8[i] = s.history[(s.historyW - 8 + i + kSlots) % kSlots];
        s.setLength(8);
        bool kept = true;
        for (int i = 0; i < 8; i++)
            kept = kept && s.slot[i].valid && s.slot[i].skip == last8[i].skip && s.slot[i].mod == last8[i].mod;
        check(kept, "OFF -> 8 locks the last 8 steps heard, oldest first");
        std::vector<Step> loop = run(s, r, 24, 0.5f);
        check(same(loop, 0, loop, 8, 8) && same(loop, 8, loop, 16, 8), "a locked loop of 8 repeats exactly, three passes");
        int fires = 0;
        for (int i = 0; i < 8; i++) fires += loop[i].fire;
        check(fires > 0 && fires < 8, "skips 0.5 fires some of the 8 and not all");
    }
    {
        Sequencer s;
        run(s, r, 32, 0.f);
        s.setLength(8);
        Slot before[8];
        for (int i = 0; i < 8; i++) before[i] = s.slot[i];
        s.setLength(4);
        run(s, r, 4, 0.f);
        s.setLength(8);
        run(s, r, 8, 0.f);
        bool headKept = true, tailNew = true;
        for (int i = 0; i < 4; i++) headKept = headKept && s.slot[i].mod == before[i].mod;
        for (int i = 4; i < 8; i++) tailNew = tailNew && s.slot[i].mod != before[i].mod;
        check(headKept, "8 -> 4 -> 8 keeps the first 4");
        check(tailNew, "8 -> 4 -> 8 draws the last 4 again");
        for (int i = 0; i < 8; i++) before[i] = s.slot[i];
        s.setLength(16);
        run(s, r, 16, 0.f);
        bool kept = true;
        for (int i = 0; i < 8; i++) kept = kept && s.slot[i].mod == before[i].mod;
        check(kept && s.slot[15].valid, "8 -> 16 keeps the 8 and fills 8 more");
        s.setLength(0);
        bool cleared = true;
        for (int i = 0; i < kSlots; i++) cleared = cleared && !s.slot[i].valid;
        check(cleared, "back to OFF discards everything");
    }
    {
        Sequencer s;
        std::vector<Step> v = run(s, r, 200, 0.f);
        int f = 0;
        for (auto& x : v) f += x.fire;
        check(f == 200, "SKIPS 0 fires every step");
        v = run(s, r, 200, 1.f);
        f = 0;
        bool held = true;
        for (auto& x : v) {
            f += x.fire;
            held = held && x.held == v[0].held;
        }
        check(f == 0, "SKIPS 1 fires none");
        check(held, "a skipped step holds the last value");
    }
    {
        // Shortening past the playhead wraps it.
        Sequencer s;
        run(s, r, 32, 0.f);
        s.setLength(16);
        run(s, r, 11, 0.f);
        s.setLength(8);
        check(s.pos < 8, "shortening past the playhead wraps it");
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- clock

// Steps counted over `seconds`, with an external edge every `edgePeriod`
// samples (0 = none).
static int countSteps(Rig& g, float seconds, int edgePeriod, long& t) {
    int steps = 0;
    for (long i = 0; i < (long)(seconds * SR); i++, t++) {
        if (edgePeriod > 0 && t % edgePeriod == 0) g.ev.clock = true;
        if (g.tick().clock) steps++;
    }
    return steps;
}

static int cmdClock() {
    printf("clock\n");
    {
        Rig g;
        g.c.play = true;
        g.c.tempo = tempoKnob(4.f);
        long t = 0;
        int n = countSteps(g, 5.f, 0, t);
        printf("    internal 4 Hz: %d steps in 5 s\n", n);
        check(n >= 20 && n <= 21, "TEMPO at 4 Hz steps 20 times in 5 s");
    }
    const int ratios[] = {1, 2, 4, -2, -8};
    const float knobs[] = {3.5f / 7, 4.5f / 7, 5.5f / 7, 2.5f / 7, 0.5f / 7};
    for (int k = 0; k < 5; k++) {
        Rig g;
        g.c.play = true;
        g.c.clockConnected = true;
        g.c.tempo = knobs[k];
        long t = 0;
        countSteps(g, 1.f, 4800, t);                        // lock on, 10 Hz
        int n = countSteps(g, 4.f, 4800, t);
        int want = ratios[k] > 0 ? 40 * ratios[k] : 40 / -ratios[k];
        char what[96];
        snprintf(what, sizeof what, "external 10 Hz at %s%d: %d steps in 4 s (want %d)",
                 ratios[k] > 0 ? "x" : "/", std::abs(ratios[k]), n, want);
        check(std::abs(n - want) <= 1, what);
    }
    {
        Rig g;
        g.c.play = true;
        g.c.clockConnected = true;
        g.c.tempo = tempoKnob(4.f);
        long t = 0;
        countSteps(g, 1.f, 4800, t);
        int during = countSteps(g, 1.9f, 0, t);             // edges stop
        int after = countSteps(g, 2.f, 0, t);
        check(during == 0, "no steps while waiting for the external clock");
        check(after >= 7, "the internal clock returns after 2 s");
        Rig h;
        h.c = g.c;
        h.c.noAutoStart = true;
        t = 0;
        countSteps(h, 1.f, 4800, t);
        countSteps(h, 2.5f, 0, t);
        int held = countSteps(h, 2.f, 0, t);
        check(held == 0, "with auto-start disabled it stays stopped");
        h.c.play = false;
        h.tick();
        h.c.play = true;
        int again = countSteps(h, 1.f, 0, t);
        check(again >= 3, "... until play is switched again");
    }
    {
        Rig g;
        g.c.play = true;
        g.c.steps = 3;                                      // 8
        g.c.clockConnected = true;
        long t = 0;
        countSteps(g, 2.f, 4800, t);
        g.ev.reset = true;
        g.tick();
        t++;
        check(g.e.seq.pos != 0, "RESET waits for the next clock");
        while (!g.o.clock) {
            if (t % 4800 == 0) g.ev.clock = true;
            g.tick();
            t++;
        }
        check(g.e.seq.pos == 1, "RESET: the next clock plays step one");
    }
    {
        Rig g;
        g.c.play = true;
        g.c.steps = 3;
        g.c.restartOnPlay = true;
        long t = 0;
        countSteps(g, 1.3f, 0, t);
        g.c.play = false;
        g.tick();
        g.c.play = true;
        g.tick();
        check(g.e.seq.pos == 1, "restart on play: play starts on step one");
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- voice

// Seconds from the peak to 60 dB under it, on the envelope of |y|.
static float decayTime(const std::vector<float>& y) {
    size_t ip = 0;
    for (size_t i = 0; i < y.size(); i++)
        if (std::fabs(y[i]) > std::fabs(y[ip])) ip = i;
    float peak = std::fabs(y[ip]);
    size_t last = ip;
    for (size_t i = ip; i < y.size(); i++)
        if (std::fabs(y[i]) > peak * 1e-3f) last = i;
    return (last - ip) / SR;
}

static float freqOf(const std::vector<float>& y, size_t from) {
    int up = 0;
    size_t first = 0, lastUp = 0;
    for (size_t i = from + 1; i < y.size(); i++)
        if (y[i - 1] < 0.f && y[i] >= 0.f) {
            if (!up) first = i;
            lastUp = i;
            up++;
        }
    return up > 1 ? (up - 1) * SR / (lastUp - first) : 0.f;
}

static std::vector<float> render(Rig& g, float seconds) {
    std::vector<float> y;
    for (long i = 0; i < (long)(seconds * SR); i++) y.push_back(g.tick().audio);
    return y;
}

static int cmdVoice() {
    printf("voice\n");
    {
        Rig g;
        std::vector<float> y = render(g, 1.f);
        float m = 0.f;
        for (float v : y) m = std::max(m, std::fabs(v));
        check(m < 1e-3f, "silent until triggered");
    }
    // AdEnv's decay reaches zero exactly at the DECAY time.
    for (float T : {0.05f, 0.4f, 2.f}) {
        Rig g;
        g.c.volumeDecay = std::sqrt((T - kDecayMin) / (kDecayMax - kDecayMin));
        g.ev.trigger = true;
        long n = 0;
        g.tick();
        while (g.e.volumeEnv.output > 0.f && n < (long)(4.f * SR)) {
            g.tick();
            n++;
        }
        float d = n / SR - kAttack;
        char what[80];
        snprintf(what, sizeof what, "volume DECAY %.2f s: silent after %.3f s", T, d);
        check(std::fabs(d - T) < 0.002f + 0.01f * T, what);
    }
    for (float hz : {30.f, 110.f, 1500.f}) {
        Rig g;
        g.c.pitch = knobFor(hz, kPitchMin, kPitchMax);
        g.c.volumeDecay = 1.f;
        g.c.cutoff = 1.f;
        g.c.volume = 0.3f;
        g.ev.trigger = true;
        std::vector<float> y = render(g, 0.5f);
        float f = freqOf(y, 2400);
        char what[80];
        snprintf(what, sizeof what, "PITCH at %g Hz measures %.1f Hz", hz, f);
        check(std::fabs(f / hz - 1.f) < 0.02f, what);
    }
    {
        // The manual's trick: a dead cable in the input, NOISE at full, the
        // resonance alone. Retriggered so the VCA stays open.
        Rig g;
        g.c.extConnected = true;
        g.c.noise = 1.f;
        g.c.resonance = 1.f;
        g.c.cutoff = std::sqrt((440.f - kCutoffMin) / (kCutoffMax - kCutoffMin));   // fmap EXP
        g.c.volumeDecay = 1.f;
        g.c.volume = 0.5f;
        g.c.play = true;
        g.c.tempo = tempoKnob(4.f);
        std::vector<float> y = render(g, 3.f);
        float m = 0.f, rms = 0.f;
        for (size_t i = SR * 2; i < y.size(); i++) {
            m = std::max(m, std::fabs(y[i]));
            rms += y[i] * y[i];
        }
        rms = std::sqrt(rms / (y.size() - SR * 2));
        float f = freqOf(y, SR * 2);
        printf("    self-oscillation: rms %.2f V, peak %.2f V, %.1f Hz at CUTOFF 440 Hz\n", rms, m, f);
        check(rms > 0.3f, "full RESONANCE rings with the sources muted");
        check(m < 8.f, "... and stays bounded");
        check(f > 300.f && f < 600.f, "... near the cutoff");
    }
    {
        // RUST: how often the held value changes, on quiet noise at the
        // input (anything louder saturates flat at full amount, and a
        // saturated sample repeats the last one).
        for (float k : {0.f, 0.5f, 1.f}) {
            Rig g;
            Rng n;
            g.c.rust = true;
            g.c.effect = k;
            g.c.noise = 1.f;
            g.c.extConnected = true;
            g.c.cutoff = 1.f;
            g.c.volumeDecay = 1.f;
            g.c.volume = 0.3f;
            g.ev.trigger = true;
            // Counted at the sample-and-hold: the DC blocker after it moves
            // on every sample.
            int changes = 0;
            float prev = 0.f;
            for (long i = 0; i < (long)SR; i++) {
                g.c.ext = 0.01f * n.bipolar();
                g.tick();
                changes += g.e.decimator.held != prev;
                prev = g.e.decimator.held;
            }
            float f = 0.5f * k * k * k;                          // the factor rubigo gives it
            float want = SR / (std::floor(f * f * 96.f) + 1.f);  // Decimator's hold
            char what[80];
            snprintf(what, sizeof what, "RUST %.1f: %d changes/s, rate %.0f Hz", k, changes, want);
            check(changes > 0.8f * want && changes < 1.1f * want + 2, what);
        }
    }
    {
        const char* names[FX_LEN] = {"distortion", "2nd osc", "phaser", "flanger", "chorus"};
        for (int fx = 0; fx < FX_LEN; fx++)
            for (int rust = 0; rust < 2; rust++) {
                Rig g;
                g.c.fx = fx;
                g.c.rust = rust;
                g.c.effect = 1.f;
                g.c.volume = 1.f;
                g.c.cutoff = 1.f;
                g.c.resonance = 0.9f;
                g.c.noise = 0.5f;
                g.c.volumeDecay = 1.f;
                g.c.play = true;
                g.c.tempo = 0.6f;
                std::vector<float> y = render(g, 2.f);
                float m = 0.f;
                bool finite = true;
                for (float v : y) {
                    m = std::max(m, std::fabs(v));
                    finite = finite && std::isfinite(v);
                }
                char what[80];
                snprintf(what, sizeof what, "%s %s: peak %.2f V", names[fx], rust ? "RUST" : "CORROSION", m);
                check(finite && m > 1.f && m < 10.f, what);
            }
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- fuzz

static int cmdFuzz() {
    printf("fuzz\n");
    Rng r;
    r.seed(99);
    int bad = 0;
    float worst = 0.f;
    for (int p = 0; p < 300; p++) {
        Rig g;
        g.e.seed(r.next());
        Controls& c = g.c;
        c.pitch = r.uniform(); c.wave = r.next() & 1; c.pitchDecay = r.uniform();
        c.pitchAmount = r.uniform(); c.noise = r.uniform(); c.cutoff = r.uniform();
        c.resonance = r.uniform(); c.highpass = r.next() & 1; c.cutoffDecay = r.uniform();
        c.cutoffAmount = r.uniform(); c.volume = r.uniform(); c.volumeDecay = r.uniform();
        c.effect = r.uniform(); c.rust = r.next() & 1; c.mix = r.uniform();
        c.play = true; c.tempo = r.uniform(); c.skips = r.uniform(); c.stepMod = r.uniform();
        c.dest = r.next() % 3; c.steps = r.next() % 7;
        c.pitchCv = r.bipolar() * 10.f; c.noiseCv = r.bipolar() * 10.f;
        c.cutoffCv = r.bipolar() * 10.f; c.skipsCv = r.bipolar() * 10.f;
        c.stepModCv = r.bipolar() * 10.f;
        c.extConnected = r.next() & 1;
        c.fx = r.next() % FX_LEN; c.assign = r.next() % ASSIGN_LEN;
        c.altLengths = r.next() & 1;
        for (long i = 0; i < (long)(0.25f * SR); i++) {
            c.ext = r.bipolar() * 10.f;
            if ((r.next() & 1023) == 0) g.ev.trigger = true;
            if ((r.next() & 4095) == 0) c.steps = r.next() % 7;
            Output o = g.tick();
            if (!std::isfinite(o.audio) || std::fabs(o.audio) > 12.f || !std::isfinite(o.stepMod)) {
                bad++;
                break;
            }
            worst = std::max(worst, std::fabs(o.audio));
        }
    }
    printf("    300 random patches, worst peak %.2f V\n", worst);
    check(bad == 0, "every output finite and within 12 V");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- random

struct Heard {
    float rms, peak, brightness;  // brightness: the RMS frequency, in Hz
};

// Brightness as the RMS frequency, SR / 2pi * sqrt(sum dy^2 / sum y^2): the
// silence between hits adds nothing to either sum.
static Heard listen(Rig& g, float seconds) {
    std::vector<float> y = render(g, seconds);
    double e = 0., de = 0.;
    float m = 0.f;
    size_t from = y.size() / 4;                             // settle first
    for (size_t i = from; i < y.size(); i++) {
        e += (double)y[i] * y[i];
        de += (double)(y[i] - y[i - 1]) * (y[i] - y[i - 1]);
        m = std::max(m, std::fabs(y[i]));
    }
    size_t n = y.size() - from;
    float b = e > 0. ? SR / (2.f * kPi) * (float)std::sqrt(de / e) : 0.f;
    return {(float)std::sqrt(e / n), m, b};
}

// Rhythms are sparse, so a short kick twice a second has a low rms however
// loud it is: judged on the peak.
static bool inaudible(const Heard& h) { return h.peak < 0.5f; }

static int cmdRandom() {
    printf("random\n");
    const int N = 200;
    Rng r;
    r.seed(4242);
    auto u = [&r]() { return r.uniform(); };
    int deadUniform = 0;
    for (int i = 0; i < N; i++) {
        Rig g;
        g.e.seed(r.next());
        Controls& c = g.c;
        c.pitch = u(); c.wave = r.next() & 1; c.pitchDecay = u(); c.pitchAmount = u();
        c.noise = u(); c.cutoff = u(); c.resonance = u(); c.highpass = r.next() & 1;
        c.cutoffDecay = u(); c.cutoffAmount = u(); c.volume = u(); c.volumeDecay = u();
        c.effect = u(); c.rust = r.next() & 1; c.mix = u(); c.tempo = u(); c.skips = u();
        c.stepMod = u(); c.dest = r.next() % 3; c.steps = r.next() % 7; c.play = true;
        deadUniform += inaudible(listen(g, 4.f));
    }
    printf("    uniform: %d of %d inaudible\n", deadUniform, N);
    int deadReasoned = 0;
    for (int a = 0; a < ARCH_LEN; a++) {
        int dead = 0;
        double rms = 0., cen = 0., pk = 0.;
        const int M = N / ARCH_LEN;
        for (int i = 0; i < M; i++) {
            Rig g;
            g.e.seed(r.next());
            apply(roll(a, u), g.c);
            g.c.play = true;
            Heard h = listen(g, 4.f);
            dead += inaudible(h);
            rms += h.rms;
            cen += h.brightness;
            pk += h.peak;
        }
        deadReasoned += dead;
        printf("    %-9s %2d of %d inaudible, mean rms %.2f V, peak %.2f V, brightness %5.0f Hz\n",
               archetypeName(a), dead, M, rms / M, pk / M, cen / M);
    }
    char what[80];
    snprintf(what, sizeof what, "reasoned: %d of %d inaudible, uniform %d", deadReasoned, N, deadUniform);
    check(deadReasoned * 4 <= deadUniform && deadReasoned <= N / 20, what);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- cpu

static int cmdCpu() {
    Rig g;
    g.c.play = true;
    g.c.tempo = 0.5f;
    g.c.resonance = 0.7f;
    g.c.effect = 0.6f;
    g.c.rust = true;
    for (int fx : {FX_DISTORTION, FX_FLANGER, FX_PHASER}) {
        g.c.fx = fx;
        const long n = 48000 * 10;
        auto t0 = std::chrono::steady_clock::now();
        float acc = 0.f;
        for (long i = 0; i < n; i++) acc += g.tick().audio;
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
        printf("fx %d: %.1f ns/sample (%.2f%% of a 48 kHz core) %g\n", fx, ns, ns * 48000 / 1e7, acc * 0);
    }
    return 0;
}

// ---------------------------------------------------------------- wav

static void writeWav(const std::string& path, const std::vector<float>& x, float gain = 0.2f) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t n = x.size(), sr = SR, bytes = n * 2;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(1);
    u32(sr);
    u32(sr * 2);
    u16(2);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(bytes);
    for (float v : x) {
        int s = (int)std::lround(std::max(-1.f, std::min(1.f, v * gain)) * 32767);
        u16((uint16_t)(int16_t)s);
    }
    fclose(f);
}

static int cmdWav(const std::string& dir) {
    struct Scene {
        const char* name;
        void (*set)(Controls&);
    };
    Scene scenes[] = {
        // The manual's kick: PITCH at minimum, pitch DECAY between <- and
        // up-left, AMOUNT between up-right and ->.
        {"kick", [](Controls& c) {
             c.pitch = 0.f; c.pitchDecay = 0.25f; c.pitchAmount = 0.75f; c.cutoff = 0.4f;
             c.volumeDecay = 0.45f; c.tempo = 0.3f; c.steps = 5; c.skips = 0.4f; c.wave = SQUARE;
         }},
        // The resonant melody: cutoff and its envelope at zero, RESONANCE
        // full, STEP MOD half on CUTOFF.
        {"resonant_melody", [](Controls& c) {
             c.cutoff = 0.f; c.cutoffAmount = 0.f; c.resonance = 1.f; c.dest = DEST_CUTOFF;
             c.stepMod = 0.5f; c.volumeDecay = 0.5f; c.tempo = 0.35f; c.steps = 5;
         }},
        // The drone: volume DECAY and TEMPO full, no skips.
        {"drone", [](Controls& c) {
             c.volumeDecay = 1.f; c.tempo = 1.f; c.skips = 0.f; c.cutoff = 0.5f; c.resonance = 0.5f;
             c.dest = DEST_CUTOFF; c.stepMod = 0.6f; c.pitch = 0.15f;
         }},
        // Kick and noise: a kick, NOISE at 0, STEP MOD full on NOISE.
        {"kick_and_noise", [](Controls& c) {
             c.pitch = 0.f; c.pitchDecay = 0.25f; c.pitchAmount = 0.8f; c.cutoff = 0.6f;
             c.dest = DEST_NOISE; c.stepMod = 1.f; c.volumeDecay = 0.4f; c.tempo = 0.3f; c.steps = 3;
         }},
        {"rust", [](Controls& c) {
             c.rust = true; c.effect = 0.85f; c.pitch = 0.5f; c.cutoff = 0.7f; c.resonance = 0.4f;
             c.dest = DEST_PITCH; c.stepMod = 0.4f; c.volumeDecay = 0.4f; c.tempo = 0.35f;
             c.steps = 5; c.skips = 0.3f;
         }},
    };
    for (auto& s : scenes) {
        Rig g;
        g.c = Controls();
        s.set(g.c);
        g.c.play = true;
        std::vector<float> y = render(g, 8.f);
        writeWav(dir + "/" + s.name + ".wav", y);
        printf("%s/%s.wav\n", dir.c_str(), s.name);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "seq") return cmdSeq();
    if (cmd == "clock") return cmdClock();
    if (cmd == "voice") return cmdVoice();
    if (cmd == "random") return cmdRandom();
    if (cmd == "fuzz") return cmdFuzz();
    if (cmd == "cpu") return cmdCpu();
    if (cmd == "wav" && argc > 2) return cmdWav(argv[2]);
    fprintf(stderr, "usage: rubigo_probe seq|clock|voice|random|fuzz|cpu|wav <dir>\n");
    return 2;
}
