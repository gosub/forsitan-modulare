// nodi.hpp - the engine of nodi: a continuous-time sequencer.
//
// A clone of New Systems Instruments' Discrete Map with its A / B / C
// Expander, written from the hardware's manual and doc/design/nodi.md. Per
// sample and per channel:
//
//   path   = how X moved since the last sample: one straight segment for an
//            external X, or the internal ramp's exact path, which can wrap
//            (up to the top, a jump to just under the bottom, up again)
//   events = every threshold X crossed on that path, with its time inside
//            the sample, ordered by time and, when coincident, in the
//            direction of travel; plus a rising edge at EXT
//   stage  = the last RISE stage crossed rising or FALL stage crossed
//            falling; EXT leaves no stage active
//   f(X)   = the stage's slider voltage + Y, COM = the active group's input
//
// A stage fires exactly at its threshold and re-arms only after X has been
// 5 mV past it the other way, so a still or noisy X sitting on a threshold
// fires it once; a jump that lands 5 mV or more past it always fires. A moving threshold
// crosses a still X just as a moving X crosses a still threshold.
//
// Every output leaves one sample late. That is the room a polyBLEP needs to
// round a step on both sides, and the gates are delayed with it, so a gate
// and the f(X) it announces always arrive on the same sample. Whether a step
// is rounded is decided per step: in Auto only when it follows the previous
// step on that output within 2 ms, so an oscillator is band-limited and a
// sequence of notes stays exact where a sample-and-hold reads it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nodi {

// ---------------------------------------------------------------- constants

const int kStages = 8;
const int kGroups = 3;
const int kMaxChannels = 16;

const float kLow = -5.f;                  // the ramp, and the default BELOW
const float kHigh = 5.f;                  // the ramp, and the default ABOVE
// The ramp's reset reaches this far under the bottom, and ONCE parks this
// far over the top, both in no time: a threshold at either end always fires.
const float kOvershoot = 0.02f;           // volts
const float kHysteresis = 0.005f;         // volts past a threshold to re-arm

const float kSlowMin = 1.f / 240.f;       // Hz, 4 minutes per cycle
const float kSlowMax = 9.f;
const float kFastMin = 4.f;
const float kFastMax = 12000.f;
const float kMaxIncrement = 0.5f;         // ramp cycles per sample
const float kGateMin = 6e-6f;             // seconds; never under a sample
const float kGateMax = 2.8f;
const float kEocSeconds = 0.001f;
const float kSchmittLow = 1.f;            // SYNC, SYNC/N and EXT
const float kSchmittHigh = 4.f;
// Samples after one of its own activations for which EXT is ignored. The
// hardware's 12.5 us is shorter than a sample; in Rack the round trip
// GATE -> other module -> its GATE -> our EXT is four samples, two cables
// and two one-sample output delays.
const int kExtLockout = 8;
const float kAutoBlepSeconds = 0.002f;    // steps closer than this are rounded

enum Direction { RISE = 0, OFF = 1, FALL = 2 };
enum Range { RANGE_BIPOLAR = 0, RANGE_FIVE = 1, RANGE_HALF = 2 };
enum AntiAlias { AA_AUTO = 0, AA_OFF = 1, AA_ON = 2 };

// ---------------------------------------------------------------- laws

// The RATE knob, 0..1, exponential across either range.
inline float rateHz(float knob, bool fast) {
    knob = std::min(std::max(knob, 0.f), 1.f);
    float lo = fast ? kFastMin : kSlowMin, hi = fast ? kFastMax : kSlowMax;
    return lo * std::pow(hi / lo, knob);
}

// GATE LEN, 0..1, exponential from 6 us to 2.8 s.
inline float gateSeconds(float knob) {
    knob = std::min(std::max(knob, 0.f), 1.f);
    return kGateMin * std::pow(kGateMax / kGateMin, knob);
}

inline float rangeLow(int range) { return range == RANGE_BIPOLAR ? -5.f : 0.f; }
inline float rangeHigh(int range) { return range == RANGE_HALF ? 2.5f : 5.f; }

// An f(X) slider, 0..1, in volts.
inline float sliderVolts(float s, int range) {
    float lo = rangeLow(range), hi = rangeHigh(range);
    return lo + (hi - lo) * s;
}

// A voltage as an f(X) slider position, clamped to the range.
inline float voltsSlider(float v, int range) {
    float lo = rangeLow(range), hi = rangeHigh(range);
    return std::min(std::max((v - lo) / (hi - lo), 0.f), 1.f);
}

// ---------------------------------------------------------------- controls

struct Controls {
    // clock
    float rate = 0.5f;
    bool fast = false;
    bool once = false;
    float voct = 0.f;             // volts at V/O
    float fmAmount = 0.f;         // 0..1
    float fmCv = 0.f;             // volts at FM
    int syncN = 1;                // 1..16
    // event
    bool length = true;           // LENGTH, else POSIT.
    float threshold[kStages];     // sliders, 0..1
    int direction[kStages];
    int group[kStages];           // 0..2 = A, B, C
    float groupAmount[kGroups];   // THR attenuators, 0..1
    // map
    float value[kStages];         // f(X) sliders, 0..1
    int range = RANGE_HALF;
    float gateLength = 0.3f;      // knob, 0..1
    int antiAlias = AA_AUTO;

    Controls() {
        for (int k = 0; k < kStages; k++) {
            threshold[k] = 0.5f;
            direction[k] = k == 0 ? FALL : RISE;
            group[k] = k % kGroups;
            value[k] = 0.f;
        }
        for (int g = 0; g < kGroups; g++) groupAmount[g] = 1.f;
    }
};

// Where the eight thresholds sit, in volts, for a space BELOW..ABOVE and the
// three group offsets (THR CV already through its attenuator).
inline void thresholds(const Controls& c, float below, float above,
                       const float* groupOffset, float* th) {
    float span = above - below;
    if (c.length) {
        float sum = 0.f;
        for (int k = 0; k < kStages; k++) sum += std::max(c.threshold[k], 0.f);
        float cum = 0.f;
        for (int k = 0; k < kStages; k++) {
            // All lengths at zero: the ladder is shorted and every threshold
            // sits in the middle, as the manual describes.
            th[k] = sum > 1e-6f ? below + span * cum / sum : below + 0.5f * span;
            cum += std::max(c.threshold[k], 0.f);
        }
    } else {
        for (int k = 0; k < kStages; k++) th[k] = below + span * c.threshold[k];
    }
    for (int k = 0; k < kStages; k++) th[k] += groupOffset[c.group[k]];
}

// ---------------------------------------------------------------- path

// A piece of X's motion inside one sample, t in 0..1 (0 = the last sample,
// 1 = this one). t0 == t1 is a jump.
struct Segment {
    float t0, t1, x0, x1;
};

const int kMaxSegments = 12;

struct Path {
    Segment seg[kMaxSegments];
    int n = 0;
    void clear() { n = 0; }
    void add(float t0, float t1, float x0, float x1) {
        if (n < kMaxSegments) seg[n++] = {t0, t1, x0, x1};
    }
    float start() const { return n ? seg[0].x0 : 0.f; }
    float end() const { return n ? seg[n - 1].x1 : 0.f; }
};

// ---------------------------------------------------------------- steps

// Rounds a step with a two-sample polyBLEP, at the price of one sample of
// delay: a step inside the interval ending at this sample corrects both this
// sample and the one before it, which has not left yet.
//
// Whether to round is decided once per sample, by the time since the last
// sample that had a step: a jump at X across three thresholds is three steps
// inside one sample, and rounding the second and third because they follow
// the first would smear a quantizer's output over three samples.
struct Stepper {
    float held = 0.f;         // the naive value one sample back
    float corrHeld = 0.f;     // its correction
    float corrNow = 0.f;      // this sample's correction so far
    int since = 1 << 30;      // samples since the last sample with a step
    bool decided = false;     // this sample has had a step
    bool round = false;       // and whether its steps are rounded

    // A step of `a` at `tau` (0..1] inside the current interval.
    void step(float a, float tau, int mode, int autoSamples) {
        if (a == 0.f) return;
        if (!decided) {
            round = mode == AA_ON || (mode == AA_AUTO && since < autoSamples);
            decided = true;
        }
        if (!round) return;
        float u = 1.f - tau;
        corrHeld += 0.5f * a * u * u;
        corrNow -= 0.5f * a * tau * tau;
    }
    // This sample's naive value in, the output (one sample back) out.
    float out(float naive) {
        float o = held + corrHeld;
        held = naive;
        corrHeld = corrNow;
        corrNow = 0.f;
        if (decided) since = 0;
        decided = false;
        if (since < (1 << 30)) since++;
        return o;
    }
};

// A gate timer, delayed by the same sample as the Steppers.
struct Gate {
    int remaining = 0;
    bool held = false;
    // A trigger while high restarts the timer: the gate carries over.
    void trigger(int samples) { remaining = samples; }
    bool out() {
        bool o = held;
        held = remaining > 0;
        if (remaining > 0) remaining--;
        return o;
    }
};

struct Schmitt {
    bool high = false;
    float prev = 0.f;
    // True on a rising edge; *t is where in the sample it crossed the high
    // threshold, by linear interpolation.
    bool process(float v, float* t) {
        bool edge = false;
        if (high) {
            if (v <= kSchmittLow) high = false;
        } else if (v >= kSchmittHigh) {
            high = true;
            edge = true;
            float d = v - prev;
            *t = d > 0.f ? std::min(std::max((kSchmittHigh - prev) / d, 0.f), 1.f) : 1.f;
        }
        prev = v;
        return edge;
    }
};

// ---------------------------------------------------------------- clock

struct Clock {
    double phase = 0.0;       // 0..1 = -5..+5 V
    bool parked = false;      // ONCE, waiting at the top
    bool fresh = true;        // the first sample starts with a reset
    int count = 0;            // SYNC/N edges
    Schmitt sync, syncN;
    Stepper ramp;
    Gate eoc;
    Path path;

    float value() const { return parked ? kHigh : kLow + (kHigh - kLow) * (float)phase; }

    void reset() {
        phase = 0.0;
        parked = false;
        fresh = true;
        count = 0;
    }

    // One sample: builds `path` and returns the RAMP output.
    float process(float hz, bool once, float syncV, float syncNV, int n,
                  float sampleRate, int aaMode, bool* eocOut) {
        const float top = kHigh + kOvershoot, bottom = kLow - kOvershoot;
        const int autoSamples = (int)(kAutoBlepSeconds * sampleRate);
        const int eocSamples = std::max(1, (int)std::min(kEocSeconds * sampleRate,
                                                         0.5f * sampleRate / std::max(hz, 1e-6f)));
        double inc = std::min((double)hz / sampleRate, (double)kMaxIncrement);

        // One reset at most per sample, at the earlier of the two jacks.
        float ts = 2.f, t1;
        if (sync.process(syncV, &t1)) {
            ts = t1;
            count = 0;
        }
        if (syncN.process(syncNV, &t1)) {
            if (++count >= std::max(n, 1)) {
                count = 0;
                ts = std::min(ts, t1);
            }
        }

        path.clear();
        float t = 0.f;
        if (fresh) {
            // Start as if a cycle had just ended: the first sample is a
            // reset, so FALL stages at the bottom fire and step 1 plays.
            path.add(0.f, 0.f, top, bottom);
            path.add(0.f, 0.f, bottom, kLow);
            fresh = false;
            parked = false;
            phase = 0.0;
        }
        float x = parked ? top : value();
        bool eocNow = false;
        while (t < 1.f) {
            if (parked) {
                // Back on LOOP, a parked ramp restarts at once, as a sync
                // would restart it; its EOC fired when it parked.
                if (!once) ts = t;
                if (ts <= 1.f && ts >= t) {
                    path.add(t, ts, x, x);
                    path.add(ts, ts, x, bottom);
                    path.add(ts, ts, bottom, kLow);
                    ramp.step(kLow - kHigh, std::max(ts, 1e-6f), aaMode, autoSamples);
                    parked = false;
                    phase = 0.0;
                    t = ts;
                    ts = 2.f;
                    x = kLow;
                    continue;
                }
                path.add(t, 1.f, x, x);
                break;
            }
            float tWrap = inc > 0.0 ? t + (float)((1.0 - phase) / inc) : 2.f;
            float tSync = ts >= t ? ts : 2.f;
            float tEnd = std::min(1.f, std::min(tWrap, tSync));
            double ph = std::min(phase + inc * (tEnd - t), 1.0);
            float xe = kLow + (kHigh - kLow) * (float)ph;
            if (tEnd > t) path.add(t, tEnd, x, xe);
            phase = ph;
            t = tEnd;
            x = xe;
            if (tSync <= tWrap && tSync <= 1.f) {
                // A sync interrupts a running cycle: a falling sweep from
                // wherever the ramp is to just under the bottom.
                path.add(t, t, x, bottom);
                path.add(t, t, bottom, kLow);
                ramp.step(kLow - x, std::max(t, 1e-6f), aaMode, autoSamples);
                eocNow = true;
                phase = 0.0;
                ts = 2.f;
                x = kLow;
            } else if (tWrap <= 1.f) {
                eocNow = true;
                path.add(t, t, kHigh, top);
                if (once) {
                    parked = true;
                    phase = 1.0;
                    x = top;
                } else {
                    path.add(t, t, top, bottom);
                    path.add(t, t, bottom, kLow);
                    ramp.step(kLow - kHigh, std::max(t, 1e-6f), aaMode, autoSamples);
                    phase = 0.0;
                    x = kLow;
                }
            } else {
                break;
            }
        }
        if (eocNow) eoc.trigger(eocSamples);
        *eocOut = eoc.out();
        return ramp.out(value());
    }
};

// ---------------------------------------------------------------- channel

struct ChannelIn {
    float x = 0.f;
    float above = kHigh, below = kLow;
    float ext = 0.f;
    float y = 0.f;
    float groupCv[kGroups] = {0.f, 0.f, 0.f};
    float sw[kGroups] = {0.f, 0.f, 0.f};     // the switch's A / B / C inputs
};

struct ChannelOut {
    float fx = 0.f;
    float com = 0.f;
    bool gate = false;
    bool groupGate[kGroups] = {false, false, false};
};

struct Event {
    float t;
    int seg;
    float key;        // order among coincident events, in travel direction
    int stage;        // -1 = EXT
    bool up;
};

struct Channel {
    bool ready = false;
    bool armedUp[kStages], armedDown[kStages];
    float thPrev[kStages];
    float xPrev = 0.f;
    int active = -1;          // stage, -1 = none
    int activeGroup = 0;
    int sinceActivation = 1 << 20;
    Schmitt ext;
    Gate gate, groupGate[kGroups];
    Stepper fx, com;

    // For the lights and the X indicator.
    float th[kStages];
    float x = 0.f;
    bool fired[kStages];      // stages activated this sample

    void reset() {
        ready = false;
        active = -1;
        sinceActivation = 1 << 20;
    }

    float stageVolts(const Controls& c) const {
        return active >= 0 ? sliderVolts(c.value[active], c.range) : 0.f;
    }
    float switchIn(const ChannelIn& in) const {
        return active >= 0 ? in.sw[activeGroup] : 0.f;
    }

    // gateSamples and autoSamples are the same for every channel, so the
    // engine works them out once a sample.
    void process(const Controls& c, const Path* internal, const ChannelIn& in,
                 int gateSamples, int autoSamples, ChannelOut& out) {

        float offset[kGroups];
        for (int g = 0; g < kGroups; g++) offset[g] = c.groupAmount[g] * in.groupCv[g];
        thresholds(c, in.below, in.above, offset, th);

        Path external;
        const Path* path = internal;
        if (!internal) {
            if (!ready) xPrev = in.x;
            external.add(0.f, 1.f, xPrev, in.x);
            path = &external;
        }
        x = path->end();

        if (!ready) {
            float x0 = path->start();
            for (int k = 0; k < kStages; k++) {
                armedUp[k] = x0 < th[k];
                armedDown[k] = x0 > th[k];
                thPrev[k] = th[k];
            }
            ready = true;
        }
        for (int k = 0; k < kStages; k++) fired[k] = false;
        if (sinceActivation < (1 << 20)) sinceActivation++;

        // Every crossing on the path, the threshold moving linearly across
        // the sample. A stage fires exactly at its threshold, then re-arms
        // for that direction only once X has been kHysteresis past it the
        // other way.
        Event ev[kStages * kMaxSegments + 1];
        int nev = 0;
        for (int k = 0; k < kStages; k++) {
            float dth = th[k] - thPrev[k];
            for (int s = 0; s < path->n; s++) {
                const Segment& g = path->seg[s];
                float d0 = g.x0 - (thPrev[k] + dth * g.t0);
                float d1 = g.x1 - (thPrev[k] + dth * g.t1);
                bool fire = false, up = d1 > d0;
                // A crossing that lands a whole hysteresis width past the
                // threshold fires even unarmed: X parked just past it and
                // then sent across by a sample-and-hold is a real crossing,
                // and noise inside the band never lands that far.
                if (up) {
                    if (d0 <= -kHysteresis) armedUp[k] = true;
                    if ((armedUp[k] && d1 >= 0.f) || (d0 < 0.f && d1 >= kHysteresis)) {
                        fire = true;
                        armedUp[k] = false;
                    }
                    if (d1 >= kHysteresis) armedDown[k] = true;
                } else if (d1 < d0) {
                    if (d0 >= kHysteresis) armedDown[k] = true;
                    if ((armedDown[k] && d1 <= 0.f) || (d0 > 0.f && d1 <= -kHysteresis)) {
                        fire = true;
                        armedDown[k] = false;
                    }
                    if (d1 <= -kHysteresis) armedUp[k] = true;
                } else {
                    if (d0 <= -kHysteresis) armedUp[k] = true;
                    if (d0 >= kHysteresis) armedDown[k] = true;
                }
                if (!fire) continue;
                float u = std::min(std::max(-d0 / (d1 - d0), 0.f), 1.f);
                float tc = g.t0 + u * (g.t1 - g.t0);
                float thAt = thPrev[k] + dth * tc;
                Event e;
                e.t = tc;
                e.seg = s;
                // Rising: lower thresholds first, then lower stages, so a
                // stage of zero length is passed over. Falling: the mirror.
                e.key = up ? thAt * 64.f + k * 1e-3f : -thAt * 64.f + (kStages - 1 - k) * 1e-3f;
                e.stage = k;
                e.up = up;
                ev[nev++] = e;
            }
            thPrev[k] = th[k];
        }
        float te;
        if (ext.process(in.ext, &te)) {
            Event e;
            e.t = te;
            e.seg = 0;
            for (int s = 0; s < path->n; s++)
                if (path->seg[s].t0 <= te) e.seg = s;
            e.key = 1e30f;
            e.stage = -1;
            e.up = true;
            ev[nev++] = e;
        }
        if (nev > 1) std::sort(ev, ev + nev, [](const Event& a, const Event& b) {
            if (a.t != b.t) return a.t < b.t;
            if (a.seg != b.seg) return a.seg < b.seg;
            return a.key < b.key;
        });

        for (int i = 0; i < nev; i++) {
            const Event& e = ev[i];
            float tau = std::max(e.t, 1e-6f);
            float oldV = stageVolts(c), oldSw = switchIn(in);
            if (e.stage < 0) {
                if (sinceActivation <= kExtLockout) continue;
                active = -1;
                gate.trigger(gateSamples);
            } else {
                int want = e.up ? RISE : FALL;
                if (c.direction[e.stage] != want) continue;
                active = e.stage;
                activeGroup = std::min(std::max(c.group[e.stage], 0), kGroups - 1);
                sinceActivation = 0;
                fired[e.stage] = true;
                gate.trigger(gateSamples);
                groupGate[activeGroup].trigger(gateSamples);
            }
            fx.step(stageVolts(c) - oldV, tau, c.antiAlias, autoSamples);
            com.step(switchIn(in) - oldSw, tau, c.antiAlias, autoSamples);
        }
        xPrev = in.x;

        out.fx = fx.out(stageVolts(c) + in.y);
        out.com = com.out(switchIn(in));
        out.gate = gate.out();
        for (int g = 0; g < kGroups; g++) out.groupGate[g] = groupGate[g].out();
    }
};

// ---------------------------------------------------------------- engine

struct Engine {
    float sampleRate = 48000.f;
    Clock clock;
    Channel ch[kMaxChannels];
    float ramp = 0.f;
    bool eoc = false;

    float gateKnob = -1.f;
    int gateSamples = 1;

    void setSampleRate(float sr) {
        sampleRate = sr;
        gateKnob = -1.f;
    }

    void reset() {
        clock.reset();
        for (int i = 0; i < kMaxChannels; i++) ch[i].reset();
    }

    // The clock's rate for these controls, in Hz, before the ceiling. The
    // knob's part is cached: it moves far less often than V/O.
    float rateKnob = -1.f;
    bool rateFast = false;
    float rateBase = 1.f;
    float clockHz(const Controls& c) {
        if (c.rate != rateKnob || c.fast != rateFast) {
            rateKnob = c.rate;
            rateFast = c.fast;
            rateBase = rateHz(c.rate, c.fast);
        }
        float hz = c.voct != 0.f ? rateBase * std::exp2(c.voct) : rateBase;
        return hz * std::max(0.f, 1.f + c.fmAmount * c.fmCv / 5.f);
    }

    // `internal`: X is unpatched and every channel reads the ramp.
    void process(const Controls& c, float syncV, float syncNV, bool internal,
                 int channels, const ChannelIn* in, ChannelOut* out) {
        ramp = clock.process(clockHz(c), c.once, syncV, syncNV, c.syncN,
                             sampleRate, c.antiAlias, &eoc);
        channels = std::min(std::max(channels, 1), kMaxChannels);
        if (c.gateLength != gateKnob) {
            gateKnob = c.gateLength;
            gateSamples = std::max(1, (int)(gateSeconds(c.gateLength) * sampleRate));
        }
        const int autoSamples = (int)(kAutoBlepSeconds * sampleRate);
        for (int i = 0; i < channels; i++)
            ch[i].process(c, internal ? &clock.path : nullptr, in[i], gateSamples, autoSamples, out[i]);
        // A channel that goes away starts afresh when it comes back.
        for (int i = channels; i < kMaxChannels; i++) ch[i].ready = false;
    }
};

}  // namespace nodi
