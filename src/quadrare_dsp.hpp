// quadrare_dsp.hpp - Walsh–Hadamard transform core for the quadrare module.
//
// Header-only so test/smoke_quadrare can exercise the maths without Rack.
// Everything here works on a block of `n` coefficients, where n is a power of
// two between kMinSize and kMaxSize.
#pragma once

#include <algorithm>
#include <cmath>

namespace quadrare {

// 16 panel columns, one slider + COEFF OUT jack + COEFF IN jack each. Each
// column owns exactly one Walsh coefficient, the k-th in sequency order, so
// every jack is mono at every size.
static constexpr int kSliders = 16;
static constexpr int kMinSize = 16;
static constexpr int kMaxSize = 512;
static constexpr int kSizeCount = 6;

// Transform sizes. Because only the lowest 16 coefficients are ever exposed,
// size acts as a zoom on the low end rather than a channel-count multiplier:
// coefficient spacing is fs/2n, so the 16 sliders span 8*fs/n. At 48 kHz that
// is 0-24 kHz at n=16 and 0-750 Hz at n=512.
inline int sizeAt(int i) {
    static const int sizes[kSizeCount] = {16, 32, 64, 128, 256, 512};
    return sizes[std::min(std::max(i, 0), kSizeCount - 1)];
}

// ── the transform ───────────────────────────────────────────────────────────

// In-place fast Walsh–Hadamard transform in natural (Kronecker/Hadamard)
// order. Unnormalized, and its own inverse up to a factor of n: applying it
// twice multiplies by n. The forward direction is used raw and the inverse
// scaled by 1/n, per the spec's normalization convention.
inline void fwht(float* d, int n) {
    for (int len = 1; len < n; len <<= 1)
        for (int start = 0; start < n; start += len << 1)
            for (int i = start; i < start + len; ++i) {
                const float a = d[i];
                const float b = d[i + len];
                d[i]       = a + b;
                d[i + len] = a - b;
            }
}

inline void scale(float* d, int n, float g) {
    for (int i = 0; i < n; ++i) d[i] *= g;
}

// ── sequency ordering ───────────────────────────────────────────────────────
//
// The panel presents coefficients in sequency order (fewest sign changes
// first, so band 0 is the block mean), while the FWHT above produces natural
// order. Row h of the Hadamard matrix has grayToBinary(bitReverse(h)) sign
// changes, which gives the permutation between the two.

inline int bitReverse(int v, int bits) {
    int r = 0;
    for (int i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

inline int grayToBinary(int g) {
    int b = g;
    while (g >>= 1) b ^= g;
    return b;
}

inline int sequencyOf(int h, int bits) { return grayToBinary(bitReverse(h, bits)); }

// Sign of the h-th natural-order Walsh function at sample t: the Hadamard
// matrix is H[h][t] = (-1)^popcount(h & t). Used to build the per-coefficient
// component outputs directly, instead of one inverse transform per slider.
inline float walshSign(int h, int t) {
    unsigned v = (unsigned) (h & t);
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1u) ? -1.f : 1.f;
}

inline int log2i(int n) {
    int b = 0;
    while ((1 << b) < n) ++b;
    return b;
}

// seq2nat[s] is the natural-order index of the s-th sequency coefficient.
struct Permutation {
    int seq2nat[kMaxSize] = {};
    int size = 0;

    void build(int n) {
        if (size == n) return;
        const int bits = log2i(n);
        for (int h = 0; h < n; ++h) seq2nat[sequencyOf(h, bits)] = h;
        size = n;
    }

    void toSequency(const float* nat, float* seq, int n) const {
        for (int s = 0; s < n; ++s) seq[s] = nat[seq2nat[s]];
    }

    void toNatural(const float* seq, float* nat, int n) const {
        for (int s = 0; s < n; ++s) nat[seq2nat[s]] = seq[s];
    }
};

// ── the lossy stage ─────────────────────────────────────────────────────────

// KEEP: rank thresholding, the classic transform-coding move. Retain the k
// largest-magnitude coefficients and zero the rest. Scale-invariant.
// `scratch` must hold at least n floats.
inline void keepLargest(float* c, int n, int k, float* scratch) {
    if (k >= n) return;
    if (k <= 0) {
        std::fill(c, c + n, 0.f);
        return;
    }
    for (int i = 0; i < n; ++i) scratch[i] = std::fabs(c[i]);
    std::nth_element(scratch, scratch + (n - k), scratch + n);
    const float thr = scratch[n - k];
    // Ties can keep marginally more than k, which is harmless.
    for (int i = 0; i < n; ++i)
        if (std::fabs(c[i]) < thr) c[i] = 0.f;
}

// QUANT: coefficient bit reduction. The grid is scaled to the block's peak
// coefficient, so the effect does not depend on how hard the module is
// driven, and no drive control is needed to make it useful (see the
// "Design notes" in doc/quadrare.md).
inline void quantize(float* c, int n, float levels) {
    if (levels <= 0.f) return;
    float peak = 0.f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(c[i]));
    if (peak <= 1e-12f) return;
    const float step = peak / levels;
    const float inv  = 1.f / step;
    for (int i = 0; i < n; ++i) c[i] = std::round(c[i] * inv) * step;
}

}  // namespace quadrare
