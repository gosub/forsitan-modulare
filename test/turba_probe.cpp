// turba_probe — measurement harness for the chaotic bank.
//
// Not run by `make check`; this is the bench that the numbers in doc/turba.md
// come from. It measures:
//   levels    output RMS/peak and the CV out range, per topology
//   flow      what the flow macro does to level, brightness and chaos
//   macros    spectral centroid against each macro knob
//   lyapunov  divergence rate of two runs from almost the same state, the
//             evidence that this thing is actually chaotic and not just busy
//   cpu       real time per sample against the audio budget
//
// Usage: ./turba_probe [levels|flow|macros|lyapunov|cpu|all]

#include "smoke_harness.hpp"
#include "../src/turba.cpp"

#include <ctime>
#include <vector>

static void settle(Turba& m, long& frame, double seconds) {
    const int n = (int)(seconds * SR);
    for (int i = 0; i < n; i++) m.process(makeArgs(frame++));
}

struct Meas {
    double rms = 0, peak = 0, centroid = 0, cvPeak = 0, cvRms = 0;
};

// Spectral centroid the cheap way: the ratio of the RMS of the first
// difference to the RMS of the signal is proportional to a mean frequency.
static Meas measure(Turba& m, long& frame, double seconds) {
    const int n = (int)(seconds * SR);
    double s2 = 0, d2 = 0, cv2 = 0;
    double peak = 0, cvPeak = 0;
    float prev = 0.f;
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(frame++));
        const float v = m.outputs[Turba::LEFT_OUTPUT].getVoltage();
        const float c = m.outputs[Turba::CV_OUTPUT].getVoltage();
        s2 += (double)v * v;
        const float d = v - prev;
        d2 += (double)d * d;
        prev = v;
        cv2 += (double)c * c;
        if (std::fabs(v) > peak) peak = std::fabs(v);
        if (std::fabs(c) > cvPeak) cvPeak = std::fabs(c);
    }
    Meas r;
    r.rms = std::sqrt(s2 / n);
    r.peak = peak;
    r.cvRms = std::sqrt(cv2 / n);
    r.cvPeak = cvPeak;
    // f = (SR / 2pi) * asin(0.5 * rms(diff)/rms(sig)) is exact for a sine;
    // good enough as a relative brightness figure here.
    const double ratio = r.rms > 1e-9 ? std::sqrt(d2 / n) / r.rms : 0.0;
    r.centroid = (SR / (2 * M_PI)) * std::asin(std::min(0.5 * ratio, 1.0));
    return r;
}

static void probeLevels() {
    printf("\n== levels, per topology (level knob at 50%%) ==\n");
    printf("topology       rms      peak     centroid  cv_rms   cv_peak\n");
    static const char* name[3] = {"loop", "pre", "bare"};
    for (int t = 0; t < 3; t++) {
        Turba m;
        m.params[Turba::MODE_PARAM].setValue((float)t);
        long frame = 0;
        settle(m, frame, 3.0);
        Meas r = measure(m, frame, 8.0);
        printf("%-12s %7.3f  %7.3f  %8.0f  %7.3f  %7.3f\n",
               name[t], r.rms, r.peak, r.centroid, r.cvRms, r.cvPeak);
    }
}

static void probeFlow() {
    printf("\n== flow macro ==\n");
    printf("flow      rms      peak    centroid\n");
    for (int i = -4; i <= 4; i++) {
        const float f = i / 4.f;
        Turba m;
        m.params[Turba::FLOW_PARAM].setValue(f);
        long frame = 0;
        settle(m, frame, 4.0);
        Meas r = measure(m, frame, 8.0);
        printf("%+5.2f  %7.3f  %7.3f  %8.0f\n", f, r.rms, r.peak, r.centroid);
    }
}

static void probeMacros() {
    printf("\n== the other three macros ==\n");
    static const int knob[3] = {Turba::PITCH_PARAM, Turba::CUTOFF_PARAM,
                                Turba::DELAY_PARAM};
    static const char* name[3] = {"pitch", "cutoff", "delay"};
    for (int k = 0; k < 3; k++) {
        printf("%-8s  knob      rms    centroid\n", name[k]);
        for (int i = -2; i <= 2; i++) {
            const float f = i / 2.f;
            Turba m;
            m.params[knob[k]].setValue(f);
            long frame = 0;
            settle(m, frame, 3.0);
            Meas r = measure(m, frame, 6.0);
            printf("         %+5.2f  %7.3f  %8.0f\n", f, r.rms, r.centroid);
        }
    }
}

// Two engines from the same state, one nudged by 1e-6 on a single channel's
// phase. If the bank is chaotic the difference grows exponentially; the
// slope of log|diff| is a largest-Lyapunov-exponent estimate in nepers per
// second. Positive means chaos, near zero means it is only quasi-periodic.
static void probeLyapunov() {
    printf("\n== divergence (largest Lyapunov estimate) ==\n");
    printf("flow    lambda(1/s)   verdict\n");
    for (int i = -2; i <= 2; i++) {
        const float f = i / 2.f;
        Turba a, b;
        a.params[Turba::FLOW_PARAM].setValue(f);
        b.params[Turba::FLOW_PARAM].setValue(f);
        long fa = 0, fb = 0;
        settle(a, fa, 4.0);
        settle(b, fb, 4.0);
        // copy a's engine state into b, then nudge
        b.eng = a.eng;
        b.eng.phase[0] += 1e-6f;

        const double d0 = 1e-6;
        double sum = 0.0;
        int renorms = 0;
        const int step = (int)(0.02 * SR);
        for (int seg = 0; seg < 150; seg++) {
            for (int n = 0; n < step; n++) {
                a.process(makeArgs(fa++));
                b.process(makeArgs(fb++));
            }
            double d = 0;
            for (int c = 0; c < NCH; c++) {
                const double e = a.eng.y[c] - b.eng.y[c];
                d += e * e;
            }
            d = std::sqrt(d);
            if (d < 1e-14) d = 1e-14;
            if (d > 1e-3) {
                // renormalise back onto the reference trajectory
                sum += std::log(d / d0);
                renorms++;
                const double scale = d0 / d;
                for (int c = 0; c < NCH; c++)
                    b.eng.y[c] = a.eng.y[c] + (b.eng.y[c] - a.eng.y[c]) * scale;
            }
        }
        const double seconds = 150 * 0.02;
        const double lambda = sum / seconds;
        printf("%+5.2f  %11.2f   %s\n", f, lambda,
               lambda > 1.0 ? "chaotic" : (lambda > 0.05 ? "weakly chaotic"
                                                         : "not chaotic"));
        (void)renorms;
    }
}

static void probeCpu() {
    printf("\n== cpu ==\n");
    Turba m;
    long frame = 0;
    settle(m, frame, 1.0);
    const int n = (int)(20 * SR);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) m.process(makeArgs(frame++));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double secs = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
    printf("%.1f ns/sample, %.2f%% of one core at %.0f kHz\n",
           secs / n * 1e9, 100.0 * secs / (n / SR), SR / 1000.0);
}

int main(int argc, char** argv) {
    rack::random::init();
    const char* which = argc > 1 ? argv[1] : "all";
    const bool all = !std::strcmp(which, "all");
    if (all || !std::strcmp(which, "levels")) probeLevels();
    if (all || !std::strcmp(which, "flow")) probeFlow();
    if (all || !std::strcmp(which, "macros")) probeMacros();
    if (all || !std::strcmp(which, "lyapunov")) probeLyapunov();
    if (all || !std::strcmp(which, "cpu")) probeCpu();
    return 0;
}
