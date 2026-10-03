// spira.hpp - the engine of the spira module, free of Rack.
//
// A line and the circles that grow off it. The line is the input as it
// passes, written into a long buffer; HOLD freezes the buffer and the line
// becomes a loop of its last LINE seconds. A circle is born at a point of the
// line: a window of SIZE seconds just behind the playhead (or up to REACH
// further back) that loops on itself while the line goes on.
//
// Every turn of a circle is one lap, and every lap is a grain: it fades in
// over the seam while the lap before it fades out, both reading the buffer,
// so a circle is a stream of grains that happen to be the same place. Between
// laps the circle may change, and that turns the circle into a spiral:
//
//   SPIRAL  the next lap lasts r times as long as this one
//   TAPE    how: by playing the same window faster (the pitch follows, as on
//           tape) or by cutting the window at the same speed
//   FADE    the level, in dB per first-lap length, so a lap half as long
//           fades half as much and a converging spiral still has a level
//           when it arrives
//   TONE    a filter that closes (or a high-pass that opens) turn by turn,
//           on the same time scale
//
// With r < 1 the spiral converges: T, rT, r^2 T... sum to T / (1 - r), so it
// ends at a known time, and on the way its laps shrink into the audio range
// and the repetition becomes a pitch. With r > 1 it unwinds, slower and
// lower, until the longest lap.
//
// Everything is in samples of the host rate. doc/design/spira.md is the
// specification; test/spira_probe measures what is claimed here.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace spira {

// ---------------------------------------------------------------- constants

const float kBufferSeconds = 48.f;   // the line's memory: LINE plus room to reach
const float kLineMin = 1.f;          // LINE, seconds
const float kLineMax = 30.f;
const float kSizeMin = 0.01f;        // SIZE, the first lap, seconds
const float kSizeMax = 4.f;
const float kRateOff = 0.02f;        // RATE below this knob position: off
const float kRateMin = 0.05f;        // RATE, circles per second
const float kRateMax = 20.f;
const float kLapMin = 0.001f;        // a converging circle ends at this lap, seconds
const float kLapMax = 16.f;          // an unwinding one stops growing here
const float kSpeedMin = 0.0625f;     // playback speed limits, 4 octaves down
const float kSpeedMax = 4.f;         // and 2 up; past them TAPE cuts instead
const float kSeamMin = 0.001f;       // the shortest crossfade between laps, seconds
const float kKillTime = 0.005f;      // a stolen circle fades out over this
const float kHoldSeam = 0.01f;       // crossfades in and out of HOLD, and the held loop's seam
const float kGainSmooth = 0.005f;    // s, the time constant on MIX and LEVEL
const float kGainMax = 4.f;          // +12 dB: a swelling circle stops here
const float kGainFloor = 1e-4f;      // -80 dB: a fading one ends here
const float kShapeDepth = 8.f;       // SHAPE at full: exp(-8), -70 dB across a lap
const float kShapeMakeup = 0.75f;    // the share of the envelope's average loss given back, in dB: all of it pushes plucks into the limiter
const float kToneLpStart = 20000.f;  // the lap filters' first cutoffs, Hz
const float kToneHpStart = 20.f;
const float kToneLpFloor = 80.f;
const float kToneHpCeiling = 8000.f;
const float kKnee = 6.f;             // SATURATE: the output passes untouched up to here, volts
const float kCeiling = 10.f;         // and bends toward this
const float kLimit = 8.f;            // LIMIT: peaks are turned down to this, volts
const float kLimitHold = 0.02f;      // s, the limiter keeps a peak this long
const float kLimitRelease = 0.15f;   // s, then lets go of it this slowly
const float kPi = 3.14159265358979f;

const int kCircles = 8;              // sounding at once
const int kSlots = 12;               // plus room for stolen ones to fade

enum Direction { DIR_FORWARD, DIR_PINGPONG, DIR_REVERSE, DIR_LEN };

// ---------------------------------------------------------------- knob laws
//
// Knobs are 0..1 (SPIRAL -1..1). Each law has its inverse, so a typed value
// finds its knob position; spira_probe checks that they round-trip.

inline float sizeSeconds(float k) { return kSizeMin * std::pow(kSizeMax / kSizeMin, k); }
inline float sizeKnob(float s) {
    s = std::max(kSizeMin, std::min(kSizeMax, s));
    return std::log(s / kSizeMin) / std::log(kSizeMax / kSizeMin);
}

inline float lineSeconds(float k) { return kLineMin * std::pow(kLineMax / kLineMin, k); }
inline float lineKnob(float s) {
    s = std::max(kLineMin, std::min(kLineMax, s));
    return std::log(s / kLineMin) / std::log(kLineMax / kLineMin);
}

// The next lap's length over this one's. Square law around the centre, so
// the slow spirals near 1 get most of the travel: 0.2 is x1.03, 0.5 x1.19,
// full x2 (or x0.5).
inline float spiralRatio(float k) {
    k = std::max(-1.f, std::min(1.f, k));
    return std::exp2(k * std::fabs(k));
}
inline float spiralKnob(float r) {
    if (!(r > 0.f)) return -1.f;
    float e = std::max(-1.f, std::min(1.f, std::log2(r)));
    return e < 0.f ? -std::sqrt(-e) : std::sqrt(e);
}

// Circles per second, 0 at the bottom of the knob.
inline float rateHz(float k) {
    if (k < kRateOff) return 0.f;
    float u = (k - kRateOff) / (1.f - kRateOff);
    return kRateMin * std::pow(kRateMax / kRateMin, u);
}
inline float rateKnob(float hz) {
    if (hz < kRateMin) return 0.f;
    hz = std::min(kRateMax, hz);
    return kRateOff + (1.f - kRateOff) * std::log(hz / kRateMin) / std::log(kRateMax / kRateMin);
}

// FADE, dB per first-lap length. The centre is 0 dB, a circle that loops
// unchanged; left fades down to -60 dB, one lap and gone; right swells up to
// +6 dB a turn. Square law either side, so the fine values near the centre
// get most of the travel: -0.16 is -1.5 dB, -0.3 -5.4, -0.5 -15.
const float kFadeMin = -60.f;
const float kFadeMax = 6.f;
inline float fadeDb(float k) {
    k = std::max(-1.f, std::min(1.f, k));
    return k < 0.f ? kFadeMin * k * k : kFadeMax * k * k;
}
inline float fadeKnob(float db) {
    db = std::max(kFadeMin, std::min(kFadeMax, db));
    return db < 0.f ? -std::sqrt(db / kFadeMin) : std::sqrt(db / kFadeMax);
}

inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }

// Linear to the knee, then a tanh toward the ceiling with a matching slope.
// SATURATE is this alone, from 6 V: a +-5 V line passes untouched, and a pile
// of loud circles bends, the harder the more it is pushed. Under LIMIT it is
// only a net from 8 V, for a NaN-free guarantee the gain already gives.
inline float softLimit(float x, float knee = kKnee) {
    float a = std::fabs(x);
    if (a <= knee) return x;
    float room = kCeiling - knee;
    float y = knee + room * std::tanh((a - knee) / room);
    return x < 0.f ? -y : y;
}

// LIMIT: one gain on both sides, from the louder one's peak. The peak is
// caught at once, held 20 ms and let go over 150 ms, and the gain brings it
// to 8 V: a pile of circles gets quieter, not distorted. A slower catch lets
// a drum's attack through to the net, which is the distortion this is here
// to avoid; the hold keeps the gain still between the peaks of a low tone
// (h3 of a 55 Hz sine at 12 V: -43 dB without it).
// Below 8 V the gain is 1 and the output is untouched.
struct Limiter {
    float env = 0.f;
    int held = 0;
    int hold = 960;
    float rel = 1.f;
    Limiter() { setSampleRate(48000.f); }
    void setSampleRate(float sr) {
        hold = (int)(kLimitHold * sr);
        rel = 1.f - std::exp(-1.f / (kLimitRelease * sr));
    }
    void process(float& l, float& r) {
        float peak = std::max(std::fabs(l), std::fabs(r));
        if (peak >= env) {
            env = peak;
            held = hold;
        } else if (held > 0) {
            held--;
        } else {
            env += rel * (peak - env);
        }
        float g = env > kLimit ? kLimit / env : 1.f;
        l = softLimit(l * g, kLimit);
        r = softLimit(r * g, kLimit);
    }
};

// ---------------------------------------------------------------- the parts

// xorshift32, one per engine, seeded by the module.
struct Rng {
    uint32_t s = 0x9e3779b9u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uniform() { return (next() >> 8) * (1.f / 16777216.f); }   // [0, 1)
};

// Two zeroed channels of `n` samples, freed when it goes. calloc, not a
// vector, so pages are committed only as the write head reaches them.
struct Buffers {
    float* l = nullptr;
    float* r = nullptr;
    size_t n = 0;
    Buffers() {}
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;
    ~Buffers() { release(); }
    bool allocate(size_t samples) {
        release();
        l = (float*)std::calloc(samples, sizeof(float));
        r = (float*)std::calloc(samples, sizeof(float));
        if (!l || !r) { release(); return false; }
        n = samples;
        return true;
    }
    void release() {
        std::free(l);
        std::free(r);
        l = r = nullptr;
        n = 0;
    }
    void swap(Buffers& o) {
        std::swap(l, o.l);
        std::swap(r, o.r);
        std::swap(n, o.n);
    }
};

// Everything the panel sets, in the engine's units.
struct Controls {
    float size = 0.25f;        // s, the first lap
    float spiral = 1.f;        // ratio, next lap over this one
    float tape = 1.f;          // 0 cut the window .. 1 change the speed
    float pitch = 0.f;         // semitones, the circle's speed at birth
    float fadeDb = -1.5f;      // dB per first-lap length
    float shape = 0.f;         // -1 each lap decays .. 0 flat .. 1 each lap swells
    float soft = 0.25f;        // 0 a 1 ms seam .. 1 laps that are all crossfade
    float tone = 0.f;          // octaves per first-lap length: < 0 darker, > 0 thinner
    float jitter = 0.f;        // 0..1, each lap's place and length, and each birth
    float rate = 0.f;          // circles per second, 0 = only on BIRTH
    float reach = 0.f;         // 0 just behind the playhead .. 1 anywhere on the line
    float anchor = 0.f;        // which end of the window stays when TAPE cuts
    float line = 8.f;          // s, how far back REACH goes; the loop under HOLD
    float mix = 0.5f;          // 0 line .. 0.5 both at unity .. 1 circles
    float level = 0.f;         // dB on the circles, before MIX
    float skips = 0.f;         // 0..1, the share of RATE and BIRTH-jack births skipped
    float spread = 0.5f;       // 0 centred .. 1 hard left or right
    int direction = DIR_FORWARD;
    bool hold = false;
    bool keepBirth = false;    // circles keep the settings they were born with
    bool saturate = false;     // the output bends (SATURATE) rather than LIMIT
};

struct Events {
    bool birth = false;        // a trigger at the jack: SKIPS may skip it
    bool press = false;        // the button: never skipped
};

struct Output {
    float l = 0.f, r = 0.f;
    bool turn = false;         // the newest circle began a lap (or was born)
    bool born = false;         // a circle was born
    float speedOct = 0.f;      // the newest circle's speed, octaves (V/oct)
    int circles = 0;           // sounding now
    // The ring of lights, one place per circle, taken in turn at birth: the
    // level each circle is at (its lap envelope, fade and end, not
    // measured) and its spiral, log2 of the ratio, -1 inward .. 1 outward.
    float ring[kCircles] = {};
    float ringSpiral[kCircles] = {};
    // The same places for the poly outputs: a lap (or the birth) began, and
    // the speed in octaves. A place with no circle keeps no speed here; the
    // module holds the last one.
    bool ringTurn[kCircles] = {};
    bool ringOn[kCircles] = {};
    float ringSpeed[kCircles] = {};
};

// The crossfade between laps, 0..1 in. Equal power, since the two laps read
// different places, with a smoothstep inside so the gain has no slope at
// either end: sin and cos alone end on a kink, and a kink under a loud
// signal is a click.
inline float seamIn(float t) {
    t = std::max(0.f, std::min(1.f, t));
    return std::sin(0.5f * kPi * t * t * (3.f - 2.f * t));
}

// One turn of a circle: a grain reading the window [a, a + len) of the
// buffer at `v` samples per sample, forwards or backwards, for `T` samples,
// with `F` samples of crossfade either side.
struct Lap {
    bool on = false;
    double a = 0.;             // window start, absolute sample index
    double len = 0.;           // window length, source samples
    double v = 1.;             // speed, source samples per output sample
    double T = 0.;             // lap length, output samples
    double x = 0.;             // output samples since the lap began
    double F = 0.;             // seam crossfade, output samples: the longest either end
    double Fin = 0., Fout = 0.;  // the fade in and out, shorter at a pluck's attack and a swell's end
    int dir = 1;
    float gain = 1.f;          // the lap's level
    float endFade = 1.f;       // a converging circle fades over its last laps
    float shape = 0.f;
    float makeup = 1.f;        // the level SHAPE's envelope takes away on average, given back
    float pan = 0.f;           // -1..1, a balance: the circle reads the stereo line

    // Where in the buffer the lap is now: past its end while it fades out.
    double position() const { return dir > 0 ? a + v * x : a + len - v * x; }

    float envelope() const {
        float e = 1.f;
        if (x < Fin) e = seamIn((float)(x / Fin));
        else if (x > T) e = seamIn(1.f - (float)std::min(1., (x - T) / Fout));
        if (shape != 0.f) {
            float u = (float)std::min(1., x / T);
            e *= shape < 0.f ? std::exp(shape * kShapeDepth * u)
                             : std::exp(-shape * kShapeDepth * (1.f - u));
        }
        return e * gain * endFade * makeup;
    }

    // SHAPE and the seam together. A decaying lap is a pluck, so its fade in
    // shortens toward the 1 ms floor as SHAPE goes left, or SOFT would eat
    // the attack; a swelling lap's fade out shortens the same way. The
    // envelope's average loss, the RMS of exp(-a u) over the lap, is given
    // back, so SHAPE changes what the circles sound like and not how loud
    // they are: on a drum loop the hits fall anywhere in the window, and a
    // pluck keeps only those near its start.
    void setShape(float s, double seamF, double floorF) {
        shape = s;
        F = seamF;
        double in = s < 0.f ? (1. + s) * (1. + s) : 1.;
        double out = s > 0.f ? (1. - s) * (1. - s) : 1.;
        Fin = std::min(seamF, std::max(floorF, seamF * in));
        Fout = std::min(seamF, std::max(floorF, seamF * out));
        float a = std::fabs(s) * kShapeDepth;
        float rms = a > 1e-4f ? std::sqrt((1.f - std::exp(-2.f * a)) / (2.f * a)) : 1.f;
        makeup = std::pow(rms, -kShapeMakeup);
    }
};

// A one-pole low-pass and a one-pole high-pass per channel, stepped at each
// lap by TONE.
struct Tone {
    float lp[2] = {0.f, 0.f}, hp[2] = {0.f, 0.f};
    float lpHz = kToneLpStart, hpHz = kToneHpStart;
    float lpA = 1.f, hpA = 0.f;          // where the coefficients are going
    float lpNow = 1.f, hpNow = 0.f;      // and where they are, glided
    float glide = 0.f;
    void set(float sr) {
        float nyq = 0.49f * sr;
        lpA = 1.f - std::exp(-2.f * kPi * std::min(lpHz, nyq) / sr);
        hpA = 1.f - std::exp(-2.f * kPi * std::min(hpHz, nyq) / sr);
        glide = 1.f - std::exp(-1.f / (0.003f * sr));
    }
    void start() {
        lpNow = lpA;
        hpNow = hpA;
    }
    // Once per sample for the circle, before its two channels.
    void tick() {
        lpNow += glide * (lpA - lpNow);
        hpNow += glide * (hpA - hpNow);
    }
    float process(int c, float s) {
        lp[c] += lpNow * (s - lp[c]);
        hp[c] += hpNow * (lp[c] - hp[c]);
        return lp[c] - hp[c];
    }
};

struct Circle {
    bool on = false;
    bool dying = false;        // stolen: fading out over kKillTime
    float kill = 1.f;
    uint32_t born = 0;         // birth order, for stealing the oldest
    Lap cur, tail;             // this lap, and the last one's crossfade
    double T0 = 1.;            // the first lap, output samples
    float pan = 0.f;
    Tone tone;
    Controls birth;            // the settings it was born with
    int ring = 0;              // its place on the ring of lights
};

// ---------------------------------------------------------------- the engine

struct Engine {
    float sr = 48000.f;
    float* buf[2] = {nullptr, nullptr};
    size_t n = 0;
    int64_t w = 0;             // samples written: the line's newest is w - 1

    // HOLD: the buffer stops, and the line loops [holdEnd - holdLen, holdEnd)
    bool held = false;
    int64_t holdEnd = 0;
    double linePos = 0.;       // the held line's playhead, absolute
    float holdMix = 0.f;       // 0 the live input .. 1 the held loop

    // MIX and LEVEL as gains, smoothed: a CV step on either jumped the
    // balance in one sample, a click whenever line and circles differ.
    // Negative until the first sample, which they take as they are.
    float dryGain = -1.f, wetGain = -1.f;
    float gainCoef = 0.00416f; // 48 kHz, until setSampleRate

    Circle circle[kSlots];
    uint32_t births = 0;
    int lead = -1;             // the newest circle still sounding
    double timer = 0.;         // samples to the next RATE birth
    bool rateWasOn = false;
    Rng rng;
    Limiter limiter;

    static size_t bufferSamples(float rate) { return (size_t)(kBufferSeconds * rate) + 8; }

    void setSampleRate(float rate) {
        sr = rate;
        limiter.setSampleRate(rate);
        gainCoef = 1.f - std::exp(-1.f / (kGainSmooth * rate));
    }

    // Hands the engine its buffers (zeroed, `size` samples each), or none:
    // without them it passes the line only.
    void attach(float* left, float* right, size_t size) {
        buf[0] = left;
        buf[1] = right;
        n = left && right ? size : 0;
        w = 0;
        held = false;
        holdMix = 0.f;
        dryGain = wetGain = -1.f;
        for (Circle& c : circle) c = Circle();
        lead = -1;
    }

    // ------------------------------------------------------------ buffer

    // Hermite interpolation at an absolute position the caller has checked.
    float read(int c, double p) const {
        int64_t i = (int64_t)std::floor(p);
        float f = (float)(p - (double)i);
        const float* b = buf[c];
        int64_t k = (i - 1) % (int64_t)n;
        if (k < 0) k += (int64_t)n;
        float y[4];
        for (int j = 0; j < 4; j++) {
            y[j] = b[k];
            if (++k == (int64_t)n) k = 0;
        }
        float c1 = 0.5f * (y[2] - y[0]);
        float c2 = y[0] - 2.5f * y[1] + 2.f * y[2] - 0.5f * y[3];
        float c3 = 0.5f * (y[3] - y[0]) + 1.5f * (y[1] - y[2]);
        return ((c3 * f + c2) * f + c1) * f + y[1];
    }

    // The span a lap may read, for `ahead` output samples from now: not yet
    // overwritten when it gets there, and already written.
    double oldest(double ahead) const {
        double lost = held ? 0. : ahead;
        return (double)(w - (int64_t)n) + lost + 4.;
    }
    double newest() const { return (double)w - 4.; }

    // Where a window of `len` source samples may lie, with `margin` source
    // samples of seam either side, for a lap of T + F output samples. Moves
    // it inside if it can; false when the line no longer holds it.
    bool place(double& a, double len, double margin, double life) const {
        double lo = oldest(life) + margin;
        double hi = newest() - margin - len;
        if (hi < lo) return false;
        if (a > hi) a = hi;
        if (a < lo) return false;
        return true;
    }

    // ------------------------------------------------------------ circles

    double seam(double T, float soft) const {
        double F = std::max((double)kSeamMin * sr, 0.5 * soft * T);
        return std::min(F, 0.5 * T);
    }

    const Controls& settings(const Circle& c, const Controls& live) const {
        return live.keepBirth ? c.birth : live;
    }

    // The lap after `p`, or one that is off: the circle has converged, faded
    // out, or lost its place on the line.
    Lap nextLap(Circle& c, const Lap& p, const Controls& live) {
        const Controls& k = settings(c, live);
        Lap q;
        double T = p.T * k.spiral;
        if (k.jitter > 0.f) T *= std::exp2(k.jitter * 0.5f * (rng.uniform() - 0.5f));
        double lapMin = kLapMin * sr, lapMax = kLapMax * sr;
        if (T < lapMin) return q;
        T = std::min(T, lapMax);
        double v = p.v * std::pow((double)k.spiral, -(double)k.tape);
        v = std::max((double)kSpeedMin, std::min((double)kSpeedMax, v));
        double len = T * v;
        double a = p.a + k.anchor * (p.len - len);
        if (k.jitter > 0.f) a += k.jitter * (rng.uniform() - 0.5f) * len;
        double F = seam(T, k.soft);
        if (!place(a, len, F * v + 4., T + F)) return q;

        // levels and tone move by the lap's share of the first lap
        double share = p.T / c.T0;
        float gain = p.gain * (float)std::pow((double)dbToGain(k.fadeDb), share);
        if (gain < kGainFloor) return q;
        q.gain = std::min(gain, kGainMax);
        if (k.tone < 0.f)
            c.tone.lpHz = std::max(kToneLpFloor, c.tone.lpHz * (float)std::exp2(k.tone * share));
        else if (k.tone > 0.f)
            c.tone.hpHz = std::min(kToneHpCeiling, c.tone.hpHz * (float)std::exp2(k.tone * share));
        c.tone.set(sr);

        q.on = true;
        q.a = a;
        q.len = len;
        q.v = v;
        q.T = T;
        q.setShape(k.shape, F, std::min(F, (double)kSeamMin * sr));
        q.x = p.x - p.T;
        q.endFade = (float)std::max(0., std::min(1., (T / lapMin - 1.) / 3.));
        if (k.direction == DIR_PINGPONG) {
            q.dir = -p.dir;
            q.pan = -p.pan;
        } else {
            q.dir = k.direction == DIR_REVERSE ? -1 : 1;
            q.pan = c.pan;
        }
        return q;
    }

    // A new circle at the playhead, SIZE long, up to REACH x LINE further
    // back. The oldest gives way when kCircles are sounding.
    bool birth(const Controls& k) {
        if (!n) return false;
        int live = 0, oldest = -1, free = -1;
        for (int i = 0; i < kSlots; i++) {
            Circle& c = circle[i];
            if (!c.on) {
                if (free < 0) free = i;
                continue;
            }
            if (c.dying) continue;
            live++;
            if (oldest < 0 || c.born < circle[oldest].born) oldest = i;
        }
        if (live >= kCircles && oldest >= 0) circle[oldest].dying = true;
        if (free < 0) {
            // every slot busy: take the quietest of the dying ones
            for (int i = 0; i < kSlots; i++)
                if (circle[i].dying && (free < 0 || circle[i].kill < circle[free].kill)) free = i;
            if (free < 0) return false;
        }

        Circle& c = circle[free];
        c = Circle();
        c.birth = k;
        double v = std::max((double)kSpeedMin,
                            std::min((double)kSpeedMax, (double)std::exp2(k.pitch / 12.f)));
        double T = (double)k.size * sr;
        double len = T * v;
        double F = seam(T, k.soft);
        double margin = F * v + 4.;
        double back = (double)rng.uniform() * k.reach * k.line * sr;
        double now = held ? linePos : (double)w;
        double a = now - margin - back - len;
        if (held) {
            double start = (double)holdEnd - holdLength(k);
            if (a < start) a += holdLength(k);
        }
        if (!place(a, len, margin, T + F)) {
            // not enough line yet: the most recent window that fits
            a = newest() - margin - len;
            if (!place(a, len, margin, T + F)) return false;
        }

        c.on = true;
        c.born = ++births;
        c.ring = (int)((births - 1) % kCircles);
        c.T0 = T;
        c.pan = k.spread * (2.f * rng.uniform() - 1.f);
        c.tone.set(sr);
        c.tone.start();
        Lap& l = c.cur;
        l.on = true;
        l.a = a;
        l.len = len;
        l.v = v;
        l.T = T;
        l.setShape(k.shape, F, std::min(F, (double)kSeamMin * sr));
        l.x = 0.;
        l.dir = k.direction == DIR_REVERSE ? -1 : 1;
        l.pan = c.pan;
        l.endFade = 1.f;
        lead = free;
        return true;
    }

    // SKIPS: this birth is skipped, as rubigo's SKIPS skips a step. The
    // timing stays; the grid thins.
    bool skipped(const Controls& k) { return k.skips > 0.f && rng.uniform() < k.skips; }

    // ------------------------------------------------------------ the line

    // LINE, or less: what the buffer holds, and what has been written.
    double holdLength(const Controls& k) const {
        double len = (double)k.line * sr;
        double written = (double)(held ? holdEnd : w) - (double)kHoldSeam * sr - 16.;
        len = std::min(len, (double)n - (double)kHoldSeam * sr - 16.);
        len = std::min(len, written);
        return std::max(1., len);
    }

    // The held loop at its playhead, crossfaded over its seam.
    void heldLine(const Controls& k, float out[2]) {
        double len = holdLength(k);
        double start = (double)holdEnd - len;
        if (linePos < start || linePos >= (double)holdEnd)
            linePos = start + std::fmod(std::max(0., linePos - start), len);
        double S = std::min((double)kHoldSeam * sr, 0.5 * len);
        double toEnd = (double)holdEnd - linePos;
        for (int c = 0; c < 2; c++) out[c] = read(c, linePos - 2.);
        if (toEnd < S) {
            // the loop's end fades out as the material before its start fades in
            float u = (float)(1. - toEnd / S);
            float gOut = seamIn(1.f - u), gIn = seamIn(u);
            for (int c = 0; c < 2; c++)
                out[c] = gOut * out[c] + gIn * read(c, linePos - len - 2.);
        }
        linePos += 1.;
        if (linePos >= (double)holdEnd) linePos -= len;
    }

    // ------------------------------------------------------------ process

    Output process(const Controls& k, const Events& ev, float inL, float inR) {
        Output o;
        float in[2] = {inL, inR};

        // HOLD: freeze the line at the playhead
        if (n) {
            if (k.hold && !held) {
                held = true;
                // Back on while the last loop is still fading out: pick that
                // loop up where it is. A new one starting at another place
                // under the fade is a step.
                if (holdMix == 0.f) {
                    holdEnd = w;
                    linePos = (double)holdEnd - holdLength(k);
                }
            } else if (!k.hold && held) {
                held = false;
            }
        }
        float step = 1.f / (kHoldSeam * sr);
        holdMix = held ? std::min(1.f, holdMix + step) : std::max(0.f, holdMix - step);

        float line[2] = {in[0], in[1]};
        if (n && holdMix > 0.f) {
            float h[2];
            heldLine(k, h);
            float m = holdMix * holdMix * (3.f - 2.f * holdMix);
            for (int c = 0; c < 2; c++) line[c] = in[c] + m * (h[c] - in[c]);
        }
        if (n && !held) {
            size_t i = (size_t)(w % (int64_t)n);
            buf[0][i] = in[0];
            buf[1][i] = in[1];
            w++;
        }

        // births: BIRTH, and RATE's own clock
        if (ev.press && birth(k)) o.turn = o.born = true;
        else if (ev.birth && !skipped(k) && birth(k)) o.turn = o.born = true;
        if (o.born) o.ringTurn[circle[lead].ring] = true;
        if (k.rate > 0.f) {
            double period = sr / k.rate;
            if (!rateWasOn) timer = 0.;
            timer = std::min(timer, 2. * period);
            timer -= 1.;
            if (timer <= 0.) {
                if (!skipped(k) && birth(k)) {
                    o.turn = o.born = true;
                    o.ringTurn[circle[lead].ring] = true;
                }
                double j = k.jitter > 0.f ? std::exp2(k.jitter * 2.f * (rng.uniform() - 0.5f)) : 1.f;
                timer += period * j;
            }
        }
        rateWasOn = k.rate > 0.f;

        // the circles
        float wet[2] = {0.f, 0.f};
        float killStep = 1.f / (kKillTime * sr);
        for (int i = 0; i < kSlots; i++) {
            Circle& c = circle[i];
            if (!c.on) continue;
            float s[2] = {0.f, 0.f};
            float level = 0.f;
            for (int which = 0; which < 2; which++) {
                Lap& l = which ? c.tail : c.cur;
                if (!l.on) continue;
                float e = l.envelope();
                level = std::max(level, e);
                double p = l.position();
                // a balance, not a pan: the circle is already stereo, so the
                // centre is unity and turning only takes from one side
                float gl = std::min(1.f, 1.f - l.pan), gr = std::min(1.f, 1.f + l.pan);
                float x0 = read(0, p), x1 = read(1, p);
                s[0] += e * gl * x0;
                s[1] += e * gr * x1;
            }
            // advance: a finished lap becomes the tail, and the next one starts
            if (c.tail.on) {
                c.tail.x += 1.;
                if (c.tail.x >= c.tail.T + c.tail.Fout) c.tail.on = false;
            }
            if (c.cur.on) {
                c.cur.x += 1.;
                if (c.cur.x >= c.cur.T) {
                    c.tail = c.cur;
                    c.cur = c.dying ? Lap() : nextLap(c, c.tail, k);
                    if (c.cur.on && i == lead) o.turn = true;
                    if (c.cur.on && !c.dying) o.ringTurn[c.ring] = true;
                }
            }
            if (c.dying) {
                c.kill -= killStep;
                if (c.kill <= 0.f) {
                    c.on = false;
                    continue;
                }
            }
            if (!c.cur.on && !c.tail.on) {
                c.on = false;
                continue;
            }
            float g = c.kill * c.kill * (3.f - 2.f * c.kill);
            if (!c.dying) {
                o.ringOn[c.ring] = true;
                o.ringSpeed[c.ring] = (float)std::log2(c.cur.on ? c.cur.v : c.tail.v);
            }
            // a stolen circle and the one taking its place share a light
            if (level * g >= o.ring[c.ring]) {
                o.ring[c.ring] = level * g;
                o.ringSpiral[c.ring] = std::log2(settings(c, k).spiral);
            }
            c.tone.tick();
            for (int ch = 0; ch < 2; ch++) wet[ch] += g * c.tone.process(ch, s[ch]);
        }

        // the newest circle still sounding leads TURN and V/OCT
        if (lead >= 0 && (!circle[lead].on || circle[lead].dying)) {
            lead = -1;
            for (int i = 0; i < kSlots; i++)
                if (circle[i].on && !circle[i].dying && (lead < 0 || circle[i].born > circle[lead].born)) lead = i;
        }
        for (int i = 0; i < kSlots; i++) o.circles += circle[i].on && !circle[i].dying;
        if (lead >= 0) o.speedOct = (float)std::log2(circle[lead].cur.on ? circle[lead].cur.v : circle[lead].tail.v);

        // line and circles: both at unity in the middle of MIX
        float dryTo = std::min(1.f, 2.f * (1.f - k.mix));
        float wetTo = std::min(1.f, 2.f * k.mix) * dbToGain(k.level);
        if (dryGain < 0.f) {
            dryGain = dryTo;
            wetGain = wetTo;
        }
        dryGain += gainCoef * (dryTo - dryGain);
        wetGain += gainCoef * (wetTo - wetGain);
        float dry = dryGain, wg = wetGain;
        o.l = dry * line[0] + wg * wet[0];
        o.r = dry * line[1] + wg * wet[1];
        if (k.saturate) {
            o.l = softLimit(o.l);
            o.r = softLimit(o.r);
        } else {
            limiter.process(o.l, o.r);
        }
        return o;
    }
};

}  // namespace spira
