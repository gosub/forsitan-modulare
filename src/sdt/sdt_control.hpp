// sdt_control.hpp — the control layer: stochastic processes that drive the
// contact models rather than make sound themselves.
//
// Ported from SDTControl.c (Sound Design Toolkit, GPL-3.0-or-later).
//
//   Crumpling — a stationary point process of buckling events. Each event has
//               an exponentially distributed energy and a fragment size. Feed
//               energy to the impact velocity and size to the resonators'
//               fragment size and you get paper being squeezed.
//   Breaking  — the same process running down a finite energy budget: events
//               get weaker and rarer as the object comes apart, and it stops.
//   Scraping  — a surface profile: noise in, a sawtooth-decaying "ground
//               trace" out, whose bumps are the force of a probe dragged over
//               a rough surface at a given speed.
#pragma once

#include "sdt_common.hpp"

namespace sdt {

// SDT_fclip bounds on the exponential energy draw.
static const double kUndershoot = 0.1;
static const double kOvershoot = 10.0;

struct Event {
    double energy = 0.0;
    double size = 0.0;
};

struct Crumpling {
    double crushingEnergy = 1.0;  // J, the energy of the average event
    double granularity = 0.01;    // event probability per sample
    double fragmentation = 0.5;   // spread of fragment sizes
    Rng rng{20260810u};

    void setCrushingEnergy(double f) { crushingEnergy = std::max(kMicro, f); }
    void setGranularity(double f) { granularity = fclip(f, 0.0, 1.0); }
    void setFragmentation(double f) { fragmentation = fclip(f, 0.0, 1.0); }

    Event process() {
        Event e;
        if (rng.frand() < granularity) {
            const double fragment = 1.0 - fragmentation + fragmentation * rng.frand();
            e.energy = crushingEnergy * fclip(rng.expRand(1.45), kUndershoot, kOvershoot);
            e.size = std::max(kMicro, fragment * (0.5 + 0.5 * rng.frand()));
        }
        return e;
    }
};

struct Breaking {
    double storedEnergy = 100.0;  // J, how much the object holds before it goes
    double crushingEnergy = 1.0;
    double granularity = 0.01;
    double fragmentation = 0.5;
    double remainingEnergy = 0.0;
    Rng rng{20260811u};

    void setStoredEnergy(double f) { storedEnergy = std::max(kMicro, f); }
    void setCrushingEnergy(double f) { crushingEnergy = std::max(kMicro, f); }
    void setGranularity(double f) { granularity = fclip(f, 0.0, 1.0); }
    void setFragmentation(double f) { fragmentation = fclip(f, 0.0, 1.0); }

    void reset() { remainingEnergy = 1.0; }
    bool finished() const { return remainingEnergy <= crushingEnergy / storedEnergy; }

    Event process() {
        Event e;
        if (!finished()) {
            const double success = granularity * remainingEnergy;
            if (rng.frand() < success) {
                const double fragment =
                    1.0 - fragmentation + fragmentation * remainingEnergy;
                e.energy = crushingEnergy * remainingEnergy *
                           fclip(rng.expRand(1.45), kUndershoot, kOvershoot);
                e.size = std::max(kMicro, fragment * (0.5 + 0.5 * rng.frand()));
                remainingEnergy -= e.energy / storedEnergy;
            }
        } else {
            remainingEnergy = 0.0;
        }
        return e;
    }
};

// The decay of the ground trace: how fast the probe leaves a bump behind,
// which is grain times speed — SDT_groundDecay.
inline double groundDecay(double grain, double velocity) {
    return fclip(2.0 * grain * std::fabs(velocity), 0.0, 2.0);
}

struct Scraping {
    double grain = 0.0;     // probe width against the surface texture
    double force = 0.0;     // N
    double velocity = 0.0;  // m/s
    double decay = 0.0, groundTrace = 0.0;

    void setGrain(double f) {
        grain = fclip(f, 0.0, 1.0);
        decay = groundDecay(grain, velocity);
    }
    void setForce(double f) { force = std::max(0.0, f); }
    void setVelocity(double f) {
        velocity = f;
        decay = groundDecay(grain, velocity);
    }

    // `in` is the surface profile, usually noise.
    double process(double in) {
        double out = 0.0;
        const double currGround = std::max(groundTrace - decay, in);
        if (currGround > groundTrace && decay != 0.0) {
            const double bump = (currGround - groundTrace) / std::sqrt(decay);
            out -= force * velocity * velocity * bump;
        }
        groundTrace = currGround;
        return out;
    }
};

}  // namespace sdt
