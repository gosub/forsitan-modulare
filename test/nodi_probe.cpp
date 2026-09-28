// nodi_probe - measurement harness for the nodi engine. It drives
// src/nodi/nodi.hpp directly and needs no Rack.
//
//   ./nodi_probe cross       activation times against a ramp of known rate,
//                            in both modes; the order on the ramp's reset; a
//                            threshold swept under a still X; a noisy X on a
//                            threshold; the top of the ramp, ONCE and SYNC/N
//   ./nodi_probe length      thresholds from lengths: even, zero lengths,
//                            all zero, ABOVE / BELOW, group offsets; a stage
//                            of zero length never stays active
//   ./nodi_probe ext         EXT cancels, gates and locks out; two engines
//                            cross-patched through one-sample cables play
//                            sixteen steps in order
//   ./nodi_probe alias       the graphic VCO and RAMP: alias energy with the
//                            steps rounded and without; Auto leaves a slow
//                            sequence's steps exact
//   ./nodi_probe shapes      the context menu's presets, transforms and
//                            setups: scales on exact semitones, transforms
//                            that undo, conversions that keep every
//                            threshold, the quantizer setup exact both ways
//   ./nodi_probe cpu         ns per sample
//   ./nodi_probe wav <dir>   scenes to listen to
//
// Every command but `cpu` and `wav` exits nonzero on a failed check.

#include "../src/nodi/nodi.hpp"
#include "../src/nodi/shapes.hpp"

#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nodi;

static const float SR = 48000.f;
static int failures = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// The RATE knob position for a frequency in the current range.
static float knobFor(float hz, bool fast) {
    float lo = fast ? kFastMin : kSlowMin, hi = fast ? kFastMax : kSlowMax;
    return std::log(hz / lo) / std::log(hi / lo);
}

struct Rig {
    Engine e;
    Controls c;
    ChannelIn in[kMaxChannels];
    ChannelOut out[kMaxChannels];
    float sync = 0.f, syncN = 0.f;
    bool internal = true;
    int channels = 1;
    explicit Rig(float sr = SR) { e.setSampleRate(sr); }
    void run() { e.process(c, sync, syncN, internal, channels, in, out); }
    int active(int i = 0) const { return e.ch[i].active; }
    bool fired(int k, int i = 0) const { return e.ch[i].fired[k]; }
};

// The first sample at which each stage fires, from `from` on.
static void firstFires(Rig& r, int samples, int* first) {
    for (int k = 0; k < kStages; k++) first[k] = -1;
    for (int n = 0; n < samples; n++) {
        r.run();
        for (int k = 0; k < kStages; k++)
            if (first[k] < 0 && r.fired(k)) first[k] = n;
    }
}

// ---------------------------------------------------------------- cross

static int cmdCross() {
    printf("cross\n");
    char what[160];

    // The manual's first quick start, at 1 Hz: step k at k/8 of the cycle.
    {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        int first[kStages];
        firstFires(r, 48000 - 100, first);
        bool ok = true;
        for (int k = 0; k < kStages; k++) {
            int want = (int)(k * SR / 8.f);
            printf("    stage %d at %6d, expected %6d\n", k + 1, first[k], want);
            ok = ok && first[k] >= 0 && std::abs(first[k] - want) <= 1;
        }
        check(ok, "LENGTH, even lengths: stage k fires at k/8 of the cycle, +-1 sample");
        int wrap = -1;
        for (int n = 0; n < 200 && wrap < 0; n++) {
            r.run();
            if (r.fired(0)) wrap = 48000 - 100 + n;
        }
        snprintf(what, sizeof what, "stage 1 (FALL) fires on the reset, at %d (48000)", wrap);
        check(std::abs(wrap - 48000) <= 1, what);
    }

    // POSIT. on a diagonal is the same sequence; on the mirrored diagonal
    // stage 8 plays first.
    for (int mirror = 0; mirror < 2; mirror++) {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        r.c.length = false;
        for (int k = 0; k < kStages; k++) {
            r.c.threshold[k] = (mirror ? kStages - 1 - k : k) / 8.f;
            r.c.direction[k] = RISE;
        }
        int first[kStages];
        firstFires(r, 48000 - 100, first);
        bool ok = true;
        for (int k = 0; k < kStages; k++) {
            // the bottom one sits kEdgeInset up, 1/1000 of the cycle
            int want = (int)(std::max(r.c.threshold[k], kEdgeInset / 10.f) * SR);
            ok = ok && std::abs(first[k] - want) <= 1;
        }
        check(ok, mirror ? "POSIT., mirrored diagonal: stage 8 first, stage 1 last"
                         : "POSIT., diagonal, all RISE: the bottom stage fires 10 mV into the ramp");
    }

    // The reset is a falling sweep: every FALL stage fires in it, top to
    // bottom, and the lowest is left active.
    {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        r.c.length = false;
        const float pos[kStages] = {0.7f, 0.2f, 0.9f, 0.1f, 0.5f, 0.6f, 0.3f, 0.8f};
        for (int k = 0; k < kStages; k++) {
            r.c.threshold[k] = pos[k];
            r.c.direction[k] = FALL;
        }
        for (int n = 0; n < 48000 - 10; n++) r.run();
        int count = 0, activeAfter = -1;
        for (int n = 0; n < 20; n++) {
            r.run();
            for (int k = 0; k < kStages; k++) count += r.fired(k);
            if (count) {
                activeAfter = r.active();
                break;
            }
        }
        snprintf(what, sizeof what, "reset with all FALL: %d stages fire in one sample, stage %d left (4)",
                 count, activeAfter + 1);
        check(count == kStages && activeAfter == 3, what);
    }

    // A RISE stage at the bottom fires just after a FALL stage there fires on
    // the reset: both sit kEdgeInset up.
    {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        r.c.length = false;
        for (int k = 0; k < kStages; k++) {
            r.c.threshold[k] = 0.5f;
            r.c.direction[k] = OFF;
        }
        r.c.threshold[0] = 0.f;
        r.c.direction[0] = RISE;
        r.c.threshold[1] = 0.f;
        r.c.direction[1] = FALL;
        for (int n = 0; n < 48000 + 5; n++) r.run();
        bool fallFirst = r.active() == 1;
        for (int n = 0; n < 50; n++) r.run();
        check(fallFirst && r.active() == 0,
              "RISE and FALL at the bottom: FALL fires on the reset, RISE 48 samples later");
    }

    // A signal that reaches the rails and no further, as an LFO does, still
    // crosses the stages at the ends of the space, both ways, every cycle.
    {
        Rig r;
        r.internal = false;
        for (int k = 0; k < kStages; k++) r.c.direction[k] = OFF;
        r.c.direction[0] = FALL;                      // LENGTH: at the bottom
        r.c.direction[7] = RISE;
        r.c.threshold[7] = 0.f;                       // stage 8 up to the top
        int fires[kStages] = {0};
        for (int n = 0; n < 48000; n++) {
            float ph = (float)(n % 4800) / 4800.f;    // 10 Hz, exactly +-5 V
            r.in[0].x = ph < 0.5f ? -5.f + 20.f * ph : 15.f - 20.f * ph;
            r.run();
            for (int k = 0; k < kStages; k++) fires[k] += r.fired(k);
        }
        snprintf(what, sizeof what, "a +-5 V triangle, 10 cycles: FALL at the bottom fires %d, "
                 "RISE at +5 V (zero-length stage 8) %d (10, 10)", fires[0], fires[7]);
        check(fires[0] == 10 && fires[7] == 10, what);
    }

    // A threshold swept down under a still X fires the RISE stage once, when
    // it passes X.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        for (int k = 0; k < kStages; k++) r.c.direction[k] = OFF;
        r.c.direction[0] = RISE;
        r.in[0].x = 0.3f;
        int fires = 0, at = -1;
        for (int n = 0; n < 2000; n++) {
            // 0.6 -> 0.4 of the space: +1 V down to -1 V over 2000 samples.
            r.c.threshold[0] = 0.6f - 0.2f * n / 2000.f;
            r.run();
            if (r.fired(0)) {
                fires++;
                at = n;
            }
        }
        snprintf(what, sizeof what, "threshold swept under X = 0.3 V: %d fire at %d (700)", fires, at);
        check(fires == 1 && std::abs(at - 700) <= 1, what);
    }

    // A noisy X sitting on a threshold fires it once.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        for (int k = 0; k < kStages; k++) r.c.direction[k] = OFF;
        r.c.direction[0] = RISE;
        r.c.threshold[0] = 0.5f;
        uint32_t seed = 1;
        int fires = 0;
        r.in[0].x = -1.f;
        r.run();
        for (int n = 0; n < 48000; n++) {
            seed = seed * 1664525u + 1013904223u;
            r.in[0].x = 0.002f * ((seed >> 8) / 8388608.f - 1.f);   // +-2 mV
            r.run();
            fires += r.fired(0);
        }
        snprintf(what, sizeof what, "X = 0 V +- 2 mV of noise on a threshold at 0 V: fires %d time(s)", fires);
        check(fires == 1, what);
    }

    // X parked just past a FALL threshold, inside the hysteresis band, then
    // sent well below it in one jump: that is a crossing, and it fires.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        for (int k = 0; k < kStages; k++) r.c.direction[k] = OFF;
        r.c.direction[0] = FALL;
        r.c.direction[1] = RISE;
        r.c.threshold[0] = r.c.threshold[1] = 0.5f;
        r.in[0].x = -1.f;
        r.run();
        r.in[0].x = 0.002f;                  // RISE fires, FALL is not armed
        r.run();
        int before = r.active();
        r.in[0].x = -1.f;
        r.run();
        snprintf(what, sizeof what, "parked 2 mV past a FALL threshold, then a jump to -1 V: stage %d -> %d (2 -> 1)",
                 before + 1, r.active() + 1);
        check(before == 1 && r.active() == 0, what);
    }

    // The very top of the ramp fires, every cycle.
    {
        Rig r;
        r.c.rate = knobFor(10.f, true);
        r.c.fast = true;
        r.c.length = false;
        for (int k = 0; k < kStages; k++) r.c.direction[k] = OFF;
        r.c.direction[7] = RISE;
        r.c.threshold[7] = 1.f;
        int fires = 0;
        for (int n = 0; n < 48000; n++) {
            r.run();
            fires += r.fired(7);
        }
        snprintf(what, sizeof what, "a RISE threshold at +5 V on a 10 Hz ramp: %d fires in 1 s (10)", fires);
        check(fires == 10, what);
    }

    // ONCE parks at the top; SYNC/N restarts it on the N-th edge.
    {
        Rig r;
        r.c.rate = knobFor(4.f, false);
        r.c.once = true;
        r.c.syncN = 3;
        int eocs = 0, cycles = 0;
        bool eocPrev = false;
        for (int n = 0; n < 48000; n++) {
            r.syncN = (n % 4800) < 100 ? 10.f : 0.f;    // an edge every 0.1 s
            r.run();
            if (r.e.eoc && !eocPrev) eocs++;
            eocPrev = r.e.eoc;
            cycles += r.fired(0);
        }
        // 4 Hz: a cycle is 0.25 s and parks until the third edge, so cycles
        // start at 0, 0.3, 0.6 and 0.9 s and reach the top (EOC) at 0.25,
        // 0.55 and 0.85 s; the fourth tops out after the second is over.
        snprintf(what, sizeof what, "ONCE + SYNC/N 3, edges every 0.1 s: %d cycles (4), %d EOC (3)",
                 cycles, eocs);
        check(cycles == 4 && eocs == 3, what);
    }

    // Back to LOOP, a parked ramp restarts without a sync, and without a
    // second EOC.
    {
        Rig r;
        r.c.rate = knobFor(4.f, false);
        r.c.once = true;
        int eocs = 0, cycles = 0;
        bool eocPrev = false;
        for (int n = 0; n < 43200; n++) {     // 0.9 s
            if (n == 24000) r.c.once = false;   // parked since 0.25 s
            r.run();
            if (r.e.eoc && !eocPrev) eocs++;
            eocPrev = r.e.eoc;
            cycles += r.fired(0);
        }
        // Cycles start at 0, 0.5 and 0.75 s; EOC at 0.25 s, then at 0.75 s
        // as the third cycle starts.
        snprintf(what, sizeof what, "ONCE parked, LOOP at 0.5 s: %d cycles (3), %d EOC (2)",
                 cycles, eocs);
        check(cycles == 3 && eocs == 2, what);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- length

static int cmdLength() {
    printf("length\n");
    char what[160];
    const float zero[kGroups] = {0.f, 0.f, 0.f};
    float th[kStages];

    Controls c;
    thresholds(c, kLow, kHigh, zero, th);
    bool ok = true;
    for (int k = 0; k < kStages; k++)
        ok = ok && std::fabs(th[k] - std::max(-5.f + 1.25f * k, kLow + kEdgeInset)) < 1e-5f;
    check(ok, "even lengths: -4.99 (10 mV inside the space), -3.75 ... +3.75 V");

    for (int k = 0; k < kStages; k++) c.threshold[k] = 0.1f * (k + 1);
    thresholds(c, kLow, kHigh, zero, th);
    float sum = 3.6f, cum = 0.f;
    ok = true;
    for (int k = 0; k < kStages; k++) {
        ok = ok && std::fabs(th[k] - std::max(-5.f + 10.f * cum / sum, kLow + kEdgeInset)) < 1e-4f;
        cum += 0.1f * (k + 1);
    }
    check(ok, "lengths 0.1 .. 0.8: proportional, first at the bottom, the last reaching the top");

    for (int k = 0; k < kStages; k++) c.threshold[k] = 0.f;
    thresholds(c, kLow, kHigh, zero, th);
    ok = true;
    for (int k = 0; k < kStages; k++) ok = ok && th[k] == 0.f;
    check(ok, "every length zero: all thresholds in the middle");

    Controls d;
    thresholds(d, 0.f, 10.f, zero, th);
    check(std::fabs(th[0] - kEdgeInset) < 1e-6f && std::fabs(th[7] - 8.75f) < 1e-5f,
          "BELOW 0, ABOVE 10: 0.01 .. 8.75 V");

    const float off[kGroups] = {1.f, -2.f, 0.5f};
    d.groupAmount[1] = 0.5f;
    float offs[kGroups] = {off[0] * d.groupAmount[0], off[1] * d.groupAmount[1], off[2] * d.groupAmount[2]};
    thresholds(d, kLow, kHigh, offs, th);
    ok = true;
    for (int k = 0; k < kStages; k++) {
        float want = std::max(-5.f + 1.25f * k, kLow + kEdgeInset) + offs[d.group[k]];
        ok = ok && std::fabs(th[k] - want) < 1e-5f;
    }
    check(ok, "group offsets add to their stages' thresholds, in volts, after the mode");

    // A stage of zero length is passed over on the way up.
    {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        r.c.threshold[3] = 0.f;
        bool seen = false;
        int firedCount = 0;
        for (int n = 0; n < 48000; n++) {
            r.run();
            if (r.active() == 3) seen = true;
            firedCount += r.fired(3);
        }
        snprintf(what, sizeof what, "stage 4 at zero length: fires %d time(s) but is never left active",
                 firedCount);
        check(!seen, what);
    }

    // OFF keeps its length: stage 3 OFF, stage 4 still at its place.
    {
        Rig r;
        r.c.rate = knobFor(1.f, false);
        r.c.direction[2] = OFF;
        int first[kStages];
        firstFires(r, 47000, first);
        snprintf(what, sizeof what, "stage 3 OFF: never fires, stage 4 still at 3/8 (%d, 18000)", first[3]);
        check(first[2] < 0 && std::abs(first[3] - 18000) <= 1, what);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- ext

static int cmdExt() {
    printf("ext\n");
    char what[160];

    // EXT cancels: no stage, f(X) = +Y, and a gate.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        r.c.range = RANGE_FIVE;
        for (int k = 0; k < kStages; k++) {
            r.c.direction[k] = OFF;
            r.c.value[k] = 0.5f;
        }
        r.c.direction[0] = RISE;
        r.c.threshold[0] = 0.5f;
        r.c.gateLength = 0.f;
        r.in[0].y = 1.f;
        r.in[0].x = -1.f;
        r.run();
        r.in[0].x = 1.f;
        for (int n = 0; n < 100; n++) r.run();
        float before = r.out[0].fx;
        r.in[0].ext = 10.f;
        r.run();
        bool gateSeen = false;
        for (int n = 0; n < 3; n++) {
            r.run();
            gateSeen = gateSeen || r.out[0].gate;
        }
        snprintf(what, sizeof what, "EXT cancels: f(X) %.3f -> %.3f V (3.5 -> 1, the +Y), gate %s",
                 before, r.out[0].fx, gateSeen ? "yes" : "no");
        check(r.active() == -1 && std::fabs(before - 3.5f) < 1e-5f &&
              std::fabs(r.out[0].fx - 1.f) < 1e-5f && gateSeen, what);
    }

    // The lockout: an EXT edge k samples after an activation.
    {
        bool ok = true;
        for (int k = 1; k <= kExtLockout + 2; k++) {
            Rig r;
            r.internal = false;
            r.c.length = false;
            for (int s = 0; s < kStages; s++) r.c.direction[s] = OFF;
            r.c.direction[0] = RISE;
            r.in[0].x = -1.f;
            r.run();
            r.in[0].x = 1.f;
            r.run();                             // activation
            for (int n = 1; n < k; n++) r.run();
            r.in[0].ext = 10.f;
            r.run();                             // edge, k samples later
            bool cancelled = r.active() == -1;
            bool want = k > kExtLockout;
            ok = ok && cancelled == want;
        }
        snprintf(what, sizeof what, "EXT is ignored up to %d samples after an activation, obeyed after",
                 kExtLockout);
        check(ok, what);
    }

    // Two engines as one sixteen-step sequencer, the manual's patch, with
    // Rack's one-sample cables: A's RAMP -> B's X, A's f(X) -> B's +Y,
    // GATE -> EXT both ways, A over -5..0 V and B over 0..+5 V.
    {
        Rig a, b;
        a.c.rate = knobFor(1.f, false);
        b.internal = false;
        a.c.range = b.c.range = RANGE_FIVE;
        a.c.gateLength = b.c.gateLength = 0.3f;     // well under a step
        for (int k = 0; k < kStages; k++) {
            a.c.value[k] = k / 16.f;
            b.c.value[k] = (k + 8) / 16.f;
            b.c.direction[k] = RISE;
        }
        a.in[0].below = -5.f;
        a.in[0].above = 0.f;
        b.in[0].below = 0.f;
        b.in[0].above = 5.f;
        float aRamp = 0.f, aFx = 0.f, bFx = 0.f;
        bool aGate = false, bGate = false;
        // Where one module hands over to the other, both are active until
        // the cancel comes back through two cables: a short run of their
        // sum. Those runs are counted apart from the steps.
        std::vector<float> raw;
        std::vector<int> lens;
        float last = -99.f;
        int runLen = 0;
        for (int n = 0; n < 2 * 48000 + 200; n++) {
            // Every cable carries last sample's output.
            a.in[0].ext = bGate ? 10.f : 0.f;
            b.in[0].ext = aGate ? 10.f : 0.f;
            b.in[0].x = aRamp;
            b.in[0].y = aFx;
            a.run();
            b.run();
            aRamp = a.e.ramp;
            aFx = a.out[0].fx;
            aGate = a.out[0].gate;
            bGate = b.out[0].gate;
            bFx = b.out[0].fx;
            if (n < 48000) continue;              // the second cycle
            if (std::fabs(bFx - last) > 1e-4f) {
                if (runLen) lens.push_back(runLen);
                raw.push_back(bFx);
                last = bFx;
                runLen = 0;
            }
            runLen++;
        }
        std::vector<float> seq;
        int overlaps = 0, longestOverlap = 0, shortest = 1 << 30, longest = 0;
        for (size_t i = 1; i < lens.size(); i++) {
            if (lens[i] <= 8) {
                overlaps++;
                longestOverlap = std::max(longestOverlap, lens[i]);
                continue;
            }
            seq.push_back(raw[i]);
            if (i + 1 < lens.size()) {
                shortest = std::min(shortest, lens[i]);
                longest = std::max(longest, lens[i]);
            }
        }
        bool ok = seq.size() >= 16;
        int start = 0;
        for (size_t i = 0; i < seq.size(); i++)
            if (std::fabs(seq[i]) < 1e-4f) {
                start = (int)i;
                break;
            }
        for (int k = 0; ok && k < 16; k++)
            ok = start + k < (int)seq.size() && std::fabs(seq[start + k] - 5.f * k / 16.f) < 1e-3f;
        printf("    B's f(X), second cycle:");
        for (size_t i = 0; i < seq.size() && i < 20; i++) printf(" %.3f", seq[i]);
        printf("\n");
        printf("    step lengths %d .. %d samples (3000 each)\n", shortest, longest);
        check(ok, "two engines cross-patched play sixteen steps in order, one voltage each");
        snprintf(what, sizeof what, "the handovers overlap %d time(s) per cycle (1..2), for %d samples (<= 4)",
                 overlaps, longestOverlap);
        check(overlaps >= 1 && overlaps <= 2 && longestOverlap <= 4, what);
        // B's stage 1 sits 10 mV over A's top, 1/1000 of the 48000-sample cycle
        check(shortest >= 2940 && longest <= 3060,
              "and the sixteen steps are even, +-60 samples (the 10 mV inset is 48)");
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- alias

static void fft(std::vector<std::complex<double>>& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; j++) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Energy away from the harmonics of f0 over energy on them, in dB, within
// the band up to `band` Hz.
static double aliasDb(const std::vector<float>& x, double f0, double band = 20000.0) {
    const size_t n = 1 << 16;
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        a[i] = x[i + 4096] * w;
    }
    fft(a);
    double on = 0, off = 0, binHz = SR / (double)n;
    for (size_t i = 3; i < n / 2 && i * binHz < band; i++) {
        double f = i * binHz, h = f / f0;
        double dist = std::fabs(h - std::round(h)) * f0;
        double p = std::norm(a[i]);
        if (dist < 4 * binHz) on += p;
        else off += p;
    }
    return 10 * std::log10(off / on);
}

static std::vector<float> renderVco(float hz, int aa, bool rampOut) {
    Rig r;
    r.c.fast = true;
    r.c.rate = knobFor(hz, true);
    r.c.range = RANGE_BIPOLAR;
    r.c.antiAlias = aa;
    for (int k = 0; k < kStages; k++) {
        r.c.direction[k] = RISE;
        r.c.value[k] = 0.5f + 0.45f * std::sin(2 * M_PI * (k + 0.5) / kStages);
    }
    std::vector<float> y((1 << 16) + 8192);
    for (size_t i = 0; i < y.size(); i++) {
        r.run();
        y[i] = rampOut ? r.e.ramp : r.out[0].fx;
    }
    return y;
}

static int cmdAlias() {
    printf("alias (energy off the harmonics / on them, up to 20 kHz)\n");
    printf("    %-9s %10s %10s %10s   %10s %10s\n", "", "f(X) off", "f(X) on", "f(X) auto",
           "RAMP off", "RAMP on");
    const float freqs[] = {220.5f, 1234.5f, 3001.7f, 7777.7f};
    bool better = true, autoSame = true;
    for (float f : freqs) {
        double off = aliasDb(renderVco(f, AA_OFF, false), f);
        double on = aliasDb(renderVco(f, AA_ON, false), f);
        double au = aliasDb(renderVco(f, AA_AUTO, false), f);
        double roff = aliasDb(renderVco(f, AA_OFF, true), f);
        double ron = aliasDb(renderVco(f, AA_ON, true), f);
        printf("    %7.1f Hz %8.1f dB %8.1f dB %8.1f dB   %8.1f dB %8.1f dB\n", f, off, on, au, roff, ron);
        better = better && on < off - 10.0 && ron < roff - 10.0;
        autoSame = autoSame && std::fabs(au - on) < 0.5;
    }
    check(better, "rounding the steps lowers the alias energy by more than 10 dB, f(X) and RAMP");
    check(autoSame, "Auto rounds an audio-rate f(X) as On does");

    // A slow sequence under Auto: every output sample is exactly a stage's
    // voltage, no rounded steps.
    {
        Rig r;
        r.c.rate = knobFor(2.f, false);
        r.c.range = RANGE_FIVE;
        for (int k = 0; k < kStages; k++) r.c.value[k] = k / 8.f;
        bool exact = true;
        for (int n = 0; n < 48000; n++) {
            r.run();
            if (n < 2) continue;
            float v = r.out[0].fx / 5.f * 8.f;
            exact = exact && std::fabs(v - std::round(v)) < 1e-5f;
        }
        check(exact, "Auto, a 2 Hz eight-step sequence: every sample is exactly a stage's voltage");
    }

    // The quantizer: a sample-and-hold at X jumping across several thresholds
    // in one sample. Those are several steps inside one sample, and Auto must
    // not round the later ones for following the first.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        r.c.range = RANGE_FIVE;
        for (int k = 0; k < kStages; k++) {
            r.c.threshold[k] = k / 8.f;
            r.c.direction[k] = RISE;
            r.c.value[k] = k / 8.f;
        }
        r.c.direction[0] = FALL;
        uint32_t seed = 7;
        bool exact = true;
        for (int n = 0; n < 48000; n++) {
            if (n % 4800 == 0) {
                seed = seed * 1664525u + 1013904223u;
                r.in[0].x = 10.f * ((seed >> 8) / 16777216.f) - 5.f;
            }
            r.run();
            float v = r.out[0].fx / 5.f * 8.f;
            exact = exact && std::fabs(v - std::round(v)) < 1e-5f;
        }
        check(exact, "Auto, a sample-and-hold at X jumping across thresholds: every sample exact");
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- shapes

static uint32_t shapeSeed = 12345;
static float shapeUniform() {
    shapeSeed = shapeSeed * 1664525u + 1013904223u;
    return (shapeSeed >> 8) / 16777216.f;
}

static bool onSemitones(const float* v, int range) {
    for (int k = 0; k < kStages; k++) {
        float s = sliderVolts(v[k], range) * 12.f;
        if (std::fabs(s - std::round(s)) > 1e-3f) return false;
        if (v[k] < 0.f || v[k] > 1.f) return false;
    }
    return true;
}

static bool same(const float* a, const float* b, float tol = 1e-5f) {
    for (int k = 0; k < kStages; k++)
        if (std::fabs(a[k] - b[k]) > tol) return false;
    return true;
}

static int cmdShapes() {
    printf("shapes\n");
    char what[160];

    // Every scale preset in every range lands on exact semitones.
    bool ok = true;
    for (int range = 0; range < 3; range++)
        for (int p = VALUE_ZERO; p < VALUE_RANDOM; p++) {
            if (p == VALUE_UP || p == VALUE_DOWN) continue;
            float v[kStages];
            valuePreset(p, range, v, shapeUniform);
            if (!onSemitones(v, range)) {
                ok = false;
                printf("    off semitones: %s in range %d\n", valuePresetName(p), range);
            }
        }
    check(ok, "f(X) scale presets land on exact semitones, in all three ranges");
    {
        float v[kStages];
        valuePreset(VALUE_MAJOR, RANGE_HALF, v, shapeUniform);
        const int want[kStages] = {0, 2, 4, 5, 7, 9, 11, 12};
        ok = true;
        for (int k = 0; k < kStages; k++)
            ok = ok && std::fabs(sliderVolts(v[k], RANGE_HALF) - want[k] / 12.f) < 1e-5f;
        check(ok, "major scale, 0..2.5 V: 0 2 4 5 7 9 11 12 semitones from 0 V");
    }

    // Transforms that undo each other.
    {
        float a[kStages], b[kStages];
        valuePreset(VALUE_MAJOR, RANGE_BIPOLAR, a, shapeUniform);
        std::copy(a, a + kStages, b);
        const int pairs[][2] = {{VT_REVERSE, VT_REVERSE}, {VT_ROTATE_LEFT, VT_ROTATE_RIGHT},
                                {VT_MIRROR, VT_MIRROR}, {VT_SEMITONE_UP, VT_SEMITONE_DOWN},
                                {VT_OCTAVE_UP, VT_OCTAVE_DOWN}};
        ok = true;
        for (auto& pr : pairs) {
            valueTransform(pr[0], RANGE_BIPOLAR, b, shapeUniform);
            valueTransform(pr[1], RANGE_BIPOLAR, b, shapeUniform);
            if (!same(a, b)) {
                ok = false;
                printf("    %s then %s is not the identity\n", valueTransformName(pr[0]),
                       valueTransformName(pr[1]));
            }
        }
        check(ok, "f(X): reverse, rotate, mirror, transpose each undo");
        valueTransform(VT_SHUFFLE, RANGE_BIPOLAR, b, shapeUniform);
        float sa[kStages], sb[kStages];
        std::copy(a, a + kStages, sa);
        std::copy(b, b + kStages, sb);
        std::sort(sa, sa + kStages);
        std::sort(sb, sb + kStages);
        check(same(sa, sb), "f(X) shuffle is a permutation");
        ok = true;
        valuePreset(VALUE_MINOR, RANGE_HALF, b, shapeUniform);
        for (int n = 0; n < 200; n++) {
            valueTransform(VT_MUTATE, RANGE_HALF, b, shapeUniform);
            ok = ok && onSemitones(b, RANGE_HALF);
        }
        check(ok, "f(X) mutate, 200 times: always on semitones, always in range");
        for (int k = 0; k < kStages; k++) b[k] = shapeUniform();
        valueTransform(VT_SNAP, RANGE_FIVE, b, shapeUniform);
        check(onSemitones(b, RANGE_FIVE), "f(X) snap puts random sliders on semitones");
    }

    // Each rhythm is the same thresholds in either mode, and the conversions
    // keep every threshold where it was.
    {
        const float zero[kGroups] = {0.f, 0.f, 0.f};
        ok = true;
        bool fits = true;
        for (int p = 0; p < TH_RANDOM; p++) {
            Controls len, pos;
            pos.length = false;
            thresholdPreset(p, true, len.threshold, shapeUniform);
            thresholdPreset(p, false, pos.threshold, shapeUniform);
            float a[kStages], b[kStages];
            thresholds(len, kLow, kHigh, zero, a);
            thresholds(pos, kLow, kHigh, zero, b);
            if (!same(a, b, 1e-4f)) {
                ok = false;
                printf("    %s differs between the modes\n", thresholdPresetName(p));
            }
            for (int k = 0; k < kStages; k++) fits = fits && len.threshold[k] <= 1.f && len.threshold[k] >= 0.f;
        }
        check(ok, "threshold presets: the same thresholds whether LENGTH or POSIT.");
        check(fits, "threshold presets: every LENGTH slider within its travel");

        ok = true;
        for (int trial = 0; trial < 100; trial++) {
            Controls len, pos, back;
            pos.length = false;
            for (int k = 0; k < kStages; k++) len.threshold[k] = shapeUniform();
            lengthsToPositions(len.threshold, pos.threshold);
            positionsToLengths(pos.threshold, back.threshold);
            float a[kStages], b[kStages], c[kStages];
            thresholds(len, kLow, kHigh, zero, a);
            thresholds(pos, kLow, kHigh, zero, b);
            thresholds(back, kLow, kHigh, zero, c);
            ok = ok && same(a, b, 1e-4f) && same(a, c, 1e-4f);
        }
        check(ok, "LENGTH -> POSIT. -> LENGTH, 100 random banks: every threshold kept");

        Controls pos, len;
        pos.length = false;
        const float out[kStages] = {0.f, 0.3f, 0.2f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
        std::copy(out, out + kStages, pos.threshold);
        positionsToLengths(pos.threshold, len.threshold);
        float a[kStages], b[kStages];
        thresholds(pos, kLow, kHigh, zero, a);
        thresholds(len, kLow, kHigh, zero, b);
        bool kept = true;
        for (int k = 0; k < kStages; k++)
            if (k != 2) kept = kept && std::fabs(b[k] - a[k]) < 1e-4f;
        snprintf(what, sizeof what,
                 "POSIT. -> LENGTH, stage 3 out of order: it lands on stage 4 (%.2f, %.2f V), the rest kept",
                 b[2], b[3]);
        check(std::fabs(b[2] - b[3]) < 1e-4f && kept, what);
    }

    // Switch transforms that undo.
    {
        int d[kStages], e[kStages];
        directionPreset(DIR_ALTERNATE, d);
        d[3] = OFF;
        std::copy(d, d + kStages, e);
        directionTransform(ST_FLIP, e);
        bool flipped = e[0] == FALL && e[1] == RISE && e[3] == OFF;
        directionTransform(ST_FLIP, e);
        check(flipped && std::equal(d, d + kStages, e), "directions: swap rise and fall, OFF kept, undoes");
        int g[kStages], h[kStages];
        groupPreset(GROUP_THIRDS, g, shapeUniform);
        std::copy(g, g + kStages, h);
        for (int n = 0; n < 3; n++) groupTransform(ST_FLIP, h);
        check(std::equal(g, g + kStages, h), "groups: cycling A -> B -> C three times is the identity");
    }

    // Setups: everything in range; then the three that can be heard here.
    {
        ok = true;
        for (int id = 0; id < NUM_SETUPS; id++) {
            Setup su;
            setup(id, su);
            float knob = std::log(su.hz / (su.fast ? kFastMin : kSlowMin)) /
                         std::log((su.fast ? kFastMax : kSlowMax) / (su.fast ? kFastMin : kSlowMin));
            ok = ok && knob >= 0.f && knob <= 1.f;
            for (int k = 0; k < kStages; k++)
                ok = ok && su.value[k] >= 0.f && su.value[k] <= 1.f && su.threshold[k] >= 0.f &&
                     su.threshold[k] <= 1.f && su.group[k] >= 0 && su.group[k] < kGroups;
        }
        check(ok, "every setup: sliders, groups and rate within their ranges");

        // The quantizer: a sample-and-hold jumping anywhere, from above or
        // below, always comes out as the note of the region it landed in.
        Setup su;
        setup(SETUP_QUANTIZER, su);
        Rig r;
        r.internal = false;
        r.c.length = su.length;
        r.c.range = su.range;
        for (int k = 0; k < kStages; k++) {
            r.c.value[k] = su.value[k];
            r.c.threshold[k] = su.threshold[k];
            r.c.direction[k] = su.direction[k];
        }
        const float notes[4] = {0.f, 4.f / 12.f, 7.f / 12.f, 1.f};
        // An X that has not moved yet has crossed nothing: start it below.
        r.in[0].x = -6.f;
        r.run();
        int wrong = 0;
        for (int n = 0; n < 20000; n++) {
            float x = 9.98f * shapeUniform() - 4.99f;
            r.in[0].x = x;
            r.run();
            r.run();
            r.run();
            int region = std::min(3, (int)((x + 5.f) / 2.5f));
            if (std::fabs(r.out[0].fx - notes[region]) > 1e-5f) wrong++;
        }
        snprintf(what, sizeof what, "quantizer setup, 20000 random jumps: %d on the wrong note (0)", wrong);
        check(wrong == 0, what);

        // The graphic VCO at its 110 Hz: stage 1 fires 110 times a second.
        setup(SETUP_VCO, su);
        Rig v;
        v.c.fast = su.fast;
        v.c.rate = knobFor(su.hz, true);
        for (int k = 0; k < kStages; k++) {
            v.c.value[k] = su.value[k];
            v.c.threshold[k] = su.threshold[k];
            v.c.direction[k] = su.direction[k];
        }
        v.c.range = su.range;
        int fires = 0;
        for (int n = 0; n < 48000; n++) {
            v.run();
            fires += v.fired(0);
        }
        snprintf(what, sizeof what, "graphic VCO setup: %d cycles in a second (110)", fires);
        check(std::abs(fires - 110) <= 1, what);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- cpu

static int cmdCpu() {
    const int n = 480000;
    for (int poly = 1; poly <= 16; poly += 15) {
        for (int internal = 0; internal < 2; internal++) {
            Rig r;
            r.internal = internal;
            r.channels = poly;
            r.c.fast = true;
            r.c.rate = 0.5f;
            auto t0 = std::chrono::steady_clock::now();
            // A triangle per channel, cheap enough not to be measured too.
            float phase = 0.f;
            for (int i = 0; i < n; i++) {
                phase += 0.002f;
                if (phase > 1.f) phase -= 1.f;
                for (int c = 0; c < poly; c++) {
                    float p = phase + c * 0.0625f;
                    p -= (int)p;
                    r.in[c].x = 20.f * std::fabs(p - 0.5f) - 5.f;
                }
                r.run();
            }
            double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            printf("  %2d channel(s), %s X: %6.1f ns/sample (%.2f%% of a 48 kHz sample)\n", poly,
                   internal ? "internal" : "external", ns / n, ns / n / (1e9 / 48000.) * 100.);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- wav

static void writeWav(const std::string& path, const std::vector<float>& x, float gain = 0.2f) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t n = x.size(), sr = SR, bytes = n * 2;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(1);
    u32(sr);
    u32(sr * 2);
    u16(2);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(bytes);
    for (float v : x) {
        int s = (int)std::lround(std::max(-1.f, std::min(1.f, v * gain)) * 32767);
        u16((uint16_t)(int16_t)s);
    }
    fclose(f);
    printf("  %s\n", path.c_str());
}

static int cmdWav(const std::string& dir) {
    // The graphic VCO swept from 100 Hz to 5 kHz, steps rounded and not.
    for (int aa = 0; aa < 2; aa++) {
        Rig r;
        r.c.fast = true;
        r.c.range = RANGE_BIPOLAR;
        r.c.antiAlias = aa ? AA_ON : AA_OFF;
        for (int k = 0; k < kStages; k++) {
            r.c.direction[k] = RISE;
            r.c.value[k] = 0.5f + 0.45f * std::sin(2 * M_PI * (k + 0.5) / kStages);
        }
        std::vector<float> y(SR * 6);
        for (size_t i = 0; i < y.size(); i++) {
            r.c.rate = knobFor(100.f * std::pow(50.f, i / (float)y.size()), true);
            r.run();
            y[i] = r.out[0].fx;
        }
        writeWav(dir + (aa ? "/vco_rounded.wav" : "/vco_naive.wav"), y);
    }
    // The bitcrusher from the quick start on a 110 Hz sine.
    {
        Rig r;
        r.internal = false;
        r.c.length = false;
        r.c.range = RANGE_BIPOLAR;
        r.c.antiAlias = AA_ON;
        const float pos[kStages] = {0.2f, 0.4f, 0.6f, 0.8f, 0.8f, 0.6f, 0.4f, 0.2f};
        for (int k = 0; k < kStages; k++) {
            r.c.threshold[k] = pos[k];
            r.c.value[k] = pos[k];
            r.c.direction[k] = k < 4 ? RISE : FALL;
        }
        std::vector<float> y(SR * 4);
        for (size_t i = 0; i < y.size(); i++) {
            r.in[0].x = 5.f * std::sin(2 * M_PI * 110.f * i / SR);
            r.run();
            y[i] = r.out[0].fx;
        }
        writeWav(dir + "/bitcrusher.wav", y);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "cross") return cmdCross();
    if (cmd == "length") return cmdLength();
    if (cmd == "ext") return cmdExt();
    if (cmd == "alias") return cmdAlias();
    if (cmd == "shapes") return cmdShapes();
    if (cmd == "cpu") return cmdCpu();
    if (cmd == "wav" && argc > 2) return cmdWav(argv[2]);
    fprintf(stderr, "usage: nodi_probe cross|length|ext|alias|shapes|cpu|wav <dir>\n");
    return 2;
}
