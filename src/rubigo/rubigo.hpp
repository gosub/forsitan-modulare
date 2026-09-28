// rubigo.hpp - the engine of rubigo: a digital percussion voice played by
// its own generative sequencer.
//
// A clone of Body Synths' Metal Fetishist (firmware v2.0), written from the
// hardware's reference manual and doc/design/rubigo.md, which marks what the
// manual leaves open and what was decided in its place. The voice is built
// from DaisySP's blocks (daisy.hpp), the library the hardware's Daisy
// platform ships with, and its knobs follow Daisy's fmap laws.
//
//   osc (saw/square) --.
//                      +-- NOISE crossfade -- Svf (LP/HP, fixed drive) -- effect -- VCA + clip -- DC block -- out
//   noise / ext in ----'                        (Overdrive, + Decimator on RUST, 2nd osc, phaser, flanger, chorus)
//
//   three AdEnv decay envelopes, fired together: pitch (+Hz), cutoff, volume
//
//   clock -> step -> [ skip >= SKIPS ? fire : hold ] -> step value -> PITCH / NOISE / CUTOFF (or the assigned target)
//
// Every modulation adds to its knob, as on the hardware: PITCH is the lowest
// pitch, CUTOFF the lowest cutoff, and the envelopes and the step value push
// up from there, in hertz.
// The envelope and step value raise the cutoff in LP and lower it in HP,
// which is what the hardware does, measured on its resonant kick.
//
// The sequencer keeps a table of 32 slots, each a pair of uniform random
// numbers: one compared against SKIPS to decide whether the step fires, one
// scaled by STEP MOD to become the step value. Both comparisons and the
// scaling are done live, so turning SKIPS moves triggers inside a locked
// loop one slot at a time and turning STEP MOD rescales it without changing
// its shape. A skipped step holds the previous value.
//
// The header is free of Rack headers so the probe can drive it alone.

#pragma once

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "daisy.hpp"

namespace rubigo {

// ---------------------------------------------------------------- constants

const float kPi = 3.14159265358979f;

const float kPitchMin = 30.f;             // Hz, PITCH knob (fmap LOG)
const float kPitchMax = 1500.f;
const float kPitchEnvMax = 600.f;         // Hz, pitch DECAY AMOUNT at full
const float kDecayMin = 0.001f;           // s, the three DECAY knobs (fmap EXP)
const float kDecayMax = 3.f;
const float kAttack = 0.001f;             // s, every envelope
const float kCutoffMin = 20.f;            // Hz, CUTOFF knob (fmap EXP)
const float kCutoffMax = 16000.f;         // the Svf's own ceiling at 48 kHz
// AdEnv's curves, one per envelope as the firmware can set them, each fitted
// to the Preset Book video. Volume: tau near 0.14 s at DECAY 0.36 and 0.87 s
// at 0.99, so tau = DECAY / 3.5. Pitch: the kick's sweep from pitch DECAY
// 0.27 is at 125 Hz after 50 ms and 60 Hz after 100 ms, a curve near -6.
// Cutoff: the resonant kick's HP sweep and the bassline's closing both need
// a curve near -10; the filter moves much faster than the volume.
const float kVolumeCurve = -3.5f;
const float kPitchCurve = -6.f;
const float kCutoffCurve = -10.f;
const float kFilterDrive = 2.0f;           // Svf::SetDrive, fixed
// The cutoff envelope adds hertz, AMOUNT x 5 kHz at its peak, as the pitch
// envelope does (the manual's "+600 Hz"); the step value adds up to 5 kHz x
// STEP MOD's law. A knob-space sum cannot fit the hardware: its acid peaks
// near 5 kHz from CUTOFF 0.28 at AMOUNT 0.81, its bassline near 2 kHz from
// CUTOFF 0.01 at AMOUNT 0.33 (Preset Book video).
const float kCutoffEnvHz = 5000.f;
// TEMPO's law: 0.4 + 79.6 k^2.4 Hz, the manual's ends through the step
// rates measured off the Preset Book video (4.1 Hz at 0.27, 6.2 at 0.355).
const float kTempoCurve = 2.4f;
const float kTempoMin = 0.4f;             // Hz
const float kTempoMax = 80.f;
const float kLfoMin = 0.05f;              // Hz, alternative effects' knob
const float kLfoMax = 8.f;
const float kPulse = 0.001f;              // s, TRIGGER and CLOCK outs
const float kLevel = 5.f;                 // volts, nominal output peak
const float kExtTimeoutMin = 2.f;         // s, external clock gives up after
const float kExtTimeoutPeriods = 4.f;     // ... or this many periods, if longer

const int kSlots = 32;
const int kLengths = 7;                   // detents of the STEPS knob
// The STEPS detents, OFF first, and the alternative set from the Options menu.
const int kLengthTable[kLengths] = {0, 2, 4, 8, 10, 16, 32};
const int kAltLengthTable[kLengths] = {0, 3, 5, 7, 12, 18, 24};
// TEMPO's seven zones under an external clock: negative divides.
const int kRatioTable[7] = {-8, -4, -2, 1, 2, 4, 8};

enum Wave { SAW = 0, SQUARE = 1 };
enum Dest { DEST_PITCH = 0, DEST_NOISE = 1, DEST_CUTOFF = 2 };
enum Effect { FX_DISTORTION = 0, FX_OSC2, FX_PHASER, FX_FLANGER, FX_CHORUS, FX_LEN };
// Mod assign: where the CUTOFF destination and the CUTOFF input go.
enum Assign {
    ASSIGN_CUTOFF = 0, ASSIGN_VOL_DECAY, ASSIGN_PITCH_AMOUNT, ASSIGN_CUTOFF_AMOUNT,
    ASSIGN_VOLUME, ASSIGN_EFFECT, ASSIGN_LEN
};

// ---------------------------------------------------------------- laws

inline float clamp01(float x) { return std::min(std::max(x, 0.f), 1.f); }

inline float expLaw(float knob, float lo, float hi) { return daisy::fmap(knob, lo, hi, daisy::LOG); }

inline float pitchHz(float knob) { return daisy::fmap(knob, kPitchMin, kPitchMax, daisy::LOG); }
inline float decaySeconds(float knob) { return daisy::fmap(knob, kDecayMin, kDecayMax, daisy::EXP); }
inline float cutoffHz(float knob) { return daisy::fmap(knob, kCutoffMin, kCutoffMax, daisy::EXP); }
inline float tempoHz(float knob) {
    return kTempoMin + (kTempoMax - kTempoMin) * std::pow(clamp01(knob), kTempoCurve);
}
// The knob positions for values, the inverses of the laws above, for typed
// entry.
inline float pitchKnob(float hz) { return clamp01(std::log(std::max(hz, 1e-3f) / kPitchMin) / std::log(kPitchMax / kPitchMin)); }
inline float decayKnob(float s) { return std::sqrt(clamp01((s - kDecayMin) / (kDecayMax - kDecayMin))); }
inline float cutoffKnob(float hz) { return std::sqrt(clamp01((hz - kCutoffMin) / (kCutoffMax - kCutoffMin))); }
inline float lfoKnob(float hz) { return clamp01(std::log(std::max(hz, 1e-6f) / kLfoMin) / std::log(kLfoMax / kLfoMin)); }
// The TEMPO position for a rate, the inverse of tempoHz.
inline float tempoKnob(float hz) {
    return std::pow(clamp01((hz - kTempoMin) / (kTempoMax - kTempoMin)), 1.f / kTempoCurve);
}
// The AMOUNT knobs and STEP MOD, on fmap's quadratic EXP law.
inline float amountLaw(float knob) { return daisy::fmap(knob, 0.f, 1.f, daisy::EXP); }
inline float amountKnob(float law) { return std::sqrt(clamp01(law)); }
inline float lfoHz(float knob) { return daisy::fmap(knob, kLfoMin, kLfoMax, daisy::LOG); }
// The Overdrive's drive, 0.25 (clean, unity) to 0.5 (a pre-gain of 24).
// Past 0.5 its gain runs into the hundreds: the hardware's explosions, at
// EFFECT 0.66 on CORROSION, is a dark filtered noise, which a pre-gain of
// 300 would clip into a bright wash.
inline float overdriveAmount(float knob) { return daisy::fmap(knob, 0.25f, 0.5f); }

// TEMPO as a ratio against an external clock.
inline int tempoRatio(float knob) {
    int z = (int)(clamp01(knob) * 7.f);
    return kRatioTable[std::min(z, 6)];
}

// The STEPS knob's detent (0..6) as a loop length, 0 = OFF.
inline int stepsLength(int detent, bool alt) {
    detent = std::min(std::max(detent, 0), kLengths - 1);
    return alt ? kAltLengthTable[detent] : kLengthTable[detent];
}

// The STEPS detent whose loop length is nearest to n (0 = off).
inline int stepsDetent(float n, bool alt) {
    int best = 0;
    for (int d = 1; d < kLengths; d++)
        if (std::fabs(stepsLength(d, alt) - n) < std::fabs(stepsLength(best, alt) - n)) best = d;
    return best;
}

// The middle of the TEMPO zone for a typed ratio, "x2", "2x", "/4" or
// "1/4", or -1 if it is none of the seven.
inline float ratioZoneKnob(const std::string& text) {
    std::string t;
    for (char c : text)
        if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
    int r = 0;
    size_t slash = t.find('/');
    if (slash != std::string::npos) r = -std::atoi(t.c_str() + slash + 1);
    else if (!t.empty() && t[0] == 'x') r = std::atoi(t.c_str() + 1);
    else if (t.find('x') != std::string::npos) r = std::atoi(t.c_str());
    if (r == -1) r = 1;
    for (int i = 0; i < 7; i++)
        if (kRatioTable[i] == r) return (i + 0.5f) / 7.f;
    return -1.f;
}

// A knob pushed towards 1 by a modulation m (0..1): the Mod assign law,
// "the knob sets the minimum while modulation sets the maximum".
inline float pushUp(float knob, float m) { return clamp01(knob + clamp01(m) * (1.f - knob)); }

// ---------------------------------------------------------------- controls

struct Controls {
    // voice, knobs 0..1
    float pitch = 0.2f;
    int wave = SAW;
    float pitchDecay = 0.25f;
    float pitchAmount = 0.f;
    float noise = 0.f;
    float cutoff = 0.6f;
    float resonance = 0.f;
    bool highpass = false;
    float cutoffDecay = 0.25f;
    float cutoffAmount = 0.f;
    float volume = 0.7f;
    float volumeDecay = 0.4f;
    float effect = 0.f;           // the RUST / CORROSION knob
    bool rust = false;            // the switch up: RUST, or the intense preset
    float mix = 1.f;              // the dry/wet
    // sequencer
    bool play = true;
    float tempo = 0.2f;              // 2 Hz
    float skips = 0.f;
    float stepMod = 0.f;
    int dest = DEST_PITCH;
    int steps = 0;                // detent 0..6
    // inputs, volts
    float pitchCv = 0.f;
    float noiseCv = 0.f;
    float cutoffCv = 0.f;
    float skipsCv = 0.f;
    float stepModCv = 0.f;
    float ext = 0.f;
    bool extConnected = false;
    bool clockConnected = false;
    // context menu
    int fx = FX_DISTORTION;
    int assign = ASSIGN_CUTOFF;
    bool altLengths = false;
    bool restartOnPlay = false;
    bool noAutoStart = false;
};

// Edges the module has already detected, one sample each.
struct Events {
    bool trigger = false;         // button or TRIGGER in
    bool clock = false;           // CLOCK in, rising
    bool reset = false;           // RESET in, rising
};

// ---------------------------------------------------------------- parts

// xorshift32: the noise source and both random generators.
struct Rng {
    uint32_t s = 0x9e3779b9u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uniform() { return (next() >> 8) * (1.f / 16777216.f); }    // [0, 1)
    float bipolar() { return uniform() * 2.f - 1.f; }
};

// ---------------------------------------------------------------- sequencer

struct Slot {
    float skip = 0.f, mod = 0.f;
    bool valid = false;
};

struct Sequencer {
    Slot slot[kSlots];
    Slot history[kSlots];         // the last steps heard, a ring
    int historyW = 0;
    int historyN = 0;
    int length = 0;               // 0 = OFF
    int pos = 0;
    float held = 0.f;             // the last fired step's random value, 0..1

    // A new length from the STEPS knob. Going up from OFF locks the last
    // `n` steps heard, oldest first; going up from a loop leaves the new
    // slots empty, to be drawn when reached; going down discards the tail.
    void setLength(int n) {
        if (n == length) return;
        if (n == 0) {
            for (int i = 0; i < kSlots; i++) slot[i].valid = false;
        } else if (length == 0) {
            for (int i = 0; i < kSlots; i++) slot[i].valid = false;
            int have = std::min(n, historyN);
            for (int i = 0; i < have; i++)
                slot[i] = history[(historyW - have + i + kSlots) % kSlots];
            pos = 0;
        } else if (n < length) {
            for (int i = n; i < kSlots; i++) slot[i].valid = false;
        }
        length = n;
        if (length > 0 && pos >= length) pos %= length;
    }

    void restart() { pos = 0; }

    // One step. Returns whether it fires; `held` is updated only if it does.
    bool step(float skips, Rng& rng) {
        Slot s;
        if (length == 0) {
            s.skip = rng.uniform();
            s.mod = rng.uniform();
            s.valid = true;
        } else {
            Slot& t = slot[pos];
            if (!t.valid) {
                t.skip = rng.uniform();
                t.mod = rng.uniform();
                t.valid = true;
            }
            s = t;
            pos = (pos + 1) % length;
        }
        history[historyW] = s;
        historyW = (historyW + 1) % kSlots;
        historyN = std::min(historyN + 1, kSlots);
        bool fire = s.skip >= skips;
        if (fire) held = s.mod;
        return fire;
    }
};

// The clock: an internal oscillator, or an external clock divided or
// multiplied. Returns the number of steps due this sample (0 or 1).
struct Clock {
    double phase = 0.;            // internal, cycles
    bool external = false;
    bool internalHeld = false;    // Options: no auto-start after an external clock
    long now = 0;                 // samples
    long lastEdge = -1;
    long period = 0;              // samples between the last two edges
    int edgeCount = 0;            // for division
    int subsLeft = 0;             // multiplication: steps still to place
    long subPeriod = 0;
    long nextSub = 0;
    bool wasPlaying = false;
    bool primed = false;

    bool process(const Controls& c, bool edge, float sampleRate, bool& started) {
        now++;
        started = false;
        bool due = false;
        // The first sample after a load or an add is not a switch to RUN:
        // it plays, but does not restart the sequence, and with a clock
        // patched it waits for the clock rather than play off the grid.
        if (!primed) {
            primed = true;
            wasPlaying = c.play;
            if (c.play && !c.clockConnected) due = true;
        }
        if (c.play && !wasPlaying) {
            started = true;
            internalHeld = false;
            phase = 0.;
            if (!external) due = true;
        }
        wasPlaying = c.play;

        if (!c.clockConnected && external) {
            external = false;
            internalHeld = c.noAutoStart;
            phase = 0.;
        }
        if (edge && c.play) {
            if (lastEdge >= 0 && external) period = now - lastEdge;
            else period = 0;
            external = true;
            lastEdge = now;
            int r = tempoRatio(c.tempo);
            subsLeft = 0;
            if (r < 0) {
                if (edgeCount % (-r) == 0) due = true;
                edgeCount++;
            } else {
                due = true;
                edgeCount = 0;
                if (r > 1 && period >= r) {
                    subsLeft = r - 1;
                    subPeriod = period / r;
                    nextSub = now + subPeriod;
                }
            }
        }
        if (external) {
            if (subsLeft > 0 && now >= nextSub && c.play) {
                due = true;
                subsLeft--;
                nextSub += subPeriod;
            }
            float timeout = std::max(kExtTimeoutMin * sampleRate, kExtTimeoutPeriods * (float)period);
            if (now - lastEdge > (long)timeout) {
                external = false;
                internalHeld = c.noAutoStart;
                phase = 0.;
                lastEdge = -1;
            }
            return due;
        }
        if (!c.play || internalHeld) return due;
        phase += tempoHz(c.tempo) / sampleRate;
        if (phase >= 1.) {
            phase -= std::floor(phase);
            due = true;
        }
        return due;
    }
};

// ---------------------------------------------------------------- engine

struct Output {
    float audio = 0.f;            // volts
    float stepMod = 0.f;          // volts, 0..10
    bool trigger = false;         // the voice fired this sample
    bool clock = false;           // the sequencer stepped this sample
};

struct Engine {
    Rng rng, noiseRng;
    Sequencer seq;
    Clock clock;
    daisy::AdEnv pitchEnv, cutoffEnv, volumeEnv;
    daisy::Oscillator osc, osc2;
    daisy::Svf filter;
    daisy::Overdrive overdrive;
    daisy::Decimator decimator;
    daisy::Phaser phaser;
    daisy::Flanger flanger;
    daisy::Chorus chorus;
    float dcX = 0.f, dcY = 0.f;
    bool resetPending = false;
    int lastLength = -1;

    Engine() {
        pitchEnv.curve = kPitchCurve;
        cutoffEnv.curve = kCutoffCurve;
        volumeEnv.curve = kVolumeCurve;
        filter.setDrive(kFilterDrive);
    }

    void seed(uint32_t s) {
        rng.seed(s);
        noiseRng.seed(s * 2654435761u + 1u);
    }

    // The current step value after STEP MOD and its CV, 0..1.
    float stepValue(const Controls& c) const {
        return seq.held * amountLaw(c.stepMod + c.stepModCv * 0.1f);
    }

    // STEP MOD's own knob and CV, 0..1, before any law.
    float stepModKnob(const Controls& c) const {
        return clamp01(c.stepMod + c.stepModCv * 0.1f);
    }

    Output process(const Controls& c, const Events& e, float sampleRate) {
        Output out;

        // ---- sequencer
        int len = stepsLength(c.steps, c.altLengths);
        if (len != lastLength) {
            seq.setLength(len);
            lastLength = len;
        }
        if (e.reset) resetPending = true;
        bool started = false;
        bool due = clock.process(c, e.clock, sampleRate, started);
        if (started && c.restartOnPlay) seq.restart();
        bool fire = e.trigger;
        if (due) {
            if (resetPending) {
                seq.restart();
                resetPending = false;
            }
            out.clock = true;
            // SKIPS is linear, a percentage as the manual says: the
            // hardware's time-bomb at 0.66 fires one step in three.
            float skips = clamp01(c.skips + c.skipsCv * 0.1f);
            if (seq.step(skips, rng)) fire = true;
        }
        if (fire) {
            out.trigger = true;
            // A hit out of silence starts the oscillators from the top of
            // their cycle, so every kick has the same attack. One that
            // lands on a sounding note leaves them running: a jump there
            // would click.
            if (volumeEnv.output < 0.05f) {
                osc.reset();
                osc2.reset();
            }
            pitchEnv.trigger();
            cutoffEnv.trigger();
            volumeEnv.trigger();
        }

        // ---- modulation
        float m = stepValue(c);
        out.stepMod = 10.f * m;
        // On PITCH, STEP MOD sets the highest note on PITCH's own scale: the
        // step adds up to pitchHz(STEP MOD) - 30 Hz, so at full it spans
        // the manual's 1500 Hz, and at 0.42 (the book's acid) about 125 Hz.
        float mPitchHz = c.dest == DEST_PITCH
            ? seq.held * (pitchHz(stepModKnob(c)) - kPitchMin) * (stepModKnob(c) > 0.f ? 1.f : 0.f) : 0.f;
        float mNoise = c.dest == DEST_NOISE ? m : 0.f;
        float mAssign = c.dest == DEST_CUTOFF ? m : 0.f;
        // The CUTOFF input goes where the Mod assign menu sends it: +/-5 V
        // is the target's whole travel either way.
        float cv = c.cutoffCv * 0.2f;

        float volumeDecay = c.volumeDecay, pitchAmount = c.pitchAmount;
        float cutoffAmount = c.cutoffAmount, volume = c.volume, effect = c.effect;
        float cutMod = 0.f;
        switch (c.assign) {
            case ASSIGN_CUTOFF: cutMod = mAssign * kCutoffEnvHz; break;
            case ASSIGN_VOL_DECAY: volumeDecay = pushUp(volumeDecay, mAssign + cv); break;
            case ASSIGN_PITCH_AMOUNT: pitchAmount = pushUp(pitchAmount, mAssign + cv); break;
            case ASSIGN_CUTOFF_AMOUNT: cutoffAmount = pushUp(cutoffAmount, mAssign + cv); break;
            // The knob is the maximum and the modulation pulls it down.
            case ASSIGN_VOLUME: volume = volume * (1.f - clamp01(mAssign + cv)); break;
            case ASSIGN_EFFECT: effect = pushUp(effect, mAssign + cv); break;
        }

        // ---- envelopes
        float ep = pitchEnv.process(kAttack, decaySeconds(c.pitchDecay), sampleRate);
        float ec = cutoffEnv.process(kAttack, decaySeconds(c.cutoffDecay), sampleRate);
        float ev = volumeEnv.process(kAttack, decaySeconds(volumeDecay), sampleRate);

        // ---- sources
        bool saw = c.wave == SAW;
        float freq = pitchHz(c.pitch) * std::pow(2.f, std::min(std::max(c.pitchCv, -10.f), 10.f))
                   + ep * amountLaw(pitchAmount) * kPitchEnvMax + mPitchHz;
        freq = std::min(std::max(freq, 1.f), 0.45f * sampleRate);
        float x = osc.process(freq, saw, sampleRate);
        if (c.fx == FX_OSC2) {
            // CORROSION: -1 octave..unison, RUST: unison..+1 octave.
            float oct = c.rust ? effect : effect - 1.f;
            float f2 = std::min(freq * std::pow(2.f, oct), 0.45f * sampleRate);
            x = x + 0.5f * clamp01(c.mix) * (osc2.process(f2, saw, sampleRate) - x);
        }
        float n = clamp01(c.noise + mNoise + c.noiseCv * 0.2f);
        float src = c.extConnected ? c.ext * 0.2f : noiseRng.bipolar();
        x = x + n * (src - x);
        // A trace of noise under everything, as the hardware's own floor:
        // enough to start the filter ringing with the sources muted.
        x += 1e-5f * noiseRng.bipolar();

        // ---- filter: the knob and CV mapped, the envelope and the step value
        // added in hertz. They push the cutoff up in LP and pull it down in
        // HP: on the hardware's resonant kick the HP sweep starts low
        // at the hit and rises back to the knob. The CV is an offset either
        // way.
        float mod = ec * clamp01(cutoffAmount) * kCutoffEnvHz + cutMod;
        float base = cutoffHz(c.cutoff + (c.assign == ASSIGN_CUTOFF ? cv : 0.f));
        float fc = std::min(std::max(base + (c.highpass ? -mod : mod), kCutoffMin), kCutoffMax);
        filter.setSampleRate(sampleRate);
        filter.setFreq(fc);
        filter.setRes(clamp01(c.resonance));
        filter.process(x);
        x = c.highpass ? filter.outHigh : filter.outLow;

        // ---- effect
        float dry = x, wet = x;
        switch (c.fx) {
            case FX_DISTORTION:
                overdrive.setDrive(overdriveAmount(effect));
                wet = overdrive.process(x);
                // RUST: the Decimator after it, with no anti-aliasing.
                // The factor 0.5 k^3 puts the hold rate at 6.9 kHz at 0.8 and
                // 1.9 kHz at full, as the book's noise wall and 8-bit noise
                // measure on the hardware.
                if (c.rust) wet = decimator.process(wet, 0.5f * effect * effect * effect, sampleRate);
                break;
            case FX_OSC2:
                break;
            case FX_PHASER:
                wet = c.rust ? phaser.process(x, 8, lfoHz(effect), 0.9f, 0.7f, sampleRate)
                             : phaser.process(x, 4, lfoHz(effect), 0.5f, 0.2f, sampleRate);
                break;
            case FX_FLANGER:
                wet = c.rust ? flanger.process(x, 0.75f, 0.9f, 0.85f, lfoHz(effect), sampleRate)
                             : flanger.process(x, 0.3f, 0.5f, 0.3f, lfoHz(effect), sampleRate);
                break;
            case FX_CHORUS:
                wet = c.rust ? chorus.process(x, lfoHz(effect), lfoHz(effect) * 1.37f, 0.75f, 0.55f, 0.9f, 0.4f, sampleRate)
                             : chorus.process(x, lfoHz(effect), lfoHz(effect) * 1.37f, 0.6f, 0.5f, 0.5f, 0.1f, sampleRate);
                break;
        }
        if (c.fx != FX_OSC2) x = dry + clamp01(c.mix) * (wet - dry);

        // ---- volume: the VCA, then the asymmetric clipping VOLUME drives
        float y = x * ev * daisy::fmap(volume, 0.f, 2.5f);
        y = y >= 0.f ? daisy::softClip(y) : daisy::softClip(1.5f * y) * (1.f / 1.5f);

        // ---- DC block, 10 Hz: the clipper is asymmetric
        float r = 1.f - 2.f * kPi * 10.f / sampleRate;
        dcY = y - dcX + r * dcY;
        dcX = y;
        if (!std::isfinite(dcY)) dcY = dcX = 0.f;
        out.audio = kLevel * dcY;
        return out;
    }
};

}  // namespace rubigo
