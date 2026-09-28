// daisy.hpp - the DaisySP building blocks rubigo's voice is made of.
//
// The Metal Fetishist runs on an Electrosmith Daisy, and every block its
// manual names has a DaisySP counterpart that behaves as described: an SVF
// whose fixed drive shapes only the resonance (Svf), an overdrive that
// "increases the perceived volume" (Overdrive, whose post-gain does exactly
// that), a downsampler reaching a few hundred hertz (Decimator), decay-only
// envelopes that retrigger from where they are (AdEnv), polyBLEP
// oscillators, and a phaser, flanger and chorus. The firmware is closed, so
// that it uses these is inference; that it sounds like it does is the test.
//
// Ported from DaisySP (https://github.com/electro-smith/DaisySP), trimmed to
// what rubigo uses and made sample-rate aware where the original assumes
// 48 kHz. The original's notice:
//
//   DaisySP, copyright (c) 2020 Electrosmith, Corp.
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
//   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rubigo {
namespace daisy {

const float kPi = 3.14159265358979f;

inline float fclamp(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

// fmap: how Daisy firmware turns a 0..1 knob into a range. EXP is
// quadratic, LOG is truly exponential.
enum Mapping { LINEAR, EXP, LOG };
inline float fmap(float in, float min, float max, Mapping curve = LINEAR) {
    in = fclamp(in, 0.f, 1.f);
    switch (curve) {
        case EXP: return fclamp(min + in * in * (max - min), min, max);
        case LOG: return fclamp(min * std::pow(max / min, in), min, max);
        default: return fclamp(min + in * (max - min), min, max);
    }
}

inline float softLimit(float x) { return x * (27.f + x * x) / (27.f + 9.f * x * x); }
inline float softClip(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return softLimit(x);
}

// -------------------------------------------------------------- AdEnv

// Attack-decay envelope. A trigger restarts the attack from the current
// value; each segment follows 1 - exp(x) with x running from 0 to `curve`,
// so a negative curve falls fast and then slowly, and reaches its end
// exactly at the segment's time.
struct AdEnv {
    enum { IDLE, ATTACK, DECAY };
    int segment = IDLE;
    float output = 0.f;
    float retrig = 0.f;
    float curveX = 0.f;
    float curve = -8.f;

    void trigger() {
        segment = ATTACK;
        curveX = 0.f;
        retrig = output;
    }
    float process(float attack, float decay, float sampleRate) {
        if (segment == IDLE) return output = 0.f;
        float time = segment == ATTACK ? attack : decay;
        float samples = std::max(time * sampleRate, 1.f);
        float beg = segment == ATTACK ? retrig : 1.f;
        float end = segment == ATTACK ? 1.f : 0.f;
        float out = output;
        float inc = (end - beg) / (1.f - std::exp(curve));
        curveX += curve / samples;
        float val = beg + inc * (1.f - std::exp(curveX));
        if (!std::isfinite(val)) val = 0.f;
        if (curveX <= curve) {
            curveX = 0.f;
            if (segment == ATTACK) {
                segment = DECAY;
                val = 1.f;
            } else {
                segment = IDLE;
                val = 0.f;
            }
        }
        output = val;
        return out;
    }
};

// -------------------------------------------------------------- Oscillator

inline float polyBlep(float dt, float t) {
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

// WAVE_POLYBLEP_SAW and WAVE_POLYBLEP_SQUARE, with the original's 0.707 on
// the square and its falling saw.
struct Oscillator {
    float phase = 0.f;
    void reset() { phase = 0.f; }
    float process(float freq, bool saw, float sampleRate) {
        float inc = fclamp(freq / sampleRate, 0.f, 0.45f);
        float out;
        if (saw) {
            out = 2.f * phase - 1.f;
            out -= polyBlep(inc, phase);
            out *= -1.f;
        } else {
            out = phase < 0.5f ? 1.f : -1.f;
            out += polyBlep(inc, phase);
            float t2 = phase + 0.5f;
            if (t2 >= 1.f) t2 -= 1.f;
            out -= polyBlep(inc, t2);
            out *= 0.707f;
        }
        phase += inc;
        if (phase >= 1.f) phase -= 1.f;
        return out;
    }
};

// -------------------------------------------------------------- Svf

// Chamberlin state-variable filter run twice per sample, with the drive as
// a cubic term on the band state, scaled by the resonance: a clean filter
// at low resonance, a saturating and self-oscillating one at the top.
struct Svf {
    float sr = 48000.f;
    float fc = 200.f, res = 0.f, preDrive = 0.5f, drive = 0.f;
    float freq = 0.25f, damp = 2.f;
    float low = 0.f, high = 0.f, band = 0.f, notch = 0.f;
    float outLow = 0.f, outHigh = 0.f;

    void setSampleRate(float s) { sr = s; }
    void setFreq(float f) {
        fc = fclamp(f, 1.0e-6f, sr / 3.f);
        freq = 2.f * std::sin(kPi * std::min(0.25f, fc / (sr * 2.f)));
        updateDamp();
    }
    void setRes(float r) {
        res = fclamp(r, 0.f, 1.f);
        updateDamp();
        drive = preDrive * res;
    }
    void setDrive(float d) {
        preDrive = fclamp(d * 0.1f, 0.f, 1.f);
        drive = preDrive * res;
    }
    // DaisySP's damping reaches exactly zero at full resonance: a lossless
    // loop that rings only if something rings it. The hardware's does ring
    // with its input dead, off its own converter noise, so over the last 5%
    // of the knob the damping goes slightly negative (rubigo's addition) and
    // the cubic drive term sets the level it settles at.
    void updateDamp() {
        float nudge = res > 0.95f ? (res - 0.95f) / 0.05f : 0.f;
        damp = std::min(2.f * (1.f - std::pow(res, 0.25f)), std::min(2.f, 2.f / freq - freq * 0.5f))
             - kSelfOsc * nudge * nudge;
    }
    static constexpr float kSelfOsc = 0.2f;
    void pass(float in) {
        notch = in - damp * band;
        low = low + freq * band;
        high = notch - low;
        band = freq * high + band - drive * band * band * band;
    }
    void process(float in) {
        pass(in);
        outLow = 0.5f * low;
        outHigh = 0.5f * high;
        pass(in);
        outLow += 0.5f * low;
        outHigh += 0.5f * high;
        if (!std::isfinite(band) || !std::isfinite(low)) low = high = band = notch = outLow = outHigh = 0.f;
    }
};

// -------------------------------------------------------------- Overdrive

struct Overdrive {
    float preGain = 1.f, postGain = 1.f;
    void setDrive(float d) {
        d = fclamp(d, 0.f, 1.f);
        float drive = 2.f * d;
        float drive2 = drive * drive;
        float a = drive * 0.5f;
        float b = drive2 * drive2 * drive * 24.f;
        preGain = a + (b - a) * drive2;
        float squashed = drive * (2.f - drive);
        postGain = 1.f / softClip(0.33f + squashed * (preGain - 0.33f));
    }
    float process(float in) const { return softClip(preGain * in) * postGain; }
};

// -------------------------------------------------------------- Decimator

// The downsampling half of Decimator: hold each sample for factor^2 * 96
// samples at 48 kHz (scaled here to the engine's rate), so the rate falls
// to about 495 Hz at full.
struct Decimator {
    float held = 0.f;
    float count = 0.f;
    float process(float in, float factor, float sampleRate) {
        float threshold = factor * factor * 96.f * sampleRate / 48000.f;
        count += 1.f;
        if (count > threshold) {
            count = 0.f;
            held = in;
        }
        return held;
    }
};

// -------------------------------------------------------------- delay line

const int kDelaySize = 8192;              // > 20 ms at 384 kHz, power of two

struct DelayLine {
    float line[kDelaySize] = {};
    int w = 0;
    void write(float x) {
        line[w] = x;
        w = (w - 1 + kDelaySize) & (kDelaySize - 1);
    }
    float read(float delay) const {
        delay = fclamp(delay, 0.f, (float)(kDelaySize - 2));
        int i = (int)delay;
        float f = delay - i;
        float a = line[(w + i) & (kDelaySize - 1)];
        float b = line[(w + i + 1) & (kDelaySize - 1)];
        return a + (b - a) * f;
    }
    float allpass(float x, int delay, float coef) {
        float r = line[(w + delay) & (kDelaySize - 1)];
        float wr = x + coef * r;
        write(wr);
        return -wr * coef + r;
    }
    void clear() {
        std::fill(line, line + kDelaySize, 0.f);
        w = 0;
    }
};

// The triangle LFO the three effects share, -1..1, freq in Hz.
struct Lfo {
    float phase = 0.f, inc = 0.f;
    void setFreq(float hz, float sampleRate) {
        float f = fclamp(4.f * hz / sampleRate, 0.f, 0.25f);
        inc = inc < 0.f ? -f : f;
    }
    float process() {
        phase += inc;
        if (phase > 1.f) {
            phase = 1.f - (phase - 1.f);
            inc = -inc;
        } else if (phase < -1.f) {
            phase = -1.f - (phase + 1.f);
            inc = -inc;
        }
        return phase;
    }
};

// -------------------------------------------------------------- Phaser

struct PhaserEngine {
    DelayLine del;
    Lfo lfo;
    float deltime = 0.f, last = 0.f;
    float process(float in, float apFreq, float depth, float feedback, float sampleRate) {
        float l = lfo.process() * depth * apFreq;
        float target = sampleRate / (l + apFreq + 30.f);
        deltime += 0.0001f * (target - deltime);
        last = del.allpass(in + feedback * last, (int)deltime, 0.3f);
        return (in + last) * 0.5f;
    }
};

struct Phaser {
    static const int kMaxPoles = 8;
    PhaserEngine engines[kMaxPoles];
    // DaisySP sums its poles without scaling; divided here so the level
    // does not grow with the pole count.
    float process(float in, int poles, float lfoHz, float depth, float feedback, float sampleRate) {
        float sig = 0.f;
        for (int i = 0; i < poles; i++) {
            engines[i].lfo.setFreq(lfoHz, sampleRate);
            sig += engines[i].process(in, 200.f, depth, feedback, sampleRate);
        }
        return sig / poles;
    }
};

// -------------------------------------------------------------- Flanger

struct Flanger {
    DelayLine del;
    Lfo lfo;
    // delay 0..1 is 0.1 to 7 ms, depth up to 0.93 of it.
    float process(float in, float delay, float depth, float feedback, float lfoHz, float sampleRate) {
        lfo.setFreq(lfoHz, sampleRate);
        float d = (0.1f + delay * 6.9f) * 0.001f * sampleRate;
        float amp = fclamp(depth, 0.f, 0.93f) * d;
        float out = del.read(1.f + lfo.process() * amp + d);
        del.write(in + out * fclamp(feedback, 0.f, 1.f) * 0.97f);
        return (in + out) * 0.5f;
    }
};

// -------------------------------------------------------------- Chorus

struct ChorusEngine {
    DelayLine del;
    Lfo lfo;
    // delay 0..1 is 0.1 to 8 ms.
    float process(float in, float delay, float depth, float feedback, float lfoHz, float sampleRate) {
        lfo.setFreq(lfoHz, sampleRate);
        float d = (0.1f + delay * 7.9f) * 0.001f * sampleRate;
        float amp = fclamp(depth, 0.f, 0.93f) * d;
        float out = del.read(lfo.process() * amp + d);
        del.write(in + out * feedback);
        return (in + out) * 0.5f;
    }
};

// Two engines panned 0.25 / 0.75 and read on the left, as Chorus::Process
// returns it: 0.75 of the first and 0.25 of the second.
struct Chorus {
    ChorusEngine e[2];
    float process(float in, float lfoA, float lfoB, float delayA, float delayB, float depth,
                  float feedback, float sampleRate) {
        float a = e[0].process(in, delayA, depth, feedback, lfoA, sampleRate);
        float b = e[1].process(in, delayB, depth, feedback, lfoB, sampleRate);
        // DaisySP halves this too; kept at level here, so MIX does not
        // drop the volume when it brings the chorus in.
        return 0.75f * a + 0.25f * b;
    }
};

}  // namespace daisy
}  // namespace rubigo
