// fracture_dsp.hpp — the engine shared by crepitus and ruina.
//
// A material being worked emits sound as a point process: discrete buckling
// or micro-fracture events, each one an impact on the object it is part of.
// The SDT ships the two ends of this as separate models — SDTCrumpling is a
// stationary process (paper being squeezed forever) and SDTBreaking is the
// same process running down a finite energy budget (something coming apart) —
// and nothing in between.
//
// This is the two ends as one continuum, by making the process self-exciting.
// The event rate is
//
//     lambda(t) = base + sum over past events of  a * exp(-(t - ti)/tau)
//
// which is a Hawkes process: every event raises the chance of the next. With
// `a` scaled so the expected number of children per event is exactly the
// CRIT knob, the knob is the branching ratio and it names three regimes:
//
//   crit < 1   subcritical. Events are independent-ish; the texture is
//              stationary. This is crumpling: paper, gravel, rain on a roof.
//   crit ~ 1   critical. Events arrive in bursts of every size, with the
//              power-law avalanche statistics of a crack front. Tearing.
//   crit > 1   supercritical. One event sets off a cascade. Because the rate
//              also scales with how much object is left, the cascade eats the
//              object down to integrity = 1/crit and parks there: the process
//              drives itself onto its own critical point. Breaking, and then
//              breaking again as the module's front end lets it recover.
//
// The event energies keep the SDT's exponential draw (SDT_expRand(1.45),
// clipped) and the fragment sizes its fragmentation rule, so the grain of an
// individual event is stock SDT. What is new is only when they happen.
//
// Each event drives an impact between an inertial hammer and a modal object,
// exactly as the SDT's own crumpling~ and breaking~ help patches wire it:
// energy to the hammer's velocity, fragment size to both fragment sizes.
#pragma once

#include "sdt/sdt_common.hpp"
#include "sdt/sdt_control.hpp"
#include "sdt/sdt_filters.hpp"
#include "sdt/sdt_interactors.hpp"
#include "sdt/sdt_material.hpp"

namespace fracture {

struct Result {
    double audio = 0.0;
    bool event = false;
    double energy = 0.0;   // of the event, 0 if none
    double size = 0.0;
};

struct Engine {
    sdt::Resonator hammer, object;
    sdt::Impact impact;
    sdt::DCFilter dc;
    sdt::Rng rng{20260814u};

    double sampleRate = 48000.0;
    // ── point process ───────────────────────────────────────────────────────
    double baseRate = 20.0;       // events per second, before self-excitation
    double branching = 0.5;       // expected children per event: the CRIT knob
    double tau = 0.02;            // s, how long an event keeps its children near
    double excitation = 0.0;      // the decaying part of the intensity
    double kernelDecay = 0.999;   // exp(-1/(tau*sr)), cached
    // ── event grain ─────────────────────────────────────────────────────────
    double crushingEnergy = 1.0;
    double fragmentation = 0.5;
    double baseSize = 1.0;        // the intact object's fragment size
    // ── how much object is left ─────────────────────────────────────────────
    // 1 = intact, 0 = dust. Scales event energy and fragment size; the module
    // decides whether it recovers (crumpling) or depletes (breaking).
    double integrity = 1.0;
    // Two buckling events cannot happen at the same instant in the same
    // object, and an impact needs time to complete: without a refractory
    // period a supercritical setting saturates at one event per sample, which
    // pins the hammer's velocity instead of striking with it and the module
    // goes silent at exactly the setting that should be loudest.
    double refractory = 0.0005;   // s
    int refractoryLeft = 0;

    void init(double sr) {
        sampleRate = sr;
        hammer.setSampleRate(sr);
        object.setSampleRate(sr);
        impact.setSampleRate(sr);
        dc.setFrequency(12.0, sr);
        hammer.makeInertial(0.01);
        object.setNModes(sdt::kMaterialModes);
        object.setNPickups(1);
        object.setActiveModes(sdt::kMaterialModes);
        for (int m = 0; m < sdt::kMaterialModes; m++) object.setWeight(m, 0.02);
        object.setFragmentSize(1.0);
        impact.obj0 = &hammer;
        impact.obj1 = &object;
        impact.contact0 = 0;
        impact.contact1 = 0;
        impact.setStiffness(1e7);
        impact.setDissipation(0.001);
        impact.setShape(1.5);
        setTau(tau);
    }

    void reset() {
        hammer.reset();
        object.reset();
        impact.energy = 0.0;
        dc.reset();
        excitation = 0.0;
        refractoryLeft = 0;
    }

    void setTau(double t) {
        tau = std::max(0.001, t);
        kernelDecay = std::exp(-1.0 / (tau * sampleRate));
    }

    void setTone(const sdt::Material& mat, double f0, double decay) {
        // Pickup gain 100, as in the SDT's own crumpling~ and breaking~ help
        // patches. Unlike friction, impact has no velocity feedback worth
        // speaking of (its dissipation term is 0.001), so the gain is free to
        // do what it does here: it scales the penetration the contact
        // measures, and with it the contact time. At gain 1 the impact is so
        // soft that only the fundamental is excited and every material sounds
        // like a sine burst.
        sdt::applyMaterial(object, mat, f0, decay, 100.0);
    }

    // The current intensity in events per second, for a display or a light.
    double intensity() const { return integrity * (baseRate + excitation * sampleRate); }

    // An event the module asks for rather than one the process produced: the
    // manual strike, and the moment an object under load finally gives.
    Result strike(double energy, double size) {
        Result r;
        r.event = true;
        r.energy = energy;
        r.size = sdt::fclip(size, sdt::kMicro, 1.0);
        double outs[4] = {0, 0, 0, 0};
        impact.process(0.0, -r.energy, r.size, 0.0, 0.0, r.size, outs);
        r.audio = dc.process(outs[1]);
        if (!std::isfinite(r.audio)) {
            reset();
            r.audio = 0.0;
        }
        return r;
    }

    // One sample. With allowEvents false the process is muted but the object
    // still rings out, which is how a module holds a gate closed.
    Result process(bool allowEvents) {
        Result r;
        // Intensity per sample: base rate plus the decaying aftershock term,
        // both scaled by how much object is left. SDTBreaking scales its
        // event probability by the remaining energy in exactly this way, and
        // it is what makes the supercritical setting interesting rather than
        // a solid roar: the effective branching ratio is crit * integrity, so
        // a runaway cascade eats the object down to integrity = 1/crit and
        // parks there. The process organises itself onto its own critical
        // point, which is what a crack front does.
        const double lambda = integrity * (baseRate / sampleRate + excitation);
        if (refractoryLeft > 0) refractoryLeft--;
        if (allowEvents && refractoryLeft == 0 && integrity > 0.0 &&
            rng.frand() < sdt::fclip(lambda, 0.0, 1.0)) {
            const double fragment = 1.0 - fragmentation + fragmentation * rng.frand();
            r.energy = crushingEnergy * integrity *
                       sdt::fclip(rng.expRand(1.45), sdt::kUndershoot, sdt::kOvershoot);
            r.size = sdt::fclip(baseSize * std::max(sdt::kMicro,
                                                    fragment * (0.5 + 0.5 * rng.frand())),
                                sdt::kMicro, 1.0);
            r.event = true;
            refractoryLeft = (int)(refractory * sampleRate);
            // Scaled so the expected number of children of this event is
            // exactly `branching`, whatever tau and the sample rate are.
            excitation += branching * (1.0 - kernelDecay);
        }
        excitation *= kernelDecay;

        // The SDT wiring: event energy is the hammer's downward velocity,
        // event size is the fragment size of both bodies.
        double outs[4] = {0, 0, 0, 0};
        impact.process(0.0, -r.energy, r.size, 0.0, 0.0, r.size, outs);
        r.audio = dc.process(outs[1]);
        if (!std::isfinite(r.audio)) {
            reset();
            r.audio = 0.0;
        }
        return r;
    }
};

}  // namespace fracture
