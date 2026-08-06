// caligo_dsp.hpp — Julian Parker's Greyhole.
//
// Greyhole is not a reverb. It is a long modulated echo wrapped inside a
// nested allpass diffusion network, and what makes it sound like itself is
// that the diffuser sits in the *forward* path of a feedback loop whose delay
// is long enough to hear as repeats: every repeat is smeared further than the
// last.
//
// Sources (the algorithm, not the code — nothing here is machine-translated):
//   - Julian Parker, Greyhole (2013), DEIND project, bug fixes and interface
//     changes by Till Bovermann. SuperCollider UGen in sc3-plugins
//     (source/DEINDUGens/GreyholeRaw.dsp), GPL-2.0-or-later.
//   - The same algorithm as `jp_gh_rev`/`re.greyhole` in Faust's
//     faustlibraries/reverbs.lib, MIT.
//
//   in ─┬──> (+) ──> diffuser stage 0   (nested 4 deep, g = +diff, scale 10)
//       │     ^      diffuser stage 1   (nested 4 deep, g = -diff, scale 29)
//       │     │      diffuser stage 2   (nested 4 deep, g = +diff, scale 48)
//       │     │            v
//       │     │      one-pole damping ──────────────────> wet out
//       │     │            v
//       │     │      quadrature-modulated short delay (cos on L, sin on R)
//       │     │            v
//       │     │      long delay (crossfading integer, or slewed tape)
//       │     │            v
//       │     │      send / return break
//       │     │            v
//       │     └───── x feedback ─> saturator ─> DC block
//
// Three stages, each a four-deep *nested* allpass, each level holding one
// fractional delay line per channel: 24 delay lines. The diffusion
// coefficient alternates sign across the stages ((-1)^i * diff), which is
// where the reversed-sounding build-up at medium diff comes from. Delay
// lengths are prime numbers indexed by size*scale, and the *index* glides
// (one-pole, tau 0.23 s), so sweeping size slides through the prime table
// rather than jumping between entries.
//
// Deviations from the original, all deliberate:
//   - the hardcoded pi/2 rotator angle is exposed (spin);
//   - the long delay reaches 16 s instead of the Faust buffer's 1.486 s, and
//     can run in a pitch-bending tape mode instead of crossfading;
//   - the loop has a soft saturator and a DC blocker, so feedback past unity
//     is bounded rather than undefined;
//   - the prime delay lengths are rescaled by SR/44100 and rounded, so the
//     diffuser keeps its *time* constants across sample rates (the original
//     specifies samples, and so halves in duration from 48 to 96 kHz);
//   - the scattering constants can be reseeded (scatter);
//   - size can be walked by a slow bounded random walk (drift).
//
// Everything is Rack-independent. The caller hands over already-mapped and
// already-smoothed parameters (see Params) and calls updateControl() every
// kCoefUpdate samples for the ones whose coefficients cost trigonometry.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace caligo_dsp {

// ---------------------------------------------------------------- constants

constexpr int kStages = 3;                     // cascaded diffuser stages
constexpr int kNest = 4;                       // allpass levels per stage
constexpr int kLines = kStages * kNest * 2;    // 24 fractional delay lines

constexpr float kMinTimeSec = 0.01f;
constexpr float kMaxTimeSec = 16.f;
// 32 MB of stereo float32 is the ceiling for the long delay; below 262 kHz
// the full 16 s fits and this never bites
constexpr size_t kMaxLongFrames = 4194304;

constexpr float kMinSize = 0.5f;
constexpr float kMaxSize = 4.f;
constexpr float kSizeCeil = 5.f;      // drift may push past the knob's top
constexpr float kMaxDiff = 0.95f;     // the raw slider stops at 0.99; four
                                      // nested levels want more margin
constexpr float kMaxFeedback = 1.2f;
constexpr float kMaxSpin = 1.5707963f;

// smooth_init(0.9999) is a one-pole on the delay length in samples:
// tau = 1/(44100 * (1 - 0.9999)) = 0.2268 s. Sample-rate compensated here.
constexpr float kGlideTau = 0.2268f;
constexpr int kCoefUpdate = 16;

// the original's modulated feedback delay: 10 + d + d*cos, with
// d = (SR/44100) * 50 * modDepth samples
constexpr float kModDepthSamples = 50.f;
constexpr float kModBase = 10.f;

// the loop saturator is exactly linear below kSatLinear and asymptotes to
// kSatCeil, so at nominal levels (+/-1 = +/-5 V) it does nothing at all
constexpr float kSatLinear = 1.f;
constexpr float kSatCeil = 1.9f;

// crossfade for the dissolve-mode long delay, as a fraction of the delay
// time, bounded either side
constexpr float kXfadeFrac = 0.25f;
constexpr float kXfadeMinSec = 0.005f;
constexpr float kXfadeMaxSec = 0.25f;

// tape mode: how fast the read pointer may chase a new delay time. 0.5
// samples per sample is an octave down at the extreme.
constexpr float kTapeTau = 0.08f;
constexpr float kTapeMaxRate = 0.5f;

// ---------------------------------------------------------------- primes
// ma.primes(n), 0-indexed. The largest index this module can reach is
// kSizeCeil * (the largest scattered scale) = 5 * 141 = 705, so 768 entries
// cover it and the last one still fits in a uint16_t.

constexpr int kPrimeCount = 768;

static const uint16_t kPrimes[kPrimeCount] = {
       2,    3,    5,    7,   11,   13,   17,   19,   23,   29,   31,   37,   41,   43,   47,   53,
      59,   61,   67,   71,   73,   79,   83,   89,   97,  101,  103,  107,  109,  113,  127,  131,
     137,  139,  149,  151,  157,  163,  167,  173,  179,  181,  191,  193,  197,  199,  211,  223,
     227,  229,  233,  239,  241,  251,  257,  263,  269,  271,  277,  281,  283,  293,  307,  311,
     313,  317,  331,  337,  347,  349,  353,  359,  367,  373,  379,  383,  389,  397,  401,  409,
     419,  421,  431,  433,  439,  443,  449,  457,  461,  463,  467,  479,  487,  491,  499,  503,
     509,  521,  523,  541,  547,  557,  563,  569,  571,  577,  587,  593,  599,  601,  607,  613,
     617,  619,  631,  641,  643,  647,  653,  659,  661,  673,  677,  683,  691,  701,  709,  719,
     727,  733,  739,  743,  751,  757,  761,  769,  773,  787,  797,  809,  811,  821,  823,  827,
     829,  839,  853,  857,  859,  863,  877,  881,  883,  887,  907,  911,  919,  929,  937,  941,
     947,  953,  967,  971,  977,  983,  991,  997, 1009, 1013, 1019, 1021, 1031, 1033, 1039, 1049,
    1051, 1061, 1063, 1069, 1087, 1091, 1093, 1097, 1103, 1109, 1117, 1123, 1129, 1151, 1153, 1163,
    1171, 1181, 1187, 1193, 1201, 1213, 1217, 1223, 1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283,
    1289, 1291, 1297, 1301, 1303, 1307, 1319, 1321, 1327, 1361, 1367, 1373, 1381, 1399, 1409, 1423,
    1427, 1429, 1433, 1439, 1447, 1451, 1453, 1459, 1471, 1481, 1483, 1487, 1489, 1493, 1499, 1511,
    1523, 1531, 1543, 1549, 1553, 1559, 1567, 1571, 1579, 1583, 1597, 1601, 1607, 1609, 1613, 1619,
    1621, 1627, 1637, 1657, 1663, 1667, 1669, 1693, 1697, 1699, 1709, 1721, 1723, 1733, 1741, 1747,
    1753, 1759, 1777, 1783, 1787, 1789, 1801, 1811, 1823, 1831, 1847, 1861, 1867, 1871, 1873, 1877,
    1879, 1889, 1901, 1907, 1913, 1931, 1933, 1949, 1951, 1973, 1979, 1987, 1993, 1997, 1999, 2003,
    2011, 2017, 2027, 2029, 2039, 2053, 2063, 2069, 2081, 2083, 2087, 2089, 2099, 2111, 2113, 2129,
    2131, 2137, 2141, 2143, 2153, 2161, 2179, 2203, 2207, 2213, 2221, 2237, 2239, 2243, 2251, 2267,
    2269, 2273, 2281, 2287, 2293, 2297, 2309, 2311, 2333, 2339, 2341, 2347, 2351, 2357, 2371, 2377,
    2381, 2383, 2389, 2393, 2399, 2411, 2417, 2423, 2437, 2441, 2447, 2459, 2467, 2473, 2477, 2503,
    2521, 2531, 2539, 2543, 2549, 2551, 2557, 2579, 2591, 2593, 2609, 2617, 2621, 2633, 2647, 2657,
    2659, 2663, 2671, 2677, 2683, 2687, 2689, 2693, 2699, 2707, 2711, 2713, 2719, 2729, 2731, 2741,
    2749, 2753, 2767, 2777, 2789, 2791, 2797, 2801, 2803, 2819, 2833, 2837, 2843, 2851, 2857, 2861,
    2879, 2887, 2897, 2903, 2909, 2917, 2927, 2939, 2953, 2957, 2963, 2969, 2971, 2999, 3001, 3011,
    3019, 3023, 3037, 3041, 3049, 3061, 3067, 3079, 3083, 3089, 3109, 3119, 3121, 3137, 3163, 3167,
    3169, 3181, 3187, 3191, 3203, 3209, 3217, 3221, 3229, 3251, 3253, 3257, 3259, 3271, 3299, 3301,
    3307, 3313, 3319, 3323, 3329, 3331, 3343, 3347, 3359, 3361, 3371, 3373, 3389, 3391, 3407, 3413,
    3433, 3449, 3457, 3461, 3463, 3467, 3469, 3491, 3499, 3511, 3517, 3527, 3529, 3533, 3539, 3541,
    3547, 3557, 3559, 3571, 3581, 3583, 3593, 3607, 3613, 3617, 3623, 3631, 3637, 3643, 3659, 3671,
    3673, 3677, 3691, 3697, 3701, 3709, 3719, 3727, 3733, 3739, 3761, 3767, 3769, 3779, 3793, 3797,
    3803, 3821, 3823, 3833, 3847, 3851, 3853, 3863, 3877, 3881, 3889, 3907, 3911, 3917, 3919, 3923,
    3929, 3931, 3943, 3947, 3967, 3989, 4001, 4003, 4007, 4013, 4019, 4021, 4027, 4049, 4051, 4057,
    4073, 4079, 4091, 4093, 4099, 4111, 4127, 4129, 4133, 4139, 4153, 4157, 4159, 4177, 4201, 4211,
    4217, 4219, 4229, 4231, 4241, 4243, 4253, 4259, 4261, 4271, 4273, 4283, 4289, 4297, 4327, 4337,
    4339, 4349, 4357, 4363, 4373, 4391, 4397, 4409, 4421, 4423, 4441, 4447, 4451, 4457, 4463, 4481,
    4483, 4493, 4507, 4513, 4517, 4519, 4523, 4547, 4549, 4561, 4567, 4583, 4591, 4597, 4603, 4621,
    4637, 4639, 4643, 4649, 4651, 4657, 4663, 4673, 4679, 4691, 4703, 4721, 4723, 4729, 4733, 4751,
    4759, 4783, 4787, 4789, 4793, 4799, 4801, 4813, 4817, 4831, 4861, 4871, 4877, 4889, 4903, 4909,
    4919, 4931, 4933, 4937, 4943, 4951, 4957, 4967, 4969, 4973, 4987, 4993, 4999, 5003, 5009, 5011,
    5021, 5023, 5039, 5051, 5059, 5077, 5081, 5087, 5099, 5101, 5107, 5113, 5119, 5147, 5153, 5167,
    5171, 5179, 5189, 5197, 5209, 5227, 5231, 5233, 5237, 5261, 5273, 5279, 5281, 5297, 5303, 5309,
    5323, 5333, 5347, 5351, 5381, 5387, 5393, 5399, 5407, 5413, 5417, 5419, 5431, 5437, 5441, 5443,
    5449, 5471, 5477, 5479, 5483, 5501, 5503, 5507, 5519, 5521, 5527, 5531, 5557, 5563, 5569, 5573,
    5581, 5591, 5623, 5639, 5641, 5647, 5651, 5653, 5657, 5659, 5669, 5683, 5689, 5693, 5701, 5711,
    5717, 5737, 5741, 5743, 5749, 5779, 5783, 5791, 5801, 5807, 5813, 5821, 5827, 5839, 5843, 5849,
};

// ------------------------------------------------------------------ helpers

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// parabolic sine approximation, valid on [-pi, pi] (~0.1% error) — the
// quadrature LFO drives a +/-2.5 ms delay, which does not need better
inline float fastSin(float x) {
    constexpr float B = 1.2732395447f;    // 4/pi
    constexpr float C = -0.4052847346f;   // -4/pi^2
    float y = B * x + C * x * std::fabs(x);
    return 0.225f * (y * std::fabs(y) - y) + y;
}

// sin(2*pi*phase) for phase in [0, 1.25); cos is the same at phase + 0.25
inline float sinTurn(float phase) {
    if (phase >= 1.f) phase -= 1.f;
    float x = phase < 0.5f ? phase : phase - 1.f;
    return fastSin(x * 2.f * (float)M_PI);
}

// xorshift32 with avalanche seeding, so nearby seeds decorrelate. Same shape
// as draen_ugens.hpp's Rng: audio-thread safe and off Rack's shared state.
struct Rng {
    uint32_t s = 0x2545F491u;
    void seed(uint32_t v) {
        v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
        s = v ? v : 0x2545F491u;
    }
    inline float uniform() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s >> 8) * (1.f / 16777216.f);
    }
    inline float bipolar() { return uniform() * 2.f - 1.f; }
};

inline size_t nextPow2(size_t n) {
    size_t p = 8;
    while (p < n) p <<= 1;
    return p;
}

// The loop saturator. Bit-exact below kSatLinear and asymptotic to kSatCeil,
// with a continuous first derivative at the knee, so it is silent until the
// feedback knob (or something patched into the loop) pushes past unity and
// then it is the only thing holding the module together.
inline float softSat(float x) {
    float a = std::fabs(x);
    if (a <= kSatLinear) return x;
    constexpr float span = kSatCeil - kSatLinear;
    float e = (a - kSatLinear) * (1.f / span);
    float y = kSatLinear + span * (e / (1.f + e));
    return x < 0.f ? -y : y;
}

// one-pole DC blocker
struct DCBlock {
    float x1 = 0.f, y1 = 0.f, r = 0.995f;
    void init(float sr) { r = 1.f - 50.f / sr; clear(); }   // ~8 Hz
    void clear() { x1 = y1 = 0.f; }
    inline float process(float x) {
        float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// smooth_init(s, default): a one-pole on a control value that starts at
// `default` instead of 0, so the delay lengths do not ramp up from nothing
// at startup. Ticked at control rate.
struct Glide {
    float y = 0.f, b1 = 0.f;
    void init(float b1_, float initial) { b1 = b1_; y = initial; }
    inline float tick(float target) {
        y = target + (y - target) * b1;
        return y;
    }
};

// -------------------------------------------------- fractional delay lines

// de.fdelay1a — first-order allpass interpolated fractional delay, the
// diffuser's delay element. The allpass fraction is kept in [0.5, 1.5) so
// its coefficient stays small and well conditioned.
struct FDelay1a {
    std::vector<float> buf;
    size_t mask = 0, wp = 0;
    float maxD = 1.f, s = 0.f;

    void init(size_t frames) {
        size_t p = nextPow2(frames);
        buf.assign(p, 0.f);
        mask = p - 1;
        maxD = (float)(p - 3);
        clear();
    }

    void clear() {
        std::fill(buf.begin(), buf.end(), 0.f);
        wp = 0;
        s = 0.f;
    }

    inline float process(float x, float d) {
        buf[wp] = x;
        wp = (wp + 1) & mask;
        d = clampf(d, 1.f, maxD);
        int i = (int)(d - 0.5f);              // d >= 1, so this is floor()
        float f = d - (float)i;               // [0.5, 1.5)
        float h = (1.f - f) / (1.f + f);
        float xi = buf[(wp - 1 - (size_t)i) & mask];
        float y = h * xi + s;
        s = xi - h * y;
        return y;
    }
};

// de.fdelay4 — 4th-order Lagrange fractional delay, used only for the two
// modulated delays in the feedback path.
struct FDelay4 {
    std::vector<float> buf;
    size_t mask = 0, wp = 0;
    float maxD = 1.f;

    void init(size_t frames) {
        size_t p = nextPow2(frames);
        buf.assign(p, 0.f);
        mask = p - 1;
        maxD = (float)(p - 4);
        clear();
    }

    void clear() {
        std::fill(buf.begin(), buf.end(), 0.f);
        wp = 0;
    }

    inline float process(float x, float d) {
        buf[wp] = x;
        wp = (wp + 1) & mask;
        d = clampf(d, 2.f, maxD);
        int i = (int)d;
        float f = d - (float)i;
        // five taps at delays i-2 .. i+2; interpolate at 2 + f
        size_t base = wp - 1 - (size_t)i;
        float t[5];
        for (int k = 0; k < 5; k++)
            t[k] = buf[(base + 2 - (size_t)k) & mask];
        float x0 = 2.f + f;
        float a = x0, b = x0 - 1.f, c = x0 - 2.f, e = x0 - 3.f, g = x0 - 4.f;
        return t[0] * (b * c * e * g * (1.f / 24.f))
             + t[1] * (a * c * e * g * (-1.f / 6.f))
             + t[2] * (a * b * e * g * 0.25f)
             + t[3] * (a * b * c * g * (-1.f / 6.f))
             + t[4] * (a * b * c * e * (1.f / 24.f));
    }
};

// de.sdelay — the long delay. In dissolve mode (the original) it is an
// integer delay that crossfades between the old and the new length when the
// length changes, so sweeping the time does not bend pitch, it dissolves from
// one time into another. In tape mode the read pointer is fractional and
// slew-limited, so sweeping the time Doppler-shifts the repeats.
//
// The buffer is not a power of two — 16 s at 48 kHz would otherwise round up
// to 21 MB per channel — so the wrap is explicit.
struct LongDelay {
    std::vector<float> buf;
    size_t n = 0, wp = 0;
    int dCur = 1, dPrev = 1;
    float fade = 1.f, fadeInc = 0.f;
    float tapePos = 2.f;
    float tapeCoef = 0.f;

    void init(size_t frames, float sr) {
        n = std::max<size_t>(frames, 16);
        buf.assign(n, 0.f);
        tapeCoef = 1.f - std::exp(-1.f / (kTapeTau * sr));
        clear();
    }

    void clear() {
        std::fill(buf.begin(), buf.end(), 0.f);
        wp = 0;
        dCur = dPrev = 1;
        fade = 1.f;
        fadeInc = 0.f;
        tapePos = 2.f;
    }

    inline size_t idx(size_t k) const { return k < n ? k : k - n; }   // k < 2n

    // value at delay k samples (k >= 0), the just-written sample being k = 0
    inline float at(int k) const {
        return buf[idx(wp + n - 1 - (size_t)k)];
    }

    inline float process(float x, float d, bool tape, float xfLen) {
        buf[wp] = x;
        if (++wp >= n) wp = 0;

        if (tape) {
            float target = clampf(d, 2.f, (float)(n - 4));
            float step = (target - tapePos) * tapeCoef;
            step = clampf(step, -kTapeMaxRate, kTapeMaxRate);
            tapePos = clampf(tapePos + step, 2.f, (float)(n - 4));
            int i = (int)tapePos;
            float f = tapePos - (float)i;
            // 4-point Hermite, in order of increasing delay
            float ym1 = at(i - 1), y0 = at(i), y1 = at(i + 1), y2 = at(i + 2);
            float c = 0.5f * (y1 - ym1);
            float v = y0 - y1;
            float w = c + v;
            float a = w + v + 0.5f * (y2 - y0);
            float b = w + a;
            return ((a * f - b) * f + c) * f + y0;
        }

        // dissolve: pick up a new length only between crossfades, so a swept
        // knob becomes a staircase of dissolves rather than a pitch bend
        int target = (int)clampf(d, 1.f, (float)(n - 2));
        if (fade >= 1.f && target != dCur) {
            dPrev = dCur;
            dCur = target;
            fade = 0.f;
            fadeInc = 1.f / std::max(xfLen, 4.f);
        }
        float a = at(dCur);
        if (fade >= 1.f) {
            // so a switch to tape mode picks up where dissolve left off
            tapePos = (float)std::max(dCur, 2);
            return a;
        }
        float b = at(dPrev);
        fade += fadeInc;
        if (fade > 1.f) fade = 1.f;
        // equal power: the two taps are effectively uncorrelated
        return b * std::sqrt(1.f - fade) + a * std::sqrt(fade);
    }
};

// --------------------------------------------------------- drift generator
// A slow bounded random walk: random targets in [-1, 1] reached over a random
// 0.5..4 s with a raised-cosine ramp. Bounded by construction, C1 smooth, and
// the delay-length glide smooths it further.

struct Drift {
    Rng rng;
    float from = 0.f, to = 0.f, phase = 1.f, inc = 0.f, cur = 0.f;

    void init(uint32_t seed) {
        rng.seed(seed);
        from = to = cur = 0.f;
        phase = 1.f;
        inc = 0.f;
    }

    // dt is the control-block period in seconds
    inline float tick(float dt) {
        phase += inc;
        if (phase >= 1.f) {
            phase = 0.f;
            from = to;
            to = rng.bipolar();
            inc = dt / (0.5f + rng.uniform() * 3.5f);
        }
        float w = 0.5f - 0.5f * std::cos((float)M_PI * phase);
        cur = from + (to - from) * w;
        return cur;
    }
};

// --------------------------------------------------------- diffuser levels

// One nesting level of diffuser_aux: an orthogonal (rotation) allpass whose
// delay element is preceded by the level below it.
//
//   u   = cos(g)*x - sin(g)*v[n-1]
//   v   = fdelay1a( rotator( inner(u) ) )
//   out = sin(g)*x + cos(g)*v[n-1]
//
// The rotation is energy preserving whatever g is, which is why the original
// gets away with stacking four of them.
struct Level {
    FDelay1a lineL, lineR;
    Glide glideL, glideR;
    float vL = 0.f, vR = 0.f;      // v[n-1]: the `~` feedback and the `mem`
    int scaleL = 0, scaleR = 0;    // prime-table index scales
    float dL = 1.f, dR = 1.f;      // current glided lengths, in samples

    void clear() {
        lineL.clear();
        lineR.clear();
        vL = vR = 0.f;
    }
};

// --------------------------------------------------------------- parameters

struct Params {
    float timeSamples = 19200.f;   // long delay length, in samples
    float damp = 0.f;              // 0..0.99, referenced to 44.1 kHz
    float size = 1.f;              // kMinSize..kMaxSize
    float diff = 0.707f;           // allpass rotation angle, radians
    float spin = kMaxSpin;         // channel rotator angle, radians
    float feedback = 0.9f;         // 0..kMaxFeedback
    float modDepth = 0.1f;         // 0..1
    float modFreq = 2.f;           // Hz
    float drift = 0.f;             // 0..1, random walk depth on size
    float mix = 0.5f;              // equal power dry/wet
    float freeze = 0.f;            // 0..1, crossfaded by the module
    bool tape = false;             // long-delay change behaviour
    bool freezeBypassDamp = true;
    bool freezeOpenInput = false;
};

// ------------------------------------------------------------------- engine

struct Engine {
    float sr = 0.f;
    float ctlDt = 0.f;                       // kCoefUpdate / sr, seconds
    uint32_t delayLen[kPrimeCount] = {};     // primes rescaled to this SR

    Level level[kStages][kNest];
    FDelay4 modL, modR;
    LongDelay longL, longR;
    DCBlock dcL, dcR;
    Drift drift;

    float dampZL = 0.f, dampZR = 0.f;
    float fbL = 0.f, fbR = 0.f;              // loop signal, one sample old
    float lfoPhase = 0.f;
    float modDepthScale = 1.f;               // (SR/44100) * 50

    // control-rate coefficient cache
    float cDiff = 1.f, sDiff = 0.f;
    float cSpin = 0.f, sSpin = 1.f;
    float diffLast = -99.f, spinLast = -99.f;
    float dampB1 = 0.f, dampA0 = 1.f, dampLast = -99.f;
    float dampExp = 1.f;                     // 44100/sr
    float mixDry = 0.707f, mixWet = 0.707f, mixLast = -1.f;
    float glideB1 = 0.f;
    float sizeEff = 1.f;
    uint32_t seed = 0;

    // ------------------------------------------------------------ structure

    // The scattering network. Seed 0 is the original's constants: stage i has
    // base scale 10 + 19i, nesting adds 13 per level, and the two channels of
    // a level sit 10 indices apart. Any other seed picks new constants within
    // a band around those, which is a different room reached through the same
    // controls — and because the lengths glide, a reseed *slides* the whole
    // network into its new shape over ~0.23 s.
    void buildScales(uint32_t s) {
        seed = s;
        int base[kStages], inc[kStages], chan[kStages];
        if (s == 0) {
            for (int i = 0; i < kStages; i++) {
                base[i] = 10 + 19 * i;
                inc[i] = 13;
                chan[i] = 10;
            }
        }
        else {
            Rng rng;
            rng.seed(s);
            for (int i = 0; i < kStages; i++) {
                float nominal = (float)(10 + 19 * i);
                base[i] = (int)std::lround(nominal * (0.6f + 0.8f * rng.uniform()));
                if (base[i] < 4) base[i] = 4;
                inc[i] = 8 + (int)(rng.uniform() * 13.f);
                chan[i] = 8 + (int)(rng.uniform() * 6.f);
            }
        }
        // keep every index distinct: two lines the same length is a comb, not
        // a diffuser
        int used[kLines];
        int nUsed = 0;
        for (int i = 0; i < kStages; i++) {
            for (int j = 0; j < kNest; j++) {
                for (int c = 0; c < 2; c++) {
                    int v = base[i] + inc[i] * j + chan[i] * c;
                    bool clash = true;
                    while (clash) {
                        clash = false;
                        for (int k = 0; k < nUsed; k++)
                            if (used[k] == v) { clash = true; v++; break; }
                    }
                    int cap = maxScale(i, j, c);
                    if (v > cap) v = cap;
                    used[nUsed++] = v;
                    if (c == 0) level[i][j].scaleL = v;
                    else level[i][j].scaleR = v;
                }
            }
        }
    }

    // The largest scale a line can ever be given, whatever the seed, so its
    // buffer is allocated once and a reseed never has to allocate. The kLines
    // slack covers the collision bumps above, which are at most one per line.
    static int maxScale(int stage, int nestLevel, int chan) {
        int maxBase = (int)std::ceil((10 + 19 * stage) * 1.4f);
        return maxBase + 20 * nestLevel + 13 * chan + kLines;
    }

    int lengthAt(int scale, float size) const {
        int i = (int)(size * (float)scale);
        if (i < 0) i = 0;
        if (i >= kPrimeCount) i = kPrimeCount - 1;
        return (int)delayLen[i];
    }

    // ----------------------------------------------------------------- init

    void init(float sampleRate) {
        sr = sampleRate;
        ctlDt = (float)kCoefUpdate / sr;
        dampExp = 44100.f / sr;
        modDepthScale = (sr / 44100.f) * kModDepthSamples;
        glideB1 = std::exp(-ctlDt / kGlideTau);

        // primes rescaled to this sample rate and rounded: what matters
        // acoustically is that the 24 lengths stay mutually incommensurate,
        // and a uniform rescale preserves their ratios exactly. Snapping back
        // to primes would not — prime 31 at 48 kHz would stay 31.
        for (int i = 0; i < kPrimeCount; i++) {
            uint32_t v = (uint32_t)std::lround((float)kPrimes[i] * sr / 44100.f);
            delayLen[i] = v < 2 ? 2 : v;
        }

        buildScales(seed);
        for (int i = 0; i < kStages; i++) {
            for (int j = 0; j < kNest; j++) {
                Level& L = level[i][j];
                size_t nL = (size_t)lengthAt(maxScale(i, j, 0), kSizeCeil) + 8;
                size_t nR = (size_t)lengthAt(maxScale(i, j, 1), kSizeCeil) + 8;
                L.lineL.init(nL);
                L.lineR.init(nR);
                L.dL = (float)std::max(lengthAt(L.scaleL, 1.f) - 1, 1);
                L.dR = (float)std::max(lengthAt(L.scaleR, 1.f) - 1, 1);
                L.glideL.init(glideB1, L.dL);
                L.glideR.init(glideB1, L.dR);
            }
        }

        size_t modFrames = (size_t)(kModBase + 2.f * modDepthScale) + 16;
        modL.init(modFrames);
        modR.init(modFrames);

        size_t frames = (size_t)(kMaxTimeSec * sr) + 64;
        if (frames > kMaxLongFrames) frames = kMaxLongFrames;
        longL.init(frames, sr);
        longR.init(frames, sr);

        dcL.init(sr);
        dcR.init(sr);
        drift.init(0x5bf03635u);
        clear();
    }

    // the longest delay time this sample rate can hold, in seconds — the
    // module reports it in the knob tooltip so a user at 768 kHz is never
    // silently clamped
    float maxTimeSec() const {
        return sr > 0.f ? (float)(longL.n - 8) / sr : kMaxTimeSec;
    }

    void clear() {
        for (int i = 0; i < kStages; i++)
            for (int j = 0; j < kNest; j++)
                level[i][j].clear();
        modL.clear();
        modR.clear();
        longL.clear();
        longR.clear();
        dcL.clear();
        dcR.clear();
        dampZL = dampZR = 0.f;
        fbL = fbR = 0.f;
        lfoPhase = 0.f;
        mixLast = -1.f;
    }

    // Reseed the scattering network without touching the buffers or the
    // glides, so the lengths slide to their new values.
    void reseed(uint32_t s) {
        buildScales(s);
    }

    // ----------------------------------------------------------- control rate

    void updateControl(const Params& p) {
        // trig for the two rotations. Both are static most of the time, so
        // gate the recompute rather than building a table for it. The three
        // stages differ only in the sign of g, and cos is even, so one pair
        // covers all three.
        float diff = clampf(p.diff, 0.f, kMaxDiff);
        if (std::fabs(diff - diffLast) > 1e-5f) {
            diffLast = diff;
            cDiff = std::cos(diff);
            sDiff = std::sin(diff);
        }
        float spin = clampf(p.spin, 0.f, kMaxSpin);
        if (std::fabs(spin - spinLast) > 1e-5f) {
            spinLast = spin;
            cSpin = std::cos(spin);
            sSpin = std::sin(spin);
        }

        // si.smooth(damp), with the raw coefficient referred back to 44.1 kHz
        // so the damping *frequency* does not follow the sample rate
        float damp = clampf(p.damp, 0.f, 0.99f);
        if (std::fabs(damp - dampLast) > 1e-5f) {
            dampLast = damp;
            dampB1 = damp > 0.f ? std::pow(damp, dampExp) : 0.f;
            dampA0 = 1.f - dampB1;
        }

        // size, walked by drift, then the prime-table lookup per line
        float d = p.drift > 0.f ? drift.tick(ctlDt) : 0.f;
        sizeEff = clampf(p.size * (1.f + 0.5f * p.drift * d), kMinSize, kSizeCeil);
        for (int i = 0; i < kStages; i++) {
            for (int j = 0; j < kNest; j++) {
                Level& L = level[i][j];
                // the original subtracts 1: the explicit one-sample delays in
                // the forward and feedback paths put it back
                float tL = (float)std::max(lengthAt(L.scaleL, sizeEff) - 1, 1);
                float tR = (float)std::max(lengthAt(L.scaleR, sizeEff) - 1, 1);
                L.dL = L.glideL.tick(tL);
                L.dR = L.glideR.tick(tR);
            }
        }
    }

    // ------------------------------------------------------------- per sample

    inline void diffuse(float& xL, float& xR, int stage, int nestLevel,
                        float cd, float sd) {
        Level& L = level[stage][nestLevel];
        float uL = cd * xL - sd * L.vL;
        float uR = cd * xR - sd * L.vR;
        float oL = sd * xL + cd * L.vL;
        float oR = sd * xR + cd * L.vR;
        if (nestLevel + 1 < kNest)
            diffuse(uL, uR, stage, nestLevel + 1, cd, sd);
        float rL = uL * cSpin - uR * sSpin;
        float rR = uL * sSpin + uR * cSpin;
        L.vL = L.lineL.process(rL, L.dL);
        L.vR = L.lineR.process(rR, L.dR);
        xL = oL;
        xR = oR;
    }

    // rtnL/rtnR close the feedback loop: the module hands back either what it
    // read from the return jacks or, unpatched, the send it was given last
    // sample. Either way the loop carries exactly the one sample of latency
    // Faust's `~` does.
    void process(float inL, float inR, float rtnL, float rtnR, const Params& p,
                 float& outL, float& outR, float& sndL, float& sndR) {
        float fz = clampf(p.freeze, 0.f, 1.f);

        // freeze: unity feedback, the input shut out of the loop, damping
        // bypassed, and (the module's job) the delay time held
        float fb = clampf(p.feedback, 0.f, kMaxFeedback);
        fb += (1.f - fb) * fz;
        float inGain = p.freezeOpenInput ? 1.f : 1.f - fz;

        float gL = dcL.process(softSat(rtnL * fb));
        float gR = dcR.process(softSat(rtnR * fb));

        float xL = gL + inL * inGain;
        float xR = gR + inR * inGain;

        for (int i = 0; i < kStages; i++) {
            float sd = (i & 1) ? -sDiff : sDiff;
            diffuse(xL, xR, i, 0, cDiff, sd);
        }

        dampZL = xL * dampA0 + dampZL * dampB1;
        dampZR = xR * dampA0 + dampZR * dampB1;
        float wetL = xL, wetR = xR;
        if (dampB1 > 0.f) {
            float bypass = p.freezeBypassDamp ? fz : 0.f;
            wetL = dampZL + (xL - dampZL) * bypass;
            wetR = dampZR + (xR - dampZR) * bypass;
        }

        // the quadrature-modulated short delay, inside the feedback path only
        float depth = clampf(p.modDepth, 0.f, 1.f) * modDepthScale;
        float cosv = sinTurn(lfoPhase + 0.25f);
        float sinv = sinTurn(lfoPhase);
        lfoPhase += clampf(p.modFreq, 0.f, 20.f) / sr;
        if (lfoPhase >= 1.f) lfoPhase -= 1.f;

        float mL = modL.process(wetL, kModBase + depth + depth * cosv);
        float mR = modR.process(wetR, kModBase + depth + depth * sinv);

        float dt = clampf(p.timeSamples, kMinTimeSec * sr, (float)(longL.n - 8));
        float xf = clampf(kXfadeFrac * dt, kXfadeMinSec * sr, kXfadeMaxSec * sr);
        sndL = longL.process(mL, dt, p.tape, xf);
        sndR = longR.process(mR, dt, p.tape, xf);

        if (p.mix != mixLast) {
            mixLast = p.mix;
            float f = clampf(p.mix, 0.f, 1.f) * (float)M_PI_2;
            mixDry = std::cos(f);
            mixWet = std::sin(f);
        }
        outL = inL * mixDry + wetL * mixWet;
        outR = inR * mixDry + wetR * mixWet;
    }
};

}   // namespace caligo_dsp
