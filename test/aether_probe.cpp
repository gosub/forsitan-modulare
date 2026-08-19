// aether_probe — measurement harness for the broken transmission line.
//
// This is where the numbers behind src/aether_dsp.hpp come from. The module
// is built on two claims about the circuit, and both are checkable:
//
//   * the transmitter is a synchronous charge-balance converter, so its pulse
//     rate is (1 + u)/4 * f_carrier and it saturates at half the clock;
//   * the receiver is a PLL, so with the clocks matched the loop's control
//     voltage — the audio output — comes back as the input, and with them
//     mismatched by R = f_carrier/f_demod it comes back as R*(1+u) - 1.
//
// If those hold, everything the manual describes (matched clocks pass the
// signal, a slow demodulator clock kills the output, the ratio is the
// distortion) is a consequence rather than a tuning.
//
//   aether_probe [svfc|lock|ratio|capture|audio|tone|osc|alias|srate|cpu]
//
// With no argument it runs the lot. Rack is not involved: src/aether_dsp.hpp
// is free of Rack headers.

#include "../src/aether_dsp.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace aether;

// The engine always runs oversampled; the probe drives it at 4x 48k and
// measures there.
static const double SR = 192000.0;

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

// A 6th order Butterworth at 20 kHz, so "residual" means what survives into
// the audio band rather than the loop ripple the decimator will remove.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void lowpass(double fc, double sr, double q) {
        const double w = 2.0 * M_PI * fc / sr;
        const double alpha = std::sin(w) / (2.0 * q);
        const double c = std::cos(w);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 - c) / 2.0 / a0;
        b1 = (1.0 - c) / a0;
        b2 = b0;
        a1 = -2.0 * c / a0;
        a2 = (1.0 - alpha) / a0;
    }
    double operator()(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct BandLimit {
    Biquad s[3];
    BandLimit(double sr) {
        const double qs[3] = {0.5176, 0.7071, 1.9319};
        for (int i = 0; i < 3; i++) s[i].lowpass(20000.0, sr, qs[i]);
    }
    double operator()(double x) { return s[2](s[1](s[0](x))); }
};

static double rms(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x * x;
    return v.empty() ? 0.0 : std::sqrt(s / v.size());
}

// knob position that puts a clock at f Hz
static double knobFor(double hz) {
    return clampd(std::log2(hz / kClkMin) / kClkOctaves, 0.0, 1.0);
}

static Engine::Controls basic(double carrierHz, double demodHz,
                              double inLevel, int type) {
    Engine::Controls c;
    c.inLevel = inLevel;
    c.carrierKnob = knobFor(carrierHz);
    c.demodKnob = knobFor(demodHz);
    c.carrierCvAmt = 0.0;
    c.demodCvAmt = 0.0;
    c.tone = 1.0;
    c.type = type;
    c.inputPatched = true;
    return c;
}

static const char* typeName(int t) {
    return t == PD_XOR ? "1-xor" : (t == PD_PFD ? "2-pfd" : "3-rs");
}

// Run the engine on a constant input for `seconds`, return the mean of the
// loop's control voltage over the last third.
static double settle(Engine& e, Engine::Controls c, double inVolts,
                     double seconds) {
    const long n = (long)(seconds * SR);
    double sum = 0.0;
    long cnt = 0;
    for (long i = 0; i < n; i++) {
        e.process(c, inVolts, 0.0, 0.0, 0.0, 0.0);
        if (i > 2 * n / 3) { sum += e.loop; cnt++; }
    }
    return cnt ? sum / cnt : 0.0;
}

// ── 1. the transmitter ──────────────────────────────────────────────────────
// Count pulses out of the converter alone, against (1 + u)/4 * f_clock.
static void probeSvfc() {
    printf("\n# svfc: pulse rate against (1+u)/4 * f_clock\n");
    printf("f_clock,u,predicted_hz,measured_hz,rel_err\n");
    const double clocks[] = {1000.0, 48000.0, 320000.0};
    const double us[] = {-1.0, -0.5, 0.0, 0.5, 0.9, 1.0};
    for (double fc : clocks) {
        for (double u : us) {
            Svfc s;
            const long ticks = 200000;
            long pulses = 0;
            for (long i = 0; i < ticks; i++)
                if (s.tick(u)) pulses++;
            const double measured = (double)pulses / ticks * fc;
            const double predicted = 0.25 * (1.0 + u) * fc;
            const double err = predicted > 0.0
                ? std::fabs(measured - predicted) / predicted : measured;
            printf("%g,%g,%.1f,%.1f,%.3g\n", fc, u, predicted, measured, err);
        }
    }
}

// ── 2. lock ─────────────────────────────────────────────────────────────────
// Clocks matched: the control voltage should come back as the input, for
// every phase comparator.
static void probeLock() {
    printf("\n# lock: matched clocks, control voltage against the input\n");
    printf("type,f_clock,u,loop,err\n");
    const double clocks[] = {12000.0, 48000.0, 192000.0};
    const double us[] = {-0.6, -0.3, 0.0, 0.3, 0.6, 0.9};
    for (int t = 0; t < 3; t++) {
        for (double fc : clocks) {
            for (double u : us) {
                Engine e;
                e.setSampleRate(SR);
                e.reset();
                Engine::Controls c = basic(fc, fc, 1.0, t);
                const double got = settle(e, c, u * kInVolts, 0.35);
                printf("%s,%g,%g,%.4f,%.4f\n",
                       typeName(t), fc, u, got, got - u);
            }
        }
    }
}

// ── 3. the ratio law ────────────────────────────────────────────────────────
// v_out = R*(1 + u) - 1 with R = f_carrier/f_demod, clipped at the rails.
static void probeRatio() {
    printf("\n# ratio: control voltage against R*(1+u)-1\n");
    printf("type,R,u,predicted,loop,err\n");
    const double carrier = 48000.0;
    const double ratios[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
    const double us[] = {-0.5, 0.0, 0.5};
    for (int t = 0; t < 3; t++) {
        for (double R : ratios) {
            for (double u : us) {
                Engine e;
                e.setSampleRate(SR);
                e.reset();
                Engine::Controls c = basic(carrier, carrier / R, 1.0, t);
                const double got = settle(e, c, u * kInVolts, 0.35);
                const double want = clampd(R * (1.0 + u) - 1.0, -1.0, 1.0);
                printf("%s,%g,%g,%.4f,%.4f,%.4f\n",
                       typeName(t), R, u, want, got, got - want);
            }
        }
    }
}

// ── 4. capture range ────────────────────────────────────────────────────────
// Sweep the demodulator clock under a fixed carrier and report how far the
// loop sits from lock. The manual's "if this is set too low no output will be
// produced" should show up as the bottom of the range falling apart.
static void probeCapture() {
    printf("\n# capture: demodulator clock swept under a fixed 48 kHz carrier\n");
    printf("type,f_demod,ratio,loop,want,lock_err\n");
    const double carrier = 48000.0;
    const double u = 0.4;
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i <= 16; i++) {
            const double fd = 6000.0 * std::pow(2.0, i / 4.0);   // 6k .. 96k
            Engine e;
            e.setSampleRate(SR);
            e.reset();
            Engine::Controls c = basic(carrier, fd, 1.0, t);
            const double got = settle(e, c, u * kInVolts, 0.35);
            const double want = clampd(carrier / fd * (1.0 + u) - 1.0, -1.0, 1.0);
            printf("%s,%.0f,%.3f,%.4f,%.4f,%.4f\n",
                   typeName(t), fd, carrier / fd, got, want, got - want);
        }
    }
}

// ── 5. audio ────────────────────────────────────────────────────────────────
// A sine through matched clocks, well above it: fundamental amplitude and
// what is left over once it is removed.
static void probeAudio() {
    printf("\n# audio: 220 Hz sine, matched clocks, level and in-band residual\n");
    printf("type,f_clock,tone,out_vpk,gain,residual_db\n");
    const double f0 = 220.0;
    const double clocks[] = {6000.0, 24000.0, 96000.0, 320000.0};
    const double tones[] = {0.35, 0.7, 1.0};
    for (int t = 0; t < 3; t++) {
        for (double fc : clocks) {
            for (double tone : tones) {
                Engine e;
                e.setSampleRate(SR);
                e.reset();
                Engine::Controls c = basic(fc, fc, 1.0, t);
                c.tone = tone;
                const double amp = 4.0;
                const long warm = (long)(0.2 * SR), n = (long)(0.5 * SR);
                Bin b;
                BandLimit lp(SR);
                std::vector<double> ys;
                ys.reserve(n);
                for (long i = 0; i < warm + n; i++) {
                    const double ph = 2.0 * M_PI * f0 * i / SR;
                    const double y = lp(e.process(c, amp * std::sin(ph),
                                                  0.0, 0.0, 0.0, 0.0).out);
                    if (i >= warm) { b.add(y, ph); ys.push_back(y); }
                }
                const double out = b.mag();
                double s2 = 0.0;
                for (size_t i = 0; i < ys.size(); i++) {
                    const double ph = 2.0 * M_PI * f0 * (warm + i) / SR;
                    s2 += (ys[i] - out * std::sin(ph)) * (ys[i] - out * std::sin(ph));
                }
                const double res = std::sqrt(s2 / ys.size());
                const double db = out > 0.0
                    ? 20.0 * std::log10(res / (out / M_SQRT2)) : 0.0;
                printf("%s,%.0f,%g,%.3f,%.3f,%.1f\n",
                       typeName(t), fc, tone, out, out / amp, db);
            }
        }
    }
}

// ── 6. tone ─────────────────────────────────────────────────────────────────
// The loop filter is also the output filter, so TONE has to show up twice:
// as bandwidth, and as how well the loop tracks a moving signal.
static void probeTone() {
    printf("\n# tone: recovered amplitude against input frequency\n");
    printf("tone,f_in,out_vpk,gain_db\n");
    const double freqs[] = {50.0, 100.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0};
    const double tones[] = {0.0, 0.5, 1.0};
    for (double tone : tones) {
        for (double f0 : freqs) {
            Engine e;
            e.setSampleRate(SR);
            e.reset();
            Engine::Controls c = basic(96000.0, 96000.0, 1.0, PD_PFD);
            c.tone = tone;
            const double amp = 3.0;
            const long warm = (long)(0.2 * SR), n = (long)(0.3 * SR);
            Bin b;
            for (long i = 0; i < warm + n; i++) {
                const double ph = 2.0 * M_PI * f0 * i / SR;
                const double y = e.process(c, amp * std::sin(ph),
                                           0.0, 0.0, 0.0, 0.0).out;
                if (i >= warm) b.add(y, ph);
            }
            printf("%g,%g,%.4f,%.2f\n", tone, f0, b.mag(),
                   20.0 * std::log10(std::max(b.mag(), 1e-9) / amp));
        }
    }
}

// ── 7. the standalone oscillator ────────────────────────────────────────────
// Nothing patched: the input jack is a DC bias, the transmitter is a VCO and
// the receiver chases it. There should be sound, and it should move with both
// clocks.
static void probeOsc() {
    printf("\n# osc: nothing patched, output level and dominant partial\n");
    printf("type,in_level,f_carrier,f_demod,out_rms,peak_hz\n");
    const double pairs[][2] = {{2000.0, 2000.0}, {2000.0, 3000.0},
                               {12000.0, 5000.0}, {48000.0, 47000.0},
                               {48000.0, 24000.0}};
    for (int t = 0; t < 3; t++) {
        for (double lvl : {0.3, 0.7}) {
            for (auto& p : pairs) {
                Engine e;
                e.setSampleRate(SR);
                e.reset();
                Engine::Controls c = basic(p[0], p[1], lvl, t);
                c.inputPatched = false;
                const long warm = (long)(0.2 * SR), n = (long)(0.3 * SR);
                std::vector<double> ys;
                ys.reserve(n);
                for (long i = 0; i < warm + n; i++) {
                    const double y = e.process(c, 0.0, 0.0, 0.0, 0.0, 0.0).out;
                    if (i >= warm) ys.push_back(y);
                }
                // coarse peak pick over a log sweep of probe frequencies
                double best = 0.0, bestHz = 0.0;
                for (double hz = 20.0; hz < 12000.0; hz *= 1.06) {
                    Bin b;
                    for (size_t i = 0; i < ys.size(); i++)
                        b.add(ys[i], 2.0 * M_PI * hz * i / SR);
                    if (b.mag() > best) { best = b.mag(); bestHz = hz; }
                }
                printf("%s,%g,%.0f,%.0f,%.4f,%.0f\n",
                       typeName(t), lvl, p[0], p[1], rms(ys), bestHz);
            }
        }
    }
}

// ── 8. aliasing ─────────────────────────────────────────────────────────────
// The module is made of aliasing, but it has to be the carrier's, not the
// host's. A low carrier folds the input the way a low sample rate does; the
// probe reports where the folded partial lands so it can be checked against
// |f_carrier/2 - f_in| rather than against the host rate.
static void probeAlias() {
    printf("\n# alias: 1 kHz sine, carrier swept: fundamental and image\n");
    printf("f_carrier,fold_hz,fund_vpk,image_vpk,image_db\n");
    const double f0 = 1000.0;
    for (int i = 0; i <= 8; i++) {
        const double fc = 3000.0 * std::pow(2.0, i / 2.0);
        Engine e;
        e.setSampleRate(SR);
        e.reset();
        Engine::Controls c = basic(fc, fc, 1.0, PD_PFD);
        const double amp = 3.0;
        const long warm = (long)(0.2 * SR), n = (long)(0.3 * SR);
        std::vector<double> ys;
        ys.reserve(n);
        for (long j = 0; j < warm + n; j++) {
            const double ph = 2.0 * M_PI * f0 * j / SR;
            const double y = e.process(c, amp * std::sin(ph),
                                       0.0, 0.0, 0.0, 0.0).out;
            if (j >= warm) ys.push_back(y);
        }
        const double fold = std::fabs(fc / 4.0 - f0);   // pulse rate sits at fc/4
        Bin bf, bi;
        for (size_t j = 0; j < ys.size(); j++) {
            bf.add(ys[j], 2.0 * M_PI * f0 * j / SR);
            bi.add(ys[j], 2.0 * M_PI * fold * j / SR);
        }
        printf("%.0f,%.0f,%.4f,%.4f,%.1f\n", fc, fold, bf.mag(), bi.mag(),
               20.0 * std::log10(std::max(bi.mag(), 1e-9)
                                 / std::max(bf.mag(), 1e-9)));
    }
}

// ── 9. sample rate ──────────────────────────────────────────────────────────
// Nothing in the engine may depend on the grid it is run on: the clocks are
// scheduled in continuous time and the loop filter is integrated between the
// ticks, so the same patch has to measure the same at every rate.
static void probeSrate() {
    printf("\n# srate: one patch measured at four engine rates\n");
    printf("type,engine_hz,out_vpk,gain,residual_db\n");
    const double rates[] = {88200.0, 176400.0, 192000.0, 384000.0};
    const double f0 = 220.0;
    for (int t = 0; t < 3; t++) {
        for (double sr : rates) {
            Engine e;
            e.setSampleRate(sr);
            e.reset();
            Engine::Controls c = basic(96000.0, 96000.0, 1.0, t);
            c.tone = 0.7;
            const double amp = 4.0;
            const long warm = (long)(0.2 * sr), n = (long)(0.5 * sr);
            Bin b;
            BandLimit lp(sr);
            std::vector<double> ys;
            ys.reserve(n);
            for (long i = 0; i < warm + n; i++) {
                const double ph = 2.0 * M_PI * f0 * i / sr;
                const double y = lp(e.process(c, amp * std::sin(ph),
                                              0.0, 0.0, 0.0, 0.0).out);
                if (i >= warm) { b.add(y, ph); ys.push_back(y); }
            }
            const double out = b.mag();
            double s2 = 0.0;
            for (size_t i = 0; i < ys.size(); i++) {
                const double ph = 2.0 * M_PI * f0 * (warm + i) / sr;
                s2 += (ys[i] - out * std::sin(ph)) * (ys[i] - out * std::sin(ph));
            }
            const double res = std::sqrt(s2 / ys.size());
            printf("%s,%.0f,%.3f,%.3f,%.1f\n", typeName(t), sr, out, out / amp,
                   out > 0.0 ? 20.0 * std::log10(res / (out / M_SQRT2)) : 0.0);
        }
    }
}

// ── 10. cpu ──────────────────────────────────────────────────────────────────
static void probeCpu() {
    printf("\n# cpu: engine seconds per audio second at 4x oversampling\n");
    printf("f_carrier,f_demod,x_realtime,percent_core\n");
    const double pairs[][2] = {{1000.0, 1000.0}, {48000.0, 48000.0},
                               {320000.0, 320000.0}};
    for (auto& p : pairs) {
        Engine e;
        e.setSampleRate(SR);
        e.reset();
        Engine::Controls c = basic(p[0], p[1], 1.0, PD_XOR);
        const long n = (long)(5.0 * SR);
        auto t0 = std::chrono::steady_clock::now();
        double acc = 0.0;
        for (long i = 0; i < n; i++)
            acc += e.process(c, 2.0 * std::sin(i * 0.01), 0.0, 0.0, 0.0, 0.0).out;
        auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double audio = n / SR;
        printf("%.0f,%.0f,%.1f,%.3f%s\n", p[0], p[1], audio / secs,
               100.0 * secs / audio, acc == 1e300 ? " " : "");
    }
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* s) { return !only || !std::strcmp(only, s); };
    if (want("svfc")) probeSvfc();
    if (want("lock")) probeLock();
    if (want("ratio")) probeRatio();
    if (want("capture")) probeCapture();
    if (want("audio")) probeAudio();
    if (want("tone")) probeTone();
    if (want("osc")) probeOsc();
    if (want("alias")) probeAlias();
    if (want("srate")) probeSrate();
    if (want("cpu")) probeCpu();
    return 0;
}
