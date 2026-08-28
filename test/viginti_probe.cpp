// viginti_probe - measurement harness for the KORG35 Rev. 2 filter.
//
// Not a set of checks (viginti_invariants carries those): this is where the
// numbers come from. It measures the Lambert-W solver, the limits of the
// diode nonlinearity, the production scheme against the RK4 reference across
// sample rates and cutoffs, the linear frequency response, level dependence
// and the self-oscillation threshold, and prints a CPU figure.
//
//   viginti_probe [lambertw|eta|rk4|response|level|osc|map|alias|cpu]
//
// With no argument it runs the lot. Rack is not involved: src/viginti_dsp.hpp
// is free of Rack headers.

#include "../src/viginti_dsp.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace viginti;

// ── helpers ─────────────────────────────────────────────────────────────────

// Amplitude and phase of one frequency bin, accumulated over whole cycles.
struct Bin {
    double re = 0.0, im = 0.0;
    long n = 0;
    void add(double y, double phase) {
        re += y * std::cos(phase);
        im += y * std::sin(phase);
        n++;
    }
    double mag() const { return 2.0 * std::sqrt(re * re + im * im) / n; }
    // For y = A*sin(ph + phi): re -> (A/2)sin(phi), im -> (A/2)cos(phi).
    double phase() const { return std::atan2(re, im); }
};

static double rms(const std::vector<double>& v, size_t from = 0) {
    double s = 0.0;
    for (size_t i = from; i < v.size(); i++) s += v[i] * v[i];
    return v.size() > from ? std::sqrt(s / (v.size() - from)) : 0.0;
}

// ── 1. Lambert W ────────────────────────────────────────────────────────────
// The defining identity, W(z)*exp(W(z)) = z, is the only thing to check for
// z >= 0. Past z ~ 1e300 exp(W) is unrepresentable, so the residual is formed
// in logs there: log(w) + w - log(z).
static void probeLambertW() {
    printf("\n# lambertw: W(z)exp(W(z)) == z\n");
    printf("z,W,rel_residual\n");
    const double zs[] = {0.0, 1e-12, 1e-6, 0.01, 1.0, 2.0, 10.0, 100.0,
                         1e6, 1e12, 1e30, 1e100};
    for (double z : zs) {
        const double w = lambertW0(z);
        double res;
        if (z == 0.0) {
            res = std::fabs(w);
        } else if (z < 1e300) {
            res = std::fabs(w * std::exp(w) - z) / z;
        } else {
            res = std::fabs(std::log(w) + w - std::log(z)) / std::log(z);
        }
        printf("%g,%.17g,%.3g\n", z, w, res);
    }

    printf("\n# lambertw: W(exp(t)) against W() on the same argument\n");
    printf("t,W_exp,W_of_exp,rel_diff\n");
    const double ts[] = {-30.0, -8.0, -1.0, 0.0, 1.0, 5.0, 20.0, 100.0,
                         700.0, 1e4, 1e8};
    for (double t : ts) {
        const double a = lambertW0Exp(t);
        const double b = t < 709.0 ? lambertW0(std::exp(t)) : NAN;
        const double d = std::isfinite(b) ? std::fabs(a - b) / a : NAN;
        printf("%g,%.17g,%.17g,%.3g\n", t, a, b, d);
    }
}

// ── 2. the nonlinearity ─────────────────────────────────────────────────────
// eta is odd, has the same sign as x2, and eta(x)/x runs monotonically from
// 3*alpha*beta/(4+4beta) at zero to 3*alpha/4 at infinity (paper section 3.1).
static void probeEta() {
    const double alpha = 2.0, beta = kBeta;
    printf("\n# eta: beta=%.6g, small-signal limit=%.6g, asymptote=%.6g\n",
           beta, 3.0 * alpha * beta / (4.0 + 4.0 * beta), 0.75 * alpha);
    printf("x2,eta,eta_over_x,odd_err\n");
    const double xs[] = {0.0, 1e-9, 1e-6, 1e-3, 0.1, 1.0, 5.0, 10.0, 50.0,
                         500.0, 5000.0};
    for (double x : xs) {
        const double e = eta(x, alpha, beta);
        const double r = etaOverX(x, alpha, beta);
        const double oddErr = std::fabs(e + eta(-x, alpha, beta));
        printf("%g,%.10g,%.10g,%.3g\n", x, e, r, oddErr);
    }

    // Where the small-signal expansion is spliced onto the exact ratio: a
    // visible step here would show up as distortion at low level.
    printf("\n# eta: continuity of eta/x across the s = 1e-5 crossover\n");
    printf("x2,eta_over_x,rel_step\n");
    double prev = 0.0;
    for (double x = 1e-7; x < 1e-3; x *= 1.4) {
        const double r = etaOverX(x, alpha, beta);
        printf("%.6g,%.12g,%.3g\n", x, r, prev ? (r - prev) / r : 0.0);
        prev = r;
    }
}

// ── 3. production against the RK4 reference ─────────────────────────────────
// Both integrators are given the same input sequence and parameters. The
// discrete-gradient scheme is first-order accurate, so its error against RK4
// should fall roughly as 1/Fs. Above the oscillation threshold the two run
// into the same limit cycle at different phases and a sample-by-sample error
// stops meaning anything, so alpha stays below it here.
static void probeRK4() {
    printf("\n# rk4: production vs reference, 0.5 s of the same input\n");
    printf("signal,fs,fc,alpha,max_abs_err,rms_err,nrms_err\n");
    const double rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
    const double cutoffs[] = {100.0, 500.0, 1000.0, 5000.0, 10000.0};
    const double alphas[] = {0.0, 1.0, 1.9};
    const char* names[] = {"sine1v", "sine_hot", "noise", "impulse"};

    for (int sig = 0; sig < 4; sig++) {
        for (double fs : rates) {
            for (double fc : cutoffs) {
                if (fc > 0.4 * fs) continue;
                for (double alpha : alphas) {
                    Korg35Filter dg;
                    Korg35RK4 ref;
                    dg.setSampleRate(fs);
                    ref.setSampleRate(fs);
                    const long n = (long)(0.5 * fs);
                    double maxErr = 0.0, se = 0.0, sr2 = 0.0;
                    unsigned rng = 12345u;
                    for (long i = 0; i < n; i++) {
                        const double t = i / fs;
                        double u;
                        switch (sig) {
                            case 0: u = std::sin(2.0 * M_PI * 220.0 * t); break;
                            case 1: u = 20.0 * std::sin(2.0 * M_PI * 220.0 * t); break;
                            case 2:
                                rng = rng * 1664525u + 1013904223u;
                                u = ((double)(rng >> 8) / 8388608.0 - 1.0);
                                break;
                            default: u = (i == 0) ? 100.0 : 0.0; break;
                        }
                        const double a = dg.processSample(u, fc, alpha);
                        const double b = ref.processSample(u, fc, alpha);
                        const double e = a - b;
                        maxErr = std::max(maxErr, std::fabs(e));
                        se += e * e;
                        sr2 += b * b;
                    }
                    printf("%s,%g,%g,%g,%.4g,%.4g,%.4g\n", names[sig], fs, fc,
                           alpha, maxErr, std::sqrt(se / n),
                           sr2 > 0 ? std::sqrt(se / sr2) : 0.0);
                }
            }
        }
    }
}

// ── 4. linear frequency response ────────────────────────────────────────────
// At a low enough level the diodes never conduct and the circuit is the
// two-pole lowpass H(s) = 1/(s^2/w^2 + (2-gamma0)s/w + 1) with
// gamma0 = alpha - 3*alpha*beta/(4+4beta). Measuring the model against that
// closed form is the check that the discretization is not detuning anything.
static void probeResponse() {
    const double fs = 48000.0, fc = 1000.0;
    printf("\n# response: fc=%g Hz, input 1 mV (diodes off)\n", fc);
    printf("alpha,f,mag_dg,mag_rk4,mag_analytic,phase_dg,phase_analytic\n");
    const double alphas[] = {0.0, 1.0, 1.9};
    const double freqs[] = {20.0, 50.0, 100.0, 250.0, 500.0, 700.0, 1000.0,
                            1400.0, 2000.0, 4000.0, 8000.0, 16000.0};
    const double amp = 1e-3 / kVref;   // 1 mV, normalised

    for (double alpha : alphas) {
        const double gamma0 = alpha - 3.0 * alpha * kBeta / (4.0 + 4.0 * kBeta);
        for (double f : freqs) {
            Korg35Filter dg;
            Korg35RK4 ref;
            dg.setSampleRate(fs);
            ref.setSampleRate(fs);
            const long warm = (long)(0.2 * fs);
            const long n = (long)(0.3 * fs);
            Bin bdg, bref;
            for (long i = 0; i < warm + n; i++) {
                const double ph = 2.0 * M_PI * f * i / fs;
                const double u = amp * std::sin(ph);
                const double a = dg.processSample(u, fc, alpha);
                const double b = ref.processSample(u, fc, alpha);
                if (i >= warm) { bdg.add(a, ph); bref.add(b, ph); }
            }
            // |H| and arg H of the analytic prototype, output x2 = -H*u
            const double w = f / fc;
            const double dre = 1.0 - w * w;
            const double dim = (2.0 - gamma0) * w;
            const double magA = 1.0 / std::sqrt(dre * dre + dim * dim);
            const double phA = M_PI - std::atan2(dim, dre);
            printf("%g,%g,%.5g,%.5g,%.5g,%.4g,%.4g\n", alpha, f,
                   bdg.mag() / amp, bref.mag() / amp, magA,
                   bdg.phase(), phA > M_PI ? phA - 2.0 * M_PI : phA);
        }
    }
}

// ── 5. level dependence ─────────────────────────────────────────────────────
// The point of the model. The same waveform at rising levels must not simply
// scale: the diodes conduct sooner in the loop, gamma rises, the resonant
// peak flattens and harmonics appear. A linear filter followed by a static
// waveshaper would keep the peak gain fixed and only change the harmonics.
static void probeLevel() {
    const double fs = 48000.0, fc = 1000.0, alpha = 1.9;
    printf("\n# level: fc=%g Hz alpha=%g, sine at fc\n", fc, alpha);
    printf("in_dbv,in_volts,gain,thd_pct,h2_db,h3_db\n");
    const double dbs[] = {-60.0, -40.0, -30.0, -20.0, -10.0, 0.0, 6.0, 14.0};
    for (double db : dbs) {
        const double volts = std::pow(10.0, db / 20.0);
        const double amp = volts / kVref;
        Korg35Filter dg;
        dg.setSampleRate(fs);
        const long warm = (long)(0.3 * fs);
        const long n = (long)(0.5 * fs);
        Bin h[6];
        double sum2 = 0.0;
        for (long i = 0; i < warm + n; i++) {
            const double ph = 2.0 * M_PI * fc * i / fs;
            const double y = dg.processSample(amp * std::sin(ph), fc, alpha);
            if (i >= warm) {
                for (int k = 1; k <= 5; k++) h[k].add(y, k * ph);
                sum2 += y * y;
            }
        }
        double harm = 0.0;
        for (int k = 2; k <= 5; k++) harm += h[k].mag() * h[k].mag();
        const double f1 = h[1].mag();
        printf("%g,%.4g,%.5g,%.4g,%.4g,%.4g\n", db, volts, f1 / amp,
               100.0 * std::sqrt(harm) / f1,
               20.0 * std::log10(h[2].mag() / f1),
               20.0 * std::log10(h[3].mag() / f1));
    }
}

// ── 6. self-oscillation ─────────────────────────────────────────────────────
// The paper puts the threshold at alpha = (8+8beta)/(4+beta), just past 2, and
// the limit cycle is bounded by the diodes above it. Kick the state and see
// what survives, and at what frequency.
static void probeOsc() {
    const double fs = 48000.0;
    printf("\n# osc: threshold at alpha=%.6f\n",
           (8.0 + 8.0 * kBeta) / (4.0 + kBeta));
    printf("alpha,fc,peak_volts,rms_volts,zc_hz\n");
    const double alphas[] = {1.5, 1.99, 2.0, 2.001, 2.01, 2.1, kAlphaMax, 2.5};
    const double cutoffs[] = {100.0, 1000.0, 5000.0};
    for (double alpha : alphas) {
        for (double fc : cutoffs) {
            Korg35Filter dg;
            dg.setSampleRate(fs);
            dg.x2 = 1.0;      // the kick, in normalised units
            const long warm = (long)(2.0 * fs);
            const long n = (long)(1.0 * fs);
            double peak = 0.0, s2 = 0.0;
            int zc = 0;
            double prev = 0.0;
            for (long i = 0; i < warm + n; i++) {
                const double y = dg.processSample(0.0, fc, alpha);
                if (i >= warm) {
                    peak = std::max(peak, std::fabs(y));
                    s2 += y * y;
                    if (prev <= 0.0 && y > 0.0) zc++;
                    prev = y;
                }
            }
            printf("%g,%g,%.5g,%.5g,%g\n", alpha, fc, peak * kVref,
                   std::sqrt(s2 / n) * kVref, (double)zc);
        }
    }
}

// ── 6b. what the resonance knob does ────────────────────────────────────────
// alphaFromKnob is the one place the knob becomes a circuit parameter. Print
// the whole travel: alpha, the small-signal pole Q it implies (1/(2-alpha)),
// and the peak gain actually measured at a level the diodes ignore.
static void probeMap() {
    const double fs = 48000.0, fc = 1000.0;
    printf("\n# map: resonance knob to alpha, Q and measured peak gain\n");
    printf("knob,alpha,q_theory,peak_gain,peak_db\n");
    for (int i = 0; i <= 20; i++) {
        const double r = i / 20.0;
        const double alpha = alphaFromKnob(r);
        const double q = alpha < 2.0 ? 1.0 / (2.0 - alpha) : INFINITY;
        double gain = INFINITY;
        if (alpha < 2.0) {
            Korg35Filter dg;
            dg.setSampleRate(fs);
            const double amp = 1e-3 / kVref;
            Bin b;
            const long warm = (long)(1.0 * fs), n = (long)(0.5 * fs);
            for (long j = 0; j < warm + n; j++) {
                const double ph = 2.0 * M_PI * fc * j / fs;
                const double y = dg.processSample(amp * std::sin(ph), fc, alpha);
                if (j >= warm) b.add(y, ph);
            }
            gain = b.mag() / amp;
        }
        printf("%.2f,%.5f,%.4g,%.4g,%.4g\n", r, alpha, q, gain,
               20.0 * std::log10(gain));
    }
}

// ── 7. aliasing ─────────────────────────────────────────────────────────────
// The diodes fold the resonance into harmonics, and above Nyquist those come
// back down the spectrum. Drive the filter hard at f0 and measure the bins the
// first fifteen harmonics alias onto: |k*f0 - m*Fs|. Running the same patch at
// a higher internal rate moves those products back where they belong, so the
// difference between the two is the alias floor, and it is what sets the
// module's oversampling default.
static void probeAlias() {
    const double base = 48000.0, f0 = 3011.0, fc = 4000.0, alpha = 2.1;
    printf("\n# alias: f0=%g Hz fc=%g alpha=%g, harmonics 2..15 folded down\n",
           f0, fc, alpha);
    printf("internal_rate,fundamental,worst_alias_db,total_alias_db\n");
    for (int r = 1; r <= 16; r *= 2) {
        const double fs = base * r;
        Korg35Filter dg;
        dg.setSampleRate(fs);
        const long warm = (long)(0.3 * fs), n = (long)(0.5 * fs);
        // the alias frequencies, folded into the audio band
        double af[16];
        int na = 0;
        for (int k = 2; k <= 15; k++) {
            double f = std::fmod(k * f0, base);
            if (f > base / 2) f = base - f;
            if (f > 100.0 && k * f0 > base / 2) af[na++] = f;
        }
        Bin fund, al[16];
        const double amp = 5.0 / kVref;
        for (long i = 0; i < warm + n; i++) {
            const double t = i / fs;
            const double y = dg.processSample(amp * std::sin(2.0 * M_PI * f0 * t),
                                              fc, alpha);
            if (i >= warm) {
                fund.add(y, 2.0 * M_PI * f0 * t);
                for (int a = 0; a < na; a++) al[a].add(y, 2.0 * M_PI * af[a] * t);
            }
        }
        double worst = 0.0, total = 0.0;
        for (int a = 0; a < na; a++) {
            worst = std::max(worst, al[a].mag());
            total += al[a].mag() * al[a].mag();
        }
        printf("%gx,%.5g,%.4g,%.4g\n", (double)r, fund.mag(),
               20.0 * std::log10(worst / fund.mag()),
               20.0 * std::log10(std::sqrt(total) / fund.mag()));
    }
}

// ── 8. cost ─────────────────────────────────────────────────────────────────
static void probeCpu() {
    const double fs = 48000.0;
    const long n = 4000000;
    Korg35Filter dg;
    dg.setSampleRate(fs);
    auto t0 = std::chrono::steady_clock::now();
    double acc = 0.0;
    for (long i = 0; i < n; i++)
        acc += dg.processSample(10.0 * std::sin(i * 0.01), 1000.0, 1.9);
    auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("\n# cpu (sink %.3g)\n", acc);
    printf("ns_per_sample,realtime_pct_at_48k_1ch\n");
    printf("%.3f,%.3f\n", 1e9 * secs / n, 100.0 * secs * fs / n);
}

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "all";
    const bool all = !std::strcmp(what, "all");
    if (all || !std::strcmp(what, "lambertw")) probeLambertW();
    if (all || !std::strcmp(what, "eta")) probeEta();
    if (all || !std::strcmp(what, "rk4")) probeRK4();
    if (all || !std::strcmp(what, "response")) probeResponse();
    if (all || !std::strcmp(what, "level")) probeLevel();
    if (all || !std::strcmp(what, "osc")) probeOsc();
    if (all || !std::strcmp(what, "map")) probeMap();
    if (all || !std::strcmp(what, "alias")) probeAlias();
    if (all || !std::strcmp(what, "cpu")) probeCpu();
    return 0;
}
