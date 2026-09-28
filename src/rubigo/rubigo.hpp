// rubigo.hpp - the engine of rubigo: a digital percussion voice played by
// its own generative sequencer.
//
// A clone of Body Synths' Metal Fetishist (firmware v2.0), written from the
// hardware's reference manual and doc/design/rubigo.md, which marks what the
// manual leaves open and what was decided in its place.
//
//   osc (saw/square) --.
//                      +-- NOISE crossfade -- drive -- LP/HP SVF -- effect -- VCA + clip -- DC block -- out
//   noise / ext in ----'                                  (distortion, rust, 2nd osc, phaser, flanger, chorus)
//
//   three decay-only envelopes, fired together: pitch (+Hz), cutoff (+oct), volume
//
//   clock -> step -> [ skip >= SKIPS ? fire : hold ] -> step value -> PITCH / NOISE / CUTOFF (or the assigned target)
//
// Every modulation adds to its knob, as on the hardware: PITCH is the lowest
// pitch, CUTOFF the lowest cutoff, and the envelopes and the step value push
// up from there. Cutoff modulation always raises the cutoff, so it opens the
// filter in LP and closes it in HP, which is what the manual says the
// envelope does.
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
#include <cstdint>

namespace rubigo {

// ---------------------------------------------------------------- constants

const float kPi = 3.14159265358979f;

const float kPitchMin = 30.f;             // Hz, PITCH knob
const float kPitchMax = 1500.f;
const float kPitchEnvMax = 600.f;         // Hz, pitch DECAY AMOUNT at full
const float kStepPitchMax = 1500.f;       // Hz, step value 1 on PITCH
const float kDecayMin = 0.001f;           // s, the three DECAY knobs
const float kDecayMax = 3.f;
const float kAttack = 0.001f;             // s, every envelope
const float kCutoffMin = 20.f;            // Hz, CUTOFF knob
const float kCutoffMax = 20000.f;
const float kCutoffEnvOct = 8.f;          // octaves, cutoff AMOUNT at full
const float kStepCutoffOct = 8.f;         // octaves, step value 1 on CUTOFF
const float kCutoffCvOct = 2.f;           // octaves per volt: +/-5 V = full travel
const float kTempoMin = 0.4f;             // Hz
const float kTempoMax = 80.f;
const float kRustMinRate = 400.f;         // Hz, RUST knob at full
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

inline float expLaw(float knob, float lo, float hi) {
    return lo * std::pow(hi / lo, clamp01(knob));
}

inline float pitchHz(float knob) { return expLaw(knob, kPitchMin, kPitchMax); }
inline float decaySeconds(float knob) { return expLaw(knob, kDecayMin, kDecayMax); }
inline float cutoffHz(float knob) { return expLaw(knob, kCutoffMin, kCutoffMax); }
inline float tempoHz(float knob) { return expLaw(knob, kTempoMin, kTempoMax); }
inline float lfoHz(float knob) { return expLaw(knob, kLfoMin, kLfoMax); }

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
    float tempo = 0.3f;
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

// Decay-only envelope: a 1 ms linear rise from wherever it is, then an
// exponential fall that reaches -60 dB at the DECAY time. A retrigger starts
// the rise from the current value, so fast steps never drop it to zero.
struct Envelope {
    float v = 0.f;
    bool rising = false;
    void trigger() { rising = true; }
    float process(float decay, float sampleTime) {
        if (rising) {
            v += sampleTime / kAttack;
            if (v >= 1.f) {
                v = 1.f;
                rising = false;
            }
        } else {
            v *= std::exp(-6.9077553f * sampleTime / decay);    // ln 1000
            if (v < 1e-7f) v = 0.f;
        }
        return v;
    }
};

inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

struct Oscillator {
    float phase = 0.f;
    float process(float freq, int wave, float sampleTime) {
        float dt = std::min(freq * sampleTime, 0.45f);
        phase += dt;
        if (phase >= 1.f) phase -= std::floor(phase);
        float y;
        if (wave == SAW) {
            y = 2.f * phase - 1.f - polyBlep(phase, dt);
        } else {
            y = phase < 0.5f ? 1.f : -1.f;
            y += polyBlep(phase, dt);
            float t2 = phase + 0.5f;
            if (t2 >= 1.f) t2 -= 1.f;
            y -= polyBlep(t2, dt);
        }
        return y;
    }
};

// How far the filter's states may swing: the level of self-oscillation.
const float kSvfLimit = 4.f;

// Zero-delay-feedback state-variable filter (Simper's form) with the band
// state soft-limited, so full resonance rings on its own and stays bounded.
struct Svf {
    float ic1 = 0.f, ic2 = 0.f;
    float process(float x, float freq, float res, bool highpass, float sampleTime) {
        float g = std::tan(kPi * std::min(freq * sampleTime, 0.49f));
        // k = 2 is no resonance; slightly below zero at the top of the knob,
        // so it oscillates and the limit below sets how loud.
        float k = 2.f - 2.03f * res;
        float a1 = 1.f / (1.f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        ic1 = kSvfLimit * std::tanh(ic1 * (1.f / kSvfLimit));
        ic2 = kSvfLimit * std::tanh(ic2 * (1.f / kSvfLimit));
        if (!std::isfinite(ic1) || !std::isfinite(ic2)) ic1 = ic2 = 0.f;
        return highpass ? x - k * v1 - v2 : v2;
    }
};

// The digital overdrive both CORROSION and RUST start with. A bias before
// the tanh, taken off again after, makes it asymmetric: even harmonics, the
// crunch. Part of the gain is given back so it sounds louder, not deafening.
inline float corrode(float x, float amount) {
    float gain = 1.f + 39.f * amount * amount;
    const float bias = 0.3f;
    float y = std::tanh(gain * x + bias) - std::tanh(bias);
    return y / (1.f + 0.25f * std::log(gain));
}

// The VOLUME stage's clipper: harder on the negative side.
inline float volumeClip(float x) {
    return x >= 0.f ? std::tanh(x) : std::tanh(1.6f * x) * (1.f / 1.6f) * 1.2f;
}

const int kDelaySize = 16384;             // > 25 ms at 384 kHz, power of two

struct DelayLine {
    float buf[kDelaySize] = {};
    int w = 0;
    void push(float x) {
        buf[w] = x;
        w = (w + 1) & (kDelaySize - 1);
    }
    // `samples` back from the newest sample, linearly interpolated.
    float read(float samples) const {
        samples = std::min(std::max(samples, 1.f), (float)(kDelaySize - 2));
        int i = (int)samples;
        float f = samples - i;
        float a = buf[(w - 1 - i) & (kDelaySize - 1)];
        float b = buf[(w - 2 - i) & (kDelaySize - 1)];
        return a + (b - a) * f;
    }
};

// Phaser, flanger and chorus, each with a subtle and an intense preset. The
// knob sets the LFO. Each returns its wet signal: the dry plus the effected
// copy, which is what makes a phaser's notches and a flanger's comb.
struct Modulators {
    float lfo = 0.f;
    float ap[6] = {};             // first-order allpass states
    float phaserFb = 0.f;
    float flangerFb = 0.f;
    DelayLine line;

    void clear() {
        std::fill(ap, ap + 6, 0.f);
        phaserFb = flangerFb = 0.f;
        std::fill(line.buf, line.buf + kDelaySize, 0.f);
    }

    float tri(float p) const {
        p -= std::floor(p);
        return p < 0.5f ? 4.f * p - 1.f : 3.f - 4.f * p;
    }

    float process(float x, int fx, bool intense, float knob, float sampleTime) {
        lfo += lfoHz(knob) * sampleTime;
        if (lfo >= 1.f) lfo -= 1.f;
        float l = tri(lfo);
        float sr = 1.f / sampleTime;
        if (fx == FX_PHASER) {
            // Six stages swept across 200 Hz..2 kHz (subtle) or 100 Hz..6 kHz.
            float lo = intense ? 100.f : 200.f, hi = intense ? 6000.f : 2000.f;
            float f = lo * std::pow(hi / lo, 0.5f + 0.5f * l);
            float t = std::tan(kPi * std::min(f * sampleTime, 0.49f));
            float c = (t - 1.f) / (t + 1.f);
            float y = x + phaserFb * (intense ? 0.7f : 0.3f);
            for (int i = 0; i < 6; i++) {
                float o = c * y + ap[i];
                ap[i] = y - c * o;
                y = o;
            }
            phaserFb = std::tanh(y);
            return 0.5f * (x + y);
        }
        if (fx == FX_FLANGER) {
            float lo = intense ? 0.2f : 0.5f, hi = intense ? 5.f : 2.f;    // ms
            float d = (lo + (hi - lo) * (0.5f + 0.5f * l)) * 0.001f * sr;
            float fb = intense ? 0.85f : 0.4f;
            line.push(x + fb * flangerFb);
            float y = line.read(d);
            flangerFb = std::tanh(y);
            return 0.5f * (x + y);
        }
        // chorus: two taps on opposite LFO phases, no feedback
        line.push(x);
        float depth = intense ? 6.f : 2.f;                                // ms
        float d1 = (15.f + depth * l) * 0.001f * sr;
        float d2 = (15.f - depth * l) * 0.001f * sr;
        float wet = 0.5f * (line.read(d1) + line.read(d2));
        return intense ? 0.4f * x + 0.8f * wet : 0.6f * x + 0.5f * wet;
    }
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

    bool process(const Controls& c, bool edge, float sampleRate, bool& started) {
        now++;
        started = false;
        bool due = false;
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
    Envelope pitchEnv, cutoffEnv, volumeEnv;
    Oscillator osc, osc2;
    Svf filter;
    Modulators mods;
    float rustPhase = 0.f, rustHeld = 0.f;
    float dcX = 0.f, dcY = 0.f;
    bool resetPending = false;
    int lastLength = -1;

    void seed(uint32_t s) {
        rng.seed(s);
        noiseRng.seed(s * 2654435761u + 1u);
    }

    // The current step value after STEP MOD and its CV, 0..1.
    float stepValue(const Controls& c) const {
        return seq.held * clamp01(c.stepMod + c.stepModCv * 0.1f);
    }

    Output process(const Controls& c, const Events& e, float sampleRate) {
        float st = 1.f / sampleRate;
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
            float skips = clamp01(c.skips + c.skipsCv * 0.1f);
            if (seq.step(skips, rng)) fire = true;
        }
        if (fire) {
            out.trigger = true;
            pitchEnv.trigger();
            cutoffEnv.trigger();
            volumeEnv.trigger();
        }

        // ---- modulation
        float m = stepValue(c);
        out.stepMod = 10.f * m;
        float mPitch = c.dest == DEST_PITCH ? m : 0.f;
        float mNoise = c.dest == DEST_NOISE ? m : 0.f;
        float mAssign = c.dest == DEST_CUTOFF ? m : 0.f;
        // The CUTOFF input goes where the Mod assign menu sends it, as 0..1
        // over +/-5 V for the other targets; for the cutoff, in octaves.
        float cvAssign = c.assign == ASSIGN_CUTOFF ? 0.f : c.cutoffCv * 0.2f;

        float volumeDecay = c.volumeDecay, pitchAmount = c.pitchAmount;
        float cutoffAmount = c.cutoffAmount, volume = c.volume, effect = c.effect;
        float cutOct = 0.f;
        switch (c.assign) {
            case ASSIGN_CUTOFF:
                cutOct = mAssign * kStepCutoffOct + c.cutoffCv * kCutoffCvOct;
                break;
            case ASSIGN_VOL_DECAY: volumeDecay = pushUp(volumeDecay, mAssign + cvAssign); break;
            case ASSIGN_PITCH_AMOUNT: pitchAmount = pushUp(pitchAmount, mAssign + cvAssign); break;
            case ASSIGN_CUTOFF_AMOUNT: cutoffAmount = pushUp(cutoffAmount, mAssign + cvAssign); break;
            // The knob is the maximum and the modulation pulls it down.
            case ASSIGN_VOLUME: volume = volume * (1.f - clamp01(mAssign + cvAssign)); break;
            case ASSIGN_EFFECT: effect = pushUp(effect, mAssign + cvAssign); break;
        }

        // ---- envelopes
        float ep = pitchEnv.process(decaySeconds(c.pitchDecay), st);
        float ec = cutoffEnv.process(decaySeconds(c.cutoffDecay), st);
        float ev = volumeEnv.process(decaySeconds(volumeDecay), st);

        // ---- sources
        float freq = pitchHz(c.pitch) * std::pow(2.f, std::min(std::max(c.pitchCv, -10.f), 10.f))
                   + ep * pitchAmount * kPitchEnvMax + mPitch * kStepPitchMax;
        freq = std::min(std::max(freq, 1.f), 0.45f * sampleRate);
        float x = osc.process(freq, c.wave, st);
        if (c.fx == FX_OSC2) {
            // CORROSION: -1 octave..unison, RUST: unison..+1 octave.
            float oct = c.rust ? effect : effect - 1.f;
            float f2 = std::min(freq * std::pow(2.f, oct), 0.45f * sampleRate);
            x = x + 0.5f * clamp01(c.mix) * (osc2.process(f2, c.wave, st) - x);
        }
        float n = clamp01(c.noise + mNoise + c.noiseCv * 0.2f);
        float src = c.extConnected ? c.ext * 0.2f : noiseRng.bipolar();
        x = x + n * (src - x);
        // A trace of noise under everything, as the hardware's own floor:
        // enough to start the filter ringing with the sources muted.
        x += 1e-5f * noiseRng.bipolar();

        // ---- filter, with its fixed drive in front
        float fc = cutoffHz(c.cutoff) * std::pow(2.f, ec * cutoffAmount * kCutoffEnvOct + cutOct);
        fc = std::min(std::max(fc, kCutoffMin), 0.45f * sampleRate);
        x = std::tanh(1.5f * x);
        x = filter.process(x, fc, clamp01(c.resonance), c.highpass, st);

        // ---- effect
        float dry = x, wet = x;
        switch (c.fx) {
            case FX_DISTORTION: {
                wet = corrode(x, effect);
                if (c.rust) {
                    // Sample-and-hold from the engine rate down to 400 Hz,
                    // with no anti-aliasing: the aliasing is the point.
                    float rate = sampleRate * std::pow(kRustMinRate / sampleRate, effect);
                    rustPhase += rate * st;
                    if (rustPhase >= 1.f) {
                        rustPhase -= std::floor(rustPhase);
                        rustHeld = wet;
                    }
                    wet = rustHeld;
                }
                x = dry + clamp01(c.mix) * (wet - dry);
                break;
            }
            case FX_OSC2:
                break;
            default:
                wet = mods.process(x, c.fx, c.rust, effect, st);
                x = dry + clamp01(c.mix) * (wet - dry);
                break;
        }

        // ---- volume: the VCA, then the clipping VOLUME drives into
        float drive = 0.3f + 2.7f * clamp01(volume);
        float y = volumeClip(x * ev * drive) * clamp01(volume * 1.5f);

        // ---- DC block, 10 Hz: the clippers are asymmetric
        float r = 1.f - 2.f * kPi * 10.f * st;
        dcY = y - dcX + r * dcY;
        dcX = y;
        if (!std::isfinite(dcY)) dcY = dcX = 0.f;
        out.audio = kLevel * dcY;
        return out;
    }
};

}  // namespace rubigo
