// sdt_interactors.hpp — the two contact models: impact (Hunt-Crossley in the
// Marhefka-Orin form) and dry friction (the elasto-plastic bristle model).
//
// Ported from SDTInteractors.c (Sound Design Toolkit, GPL-3.0-or-later).
//
// An interactor computes the contact force between two resonators from their
// relative position and velocity, then applies it to both with opposite signs.
// The energy check around it is the part that keeps these stable: a contact is
// not allowed to hand the pair more energy than they had, and the bisection
// walks the force back down until it doesn't.
#pragma once

#include "sdt_common.hpp"
#include "sdt_resonator.hpp"

namespace sdt {

// Base: two resonators, one contact point on each, the energy bookkeeping.
struct Interactor {
    Resonator* obj0 = nullptr;
    Resonator* obj1 = nullptr;
    int contact0 = 0, contact1 = 0;
    double energy = 0.0;
    double timeStep = 1.0 / 48000.0;

    virtual ~Interactor() {}
    virtual double force() = 0;

    void setSampleRate(double sr) { timeStep = 1.0 / sr; }

    // Bisect the contact force down until the pair does not gain energy.
    double solvedForce() {
        static const double kMaxError = 0.001;
        static const int kMaxIterations = 50;

        double f = force();
        const double h = obj0->computeEnergy(contact0, 0.0) +
                         obj1->computeEnergy(contact1, 0.0) + energy;
        double w = obj0->computeEnergy(contact0, f) + obj1->computeEnergy(contact1, -f) - h;
        if (w > 0.0) {
            double f0 = 0.0, f1 = f;
            int count = 0;
            while ((w > 0.0 || w < -kMaxError * h) && count < kMaxIterations) {
                f = (f0 + f1) / 2.0;
                w = obj0->computeEnergy(contact0, f) + obj1->computeEnergy(contact1, -f) - h;
                if (w < 0.0)
                    f0 = f;
                else
                    f1 = f;
                count++;
            }
        }
        energy = -w;
        return f;
    }

    // One sample. f0/f1 are external forces, v0/v1 externally imposed
    // velocities (0 = don't touch), s0/s1 fragment sizes (0 = don't touch).
    // Returns the displacement of each object at its first pickup.
    void process(double f0, double v0, double s0, double f1, double v1, double s1, double* outs) {
        if (obj0) obj0->applyForce(contact0, f0);
        if (obj1) obj1->applyForce(contact1, f1);
        if (s0 != 0.0 && obj0) obj0->setFragmentSize(s0);
        if (s1 != 0.0 && obj1) obj1->setFragmentSize(s1);
        if (v0 != 0.0 && obj0) {
            obj0->setPosition(contact0, obj1 ? obj1->getPosition(contact1) : 0.0);
            obj0->setVelocity(contact0, v0);
        }
        if (v1 != 0.0 && obj1) {
            obj1->setPosition(contact1, obj0 ? obj0->getPosition(contact0) : 0.0);
            obj1->setVelocity(contact1, v1);
        }
        if (obj0 && obj1) {
            const double f = solvedForce();
            obj0->applyForce(contact0, f);
            obj1->applyForce(contact1, -f);
        }
        int n = 0;
        if (obj0) {
            obj0->process();
            for (int p = 0; p < obj0->nPickups; p++) outs[n++] = obj0->getPosition(p);
        }
        if (obj1) {
            obj1->process();
            for (int p = 0; p < obj1->nPickups; p++) outs[n++] = obj1->getPosition(p);
        }
    }
};

// Impact: f = k * p^shape * (1 + dissipation * v), only while interpenetrating.
struct Impact : Interactor {
    double stiffness = 0.0, dissipation = 0.0, shape = 1.0;

    void setStiffness(double f) { stiffness = std::max(0.0, f); }
    void setDissipation(double f) { dissipation = std::max(0.0, f); }
    void setShape(double f) { shape = std::max(1.0, f); }

    double force() override {
        const double p = obj1->getPosition(contact1) - obj0->getPosition(contact0);
        if (p <= 0.0) {
            energy = 0.0;
            return 0.0;
        }
        const double v = obj1->getVelocity(contact1) - obj0->getVelocity(contact0);
        return stiffness * std::pow(p, shape) * (1.0 + dissipation * v);
    }
};

// Friction: the elasto-plastic bristle model. z is the average bristle
// deflection; alpha is how much of the sliding is plastic. The stick-slip
// cycle this produces is the creak at low velocity and the squeal at high.
struct Friction : Interactor {
    double fn = 0.0;    // normal force, N
    double vs = 0.1;    // Stribeck velocity
    double ks = 0.8;    // static friction coefficient
    double kd = 0.2;    // dynamic friction coefficient
    double kba = 0.1;   // break-away fraction
    double s0 = 1000.0; // bristle stiffness
    double s1 = 10.0;   // bristle dissipation
    double s2 = 10.0;   // viscosity
    double s3 = 0.5;    // noisiness
    double fs = 0.0, fc = 0.0, z = 0.0;
    // The plastic fraction of the last sample's bristle displacement: 0 while
    // the contact sticks, 1 once it is fully sliding. Not part of the SDT API,
    // exposed here because it is the model's own account of a slip event.
    double alpha = 0.0;
    Rng rng{20260809u};

    void setNormalForce(double f) {
        fn = std::max(0.0, f);
        fs = fn * ks;
        fc = fn * kd;
    }
    void setStribeckVelocity(double f) { vs = std::max(0.0, f); }
    void setStaticCoefficient(double f) {
        ks = fclip(f, 0.0, 1.0);
        fs = fn * ks;
    }
    void setDynamicCoefficient(double f) {
        kd = fclip(f, 0.0, 1.0);
        fc = fn * kd;
    }
    void setBreakAway(double f) { kba = fclip(f, 0.0, 1.0); }
    void setStiffness(double f) { s0 = std::max(0.0, f); }
    void setDissipation(double f) { s1 = std::max(0.0, f); }
    void setViscosity(double f) { s2 = std::max(0.0, f); }
    void setNoisiness(double f) { s3 = std::max(0.0, f); }

    double force() override {
        energy = 0.0;
        const double v = obj1->getVelocity(contact1) - obj0->getVelocity(contact0);
        if (fn <= 0.0) {
            z = 0.0;
            return 0.0;
        }
        const double vRatio = v / vs;
        const int vSgn = signum(v), zSgn = signum(z);
        const double zss = vSgn * (fc + (fs - fc) * std::exp(-vRatio * vRatio)) / s0;
        const double zba = vSgn * kba * fc / s0;
        if (vSgn != zSgn)
            alpha = 0.0;
        else if (std::fabs(z) < std::fabs(zba))
            alpha = 0.0;
        else if (std::fabs(z) < std::fabs(zss))
            alpha = 0.5 + 0.5 * std::sin(kPi * (z - 0.5 * (zss + zba)) / (zss - zba));
        else
            alpha = 1.0;
        double dz = v * (1.0 - alpha * z / zss);
        if (!std::isfinite(dz)) dz = 0.0;
        const double w = rng.white() * std::sqrt(std::fabs(v) * fn);
        const double f = s0 * z + s1 * dz + s2 * v + s3 * w;
        z += dz * timeStep;
        return f;
    }
};

}  // namespace sdt
