// viginti_invariants - property-based checks for the KORG35 Rev. 2 filter.
//
// smoke_viginti checks the module at fixed points; viginti_probe measures it
// and is where the thresholds below came from. This harness asserts the
// properties that must hold *everywhere*, over randomized parameters, and it
// is the one that would catch a broken Lambert-W or a discretization that has
// drifted away from the continuous model.
//
// Failures print the offending case to stderr, and the RNG is seeded (see
// --seed) so any failure reproduces exactly.
//
// ── the invariants ─────────────────────────────────────────────────────────
//
// The Lambert-W solver:
//   W1 identity    W(z)exp(W(z)) = z over 24 decades of z
//   W2 exp_form    W(exp(t)) agrees with W() where both can be evaluated, and
//                  keeps solving past the t where exp(t) overflows
//
// The diode nonlinearity (paper section 3.1):
//   N1 odd         eta(-x) = -eta(x) exactly
//   N2 sign        x*eta(x) >= 0
//   N3 limits      eta(x)/x rises monotonically from 3*alpha*beta/(4+4beta)
//                  at zero to 3*alpha/4 asymptotically, and never leaves that
//                  interval
//   N4 continuous  no step where the small-signal expansion is spliced in
//
// The discretization:
//   D1 det         the 2x2 solve stays far from singular over the whole
//                  parameter range
//   D2 converges   the discrete-gradient scheme approaches the RK4 reference
//                  as the sample rate rises, at every cutoff and resonance
//   D3 linear      at a level where the diodes never conduct it matches the
//                  analytic two-pole response
//
// The system:
//   S1 stable      below the paper's threshold the zero-input state decays
//   S2 oscillates  above it the state falls into a bounded limit cycle
//   S3 finite      hot input and audio-rate modulation of both parameters
//                  stay finite and bounded
//   S4 nonlinear   the same waveform at different levels is not the same
//                  filter: the resonant peak falls as the level rises, which
//                  a linear filter plus an output waveshaper could not do

#include "../src/viginti_dsp.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace viginti;

static int failures = 0;
static int gScale = 1;
static uint32_t gSeed = 0x9e3779b9u;

static void report(const char* check, double value, bool pass) {
    printf("viginti,%s,%g,%s\n", check, value, pass ? "PASS" : "FAIL");
    if (!pass) failures++;
}

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double uni() { return (double)(next() >> 8) * (1.0 / 16777216.0); }
    double range(double lo, double hi) { return lo + (hi - lo) * uni(); }
    double logRange(double lo, double hi) {
        return lo * std::pow(hi / lo, uni());
    }
};

// ── the Lambert-W solver ────────────────────────────────────────────────────
static void testLambertW() {
    Rng r(gSeed);
    double worst = 0.0, worstZ = 0.0;
    const int n = 4000 * gScale;
    for (int i = 0; i < n; i++) {
        const double z = r.logRange(1e-12, 1e12);
        const double w = lambertW0(z);
        const double res = std::fabs(w * std::exp(w) - z) / z;
        if (res > worst) { worst = res; worstZ = z; }
    }
    if (worst > 1e-13) fprintf(stderr, "  W1 worst at z=%.17g\n", worstZ);
    report("W1_identity", worst, worst < 1e-13);
    report("W1_zero", lambertW0(0.0), lambertW0(0.0) == 0.0);

    worst = 0.0;
    for (int i = 0; i < n; i++) {
        const double t = r.range(-40.0, 700.0);
        const double a = lambertW0Exp(t);
        const double b = lambertW0(std::exp(t));
        worst = std::max(worst, std::fabs(a - b) / a);
    }
    report("W2_exp_form", worst, worst < 1e-13);
    // past the overflow point there is nothing to compare against, so check
    // the defining identity in logs: log(w) + w = t
    worst = 0.0;
    for (int i = 0; i < n; i++) {
        const double t = r.logRange(710.0, 1e9);
        const double w = lambertW0Exp(t);
        worst = std::max(worst, std::fabs(std::log(w) + w - t) / t);
    }
    report("W2_no_overflow", worst, worst < 1e-13);
}

// ── the nonlinearity ────────────────────────────────────────────────────────
static void testNonlinearity() {
    Rng r(gSeed ^ 0x2545f491u);
    const int n = 4000 * gScale;
    double oddErr = 0.0, signErr = 0.0, boundErr = 0.0, monoErr = 0.0;
    for (int i = 0; i < n; i++) {
        const double alpha = r.range(1e-3, kAlphaLimit);
        const double beta = r.logRange(1e-6, 1e-2);
        const double x = r.logRange(1e-9, 1e4) * (r.uni() < 0.5 ? -1.0 : 1.0);
        const double e = eta(x, alpha, beta);
        oddErr = std::max(oddErr, std::fabs(e + eta(-x, alpha, beta)));
        if (x * e < 0.0) signErr += 1.0;

        const double ratio = etaOverX(x, alpha, beta);
        const double lo = 3.0 * alpha * beta / (4.0 + 4.0 * beta);
        const double hi = 0.75 * alpha;
        // a relative slack of 1e-9 on the ends, which is the accuracy the
        // small-signal expansion is spliced in at
        if (ratio < lo * (1.0 - 1e-9) || ratio > hi * (1.0 + 1e-9))
            boundErr += 1.0;
        // monotone in |x|
        const double bigger = etaOverX(x * 1.5, alpha, beta);
        if (bigger < ratio * (1.0 - 1e-12)) monoErr += 1.0;
    }
    report("N1_odd", oddErr, oddErr == 0.0);
    report("N2_sign", signErr, signErr == 0.0);
    report("N3_limits", boundErr, boundErr == 0.0);
    report("N3_monotonic", monoErr, monoErr == 0.0);

    // N4: etaOverX switches from the small-signal expansion to the exact
    // ratio at s = 3*alpha*|x|/4 = 1e-5. Step across that point and the two
    // branches must agree: a visible seam here would be distortion sitting at
    // one fixed, very low level.
    double worstSeam = 0.0;
    for (double alpha = 0.25; alpha <= kAlphaLimit; alpha += 0.05) {
        for (double beta = 1e-6; beta < 1e-2; beta *= 3.0) {
            const double xc = 1e-5 / (0.75 * alpha);
            const double below = etaOverX(xc * (1.0 - 1e-9), alpha, beta);
            const double above = etaOverX(xc * (1.0 + 1e-9), alpha, beta);
            worstSeam = std::max(worstSeam, std::fabs(above - below) / above);
        }
    }
    report("N4_continuous", worstSeam, worstSeam < 1e-9);
}

// ── the discretization ──────────────────────────────────────────────────────
// det works out to 1 + h*(2-gamma)/2 + h*h/4, whose minimum over h is
// 1 - (gamma-2)^2/4: bounded away from zero for any gamma < 4, and gamma can
// never exceed alpha. That is 0.9885 over the resonance knob's own range and
// 0.4375 over the DSP's kAlphaLimit guard, and this checks both.
static void testDeterminant() {
    Rng r(gSeed ^ 0x5bf03635u);
    double worstKnob = 1e9, worstGuard = 1e9;
    const int n = 20000 * gScale;
    for (int i = 0; i < n; i++) {
        // h = omega/Fs over the whole supported range: 1 Hz at 192 kHz up to
        // 0.45*Fs, which is h = 2.83.
        const double h = r.logRange(3e-5, 2.9);
        const double x2 = r.logRange(1e-9, 1e5) * (r.uni() < 0.5 ? -1.0 : 1.0);
        for (int k = 0; k < 2; k++) {
            const double alpha = k ? r.range(0.0, kAlphaLimit)
                                   : alphaFromKnob(r.uni());
            const double gamma = gammaOf(x2, alpha, kBeta);
            const double det = (1.0 + 0.5 * h) * (1.0 - 0.5 * h * (gamma - 1.0))
                             + 0.25 * h * h * gamma;
            if (k) worstGuard = std::min(worstGuard, det);
            else worstKnob = std::min(worstKnob, det);
        }
    }
    report("D1_det_knob", worstKnob, worstKnob > 0.98);
    report("D1_det_guard", worstGuard, worstGuard > 0.43);
}

// Both integrators, same input, same parameters. For a frozen A the scheme is
// implicit midpoint and second-order; it is the state-dependent gamma that
// makes it first-order, so the observed rate sits between the two and the
// error must at least fall with the sample rate. Measured worst ratio per
// doubling is 0.52 (the first-order cases) and worst 44.1 kHz error 4.1e-3.
static void testConvergence() {
    const double rates[] = {44100.0, 88200.0, 176400.0};
    const double cutoffs[] = {100.0, 1000.0, 5000.0};
    const double alphas[] = {0.0, 1.0, 1.9};
    double worstBase = 0.0, worstRatio = 0.0;
    for (double fc : cutoffs) {
        for (double alpha : alphas) {
            double prev = 0.0;
            for (int ri = 0; ri < 3; ri++) {
                const double fs = rates[ri];
                Korg35Filter dg;
                Korg35RK4 ref;
                dg.setSampleRate(fs);
                ref.setSampleRate(fs);
                const long n = (long)(0.25 * fs);
                double se = 0.0, sr2 = 0.0;
                for (long i = 0; i < n; i++) {
                    // 1 V of a sine, hot enough that the diodes are working
                    const double u = (1.0 / kVref)
                        * std::sin(2.0 * M_PI * 220.0 * i / fs);
                    const double a = dg.processSample(u, fc, alpha);
                    const double b = ref.processSample(u, fc, alpha);
                    se += (a - b) * (a - b);
                    sr2 += b * b;
                }
                const double nrms = std::sqrt(se / sr2);
                if (ri == 0) worstBase = std::max(worstBase, nrms);
                else worstRatio = std::max(worstRatio, nrms / prev);
                prev = nrms;
            }
        }
    }
    report("D2_rk4_44k", worstBase, worstBase < 1e-2);
    report("D2_converges", worstRatio, worstRatio < 0.6);
}

// At 1 mV the diodes never conduct and the circuit is the two-pole prototype
// 1/(s^2/w^2 + (2-gamma0)s/w + 1), inverted. Anything that detuned the
// discretization or mis-scaled alpha would show here first.
static void testLinearResponse() {
    const double fs = 48000.0, fc = 1000.0;
    const double amp = 1e-3 / kVref;
    double worst = 0.0;
    for (double alpha = 0.0; alpha <= 1.9; alpha += 0.475) {
        const double gamma0 = alpha - 3.0 * alpha * kBeta / (4.0 + 4.0 * kBeta);
        for (double f = 100.0; f <= 4000.0; f *= 2.0) {
            Korg35Filter dg;
            dg.setSampleRate(fs);
            double re = 0.0, im = 0.0;
            const long warm = (long)(0.2 * fs), n = (long)(0.3 * fs);
            for (long i = 0; i < warm + n; i++) {
                const double ph = 2.0 * M_PI * f * i / fs;
                const double y = dg.processSample(amp * std::sin(ph), fc, alpha);
                if (i >= warm) { re += y * std::cos(ph); im += y * std::sin(ph); }
            }
            const double mag = 2.0 * std::sqrt(re * re + im * im) / n / amp;
            const double w = f / fc;
            const double dre = 1.0 - w * w, dim = (2.0 - gamma0) * w;
            const double want = 1.0 / std::sqrt(dre * dre + dim * dim);
            worst = std::max(worst, std::fabs(mag - want) / want);
        }
    }
    report("D3_linear_response", worst, worst < 0.02);
}

// ── the system ──────────────────────────────────────────────────────────────
static void testStability() {
    const double fs = 48000.0;
    const double threshold = (8.0 + 8.0 * kBeta) / (4.0 + kBeta);

    // S1: below the threshold, a kicked state decays to nothing.
    double worstDecay = 0.0;
    for (double alpha = 0.0; alpha < threshold - 0.01; alpha += 0.4) {
        for (double fc = 100.0; fc <= 5000.0; fc *= 5.0) {
            Korg35Filter dg;
            dg.setSampleRate(fs);
            dg.x2 = 1.0;
            for (long i = 0; i < (long)(5.0 * fs); i++)
                dg.processSample(0.0, fc, alpha);
            worstDecay = std::max(worstDecay, std::fabs(dg.x2));
        }
    }
    report("S1_zero_input_decays", worstDecay, worstDecay < 1e-6);

    // S2: above it, a bounded limit cycle. Bounded by the diodes, so the
    // amplitude must not depend on how hard it was kicked.
    double worstSpread = 0.0, minRms = 1e9, maxPeak = 0.0;
    for (double alpha = 2.05; alpha <= kAlphaMax; alpha += 0.05) {
        double amp[2];
        for (int k = 0; k < 2; k++) {
            Korg35Filter dg;
            dg.setSampleRate(fs);
            dg.x2 = k ? 1e-3 : 20.0;
            for (long i = 0; i < (long)(4.0 * fs); i++)
                dg.processSample(0.0, 1000.0, alpha);
            double s2 = 0.0;
            const long n = (long)(0.5 * fs);
            for (long i = 0; i < n; i++) {
                const double y = dg.processSample(0.0, 1000.0, alpha);
                s2 += y * y;
                maxPeak = std::max(maxPeak, std::fabs(y));
            }
            amp[k] = std::sqrt(s2 / n);
        }
        minRms = std::min(minRms, std::min(amp[0], amp[1]));
        worstSpread = std::max(worstSpread,
            std::fabs(amp[0] - amp[1]) / std::max(amp[0], amp[1]));
    }
    report("S2_oscillates", minRms * kVref, minRms * kVref > 0.02);
    report("S2_limit_cycle", worstSpread, worstSpread < 0.02);
    report("S2_bounded", maxPeak * kVref, maxPeak * kVref < 2.0);
}

// Everything modulated at audio rate, at levels well past anything the
// circuit would see, must stay finite and bounded.
static void testRobustness() {
    Rng r(gSeed ^ 0x846ca68bu);
    const double fs = 48000.0;
    long nans = 0;
    double peak = 0.0;
    const int patches = 40 * gScale;
    for (int p = 0; p < patches; p++) {
        Korg35Filter dg;
        dg.setSampleRate(fs);
        const double fcLo = r.logRange(20.0, 2000.0);
        const double fcHi = r.logRange(fcLo, 20000.0);
        const double aLo = r.range(0.0, kAlphaMax);
        const double aHi = r.range(aLo, kAlphaMax);
        const double drive = r.logRange(0.1, 200.0);
        const double modHz = r.logRange(0.1, 4000.0);
        for (long i = 0; i < (long)(0.5 * fs); i++) {
            const double m = 0.5 + 0.5 * std::sin(2.0 * M_PI * modHz * i / fs);
            const double fc = fcLo + m * (fcHi - fcLo);
            const double alpha = aLo + (1.0 - m) * (aHi - aLo);
            const double u = drive * std::sin(2.0 * M_PI * 130.0 * i / fs)
                           + 0.3 * drive * r.range(-1.0, 1.0);
            const double y = dg.processSample(u, fc, alpha);
            if (!std::isfinite(y)) nans++;
            peak = std::max(peak, std::fabs(y));
        }
    }
    report("S3_finite", (double)nans, nans == 0);
    // 200 normalised is 15 V at the circuit; the output cannot exceed the
    // input by much once the resonance has been squashed by it
    report("S3_bounded", peak, peak < 1e3);
}

// The whole reason for the model: the resonant peak must fall as the level
// rises. A linear filter with a waveshaper on its output would hold it.
static void testLevelDependence() {
    const double fs = 48000.0, fc = 1000.0, alpha = 1.9;
    double prevGain = 1e9;
    bool monotone = true;
    double lowGain = 0.0, highGain = 0.0;
    for (int i = 0; i <= 6; i++) {
        const double volts = std::pow(10.0, (-60.0 + 12.0 * i) / 20.0);
        const double amp = volts / kVref;
        Korg35Filter dg;
        dg.setSampleRate(fs);
        double re = 0.0, im = 0.0;
        const long warm = (long)(0.3 * fs), n = (long)(0.5 * fs);
        for (long j = 0; j < warm + n; j++) {
            const double ph = 2.0 * M_PI * fc * j / fs;
            const double y = dg.processSample(amp * std::sin(ph), fc, alpha);
            if (j >= warm) { re += y * std::cos(ph); im += y * std::sin(ph); }
        }
        const double gain = 2.0 * std::sqrt(re * re + im * im) / n / amp;
        if (gain > prevGain * 1.001) monotone = false;
        prevGain = gain;
        if (i == 0) lowGain = gain;
        if (i == 6) highGain = gain;
    }
    report("S4_peak_falls", lowGain / highGain, lowGain / highGain > 5.0);
    report("S4_monotone", monotone ? 1 : 0, monotone);
}

int main(int argc, char** argv) {
    bool header = true;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-header")) header = false;
        else if (!std::strcmp(argv[i], "--long")) gScale = 8;
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            gSeed = (uint32_t)strtoul(argv[++i], nullptr, 0);
    }
    if (header) printf("module,check,value,pass\n");
    fprintf(stderr, "viginti_invariants: seed 0x%08x, scale %d\n", gSeed, gScale);

    testLambertW();
    testNonlinearity();
    testDeterminant();
    testConvergence();
    testLinearResponse();
    testStability();
    testRobustness();
    testLevelDependence();

    return failures ? 1 : 0;
}
