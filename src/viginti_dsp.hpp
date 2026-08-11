// viginti_dsp.hpp — the KORG35 Rev. 2 nonlinear lowpass, free of Rack headers
// so test/viginti_probe and test/viginti_invariants can measure it directly.
//
// The circuit is the OTA-based lowpass of the later MS-20 revisions: two RC
// stages buffered by OTAs, with a non-inverting amplifier of gain G ~ 4 in the
// resonance feedback path whose gain is limited by three series diodes each
// way. Those diodes sit *inside* the feedback loop, which is the whole point:
// the resonance, not the signal, is what distorts, and it does so by level.
//
// The model, the discrete-gradient scheme and the stability bounds are from
//   M. Danish, S. Bilbao, M. Ducceschi, "Applications of Port Hamiltonian
//   Methods to Non-Iterative Stable Simulations of the KORG35 and MOOG 4-Pole
//   VCF", Proc. DAFx20in21, Vienna, 2021, section 3.1.
// Their equations (19)-(22) are the continuous model, (31) the discretization.
// The paper leaves alpha and beta as symbols; the component values behind them
// come from its own reference [25], T. E. Stinchcombe, "A Study of the Korg
// MS10 & MS20 Filters", Aug. 2006 (section 2.1 and figure 16). See
// doc/viginti.md.
//
// Two integrators are provided and must agree: Korg35Filter is the real-time
// discrete-gradient scheme, Korg35RK4 is the continuous model integrated by
// RK4 and exists to be the oracle it is measured against.

#pragma once

#include <algorithm>
#include <cmath>

namespace viginti {

// ── diode and component constants ───────────────────────────────────────────
// Thermal voltage and emission coefficient, as the paper takes them. Vref is
// the voltage the whole system is normalised by: three diode drops' worth of
// thermal voltage, because the feedback amplifier is clamped by three series
// diodes in each direction.
static const double kVT = 0.02585;      // V
static const double kN = 1.0;           // diode emission coefficient
static const double kVref = 3.0 * kN * kVT;   // 0.07755 V

// Feedback amplifier around the resonance pot: R2 = 10k, R1 = 3.3k (2.2k
// fixed plus a 2.2k preset at its midpoint), so G = 1 + R2/R1 ~ 4.03, which
// is the "1 + 10/3.3 = 4" of the Stinchcombe study. The pot then divides that
// down by at most 10/(8.2 + 10), so the circuit's resonance parameter
// alpha = K*G can reach 2.21 and no further. The paper's own figure of 2.2.
static const double kR1 = 3.3e3;
static const double kR2 = 10.0e3;
static const double kG = 1.0 + kR2 / kR1;              // 4.0303
static const double kKmax = 10.0 / (8.2 + 10.0);       // 0.5495
static const double kAlphaMax = kG * kKmax;            // 2.2144

// beta = Isat*R2/Vref. Isat is the 1N4148 saturation current fitted in
// Esqueda et al. (DAFx-22), the same figure vespae uses for its diode pair.
// beta is small, ~3e-4, and enters only through logarithms: it sets where the
// diodes start to conduct, not how hard they clamp.
static const double kIsat = 2.52e-9;
static const double kBeta = kIsat * kR2 / kVref;       // 3.25e-4

// Hard guard on alpha. The system self-oscillates above (8+8beta)/(4+beta),
// i.e. just past 2, and the paper's Lyapunov analysis fails at alpha = 8 with
// ill-conditioning already visible from 7.7. The circuit cannot exceed 2.21,
// and this limit only exists so that a caller doing its own mapping cannot
// walk the 2x2 solve towards a singular determinant (which needs gamma = 4).
static const double kAlphaLimit = 3.5;

// Above this the state is a runaway, not a signal: reset rather than let it
// reach the point where exp() overflows.
static const double kStateLimit = 1e6;

// ── Lambert W, principal branch, z >= 0 ─────────────────────────────────────
// Halley's method from an initial guess good enough that three iterations
// reach double precision. log1p(z) is an upper bound on W(z) and tight for
// small z; past e the standard asymptotic series is better.
inline double lambertW0(double z) {
    if (!(z > 0.0)) return 0.0;
    double w;
    if (z < 2.718281828459045) {
        w = std::log1p(z);
    } else {
        const double L1 = std::log(z);
        const double L2 = std::log(L1);
        w = L1 - L2 + L2 / L1;
    }
    for (int i = 0; i < 6; i++) {
        const double e = std::exp(w);
        const double f = w * e - z;
        const double dw = f / (e * (w + 1.0) - (w + 2.0) * f / (2.0 * w + 2.0));
        w -= dw;
        if (std::fabs(dw) <= 1e-16 * std::fabs(w)) break;
    }
    return w;
}

// W(exp(t)), which is what the model actually needs. Evaluating exp(t) first
// overflows for t > 709 and the diode argument gets there on a loud enough
// signal, so past t = 1 solve w + log(w) = t instead: same root, no exp() at
// all, and no defensive clamp that would silently turn the diodes into some
// other waveshaper.
inline double lambertW0Exp(double t) {
    if (t <= 1.0) return lambertW0(std::exp(t));
    // Asymptotic start, then Newton on g(w) = w + log(w) - t.
    const double L2 = std::log(t);
    double w = t - L2 + L2 / t;
    for (int i = 0; i < 6; i++) {
        const double g = w + std::log(w) - t;
        const double dw = g / (1.0 + 1.0 / w);
        w -= dw;
        if (std::fabs(dw) <= 1e-16 * w) break;
    }
    return w;
}

// ── the diode nonlinearity, paper eq. (20) ──────────────────────────────────
//   eta(x2) = lambda*W(beta*exp(3*lambda*alpha*x2/4 + beta)) - lambda*beta
// with lambda = sgn(x2). Note 3*lambda*alpha*x2/4 = 3*alpha*|x2|/4, so the
// Lambert-W argument is a function of |x2| alone and eta is odd.
// log(beta) is a constant of the model, so the real-time path passes it in
// rather than paying for a log() per sample; the plain form is what the
// reference and the tests use.
inline double eta(double x2, double alpha, double beta, double logBeta) {
    if (x2 == 0.0) return 0.0;
    const double lambda = x2 > 0.0 ? 1.0 : -1.0;
    const double t = logBeta + 0.75 * alpha * std::fabs(x2) + beta;
    return lambda * (lambertW0Exp(t) - beta);
}

inline double eta(double x2, double alpha, double beta) {
    return eta(x2, alpha, beta, std::log(beta));
}

// eta(x2)/x2, which is what the state-space form wants. Two things go wrong
// near zero: the division itself, and the cancellation in W(...) - beta, since
// both terms are beta to first order. Below the crossover use the paper's own
// small-signal expansion, eq. (23).
//
// The crossover is on s = 3*alpha*|x2|/4, the argument the expansion is in,
// not on x2: at fixed x2 the expansion gets worse as alpha rises. Its error is
// O(s^2) ~ 1e-10 relative at s = 1e-5, and the cancellation it replaces is of
// the same order there, so that is where they cross.
inline double etaOverX(double x2, double alpha, double beta, double logBeta) {
    const double s = 0.75 * alpha * std::fabs(x2);
    const double d0 = 3.0 * alpha * beta / (4.0 + 4.0 * beta);
    if (s < 1e-5) {
        const double onePlusB = 1.0 + beta;
        return d0 * (1.0 + s / (2.0 * onePlusB * onePlusB));
    }
    return eta(x2, alpha, beta, logBeta) / x2;
}

inline double etaOverX(double x2, double alpha, double beta) {
    return etaOverX(x2, alpha, beta, std::log(beta));
}

// gamma(x2) = alpha - eta(x2)/x2, the state-dependent damping. It runs from
// alpha - 3*alpha*beta/(4+4beta) (about alpha, diodes off) down to alpha/4
// (diodes hard on): the loop gain the resonance sees falls as the signal
// grows, which is the level dependence the whole model exists for.
inline double gammaOf(double x2, double alpha, double beta, double logBeta) {
    return alpha - etaOverX(x2, alpha, beta, logBeta);
}

inline double gammaOf(double x2, double alpha, double beta) {
    return alpha - etaOverX(x2, alpha, beta);
}

// ── the continuous model, paper eq. (19) ────────────────────────────────────
// Not an aggregate with default member initializers: the plugin builds as
// C++11, where that would make the brace initialisers below ill-formed.
struct State {
    State() : x1(0.0), x2(0.0) {}
    State(double a, double b) : x1(a), x2(b) {}
    double x1;
    double x2;
};

inline State derivative(const State& s, double u, double omega,
                        double alpha, double beta) {
    const double e = eta(s.x2, alpha, beta);
    return {
        omega * (-s.x1 - alpha * s.x2 + e - u),
        omega * (s.x1 + (alpha - 1.0) * s.x2 - e)
    };
}

// ── production: first-order discrete gradient, paper eq. (31) ───────────────
// delta x = h*[I - (h/2)A(x_n)]^-1 * (A(x_n)*x_n - G*u_n), h = omega/Fs, with
// A evaluated at the current state and the 2x2 system solved in scalars.
//
// The determinant works out to 1 + h*(2 - gamma)/2 + h*h/4, which is positive
// for every h as long as gamma < 4; gamma <= alpha <= kAlphaLimit keeps it
// there with room to spare. There is no iteration and no per-sample branch on
// convergence: cost is one Lambert-W and a handful of flops.
struct Korg35Filter {
    void setSampleRate(double sampleRate) {
        sr = sampleRate;
        dt = 1.0 / sampleRate;
    }
    void setBeta(double b) { beta = b; logBeta = std::log(b); }
    void reset() { x1 = 0.0; x2 = 0.0; }

    // cutoffHz and alpha are per-sample values: nothing is cached between
    // calls, so both can be modulated at audio rate.
    double processSample(double u, double cutoffHz, double alpha) {
        alpha = std::max(0.0, std::min(alpha, kAlphaLimit));
        const double fc = std::max(1.0, std::min(cutoffHz, 0.45 * sr));
        const double h = 2.0 * M_PI * fc * dt;

        const double gamma = gammaOf(x2, alpha, beta, logBeta);

        const double B11 = 1.0 + 0.5 * h;
        const double B12 = 0.5 * h * gamma;
        const double B21 = -0.5 * h;
        const double B22 = 1.0 - 0.5 * h * (gamma - 1.0);

        const double q1 = h * (-x1 - gamma * x2 - u);
        const double q2 = h * (x1 + (gamma - 1.0) * x2);

        const double det = B11 * B22 - B12 * B21;

        x1 += (q1 * B22 - B12 * q2) / det;
        x2 += (B11 * q2 - q1 * B21) / det;

        if (!std::isfinite(x1) || !std::isfinite(x2)
            || std::fabs(x1) > kStateLimit || std::fabs(x2) > kStateLimit) {
            reset();
        }
        return x2;
    }

    // The normalised state, for harnesses that want to look inside.
    double x1 = 0.0, x2 = 0.0;

private:
    double sr = 48000.0;
    double dt = 1.0 / 48000.0;
    double beta = kBeta;
    double logBeta = std::log(kBeta);
};

// ── reference: RK4 on the continuous equations ──────────────────────────────
// Not the production integrator. It exists so the discrete-gradient scheme has
// something to be wrong against, and is deliberately written the obvious way.
struct Korg35RK4 {
    void setSampleRate(double sampleRate) {
        sr = sampleRate;
        dt = 1.0 / sampleRate;
    }
    void setBeta(double b) { beta = b; }
    void reset() { state = State(); }

    double processSample(double u, double cutoffHz, double alpha) {
        const double omega = 2.0 * M_PI * cutoffHz;

        const State k1 = derivative(state, u, omega, alpha, beta);
        const State s2 = {state.x1 + 0.5 * dt * k1.x1,
                          state.x2 + 0.5 * dt * k1.x2};
        const State k2 = derivative(s2, u, omega, alpha, beta);
        const State s3 = {state.x1 + 0.5 * dt * k2.x1,
                          state.x2 + 0.5 * dt * k2.x2};
        const State k3 = derivative(s3, u, omega, alpha, beta);
        const State s4 = {state.x1 + dt * k3.x1, state.x2 + dt * k3.x2};
        const State k4 = derivative(s4, u, omega, alpha, beta);

        state.x1 += dt / 6.0 * (k1.x1 + 2.0 * k2.x1 + 2.0 * k3.x1 + k4.x1);
        state.x2 += dt / 6.0 * (k1.x2 + 2.0 * k2.x2 + 2.0 * k3.x2 + k4.x2);
        return state.x2;
    }

    State state;

private:
    double sr = 48000.0;
    double dt = 1.0 / 48000.0;
    double beta = kBeta;
};

// ── control mappings ────────────────────────────────────────────────────────
// One place for the knob semantics, so the DSP never learns about knobs and
// the module never learns about alpha.

// Resonance. Mapping straight onto alpha wastes the control: the small-signal
// pole Q is 1/(2 - alpha), so nothing much happens below alpha = 1.5 and
// everything happens in the last tenth. Instead make Q exponential in the
// knob up to kResOscKnob, then run alpha from there to the circuit's ceiling.
static const double kResOscKnob = 0.85;   // where self-oscillation starts
static const double kResQMin = 0.5;       // Q at knob 0, i.e. alpha = 0
static const double kResQMax = 50.0;      // Q just below the oscillation point

inline double alphaFromKnob(double r) {
    r = std::max(0.0, std::min(r, 1.0));
    if (r <= kResOscKnob) {
        const double q = kResQMin
            * std::pow(kResQMax / kResQMin, r / kResOscKnob);
        return 2.0 - 1.0 / q;
    }
    const double a0 = 2.0 - 1.0 / kResQMax;
    const double t = (r - kResOscKnob) / (1.0 - kResOscKnob);
    return a0 + t * (kAlphaMax - a0);
}

// Cutoff, in octaves above kFcMin. The MS-20's own cutoff sweep is wider than
// the audio band at both ends; this is the useful part of it.
static const double kFcMin = 20.0;
static const double kFcOctaves = 10.0;    // 20 Hz - 20.48 kHz

// Rack volts to circuit volts. A 10 Vpp modular signal is the +-1 V the
// filter's audio input expects, and the model's DC gain is exactly -1, so the
// same factor undoes it on the way out: at unity drive and unity level the
// module is a unity-gain filter. Nothing is normalised behind the user's back
// beyond this one documented constant.
static const double kVoltScale = 0.2;

}  // namespace viginti
