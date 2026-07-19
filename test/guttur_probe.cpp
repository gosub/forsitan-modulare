// guttur_probe — diagnostic maps for the guttur engine.
//
// Not a pass/fail check (that is smoke_guttur): this prints two tables that
// explain *why* the module sounds the way it does at a given setting. Both
// exist because they are what found the 2026-07-19 bugs.
//
//   shapers   the distortion transfer curves. Every shaper here sits inside
//             the feedback loop, so one that is non-monotonic folds instead
//             of clipping, and one that is unbounded diverges the loop. The
//             kvraudio tanh fit did exactly that (railed the output within
//             100 ms); fastatan still folds above |v| = 1.89 by choice.
//
//   forcing   dead-air fraction against forcing frequency and against loop
//             drive. Silence is a two-factor failure: an overdriven feedback
//             loop collapses the Duffing onto a fixed point, and only a
//             forcing sine at audio rate restarts it. Either factor alone is
//             survivable, which is why the pre-fix module needed both the
//             folding shaper and the doubled bank to go 83% silent. Run this
//             before assuming a bug: it separates "guttur is broken" from
//             "you turned TONE down with the loop cranked".
//
// Usage: ./guttur_probe [shapers|forcing]   (default: both)

#include "smoke_harness.hpp"
#include "../src/guttur.cpp"
#include <cstdlib>

static const char* kShaperNames[] = {
    "hard clip", "soft clip", "atan (fold)", "atan approx",
    "tanh approx", "atan (exact)",
};
static const int kShaperCount = 6;

static void shapers() {
    printf("# distortion transfer curves\n");
    printf("# a shaper that stops rising has folded; one that keeps rising\n"
           "# past ~1.6 is unbounded and will diverge the feedback loop.\n");
    printf("%8s", "v");
    for (int t = 0; t < kShaperCount; t++) printf("%14s", kShaperNames[t]);
    printf("\n");
    for (double v : {0.25, 0.5, 0.63, 1.0, 1.5, 1.89, 3.0, 6.0, 12.0, 50.0}) {
        printf("%8.2f", v);
        for (int t = 0; t < kShaperCount; t++)
            printf("%14.4f", guttur_dsp::distortion(v, t));
        printf("\n");
    }
    // flag the two failure modes numerically rather than by eyeball
    printf("\n%-14s %10s %10s %s\n", "shaper", "max|out|", "monotonic", "verdict");
    for (int t = 0; t < kShaperCount; t++) {
        double prev = 0, maxOut = 0;
        bool mono = true;
        for (double v = 0; v <= 60.0; v += 0.01) {
            double y = guttur_dsp::distortion(v, t);
            // 1e-6, not an epsilon: a shaper that saturates wobbles in the
            // last bits at its plateau, while a real fold drops by orders of
            // magnitude (the fastatan one falls 0.945 -> 0.07)
            if (y < prev - 1e-6) mono = false;
            maxOut = std::max(maxOut, std::fabs(y));
            prev = y;
        }
        printf("%-14s %10.4f %10s %s\n", kShaperNames[t], maxOut,
               mono ? "yes" : "NO",
               !mono ? "folds" : (maxOut > 10.0 ? "UNBOUNDED" : "saturates"));
    }
}

// TONE knob position for a given omega (the knob maps 1e-4..1 over 4 decades)
static float toneFor(double omega) {
    return (float) (std::log10(omega / 1e-4) / 4.0);
}

struct Result { double rms, peak, dead; };

// 20 s of output after a 2 s settle; dead = fraction of 50 ms blocks below
// -60 dBFS, i.e. time spent in the engine's silent fixed point
static Result measure(double omega, double dt, int dist, float gainB) {
    Guttur m;
    m.params[Guttur::TONE_PARAM].setValue(toneFor(omega));
    m.params[Guttur::RATE_PARAM].setValue((float) dt);
    m.params[Guttur::DIST_PARAM].setValue((float) dist);
    m.params[Guttur::GAINB_PARAM].setValue(gainB);
    long frame = 0;
    for (long i = 0; i < (long)(2 * SR); i++) m.process(makeArgs(frame++));

    double sum2 = 0, peak = 0, bsum2 = 0;
    long n = 0, quiet = 0, blocks = 0, bn = 0;
    const long kBlock = (long)(0.05 * SR);
    for (long i = 0; i < (long)(20 * SR); i++) {
        m.process(makeArgs(frame++));
        double v = m.outputs[Guttur::OUT_OUTPUT].getVoltage();
        sum2 += v * v; peak = std::max(peak, std::fabs(v)); n++;
        bsum2 += v * v;
        if (++bn == kBlock) {
            if (std::sqrt(bsum2 / bn) < 0.01) quiet++;
            blocks++; bsum2 = 0; bn = 0;
        }
    }
    return {std::sqrt(sum2 / n), peak, blocks ? (double) quiet / blocks : 1.0};
}

static void forcing() {
    printf("# 1. output vs forcing frequency, omega*dt*44100/(2*pi) Hz\n");
    printf("# TONE and RATE multiply, so either knob alone moves this.\n\n");
    printf("%10s %8s %10s %9s %8s %8s %7s\n",
           "omega", "dt", "f (Hz)", "rms", "peak", "crest", "dead");
    struct Pt { double omega, dt; };
    Pt pts[] = {
        {2e-4, 0.03}, {1e-3, 0.03}, {0.01, 0.03}, {0.1, 0.03}, {1.0, 0.03},
        {0.02, 0.1}, {0.02, 0.5}, {0.02, 1.0}, {0.02, 2.0},
        {1e-4, 5.0}, {1e-3, 5.0}, {0.01, 5.0}, {0.02, 5.0}, {0.1, 5.0},
        {1.0, 5.0}, {0.02, 10.0},
    };
    for (const Pt& p : pts) {
        Result r = measure(p.omega, p.dt, 1, 0.f);
        printf("%10.4g %8.3g %10.1f %9.4f %8.3f %8.1f %6.0f%%\n",
               p.omega, p.dt, p.omega * p.dt * 44100.0 / (2 * M_PI),
               r.rms, r.peak, r.rms > 0 ? r.peak / r.rms : 0.0, 100.0 * r.dead);
    }

    // Sub-audio forcing alone does NOT kill the engine — excess loop drive
    // does, and sub-audio forcing then fails to restart it. Measured
    // 2026-07-19: the pre-fix 83% dead air needed both the folding shaper
    // and the doubled bank; either one tamed brings it back to 0%.
    printf("\n# 2. what actually causes the silence: loop drive\n");
    printf("# The Duffing collapses onto a fixed point when the feedback is\n"
           "# overdriven; the forcing sine then has to restart it, which a\n"
           "# sub-audio one cannot do. Vary drive, not just frequency.\n\n");
    printf("%-14s %-24s %9s %8s %7s\n",
           "forcing", "loop drive", "rms", "peak", "dead");
    struct Case { const char* forcing; double omega, dt;
                  const char* drive; int dist; float gainB; };
    Case cases[] = {
        {"sub-audio", 2e-4, 0.03, "soft clip, bank B off", 1, 0.f},
        {"sub-audio", 2e-4, 0.03, "soft clip, bank B on",  1, 1.f},
        {"sub-audio", 2e-4, 0.03, "folding atan, B off",   2, 0.f},
        {"sub-audio", 2e-4, 0.03, "folding atan, B on",    2, 1.f},
        {"~700 Hz",   0.02, 5.0,  "soft clip, bank B off", 1, 0.f},
        {"~700 Hz",   0.02, 5.0,  "soft clip, bank B on",  1, 1.f},
        {"~700 Hz",   0.02, 5.0,  "folding atan, B off",   2, 0.f},
        {"~700 Hz",   0.02, 5.0,  "folding atan, B on",    2, 1.f},
    };
    for (const Case& c : cases) {
        Result r = measure(c.omega, c.dt, c.dist, c.gainB);
        printf("%-14s %-24s %9.4f %8.3f %6.0f%%\n",
               c.forcing, c.drive, r.rms, r.peak, 100.0 * r.dead);
    }

    printf("\n# defaults are omega 0.02 / dt 5 (~700 Hz), soft clip, bank B\n"
           "# muted. The 44100 is the originals' reference rate, not an\n"
           "# assumed SR: t advances by dt*(44100/Fs) per sample, so f in Hz\n"
           "# is sample-rate invariant (verified 44.1-192 kHz).\n");
}

int main(int argc, char** argv) {
    rack::random::init();
    bool all = (argc < 2);
    for (int i = 1; i < argc || all; i++) {
        const char* what = all ? "" : argv[i];
        if (all || !std::strcmp(what, "shapers")) shapers();
        if (all) printf("\n");
        if (all || !std::strcmp(what, "forcing")) forcing();
        if (all) break;
    }
    return 0;
}
