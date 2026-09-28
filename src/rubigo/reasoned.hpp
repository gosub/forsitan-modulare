// reasoned.hpp - rubigo's reasoned random: a whole patch drawn from one of
// five archetypes, instead of every knob drawn uniformly.
//
// The ranges come from the principles doc/design/rubigo.md reads out of the
// Metal Fetishist's Preset Book #1: the pitch envelope is either off or
// kick-shaped and never in between, NOISE is none, half or all, RESONANCE is
// zero unless the filter is the voice, STEPS is OFF, 8 or 16, and so on. A
// uniform draw lands mostly between those regions, which is where the
// silences and the mush are.
//
// Knobs are 0..1, switches as the engine names them, STEPS a detent 0..6.
// Rack-free, so the probe can count what it gives.

#pragma once

#include "rubigo.hpp"

namespace rubigo {

enum Archetype { ARCH_KICK = 0, ARCH_BASS, ARCH_NOISE, ARCH_RESONANT, ARCH_DRONE, ARCH_LEN };

inline const char* archetypeName(int a) {
    static const char* names[ARCH_LEN] = {"Kick", "Bass", "Noise", "Resonant", "Drone"};
    return a >= 0 && a < ARCH_LEN ? names[a] : "";
}

// The knobs and switches a patch sets. The context menu is not part of it.
struct Patch {
    float pitch = 0.f, pitchDecay = 0.f, pitchAmount = 0.f;
    float noise = 0.f, cutoff = 0.f, resonance = 0.f;
    float cutoffDecay = 0.f, cutoffAmount = 0.f;
    float volume = 0.f, volumeDecay = 0.f;
    float effect = 0.f, mix = 1.f;
    float tempo = 0.f, skips = 0.f, stepMod = 0.f;
    int wave = SAW, dest = DEST_PITCH, steps = 0;
    bool highpass = false, rust = false;
};

const int kDetentOff = 0, kDetent8 = 3, kDetent16 = 5;

// `U` returns a uniform number in [0, 1).
template <typename U>
struct Roller {
    U& u;
    explicit Roller(U& uniform) : u(uniform) {}
    float in(float lo, float hi) { return lo + (hi - lo) * u(); }
    bool chance(float p) { return u() < p; }
    int pick(int n) { return std::min((int)(u() * n), n - 1); }

    // The book's VOLUME: higher with CORROSION, lower with RUST.
    float volumeFor(bool rust) { return rust ? in(0.6f, 0.7f) : in(0.72f, 0.82f); }
    // The effect amount: none, or at least half; RUST always at least half.
    float effectFor(bool rust) { return rust || chance(0.6f) ? in(0.5f, 0.95f) : 0.f; }
    // Continuous material is unskipped; rhythms skip between 0.3 and 0.7.
    float rhythmSkips() { return chance(0.3f) ? 0.f : in(0.3f, 0.7f); }
    int rhythmSteps() { return chance(0.55f) ? kDetent16 : chance(0.6f) ? kDetent8 : kDetentOff; }
};

template <typename U>
Patch roll(int archetype, U& uniform) {
    Roller<U> r(uniform);
    Patch p;
    p.wave = r.chance(0.5f) ? SAW : SQUARE;
    p.mix = 1.f;
    switch (archetype) {
        case ARCH_KICK:
        default:
            p.pitch = r.in(0.f, 0.15f);
            p.pitchDecay = r.in(0.18f, 0.35f);
            p.pitchAmount = r.in(0.5f, 0.85f);
            p.noise = 0.f;
            p.cutoff = r.in(0.2f, 0.4f);
            p.resonance = r.chance(0.6f) ? 0.f : r.in(0.1f, 0.3f);
            if (r.chance(0.5f)) {
                p.cutoffDecay = r.in(0.1f, 0.35f);
                p.cutoffAmount = r.in(0.4f, 0.8f);
            }
            p.volumeDecay = r.in(0.35f, 0.5f);
            p.rust = r.chance(0.2f);
            p.effect = p.rust ? r.in(0.5f, 0.7f) : r.effectFor(false);
            p.tempo = r.in(0.25f, 0.36f);
            p.skips = r.rhythmSkips();
            p.steps = r.chance(0.5f) ? kDetent16 : kDetent8;
            if (r.chance(0.6f)) {
                p.dest = DEST_NOISE;
                p.stepMod = r.in(0.6f, 1.f);
            } else {
                p.dest = DEST_PITCH;
                p.stepMod = r.in(0.2f, 0.4f);
            }
            break;
        case ARCH_BASS:
            p.pitch = r.in(0.f, 0.25f);
            if (r.chance(0.4f)) {
                p.pitchDecay = r.in(0.15f, 0.25f);
                p.pitchAmount = r.in(0.2f, 0.5f);
            }
            p.noise = 0.f;
            p.cutoff = r.in(0.15f, 0.3f);
            p.resonance = r.in(0.5f, 1.f);
            p.cutoffDecay = r.in(0.25f, 0.45f);
            p.cutoffAmount = r.in(0.5f, 0.85f);
            p.volumeDecay = r.in(0.45f, 0.7f);
            p.rust = false;
            p.effect = r.chance(0.3f) ? 0.f : r.in(0.4f, 0.6f);
            p.tempo = r.in(0.3f, 0.4f);
            p.skips = r.in(0.2f, 0.6f);
            p.steps = r.chance(0.6f) ? kDetent16 : kDetent8;
            p.dest = DEST_PITCH;
            p.stepMod = r.in(0.3f, 0.55f);
            break;
        case ARCH_NOISE:
            p.pitch = r.in(0.f, 0.2f);
            p.noise = r.chance(0.5f) ? 1.f : 0.5f;
            p.highpass = r.chance(0.3f);
            p.cutoff = p.highpass ? r.in(0.4f, 0.6f) : r.in(0.3f, 1.f);
            p.resonance = r.in(0.f, 0.2f);
            p.volumeDecay = r.in(0.3f, 0.5f);
            p.rust = r.chance(0.7f);
            p.effect = p.rust ? r.in(0.6f, 1.f) : r.in(0.5f, 0.7f);
            p.tempo = r.in(0.3f, 0.4f);
            p.skips = r.in(0.3f, 0.7f);
            p.steps = r.rhythmSteps();
            if (r.chance(0.6f)) {
                p.dest = DEST_CUTOFF;
                p.stepMod = r.in(0.4f, 0.8f);
            } else {
                p.dest = DEST_NOISE;
                p.stepMod = r.in(0.5f, 1.f);
                p.noise = 0.5f;
            }
            break;
        case ARCH_RESONANT:
            p.wave = SAW;
            p.pitch = r.in(0.1f, 0.3f);
            if (r.chance(0.5f)) {
                p.pitchDecay = r.in(0.18f, 0.3f);
                p.pitchAmount = r.in(0.5f, 0.8f);
            }
            p.noise = r.in(0.f, 0.2f);
            p.highpass = r.chance(0.6f);
            p.cutoff = p.highpass ? r.in(0.45f, 0.55f) : r.in(0.f, 0.2f);
            p.resonance = r.in(0.65f, 1.f);
            p.cutoffDecay = r.in(0.5f, 0.9f);
            p.cutoffAmount = r.in(0.7f, 1.f);
            p.volumeDecay = r.chance(0.3f) ? r.in(0.9f, 1.f) : r.in(0.3f, 0.5f);
            p.rust = true;
            p.effect = r.in(0.5f, 0.7f);
            p.tempo = r.in(0.2f, 0.36f);
            p.skips = r.in(0.f, 0.5f);
            p.steps = r.chance(0.6f) ? kDetent16 : kDetentOff;
            p.dest = DEST_CUTOFF;
            p.stepMod = r.in(0.6f, 1.f);
            break;
        case ARCH_DRONE:
            p.pitch = r.in(0.f, 0.3f);
            {
                int n = r.pick(3);
                p.noise = n == 0 ? 0.f : n == 1 ? 0.5f : 1.f;
            }
            p.cutoff = r.in(0.2f, 0.5f);
            p.resonance = r.in(0.3f, 0.8f);
            p.volumeDecay = r.in(0.95f, 1.f);
            p.rust = r.chance(0.5f);
            p.effect = p.rust ? r.in(0.6f, 1.f) : r.in(0.5f, 0.7f);
            p.tempo = r.in(0.65f, 1.f);
            p.skips = 0.f;
            p.steps = kDetentOff;
            if (r.chance(0.7f)) {
                p.dest = DEST_CUTOFF;
                p.stepMod = r.in(0.4f, 0.7f);
            } else {
                p.dest = DEST_NOISE;
                p.stepMod = 0.5f;
            }
            break;
    }
    p.volume = r.volumeFor(p.rust);
    return p;
}

// Any archetype, equally likely.
template <typename U>
Patch rollAny(U& uniform) {
    int a = std::min((int)(uniform() * ARCH_LEN), ARCH_LEN - 1);
    return roll(a, uniform);
}

// A patch into the engine's controls, for the probe.
inline void apply(const Patch& p, Controls& c) {
    c.pitch = p.pitch; c.pitchDecay = p.pitchDecay; c.pitchAmount = p.pitchAmount;
    c.noise = p.noise; c.cutoff = p.cutoff; c.resonance = p.resonance;
    c.cutoffDecay = p.cutoffDecay; c.cutoffAmount = p.cutoffAmount;
    c.volume = p.volume; c.volumeDecay = p.volumeDecay;
    c.effect = p.effect; c.mix = p.mix;
    c.tempo = p.tempo; c.skips = p.skips; c.stepMod = p.stepMod;
    c.wave = p.wave; c.dest = p.dest; c.steps = p.steps;
    c.highpass = p.highpass; c.rust = p.rust;
}

}  // namespace rubigo
