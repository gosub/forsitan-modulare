#pragma once
// scando_engine.hpp - scanned-synthesis DSP core (Rack-free, header-only)
//
// Scanned synthesis (Verplank / Mathews / Shaw, Interval Research 1998-2000):
//   - a string of N masses connected by springs vibrates slowly at "haptic"
//     rates (well below audio); its shape x[] is a dynamic wavetable.
//   - a phase accumulator *scans* x[] at audio rate to produce a pitched tone.
//   - pitch (scan speed) and timbre (string dynamics) are independent.
//
// Topology matches the classic Csound scansyn / Qu-Bit string: the masses form
// a NON-circular chain (no wrap-around spring). The two endpoints are fixed at
// zero - the "two fixed ends" boundary of the original paper (x0=v0=0,
// xN=vN=0). The first/last moving mass therefore couples to one moving neighbour
// plus a fixed wall, and because the scanned table starts and ends at zero it
// loops without a discontinuity.
//
// The per-tick integration is the symplectic (semi-implicit) Euler scheme from
// the paper's Appendix A: velocity is updated first, then position uses the new
// velocity, which keeps an oscillator stable while S*omega < 2:
//
//   spring = K*( (x[i-1]-x[i]) [if i>0] + (x[i+1]-x[i]) [if i<N-1] )
//   a[i]   = spring - C*x[i] - D*v[i] + f[i]   // centering, damping, excitation
//   v[i]  += a[i] * S
//   x[i]  += v[i] * S
//
// Mass is folded out (M = 1); the Mass control instead scales the integration
// step S, which gives the same perceptual heavy/subtle vs light/volatile result
// without the instability of dividing by a tiny mass.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace scando {

constexpr int   kN     = 128;            // number of masses == dynamic wavetable length
constexpr float kTwoPi = 6.28318530718f;

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

inline float softClip(float x) { return std::tanh(x); }

// ── tiny deterministic PRNG (xorshift32) ─────────────────────────────────────
struct Rng {
    uint32_t s = 0x1234567u;
    float bipolar() {  // [-1, 1)
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }
};

// ── one-pole DC blocker (HP ~ a few Hz) ──────────────────────────────────────
struct DCBlocker {
    float x1 = 0.f, y1 = 0.f;
    void reset() { x1 = y1 = 0.f; }
    float process(float x) {
        float y = x - x1 + 0.999f * y1;
        x1 = x; y1 = y;
        return y;
    }
};

// ── output stage: slow peak limiter + soft clip ──────────────────────────────
// The string amplitude varies hugely with the controls (a faint noise-driven
// bed up to a loud pluck). This holds the level near a musical target without
// killing the natural decay of a pluck: it only attenuates when too loud, and
// never boosts quiet passages.
struct OutStage {
    float peak    = 0.f;
    float target  = 2.0f;      // string amplitude held to ~this when loud
    float release = 0.99999f;  // ~2 s peak release
    float gain    = 0.5f;      // pre-clip drive (target*gain -> tanh knee)

    void reset() { peak = 0.f; }

    float process(float s) {
        float a = std::fabs(s);
        if (a > peak) peak = a; else peak *= release;
        float g = (peak > target) ? target / peak : 1.f;
        return softClip(s * g * gain);
    }
};

// ── hammer shapes: sine -> saw -> noise -> dual-pulse, morphed by knob ────────
struct HammerTable {
    float shapes[4][kN];

    void init() {
        Rng rng;
        for (int i = 0; i < kN; ++i) {
            float p = (float)i / kN;                 // 0..1 around the loop
            shapes[0][i] = std::sin(kTwoPi * p);      // sine
            shapes[1][i] = 2.f * p - 1.f;             // saw (zero-mean ramp)
            shapes[2][i] = rng.bipolar();             // noise (fixed table)
            // dual pulse: two bipolar bumps a half-cycle apart, zero-mean
            float a = std::exp(-200.f * (p - 0.25f) * (p - 0.25f));
            float b = std::exp(-200.f * (p - 0.75f) * (p - 0.75f));
            shapes[3][i] = a - b;
        }
    }

    // morph in [0,1] crosses the four shapes in order; writes length-kN out[]
    void build(float morph, float* out) const {
        float t  = clampf(morph, 0.f, 1.f) * 3.f;   // 0..3 across 4 shapes
        int   i0 = std::min((int)t, 2);
        int   i1 = i0 + 1;
        float f  = t - i0;
        for (int i = 0; i < kN; ++i)
            out[i] = shapes[i0][i] + f * (shapes[i1][i] - shapes[i0][i]);
    }
};

// ── the vibrating string (dynamic wavetable) ─────────────────────────────────
struct ScannedString {
    float x[kN];   // positions == the wavetable scanned for audio
    float v[kN];   // velocities

    float K = 0.3f;  // inter-mass spring stiffness
    float C = 0.4f;  // centering spring to earth
    float D = 0.1f;  // damping (negative => energy pumped in)
    float S = 0.5f;  // integration step (set from the Mass control)

    void reset() {
        for (int i = 0; i < kN; ++i) { x[i] = 0.f; v[i] = 0.f; }
    }

    void setParams(float k, float c, float d, float s) { K = k; C = c; D = d; S = s; }

    // seed the string with a shape (a "pluck"); endpoints stay fixed at zero
    void setShape(const float* shape, float amp) {
        for (int i = 0; i < kN; ++i) { x[i] = amp * shape[i]; v[i] = 0.f; }
        x[0] = x[kN - 1] = 0.f;
    }

    // one physics tick; force[] is the per-mass excitation (may be null).
    // Endpoints (0 and kN-1) are fixed walls and are not integrated.
    void update(const float* force) {
        float xo[kN];
        for (int i = 0; i < kN; ++i) xo[i] = x[i];  // snapshot for symmetric neighbours

        for (int i = 1; i < kN - 1; ++i) {
            float spring = (xo[i - 1] - xo[i]) + (xo[i + 1] - xo[i]);
            float a = K * spring - C * xo[i] - D * v[i];
            if (force) a += force[i];
            v[i] += a * S;
            x[i]  = xo[i] + v[i] * S;
            // safety clamp so aggressive settings can't run to inf/NaN
            if (!std::isfinite(x[i]) || !std::isfinite(v[i])) { x[i] = 0.f; v[i] = 0.f; }
            x[i] = clampf(x[i], -50.f, 50.f);
            v[i] = clampf(v[i], -50.f, 50.f);
        }
    }

    // read the table at fractional position phase in [0, kN) with 4-point cubic
    // (Catmull-Rom) interpolation. The table loops cleanly (pinned ends == 0), so
    // the four taps wrap circularly.
    float scan(float phase) const {
        int   i1 = (int)phase;
        float f  = phase - i1;
        int   i0 = (i1 - 1 + kN) % kN;
        int   i2 = (i1 + 1) % kN;
        int   i3 = (i1 + 2) % kN;
        i1 %= kN;
        float a0 = x[i0], a1 = x[i1], a2 = x[i2], a3 = x[i3];
        return a1 + 0.5f * f * ((a2 - a0)
             + f * (2.f * a0 - 5.f * a1 + 4.f * a2 - a3
             + f * (3.f * (a1 - a2) + a3 - a0)));
    }
};

}  // namespace scando
