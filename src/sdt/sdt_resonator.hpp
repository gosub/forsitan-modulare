// sdt_resonator.hpp — the modal resonator every SDT solid model hangs off.
//
// Ported from SDTResonators.c (Sound Design Toolkit, GPL-3.0-or-later).
// A set of parallel mass-spring-damper oscillators, one per normal mode,
// discretised with the impulse-invariant method. A mode at 0 Hz with infinite
// decay is an inertial point mass, which is how the "hammer" and the "probe"
// in these modules are built.
//
// Changes from the C: fixed-capacity arrays instead of malloc, the per-pickup
// gain sum kept in its own field instead of one past the end of the gain row,
// and the sample rate carried by the object.
#pragma once

#include <algorithm>

#include "sdt_common.hpp"

namespace sdt {

static const int kMaxModes = 16;
static const int kMaxPickups = 2;
// SDT clamps modal displacement here; without it a stiff impact can send a
// mode to infinity in one sample.
static const double kMaxPos = 10000.0;

struct Resonator {
    // per mode
    double freqs[kMaxModes] = {};
    double decays[kMaxModes] = {};
    double weights[kMaxModes] = {};
    double m[kMaxModes] = {}, k[kMaxModes] = {};
    double b1[kMaxModes] = {}, a1[kMaxModes] = {}, a2[kMaxModes] = {};
    double b0v[kMaxModes] = {}, b1v[kMaxModes] = {};
    double p0[kMaxModes] = {}, p1[kMaxModes] = {}, v[kMaxModes] = {}, f[kMaxModes] = {};
    // per pickup
    double gains[kMaxPickups][kMaxModes] = {};
    double gainSum[kMaxPickups] = {};

    double fragmentSize = 1.0;
    double timeStep = 1.0 / 48000.0;
    int nModes = 1, nPickups = 1, activeModes = 1;

    void setSampleRate(double sr) {
        timeStep = 1.0 / sr;
        update();
    }

    void updateMode(int mode) {
        const double u = std::sqrt(fragmentSize);
        const double w = kTwoPi * freqs[mode];
        const double wt = w * timeStep / u;
        const double mm = weights[mode] * fragmentSize;
        const double kk = w * w * weights[mode];
        if (wt < std::acos(-0.9995) && mm > kMicro) {
            const double d = decays[mode] * u;
            const double g = d > 0.0 ? 2.0 / d : 0.0;
            const double r = std::exp(-g * timeStep);
            const double coswt = std::cos(wt);
            const double sincwt = wt > 0.0 ? std::sin(wt) / wt : 1.0;
            const double tsincwt = sincwt * timeStep;
            b1[mode] = r * sincwt * timeStep * timeStep / mm;
            a1[mode] = -2.0 * r * coswt;
            a2[mode] = r * r;
            b0v[mode] = coswt / tsincwt - g;
            b1v[mode] = -r / tsincwt;
            v[mode] *= std::sqrt(m[mode] / mm);
            p0[mode] *= kk > 0.0 ? std::sqrt(k[mode] / kk) : 1.0;
            updateState(mode);
            m[mode] = mm;
            k[mode] = kk;
        } else {
            m[mode] = k[mode] = 0.0;
            b1[mode] = a1[mode] = a2[mode] = b0v[mode] = b1v[mode] = 0.0;
        }
    }

    void updatePickup(int pickup) {
        double s = 0.0;
        for (int mode = 0; mode < activeModes; mode++) s += gains[pickup][mode];
        gainSum[pickup] = s;
    }

    void update() {
        for (int mode = 0; mode < activeModes; mode++) updateMode(mode);
        for (int p = 0; p < nPickups; p++) updatePickup(p);
    }

    void setNModes(int n) {
        nModes = iclip(n, 1, kMaxModes);
        activeModes = std::min(activeModes, nModes);
    }
    void setNPickups(int n) { nPickups = iclip(n, 1, kMaxPickups); }
    void setActiveModes(int n) {
        activeModes = iclip(n, 0, nModes);
        update();
    }
    void setFrequency(int mode, double hz) {
        if (mode < nModes) {
            freqs[mode] = hz;
            updateMode(mode);
        }
    }
    void setDecay(int mode, double s) {
        if (mode < nModes) {
            decays[mode] = std::max(0.0, s);
            updateMode(mode);
        }
    }
    void setWeight(int mode, double kg) {
        if (mode < nModes) {
            weights[mode] = std::max(0.0, kg);
            updateMode(mode);
        }
    }
    void setGain(int pickup, int mode, double g) {
        if (mode < nModes && pickup < nPickups) {
            gains[pickup][mode] = std::max(0.0, g);
            updatePickup(pickup);
        }
    }
    void setFragmentSize(double s) {
        fragmentSize = fclip(s, 0.0, 1.0);
        for (int mode = 0; mode < activeModes; mode++) updateMode(mode);
    }

    double getPosition(int pickup) const {
        double out = 0.0;
        if (pickup < nPickups)
            for (int mode = 0; mode < activeModes; mode++) out += p0[mode] * gains[pickup][mode];
        return out;
    }
    double getVelocity(int pickup) const {
        double out = 0.0;
        if (pickup < nPickups)
            for (int mode = 0; mode < activeModes; mode++) out += v[mode] * gains[pickup][mode];
        return out;
    }
    void setPosition(int pickup, double x) {
        if (pickup < nPickups && gainSum[pickup] > 0.0)
            for (int mode = 0; mode < activeModes; mode++) {
                p0[mode] = x / gainSum[pickup];
                updateState(mode);
            }
    }
    void setVelocity(int pickup, double x) {
        if (pickup < nPickups && gainSum[pickup] > 0.0)
            for (int mode = 0; mode < activeModes; mode++) {
                v[mode] = x / gainSum[pickup];
                updateState(mode);
            }
    }

    void applyForce(int pickup, double force) {
        if (pickup >= nPickups) return;
        if (!std::isfinite(force)) force = 0.0;
        double fs[kMaxModes];
        distributeForce(pickup, fs, force);
        for (int mode = 0; mode < activeModes; mode++) f[mode] += fs[mode];
    }

    // Sum of kinetic and potential energy after adding `force`, used by the
    // interactor to keep a contact from injecting energy it does not have.
    double computeEnergy(int pickup, double force) {
        if (pickup >= nPickups) return 0.0;
        if (!std::isfinite(force)) force = 0.0;
        double fs[kMaxModes];
        distributeForce(pickup, fs, force);
        double out = 0.0;
        for (int mode = 0; mode < activeModes; mode++) {
            const double p = modalPosition(mode, f[mode] + fs[mode]);
            const double vv = modalVelocity(mode, p);
            out += 0.5 * (k[mode] * p * p + m[mode] * vv * vv) * gains[pickup][mode];
        }
        return out;
    }

    void process() {
        for (int mode = 0; mode < activeModes; mode++) {
            const double p = modalPosition(mode, f[mode]);
            v[mode] = modalVelocity(mode, p);
            p1[mode] = p0[mode];
            p0[mode] = p;
            f[mode] = 0.0;
        }
    }

    void reset() {
        for (int mode = 0; mode < kMaxModes; mode++) p0[mode] = p1[mode] = v[mode] = f[mode] = 0.0;
    }

    // ── internals ───────────────────────────────────────────────────────────
    double modalPosition(int mode, double force) const {
        return fclip(b1[mode] * force - a1[mode] * p0[mode] - a2[mode] * p1[mode], -kMaxPos,
                     kMaxPos);
    }
    double modalVelocity(int mode, double p) const {
        return b0v[mode] * p + b1v[mode] * p0[mode];
    }
    void updateState(int mode) {
        if (b1v[mode] != 0.0) p1[mode] = (v[mode] - b0v[mode] * p0[mode]) / b1v[mode];
    }
    void distributeForce(int pickup, double* fs, double force) const {
        for (int mode = 0; mode < activeModes; mode++)
            fs[mode] = gainSum[pickup] > 0.0 ? force * gains[pickup][mode] / gainSum[pickup]
                                             : force / std::max(1, activeModes);
    }

    // Convenience: an inertial point mass, one mode at 0 Hz, no decay.
    void makeInertial(double massKg) {
        setNModes(1);
        setNPickups(1);
        setActiveModes(1);
        setFrequency(0, 0.0);
        setDecay(0, 0.0);
        setWeight(0, massKg);
        setGain(0, 0, 1.0);
        setFragmentSize(1.0);
    }
};

}  // namespace sdt
