// sdt_common.hpp — constants, randomness and small helpers for the SDT port.
//
// Ported from the Sound Design Toolkit (SDT), Delle Monache, Rocchesso et al.,
// https://github.com/SkAT-VG/SDT, GPL-3.0-or-later, out of the SOb / CLOSED /
// SkAT-VG projects. The SDT is plain C built around global state (one sample
// rate, one `rand()` stream, opaque structs on the heap). This port keeps the
// algorithms exactly and changes only the plumbing:
//
//   - no globals: sample rate and time step are per object, set through
//     setSampleRate(), so two modules at different rates can coexist;
//   - no rand(): each object owns an LCG with the same distributions, so a
//     module is reproducible and does not perturb anything else;
//   - no heap in the audio path: fixed-capacity arrays instead of malloc.
//
// Pure C++11, no Rack dependencies, so the offline harnesses compile it
// standalone. Header-only, everything inline, safe to include from more than
// one translation unit.
#pragma once

#include <cmath>
#include <cstdint>

namespace sdt {

static const double kPi = 3.14159265358979323846;
static const double kTwoPi = 6.28318530717958647692;
// Speed of sound, m/s, and gravity — SDT_MACH1 / SDT_EARTH.
static const double kMach1 = 340.29;
static const double kEarth = 9.81;
static const double kMicro = 0.000001;

inline double fclip(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline int iclip(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline double scale(double x, double srcMin, double srcMax, double dstMin,
                    double dstMax, double gamma) {
    return std::pow((x - srcMin) / (srcMax - srcMin), gamma) * (dstMax - dstMin) + dstMin;
}

inline int signum(double x) { return x < 0.0 ? -1 : (x == 0.0 ? 0 : 1); }

// Length of a tube, in samples of travel time — SDT_samplesInAir.
inline double samplesInAir(double length, double sampleRate) {
    return (length < 0.0 ? 0.0 : length) / kMach1 * sampleRate;
}

inline double samplesInAirInv(double samples, double sampleRate) {
    return samples * kMach1 / sampleRate;
}

// The SDT draws uniform noise from a 32-bit LCG (SDT_whiteNoise) and uniform
// deviates from rand() (SDT_frand). Both live here as one per-object stream,
// with the same constants and the same output ranges.
struct Rng {
    uint32_t s;

    explicit Rng(uint32_t seed = 42) : s(seed) {}

    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    // [-1, 1]
    double white() { return (double)next() / 2147483647.0 - 1.0; }
    // [0, 1)
    double frand() { return (double)next() / 4294967296.0; }
    // Exponential with rate lambda — SDT_expRand.
    double expRand(double lambda) { return -std::log(1.0 - frand()) / lambda; }
};

}  // namespace sdt
