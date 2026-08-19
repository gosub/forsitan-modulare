// umbrae_probe — measurement harness for the feedback loop.
//
// The module's whole identity is that the loop screams on its own, at a
// pitch the circuit chooses. That pitch must therefore be a property of the
// modelled circuit and of nothing else, which gives this harness its one
// non-negotiable measurement: the same patch has to oscillate at the same
// frequency at every engine rate. A port that gets that wrong is a different
// instrument at 44.1 kHz and at 96 kHz, however good it sounds at one of
// them.
//
// The rest is the map: where the loop starts oscillating, what the two bands
// do to the pitch, what the drive stage does to a signal, how fast the
// envelope follower is, and what the external loop jacks do.
//
//   umbrae_probe [osc|srate|thresh|eq|drive|env|xfade|extloop|cpu]
//
// With no argument it runs the lot. Rack is not involved: src/umbrae_dsp.hpp
// is free of Rack headers.

#include "../src/umbrae_dsp.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace umbrae;

// 8x oversampling from 48 kHz, the module's default.
static const double SR = 384000.0;

// ── helpers ─────────────────────────────────────────────────────────────────

struct Bin {
    double re = 0.0, im = 0.0;
    long n = 0;
    void add(double y, double phase) {
        re += y * std::cos(phase);
        im += y * std::sin(phase);
        n++;
    }
    double mag() const { return n ? 2.0 * std::sqrt(re * re + im * im) / n : 0.0; }
};

struct Run {
    double rms = 0.0, peak = 0.0, hz = 0.0;
    long nans = 0;
};

// Pitch by hysteresis crossings: a cycle is counted when the signal comes up
// through +20% of its own peak having last been below -20%, which counts the
// fundamental rather than every wiggle a harmonic puts on it.
static double pitchOf(const std::vector<double>& y, double sr) {
    double peak = 0.0;
    for (double v : y) peak = std::max(peak, std::fabs(v));
    if (peak < 1e-3) return 0.0;   // silence has no pitch
    const double th = 0.2 * peak;
    int cycles = 0;
    bool armed = false;
    long first = -1, last = -1;
    for (size_t i = 0; i < y.size(); i++) {
        if (y[i] < -th) armed = true;
        else if (armed && y[i] > th) {
            armed = false;
            if (first < 0) first = (long)i; else last = (long)i;
            cycles++;
        }
    }
    if (cycles < 3 || last <= first) return 0.0;
    return (cycles - 1) * sr / (double)(last - first);
}

// Free-run the engine with nothing patched and measure what comes out.
static Run freeRun(Engine::Controls c, double sr, double settleS, double measS,
                   Engine* keep = nullptr) {
    Engine e;
    e.setSampleRate(sr);
    e.reset();
    for (long i = 0; i < (long)(settleS * sr); i++)
        e.process(c, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    const long n = (long)(measS * sr);
    std::vector<double> ys;
    ys.reserve(n);
    Run r;
    double s2 = 0.0;
    for (long i = 0; i < n; i++) {
        const double y = e.process(c, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0).out;
        if (!std::isfinite(y)) { r.nans++; continue; }
        ys.push_back(y);
        s2 += y * y;
        r.peak = std::max(r.peak, std::fabs(y));
    }
    r.rms = ys.empty() ? 0.0 : std::sqrt(s2 / ys.size());
    r.hz = pitchOf(ys, sr);
    if (keep) *keep = e;
    return r;
}

// A patch that is doing nothing but feeding back: X-FADE all the way to the
// loop, no input, both bands open.
static Engine::Controls howling(double fbk, double bass, double treble,
                                double bassBoost = 0.0, double trebleBoost = 0.0) {
    Engine::Controls c;
    c.drive = 0.0;
    c.bass = bass;
    c.treble = treble;
    c.bassBoost = bassBoost;
    c.trebleBoost = trebleBoost;
    c.fbk = fbk;
    c.xfade = 1.0;
    c.inputPatched = false;
    return c;
}

// ── 1. the loop on its own ──────────────────────────────────────────────────
static void probeOsc() {
    printf("\n# osc: what the loop does with nothing patched\n");
    printf("fbk,bass,treble,bass_boost,treble_boost,hz,rms,peak\n");
    const double fbks[] = {0.3, 0.5, 0.7, 0.85, 1.0};
    const double bands[][2] = {{1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {0.5, 0.5}};
    for (double fbk : fbks) {
        for (auto& b : bands) {
            const Run r = freeRun(howling(fbk, b[0], b[1]), SR, 2.0, 1.0);
            printf("%g,%g,%g,0,0,%.1f,%.3f,%.3f\n",
                   fbk, b[0], b[1], r.hz, r.rms, r.peak);
        }
    }
    printf("# and with the boosts up, which is where the manual says the tone is\n");
    for (double boost : {0.5, 1.0}) {
        for (auto& b : bands) {
            const Run r = freeRun(howling(1.0, b[0], b[1],
                                          b[0] > 0.0 ? boost : 0.0,
                                          b[1] > 0.0 ? boost : 0.0), SR, 2.0, 1.0);
            printf("1,%g,%g,%g,%g,%.1f,%.3f,%.3f\n",
                   b[0], b[1], b[0] > 0 ? boost : 0.0, b[1] > 0 ? boost : 0.0,
                   r.hz, r.rms, r.peak);
        }
    }
}

// ── 2. the one that matters ─────────────────────────────────────────────────
// The oscillation pitch is a property of the circuit, so it may not move with
// the rate the circuit is simulated at. 176.4 kHz is 4x from 44.1 kHz, the
// lowest ratio the module offers; 1.536 MHz is 16x from 96 kHz, the highest.
static void probeSrate() {
    printf("\n# srate: the same patch at every engine rate\n");
    printf("patch,engine_hz,hz,rms,spread_vs_384k\n");
    const double rates[] = {176400.0, 192000.0, 352800.0, 384000.0,
                            768000.0, 1536000.0};
    struct Patch { const char* name; Engine::Controls c; };
    Patch patches[] = {
        {"bass", howling(1.0, 1.0, 0.0, 0.5, 0.0)},
        {"treble", howling(1.0, 0.0, 1.0, 0.0, 0.5)},
        {"both", howling(0.8, 1.0, 1.0, 0.3, 0.3)},
    };
    for (auto& p : patches) {
        double ref = 0.0;
        for (double sr : rates) {
            const Run r = freeRun(p.c, sr, 2.0, 1.0);
            if (sr == 384000.0) ref = r.hz;
            printf("%s,%.0f,%.1f,%.3f,", p.name, sr, r.hz, r.rms);
            if (ref > 0.0 && r.hz > 0.0) printf("%.3f\n", r.hz / ref);
            else printf("\n");
        }
    }
}

// ── 3. where it starts ──────────────────────────────────────────────────────
// The FBK fader is a threshold control: below it the loop only colours what
// goes through, above it the loop is the sound.
static void probeThresh() {
    printf("\n# thresh: FBK swept, both bands open\n");
    printf("fbk,hz,rms\n");
    for (int i = 0; i <= 20; i++) {
        const double fbk = i / 20.0;
        const Run r = freeRun(howling(fbk, 1.0, 1.0, 0.3, 0.3), SR, 2.0, 0.5);
        printf("%.2f,%.1f,%.4f\n", fbk, r.hz, r.rms);
    }
}

// ── 4. the tone section, with the loop open ─────────────────────────────────
static void probeEq() {
    printf("\n# eq: the two bands, FBK at zero so nothing is fed back\n");
    printf("bass,treble,bass_boost,treble_boost,f_in,gain_db\n");
    const double freqs[] = {30.0, 60.0, 120.0, 250.0, 500.0, 1000.0,
                            2000.0, 4000.0, 8000.0};
    const double settings[][4] = {
        {1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0},
        {1.0, 1.0, 0.0, 0.0}, {1.0, 1.0, 1.0, 1.0},
    };
    for (auto& s : settings) {
        for (double f0 : freqs) {
            Engine e;
            e.setSampleRate(SR);
            e.reset();
            Engine::Controls c;
            c.drive = 1.0 / 3.0;      // unity through the drive stage
            c.bass = s[0]; c.treble = s[1];
            c.bassBoost = s[2]; c.trebleBoost = s[3];
            c.fbk = 0.0;
            c.xfade = 1.0;
            c.inputPatched = true;
            const double amp = 0.2;   // small, to stay off the saturators
            const long warm = (long)(0.2 * SR), n = (long)(0.2 * SR);
            Bin b;
            for (long i = 0; i < warm + n; i++) {
                const double ph = 2.0 * M_PI * f0 * i / SR;
                const double y = e.process(c, amp * std::sin(ph),
                                           0, 0, 0, 0, 0, 0).out;
                if (i >= warm) b.add(y, ph);
            }
            printf("%g,%g,%g,%g,%g,%.2f\n", s[0], s[1], s[2], s[3], f0,
                   20.0 * std::log10(std::max(b.mag(), 1e-9) / amp));
        }
    }
}

// ── 5. the drive stage ──────────────────────────────────────────────────────
// x3 gain and soft clipping at +-5 V, which is all the manual says about it.
static void probeDrive() {
    printf("\n# drive: input level against output, fader wide open\n");
    printf("in_v,out_v,gain\n");
    for (double v = 0.1; v <= 12.0; v *= 1.5) {
        Engine e;
        e.setSampleRate(SR);
        e.reset();
        Engine::Controls c;
        c.drive = 1.0;
        c.bass = c.treble = 0.0;
        c.fbk = 0.0;
        c.xfade = 0.0;                 // the clean side of the fade
        c.xfadePostDrive = true;       // ... taken after the drive stage
        c.inputPatched = true;
        double y = 0.0;
        for (long i = 0; i < 200; i++)
            y = e.process(c, v, 0, 0, 0, 0, 0, 0).out;
        printf("%.3f,%.3f,%.3f\n", v, y, y / v);
    }
}

// ── 6. the envelope follower ────────────────────────────────────────────────
static void probeEnv() {
    printf("\n# env: rise and fall of DYNAMICS against a 5 V burst\n");
    printf("decay,detector,attack_ms,decay_ms,peak_v\n");
    for (int decay = 0; decay < 2; decay++) {
        for (int lp = 0; lp < 2; lp++) {
            Engine e;
            e.setSampleRate(SR);
            e.reset();
            Engine::Controls c;
            c.drive = 0.0;
            c.fbk = 0.0;
            c.dynLongDecay = decay != 0;
            c.dynLowpass = lp != 0;
            c.inputPatched = true;
            double peak = 0.0, attackMs = -1.0, decayMs = -1.0;
            const long burst = (long)(0.5 * SR);
            for (long i = 0; i < burst; i++) {
                const double x = 5.0 * std::sin(2.0 * M_PI * 100.0 * i / SR);
                const double d = e.process(c, x, 0, 0, 0, 0, 0, 0).dynamics;
                peak = std::max(peak, d);
                if (attackMs < 0.0 && d > 0.632 * 0.9 * peak && peak > 0.5)
                    attackMs = 1000.0 * i / SR;
            }
            const double held = peak;
            for (long i = 0; i < (long)(3.0 * SR); i++) {
                const double d = e.process(c, 0.0, 0, 0, 0, 0, 0, 0).dynamics;
                if (d < 0.368 * held) { decayMs = 1000.0 * i / SR; break; }
            }
            printf("%s,%s,%.2f,%.1f,%.3f\n", decay ? "long" : "short",
                   lp ? "lowpass" : "flat", attackMs, decayMs, peak);
        }
    }
}

// ── 7. the crossfader ───────────────────────────────────────────────────────
static void probeXfade() {
    printf("\n# xfade: the clean side against the loop side\n");
    printf("xfade,clean_gain\n");
    for (int i = 0; i <= 10; i++) {
        Engine e;
        e.setSampleRate(SR);
        e.reset();
        Engine::Controls c;
        c.drive = 1.0 / 3.0;
        c.bass = c.treble = 0.0;      // nothing comes back from the tone side
        c.fbk = 0.0;
        c.xfade = i / 10.0;
        c.xfadePostDrive = true;
        c.inputPatched = true;
        const double f0 = 200.0, amp = 1.0;
        const long warm = (long)(0.05 * SR), n = (long)(0.1 * SR);
        Bin b;
        for (long j = 0; j < warm + n; j++) {
            const double ph = 2.0 * M_PI * f0 * j / SR;
            const double y = e.process(c, amp * std::sin(ph), 0, 0, 0, 0, 0, 0).out;
            if (j >= warm) b.add(y, ph);
        }
        printf("%.1f,%.3f\n", i / 10.0, b.mag() / amp);
    }
}

// ── 8. the loop taken outside ───────────────────────────────────────────────
// Patching FBK IN breaks the internal loop. Closing it again through a delay
// of your own is what the EXT FBK section is for, and the polarity switch is
// there because half the modules you might put in it inverto.
static void probeExtLoop() {
    printf("\n# extloop: FBK IN patched, loop closed through a delay outside\n");
    printf("ext_delay_ms,polarity,cv_control,hz,rms\n");
    for (double ms : {0.5, 2.0, 10.0}) {
        for (int inv = 0; inv < 2; inv++) {
            for (int cvOut = 0; cvOut < 2; cvOut++) {
                Engine e;
                e.setSampleRate(SR);
                e.reset();
                Engine::Controls c = howling(1.0, 1.0, 1.0, 0.3, 0.3);
                c.fbkInPatched = true;
                c.invertFbkOut = inv != 0;
                c.cvControlsOut = cvOut != 0;
                const int n = (int)(ms * 1e-3 * SR);
                std::vector<double> line(n + 1, 0.0);
                int w = 0;
                std::vector<double> ys;
                const long total = (long)(3.0 * SR), meas = (long)(1.0 * SR);
                for (long i = 0; i < total; i++) {
                    const double back = line[w];
                    const Engine::Frame f =
                        e.process(c, 0.0, 0, 0, 0, 0, 0, back);
                    line[w] = f.fbkSend;
                    w = (w + 1) % (int)line.size();
                    if (i >= total - meas) ys.push_back(f.out);
                }
                double s2 = 0.0;
                for (double y : ys) s2 += y * y;
                printf("%g,%s,%s,%.1f,%.3f\n", ms, inv ? "inverted" : "normal",
                       cvOut ? "out" : "in", pitchOf(ys, SR),
                       std::sqrt(s2 / std::max<size_t>(ys.size(), 1)));
            }
        }
    }
}

// ── 9. cpu ──────────────────────────────────────────────────────────────────
static void probeCpu() {
    printf("\n# cpu: engine seconds per audio second, per oversampling ratio\n");
    printf("ratio,engine_hz,x_realtime,percent_core\n");
    for (int ratio : {4, 8, 16}) {
        const double sr = 48000.0 * ratio;
        Engine e;
        e.setSampleRate(sr);
        e.reset();
        Engine::Controls c = howling(0.9, 1.0, 1.0, 0.4, 0.4);
        c.inputPatched = true;
        const long n = (long)(5.0 * sr);
        auto t0 = std::chrono::steady_clock::now();
        double acc = 0.0;
        for (long i = 0; i < n; i++)
            acc += e.process(c, 2.0 * std::sin(i * 0.01), 0, 0, 0, 0, 0, 0).out;
        auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double audio = n / sr;
        printf("%dx,%.0f,%.1f,%.3f%s\n", ratio, sr, audio / secs,
               100.0 * secs / audio, acc == 1e300 ? " " : "");
    }
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* s) { return !only || !std::strcmp(only, s); };
    if (want("osc")) probeOsc();
    if (want("srate")) probeSrate();
    if (want("thresh")) probeThresh();
    if (want("eq")) probeEq();
    if (want("drive")) probeDrive();
    if (want("env")) probeEnv();
    if (want("xfade")) probeXfade();
    if (want("extloop")) probeExtLoop();
    if (want("cpu")) probeCpu();
    return 0;
}
