// antrum_dsp.hpp - feedback delay network reverb in the Erbe-Verb idiom.
//
// The topology follows Tom Erbe's ICMC 2015 paper "Building the Erbe-Verb:
// Extending the Feedback Delay Network Reverb for Modular Synthesizer Use",
// and the block layout of davemollen's dm-Reverb (GPL-3.0), the cleanest
// public implementation of it:
//
//   in ─> predelay (forward / reversed) ───────────────┐
//                                                      │
//        ┌── 4 delay lines ──┐                         │
//        │                   v                         │
//        │   read (plain | sine vibrato | grain cloud) │
//        │                   v                         │
//        │   saturation (3rd-degree Chebyshev,         │
//        │     driven by the network's own RMS)        │
//        │                   v                         │
//        │   x decay  (the gain of the loop alone)     │
//        │                   v                         v
//        │   Hadamard matrix <───────────────────── (input)
//        │                   v
//        │   DC block ─> one-pole absorb (damping)
//        │                   v
//        └── allpass diffuser
//
//        network out + early reflection taps
//                     v
//              tilt filter ─> dry/wet mix
//
// Everything is in milliseconds; the caller hands over already-mapped
// parameters (see Params) and the engine smooths nothing - the module does
// that, since it knows the control rate.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace antrum_dsp {

// ---------------------------------------------------------------- constants

constexpr float kMinPredelayMs = 7.f;
// free-running pre-delay stops at 500 ms like the hardware; the longer
// buffer is only reachable under clock sync (hardware: 5.46 s)
constexpr float kMaxPredelayMs = 500.f;
constexpr float kMaxSyncPredelayMs = 4000.f;
constexpr float kMinSizeMs = 1.f;
constexpr float kMaxSizeMs = 500.f;
constexpr float kMaxCyclicMs = 3.f;     // sine vibrato depth, ms
constexpr float kErgodicFrac = 0.5f;    // grain scatter, as a fraction of size
constexpr float kShimmerWindowMs = 200.f;
constexpr float kShimmerRateHz = -5.f;  // -5 Hz over 200 ms = one octave up

// ------------------------------------------------------------------ helpers

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float wrap1(float x) {
    if (x >= 1.f) return x - 1.f;
    if (x < 0.f) return x + 1.f;
    return x;
}

// parabolic sine approximation, valid for x in [-pi, pi] (~0.1% error)
inline float fastSin(float x) {
    constexpr float B = 1.2732395447f;    // 4/pi
    constexpr float C = -0.4052847346f;   // -4/pi^2
    float y = B * x + C * x * std::fabs(x);
    return 0.225f * (y * std::fabs(y) - y) + y;
}

// xorshift32 - the grain scatter wants cheap, self-contained noise
struct Rng {
    uint32_t s = 0x9e3779b9u;
    inline float uniform() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s >> 8) * (1.f / 16777216.f);
    }
};

// ---------------------------------------------------------------- delay line

struct DelayLine {
    std::vector<float> buf;
    size_t mask = 0, wp = 0;
    float sr = 48000.f, maxDelay = 1.f;

    void init(float sampleRate, float maxMs) {
        sr = sampleRate;
        size_t n = (size_t)(sampleRate * maxMs * 0.001f) + 8;
        size_t p = 8;
        while (p < n) p <<= 1;
        buf.assign(p, 0.f);
        mask = p - 1;
        wp = 0;
        maxDelay = (float)(p - 2);
    }

    void clear() {
        std::fill(buf.begin(), buf.end(), 0.f);
        wp = 0;
    }

    inline void write(float v) {
        buf[wp] = v;
        wp = (wp + 1) & mask;
    }

    // linear interpolation, delay given in milliseconds
    inline float read(float ms) const {
        float d = clampf(ms * 0.001f * sr, 1.f, maxDelay);
        float rp = (float)(wp + buf.size()) - d;
        float fi = std::floor(rp);
        size_t i = (size_t)fi;
        float fr = rp - fi;
        float a = buf[i & mask];
        float b = buf[(i + 1) & mask];
        return a + (b - a) * fr;
    }
};

// ------------------------------------------------------------------- phasor

struct Phasor {
    float x = 0.f, sr = 48000.f;
    void init(float sampleRate) { sr = sampleRate; x = 0.f; }
    inline float process(float freq) {
        x = wrap1(x + freq / sr);
        return x;
    }
};

// -------------------------------------------------------- allpass diffuser
// Schroeder allpass on its own short delay line; gain 0..0.8, times mutually
// prime and under 15 ms so the echoes stay fused with the source.

struct Allpass {
    DelayLine line;
    void init(float sampleRate) { line.init(sampleRate, 16.f); }
    void clear() { line.clear(); }
    inline float process(float in, float timeMs, float gain) {
        float r = line.read(timeMs);
        float fb = r * gain;
        line.write(in + fb);
        return r - (in + fb) * gain;
    }
};

// ------------------------------------------------------------------- grains
// Ergodic ("Stressed Space Palindromes") modulation: two raised-cosine grains
// per delay line, each new grain picking a random offset within the depth.
// Sin^2 windows a half cycle apart sum to unity, so the level stays put.

struct Grains {
    float start[2] = {0.f, 0.f};
    float prevPhase[2] = {0.f, 0.f};
    float phaseOffset[2] = {0.f, 0.5f};
    Rng rng;

    void clear() {
        start[0] = start[1] = 0.f;
        prevPhase[0] = prevPhase[1] = 0.f;
    }

    inline float process(const DelayLine& line, float timeMs, float phase, float depthMs) {
        float sum = 0.f;
        for (int i = 0; i < 2; i++) {
            float p = wrap1(phase + phaseOffset[i]);
            if (p - prevPhase[i] < 0.f)             // phasor wrapped: new grain
                start[i] = rng.uniform() * depthMs;
            prevPhase[i] = p;
            float w = fastSin(p * (float)M_PI);
            sum += line.read(timeMs + start[i]) * w * w;
        }
        return sum;
    }
};

// -------------------------------------------------------- early reflections
// Six taps per channel off the first two delay lines, scaled by the same size
// parameter as the network, fading out as the space grows.

struct EarlyReflections {
    static constexpr float kMinus3dB = 0.707946f;
    static constexpr float kMinus21dB = 0.089125f;

    float factor[2][6] = {
        {0.f,    0.188f, 0.278f, 0.38f,  0.482f, 0.584f},
        {0.018f, 0.086f, 0.29f,  0.392f, 0.494f, 0.597f},
    };
    float atten[6];

    EarlyReflections() {
        for (int i = 0; i < 6; i++)
            atten[i] = std::pow(10.f, (i / 6.f * -6.f) * 0.05f);
    }

    inline void process(float sizeMs, const DelayLine* lines, float& l, float& r) const {
        float gain = (sizeMs - kMinSizeMs) * ((kMinus21dB - kMinus3dB) / kMaxSizeMs)
                   + kMinus3dB;
        float sl = 0.f, sr_ = 0.f;
        for (int i = 0; i < 6; i++) {
            sl += lines[0].read(sizeMs * factor[0][i]) * atten[i];
            sr_ += lines[1].read(sizeMs * factor[1][i]) * atten[i];
        }
        l = sl * gain;
        r = sr_ * gain;
    }
};

// ------------------------------------------------------------------ shimmer
// Granular octave-up: a 200 ms window read at -5 Hz walks toward the write
// head at twice real time. Fed the sum of input and network output, its
// output is injected back into the first two delay lines.

struct Shimmer {
    DelayLine line[2];
    Phasor phasor;

    void init(float sampleRate) {
        line[0].init(sampleRate, kShimmerWindowMs * 1.1f);
        line[1].init(sampleRate, kShimmerWindowMs * 1.1f);
        phasor.init(sampleRate);
    }

    void clear() {
        line[0].clear();
        line[1].clear();
        phasor.x = 0.f;
    }

    inline void process(float dryL, float dryR, float wetL, float wetR, float mix,
                        float& outL, float& outR) {
        outL = dryL;
        outR = dryR;
        if (mix > 0.f) {
            float base = phasor.process(kShimmerRateHz);
            float gl = 0.f, gr = 0.f;
            for (int i = 0; i < 2; i++) {
                float p = i ? wrap1(base + 0.5f) : base;
                float w = fastSin(p * (float)M_PI);
                w *= w;
                float t = p * kShimmerWindowMs;
                gl += line[0].read(t) * w;
                gr += line[1].read(t) * w;
            }
            outL += (gl - dryL) * mix;
            outR += (gr - dryR) * mix;
        }
        line[0].write((dryL + wetL) * 0.5f);
        line[1].write((dryR + wetR) * 0.5f);
    }
};

// ------------------------------------------------------------------ reverse
// Plays the pre-delay buffer backwards, two copies a half cycle apart with a
// 7 ms crossfade so the wrap does not click.

struct Reverse {
    Phasor phasor;

    void init(float sampleRate) { phasor.init(sampleRate); }
    void clear() { phasor.x = 0.f; }

    inline void process(const DelayLine& l, const DelayLine& r, float timeMs,
                        float& outL, float& outR) {
        float a = phasor.process(1000.f / std::max(timeMs, 1.f)) * 2.f;
        float b = a + 1.f;
        if (b >= 2.f) b -= 2.f;

        float xf = timeMs / kMinPredelayMs;
        float offset = 1.f / xf + 1.f;
        float up = std::min(a * xf, 1.f);
        float down = clampf((offset - a) * xf, 0.f, 1.f);
        float ga = up * down;
        float gb = 1.f - ga;

        outL = outR = 0.f;
        if (ga > 0.f) {
            outL += l.read(a * timeMs) * ga;
            outR += r.read(a * timeMs) * ga;
        }
        if (gb > 0.f) {
            outL += l.read(b * timeMs) * gb;
            outR += r.read(b * timeMs) * gb;
        }
    }
};

// -------------------------------------------------------------- tilt filter
// Bilinear transform of the analog tilt network (two RC sections around a pot),
// as modelled in dm-Reverb. Coefficients are cheap enough for control rate.

struct TiltFilter {
    static constexpr float C1 = 5.6e-9f, C2 = 5.6e-9f;
    static constexpr float R1 = 2250.f, R2 = 2250.f;
    static constexpr float RF1 = 47000.f, RF2 = 47000.f;
    static constexpr float RT = 140000.f;

    float b[3] = {1.f, 0.f, 0.f}, a[3] = {1.f, 0.f, 0.f};
    float z1L = 0.f, z1R = 0.f, z2L = 0.f, z2R = 0.f;
    float t1 = 0.f, t2 = 0.f;   // bilinear scale factors

    void init(float sampleRate) {
        float t = 1.f / sampleRate;
        t1 = t * 0.5f;
        t2 = t * t * 0.25f;
        clear();
        setTilt(0.5f);
    }

    void clear() { z1L = z1R = z2L = z2R = 0.f; }

    // tilt: 0 = full low boost, 0.5 = flat, 1 = full high boost
    void setTilt(float tilt) {
        const float C1C2 = C1 * C2;
        const float C1C2R1 = C1C2 * R1;
        const float C1C2R1R2 = C1C2R1 * R2;
        const float C1C2R2 = C1C2 * R2;
        const float C1C2R2RF2 = C1C2R2 * RF2;
        const float C2R2 = C2 * R2;
        const float C1R1 = C1 * R1;
        const float C1RF2 = C1 * RF2;
        const float C1C2RF1RF2 = C1C2 * RF1 * RF2;
        const float C2RF1 = C2 * RF1;

        float ra = RT * tilt;
        float rb = RT * (1.f - tilt);

        float sb[3], sa[3];
        sb[0] = -C1C2R2RF2 * RF1 - C1C2R2RF2 * rb - C1C2R1R2 * rb
              - C1C2R2RF2 * R1 - C1C2RF1RF2 * ra - C1C2R2RF2 * ra;
        sb[1] = -C1RF2 * RF1 - C1RF2 * rb - C2R2 * rb - C2R2 * RF2
              - C1R1 * rb - C1R1 * RF2 - C1RF2 * ra;
        sb[2] = -rb - RF2;
        sa[0] = C1C2RF1RF2 * rb + C1C2R1 * RF1 * rb + C1C2RF1RF2 * R1
              + C1C2R1R2 * RF1 + C1C2R1 * RF1 * ra + C1C2R1R2 * ra;
        sa[1] = C2RF1 * rb + C2RF1 * RF2 + C2R2 * RF1 + C1R1 * RF1
              + C2RF1 * ra + C2R2 * ra + C1R1 * ra;
        sa[2] = RF1 + ra;

        bilinear(sb, b);
        bilinear(sa, a);
        float n = a[0] != 0.f ? 1.f / a[0] : 1.f;
        for (int i = 0; i < 3; i++) { b[i] *= n; a[i] *= n; }
    }

    void bilinear(const float* s, float* out) const {
        float x0 = s[0], x1 = s[1] * t1, x2 = s[2] * t2;
        out[0] = x0 + x1 + x2;
        out[1] = -2.f * x0 + 2.f * x2;
        out[2] = x0 - x1 + x2;
    }

    inline void process(float inL, float inR, float& outL, float& outR) {
        float yL = inL * b[0] + z1L;
        float yR = inR * b[0] + z1R;
        z1L = inL * b[1] - yL * a[1] + z2L;
        z1R = inR * b[1] - yR * a[1] + z2R;
        z2L = inL * b[2] - yL * a[2];
        z2R = inR * b[2] - yR * a[2];
        outL = yL;
        outR = yR;
    }
};

// --------------------------------------------------------------- parameters

struct Params {
    float predelayMs = kMinPredelayMs;
    float reverse = 0.f;      // 0..1 crossfade into the reversed buffer
    float sizeMs = 80.f;
    float speedHz = 2.f;
    float cyclicMs = 0.f;     // sine vibrato depth (depth knob CCW)
    float ergodicMs = 0.f;    // grain scatter depth (depth knob CW)
    float shimmer = 0.f;      // 0..1, folded into the top of the ergodic range
    float diffuse = 0.f;      // allpass gain, 0..0.8
    float damp = 0.f;         // absorb pole, 0..0.993 (at 44.1 kHz)
    float decay = 0.9f;       // network feedback gain, 0..1.2
    float mix = 0.5f;
    // tilt is not here: its coefficients are expensive, so the module drives
    // Engine::tilt.setTilt() directly at control rate
};

// ------------------------------------------------------------------- engine

struct Engine {
    // delay time ratios of the four lines (mutually prime-ish)
    static constexpr float kFrac0 = 0.34306569f;
    static constexpr float kFrac1 = 0.48905109f;
    static constexpr float kFrac2 = 0.73722628f;
    static constexpr float kFrac3 = 1.f;

    float sr = 0.f;
    float frac[4] = {kFrac0, kFrac1, kFrac2, kFrac3};
    float diffTimeMs[4] = {5.75f, 9.41667f, 13.08333f, 14.91667f};
    float lfoOffset[4] = {0.f, 0.25f, 0.5f, 0.75f};

    DelayLine pre[2];
    Reverse reverse;
    DelayLine line[4];
    Allpass diffuser[4];
    Grains grains[4];
    Phasor lfo;
    Shimmer shimmer;
    EarlyReflections early;
    TiltFilter tilt;

    float absorbZ[4] = {0.f, 0.f, 0.f, 0.f};
    float dcX[4] = {0.f, 0.f, 0.f, 0.f};
    float dcY[4] = {0.f, 0.f, 0.f, 0.f};
    float dcCoeff = 0.995f;
    float avgZ = 0.f, avgB1 = 0.f;   // 20 Hz mean-square follower
    float dampExp = 1.f;             // 44100/sr, keeps the absorb pole SR-fair
    float mixDry = 0.707f, mixWet = 0.707f, mixLast = -1.f;

    void init(float sampleRate) {
        sr = sampleRate;
        float preMs = kMaxSyncPredelayMs * 1.05f;
        pre[0].init(sr, preMs);
        pre[1].init(sr, preMs);
        reverse.init(sr);
        for (int i = 0; i < 4; i++) {
            line[i].init(sr, kMaxSizeMs * (frac[i] + kErgodicFrac) + kMaxCyclicMs + 4.f);
            diffuser[i].init(sr);
            grains[i].rng.s = 0x9e3779b9u + 0x85ebca6bu * (uint32_t)(i + 1);
        }
        lfo.init(sr);
        shimmer.init(sr);
        tilt.init(sr);
        dcCoeff = 1.f - 220.5f / sr;
        avgB1 = std::exp(-2.f * (float)M_PI * 20.f / sr);
        dampExp = 44100.f / sr;
        clear();
    }

    void clear() {
        pre[0].clear();
        pre[1].clear();
        reverse.clear();
        for (int i = 0; i < 4; i++) {
            line[i].clear();
            diffuser[i].clear();
            grains[i].clear();
            absorbZ[i] = dcX[i] = dcY[i] = 0.f;
        }
        lfo.x = 0.f;
        shimmer.clear();
        tilt.clear();
        avgZ = 0.f;
        mixLast = -1.f;
    }

    // network energy, 0..1-ish - the module publishes this as the CV output
    inline float energy() const { return std::sqrt(avgZ); }

    // 3rd-degree Chebyshev fold. Past +/-2.65 the curve is held at its own
    // extreme, so it saturates rather than running away.
    static inline float chebyshev(float x) {
        if (x < -2.65155f) return 1.f;
        if (x > 2.65155f) return -1.f;
        return (0.97239411f - 0.19194795f * x * x) * x;
    }

    inline float networkRead(int i, const Params& p, float phase) {
        float t = p.sizeMs * frac[i];
        if (p.ergodicMs > 0.f) {
            float g = grains[i].process(line[i], t, wrap1(phase + lfoOffset[i]), p.ergodicMs);
            // fade in the grain cloud over the first 5% of the depth range so
            // the knob centre stays continuous
            float thresh = kErgodicFrac * p.sizeMs * 0.05f;
            if (p.ergodicMs < thresh && thresh > 0.f) {
                float plain = line[i].read(t);
                float f = p.ergodicMs / thresh;
                return plain + (g - plain) * f;
            }
            return g;
        }
        if (p.cyclicMs > 0.f) {
            float ph = wrap1(phase + lfoOffset[i]);
            float lfoV = fastSin((ph * 2.f - 1.f) * (float)M_PI) * p.cyclicMs;
            return line[i].read(t + lfoV);
        }
        return line[i].read(t);
    }

    void process(float inL, float inR, const Params& p, float& outL, float& outR) {
        // ---- pre-delay, forward and/or reversed
        float pdL, pdR;
        float t = clampf(p.predelayMs, kMinPredelayMs, kMaxSyncPredelayMs);
        if (p.reverse <= 0.f) {
            pdL = pre[0].read(t);
            pdR = pre[1].read(t);
        }
        else {
            float rL, rR;
            reverse.process(pre[0], pre[1], t, rL, rR);
            if (p.reverse >= 1.f) {
                pdL = rL;
                pdR = rR;
            }
            else {
                float fL = pre[0].read(t), fR = pre[1].read(t);
                pdL = fL + (rL - fL) * p.reverse;
                pdR = fR + (rR - fR) * p.reverse;
            }
        }
        pre[0].write(inL);
        pre[1].write(inR);

        // ---- early reflections, tapped off the lines before this sample's
        //      write, exactly as the network taps are
        float erL, erR;
        early.process(p.sizeMs, line, erL, erR);

        // ---- read the network
        float phase = lfo.process(p.speedHz);
        float taps[4];
        float peak = 0.f;
        for (int i = 0; i < 4; i++) {
            taps[i] = networkRead(i, p, phase);
            peak = std::max(peak, std::fabs(taps[i]));
        }

        // ---- saturation, driven by the network's own smoothed level
        float avg = energy();
        float satMix = clampf(avg * avg, 0.f, 1.f);
        float sat[4];
        for (int i = 0; i < 4; i++)
            sat[i] = taps[i] + (chebyshev(taps[i]) - taps[i]) * satMix;

        float netL = (sat[0] + sat[2]) * 0.5f;
        float netR = (sat[1] + sat[3]) * 0.5f;

        avgZ = peak * peak * (1.f - avgB1) + avgZ * avgB1;

        // ---- unitary (Hadamard) feedback matrix. The decay gain rides the
        //      taps rather than the delay line write, so it is the gain of
        //      the loop alone and never attenuates the incoming signal: the
        //      matrix is linear, so decay*H(sat) == H(sat*decay) and the
        //      loop behaves identically either way.
        float g[4];
        for (int i = 0; i < 4; i++)
            g[i] = sat[i] * p.decay;
        float m[4];
        m[0] = 0.5f * (g[0] - g[1] - g[2] + g[3]);
        m[1] = 0.5f * (g[0] + g[1] - g[2] - g[3]);
        m[2] = 0.5f * (g[0] - g[1] + g[2] - g[3]);
        m[3] = 0.5f * (g[0] + g[1] + g[2] + g[3]);

        // ---- the input (and the shimmer voice) enter the first two lines
        float shL, shR;
        shimmer.process(pdL, pdR, netL, netR, p.shimmer, shL, shR);
        m[0] += shL;
        m[1] += shR;

        // ---- DC block, absorb, diffuse, write back
        float b1 = p.damp > 0.f ? std::pow(p.damp, dampExp) : 0.f;
        float a0 = 1.f - b1;
        for (int i = 0; i < 4; i++) {
            float y = m[i] - dcX[i] + dcCoeff * dcY[i];
            dcX[i] = m[i];
            dcY[i] = y;
            absorbZ[i] = y * a0 + absorbZ[i] * b1;
            float d = diffuser[i].process(absorbZ[i], diffTimeMs[i], p.diffuse);
            line[i].write(d);
        }

        // ---- output: network plus early reflections, level-compensated
        float comp = avg > 0.4f ? 0.4f / avg : 1.f;
        float wetL = (netL + erL) * comp;
        float wetR = (netR + erR) * comp;

        tilt.process(wetL, wetR, wetL, wetR);

        if (p.mix != mixLast) {
            mixLast = p.mix;
            float f = p.mix * (float)M_PI_2;
            mixDry = std::cos(f);
            mixWet = std::sin(f);
        }
        outL = inL * mixDry + wetL * mixWet;
        outR = inR * mixDry + wetR * mixWet;
    }
};

}   // namespace antrum_dsp
