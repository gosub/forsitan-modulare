// umbrae_dsp.hpp — the feedback loop, free of Rack headers so
// test/umbrae_probe can measure it directly.
//
// After Bastl Instruments and Casper Electronics' Dark Matter, whose manual
// prints a full block diagram and no component values at all. What is
// modelled here is that diagram, stage for stage:
//
//   in ─┬─► DRIVE VCA (x3, soft clip at +-5 V) ─┬─► [HYPER DRIVE x7] ─► TONE
//       │        fader + CV                      │                       │
//       │                                        │      loop return ─────┤
//       ├─► envelope follower ─► DYNAMICS ───────┼──────────────────┐    │
//       │   (normalled to FBK CV and X-FADE CV)  │                  │    ▼
//       └─► X-FADE A (clean, pre or post drive)  │              FBK VCA  │
//                                                │                  ▲    │
//                              X-FADE B ◄────────┴──── TONE OUT ────┴────┘
//                                   │
//                              X-FADE ─► out
//
// The loop is the module. Everything else is there to shape what goes round
// it, and the manual is explicit that the interesting sound is the circuit
// listening to itself: "It becomes the sound of the circuit itself, its own
// resonant frequency."
//
// Which is the whole difficulty of porting it. In the hardware the loop is
// instantaneous and oscillates where the accumulated phase of the EQ, the
// loop's own band limits and the amplifiers' bandwidth comes back round; put
// a one-sample delay in it instead and it screams near Nyquist at a pitch
// that moves with the host's sample rate. So the loop here carries an
// explicit, physical propagation delay of kLoopDelay seconds — a few op-amp
// stages' worth of group delay — read out of a fractional delay line. The
// pitch is then set by modelled time constants rather than by the grid, and
// the same patch oscillates at the same frequency at every sample rate.
//
// The consequence is that the engine has a minimum useful oversampling
// ratio: one oversampled sample must be shorter than the modelled delay, or
// the grid is back in charge. See kLoopDelay and the module's menu.
//
// Everything not in the block diagram is a guess, and the frequencies are
// all guesses: the manual gives x3 drive gain, soft clipping at +-5 V, x7
// hyper drive, 0/+5 V for the envelope and the CV ranges, and nothing else.
// See doc/umbrae.md.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace umbrae {

inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// ── the constants the manual gives ──────────────────────────────────────────
static const double kDriveGain = 3.0;      // "applies x3 gain"
static const double kClipVolts = 5.0;      // "and soft clipping at +/-5V"
static const double kHyperGain = 7.0;      // features page
static const double kEnvVolts = 5.0;       // DYNAMICS out is 0/+5 V
static const double kCvVolts = 5.0;        // DRIVE, FBK and X-FADE CV
static const double kBoostCvVolts = 8.0;   // BASS and TREBLE BOOST CV
static const double kOutClip = 10.0;       // X-FADE OUT is -10/+10 V

// ── the constants it does not ───────────────────────────────────────────────
// The loop's propagation delay: a handful of op-amp stages and a VCA. It is
// what keeps the oscillation frequency a property of the circuit rather than
// of the sample rate, and it sets the lowest oversampling ratio that means
// anything — the delay line has to hold at least two samples for the
// fractional read to have anything to interpolate between, so the engine
// wants 2/kLoopDelay = 125 kHz at the very least. 4x from 44.1 kHz clears
// that, and the module offers nothing below 4x.
static const double kLoopDelay = 16e-6;    // s

// The two bands. They do not meet: between 300 Hz and 1.8 kHz both are
// falling, so with both faders down the loop has a hole in the middle and
// picks one register or the other rather than howling in between.
static const double kBassHz = 300.0;
static const double kBassQ = 0.7;
static const double kTrebleHz = 1800.0;
static const double kTrebleQ = 0.7;
static const double kBoostMax = 12.0;      // BOOST pot fully up, into the saturator

// The FBK VCA's gain. The quick start brings the loop up on the FBK fader
// alone, with the boosts at zero and the bands merely "shaping", so the VCA
// has to be able to put the loop past unity by itself: at a third of the
// fader the loop is at unity and the module colours without howling, and the
// travel above that is the howl.
static const double kFbkGain = 3.0;

// What the loop can carry: the band limits drawn on the diagram as one BPF,
// plus the bandwidth of the amplifier the whole thing runs through, plus the
// coupling capacitor that keeps the loop from latching to a rail.
static const double kLoopHpHz = 70.0;
static const double kLoopLpHz = 7000.0;
static const double kAmpHz = 40000.0;
static const double kCoupleHz = 8.0;

// The envelope follower. Its detector has a lowpass in front of it, which is
// what the manual means by "more sensitive to low frequencies so it can pick
// the kick in a drum beat or track the pitch of a melody"; the back-panel
// jumper bypasses it.
static const double kEnvDetectHz = 300.0;
static const double kEnvAttackS = 0.002;
static const double kEnvDecayShortS = 0.06;
static const double kEnvDecayLongS = 0.60;

// The HF warning light watches the loop above this.
static const double kHfWatchHz = 5000.0;

// The circuit's own noise floor, without which an ideal loop sits at zero
// for ever and never starts. A few microvolts at the tone section's input,
// inaudible until the feedback is high enough to be howling anyway.
static const double kNoiseVolts = 4e-6;

// ── building blocks ─────────────────────────────────────────────────────────

// Soft clipper: the Pade form of tanh, which is within a percent of it and
// reaches exactly the rail at three times the rail. There are four of these
// in the signal path and they run at the oversampled rate, so the real
// tanh's cost showed up in the profile.
inline double softClip(double x, double rail) {
    const double u = clampd(x / rail, -3.0, 3.0);
    const double u2 = u * u;
    return rail * u * (27.0 + u2) / (27.0 + 9.0 * u2);
}

struct OnePole {
    double s = 0.0;
    void reset() { s = 0.0; }
    double lp(double x, double k) { s += (x - s) * k; return s; }
    double hp(double x, double k) { return x - lp(x, k); }
};

// Coefficient for a one-pole at fc, exact rather than the small-angle form,
// because these run at oversampled rates where fc/sr can still be large.
inline double poleK(double fc, double sr) {
    return 1.0 - std::exp(-2.0 * M_PI * fc / sr);
}

// Topology-preserving state variable filter (Zavalishin / Cytomic form).
struct Svf {
    double ic1 = 0.0, ic2 = 0.0;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 0.0;

    void reset() { ic1 = ic2 = 0.0; }
    void set(double fc, double sr, double q) {
        const double g = std::tan(M_PI * clampd(fc, 1.0, 0.45 * sr) / sr);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void process(double v0, double& lo, double& band, double& hi) {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lo = v2;
        band = v1;
        hi = v0 - k * v1 - v2;
    }
    double lowpass(double x) { double l, b, h; process(x, l, b, h); return l; }
    double highpass(double x) { double l, b, h; process(x, l, b, h); return h; }
};

// The loop's propagation delay, read at a fractional distance so the delay
// is a duration rather than a sample count.
struct LoopDelay {
    static const int kMax = 256;      // power of two: the index wraps by mask
    double buf[kMax] = {};
    int w = 0;
    double samples = 1.0;

    void reset() {
        for (int i = 0; i < kMax; i++) buf[i] = 0.0;
        w = 0;
    }
    // The read happens before the write, so a request of d samples comes back
    // d + 1 later; the caller's duration is what has to be right, so take the
    // one back here rather than in the engine.
    void setDelay(double s) { samples = clampd(s - 1.0, 1.0, (double)(kMax - 2)); }
    double read() const {
        const double d = samples;
        const int i = (int)d;
        const double f = d - i;
        const int a = (w - i) & (kMax - 1);
        const int b = (a - 1) & (kMax - 1);
        return buf[a] + (buf[b] - buf[a]) * f;
    }
    void write(double x) {
        w = (w + 1) & (kMax - 1);
        buf[w] = x;
    }
};

// ── the engine ──────────────────────────────────────────────────────────────
struct Engine {
    struct Controls {
        // faders, 0..1
        double drive = 0.5, bass = 0.5, treble = 0.5, fbk = 0.0, xfade = 0.0;
        // pots
        double bassBoost = 0.0, trebleBoost = 0.0;
        double driveCvAmt = 0.0;                  // attenuator, 0..1
        double fbkCvAmt = 0.0, xfadeCvAmt = 0.0;  // attenuverters, -1..1
        // switches
        bool hyperDrive = false;    // x7 into the tone section
        bool dynPostDrive = false;  // DRY/DRIVE: where the follower listens
        bool dynLongDecay = false;  // DECAY
        bool xfadePostDrive = false;// DRIVE/DRY: what the clean side of the fade is
        bool invertFbkOut = false;  // OUT POLARITY
        bool cvControlsOut = true;  // CV CONTROL: VCA on the send, else on the return
        bool dynLowpass = true;     // back-panel jumper on the detector
        // patching
        bool inputPatched = false;
        bool fbkInPatched = false;
        bool fbkCvPatched = false;
        bool xfadeCvPatched = false;
    };

    struct Frame {
        double out = 0.0;        // X-FADE OUT, the module's output
        double fbkSend = 0.0;    // FBK OUT
        double dynamics = 0.0;   // DYNAMICS, 0..+5 V
        double hf = 0.0;         // how much high frequency is in the loop, 0..1
    };

    double sampleRate = 384000.0;
    double dt = 1.0 / 384000.0;

    Svf bassFilter, trebleFilter;
    OnePole ampPole, couple, loopHp, loopLp, envDetect, hfHp;
    LoopDelay delay;
    double env = 0.0, hfEnv = 0.0;
    uint32_t noiseState = 0x1d872b41u;
    double kAmp = 0.0, kCouple = 0.0, kLoopHp = 0.0, kLoopLp = 0.0;
    double kEnvDetect = 0.0, kAttack = 0.0, kDecayShort = 0.0, kDecayLong = 0.0;
    double kHf = 0.0, kHfEnv = 0.0;

    void setSampleRate(double sr) {
        sampleRate = sr;
        dt = 1.0 / sr;
        bassFilter.set(kBassHz, sr, kBassQ);
        trebleFilter.set(kTrebleHz, sr, kTrebleQ);
        kAmp = poleK(kAmpHz, sr);
        kCouple = poleK(kCoupleHz, sr);
        kLoopHp = poleK(kLoopHpHz, sr);
        kLoopLp = poleK(kLoopLpHz, sr);
        kEnvDetect = poleK(kEnvDetectHz, sr);
        kHf = poleK(kHfWatchHz, sr);
        kAttack = 1.0 - std::exp(-1.0 / (kEnvAttackS * sr));
        kDecayShort = 1.0 - std::exp(-1.0 / (kEnvDecayShortS * sr));
        kDecayLong = 1.0 - std::exp(-1.0 / (kEnvDecayLongS * sr));
        kHfEnv = 1.0 - std::exp(-1.0 / (0.05 * sr));
        delay.setDelay(kLoopDelay * sr);
    }

    void reset() {
        bassFilter.reset(); trebleFilter.reset();
        ampPole.reset(); couple.reset(); loopHp.reset(); loopLp.reset();
        envDetect.reset(); hfHp.reset();
        delay.reset();
        env = hfEnv = 0.0;
    }

    // xorshift, scaled to the noise floor above.
    double noise() {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        return kNoiseVolts * ((double)(noiseState & 0xffffffu) / 8388608.0 - 1.0);
    }

    // The oversampling ratio below which the modelled loop delay is shorter
    // than one sample and the sample grid takes over the tuning.
    static double minRateFor(double delayS) { return 1.0 / delayS; }

    Frame process(const Controls& c, double inVolts, double driveCv,
                  double fbkCv, double xfadeCv, double bassCv, double trebleCv,
                  double fbkInVolts) {
        Frame f;
        const double in = c.inputPatched ? inVolts : 0.0;

        // ── DRIVE: an input VCA with gain and soft clipping ─────────────────
        const double driveLevel = clampd(
            c.drive + c.driveCvAmt * driveCv / kCvVolts, 0.0, 1.0);
        const double driveOut = softClip(kDriveGain * driveLevel * in, kClipVolts);

        // ── DYNAMICS: the envelope follower ─────────────────────────────────
        const double detIn = c.dynPostDrive ? driveOut : in;
        const double det = c.dynLowpass ? envDetect.lp(detIn, kEnvDetect) : detIn;
        const double rect = std::fabs(det) / kClipVolts;          // 0..1-ish
        const double kEnv = rect > env
            ? kAttack : (c.dynLongDecay ? kDecayLong : kDecayShort);
        env += (rect - env) * kEnv;
        f.dynamics = clampd(env, 0.0, 1.0) * kEnvVolts;

        // Unpatched, DYNAMICS is normalled to both CV inputs.
        const double fbkCvV = c.fbkCvPatched ? fbkCv : f.dynamics;
        const double xfadeCvV = c.xfadeCvPatched ? xfadeCv : f.dynamics;

        // ── TONE: the two bands, each with its own saturating boost ─────────
        const double loopReturn = delay.read();
        const double toneIn = (c.hyperDrive ? kHyperGain : 1.0) * driveOut
                            + loopReturn + noise();

        const double bassBoost = 1.0 + kBoostMax * clampd(
            c.bassBoost + bassCv / kBoostCvVolts, 0.0, 1.0);
        const double trebleBoost = 1.0 + kBoostMax * clampd(
            c.trebleBoost + trebleCv / kBoostCvVolts, 0.0, 1.0);

        const double bassBand = bassFilter.lowpass(toneIn);
        const double trebleBand = trebleFilter.highpass(toneIn);
        const double toneSum =
              clampd(c.bass, 0.0, 1.0) * softClip(bassBoost * bassBand, kClipVolts)
            + clampd(c.treble, 0.0, 1.0) * softClip(trebleBoost * trebleBand, kClipVolts);

        // the amplifier: its bandwidth, its rails, and its coupling capacitor.
        // Casper's own description of the loop is two signals "battling it out
        // in the feedback thunderdome", and this is where they run out of room.
        const double amped = softClip(ampPole.lp(toneSum, kAmp), kClipVolts);
        const double toneOut = amped - couple.lp(amped, kCouple);

        // ── FBK: the loop, and the two jacks that open it ───────────────────
        const double fbkLevel = kFbkGain * clampd(
            c.fbk + c.fbkCvAmt * fbkCvV / kCvVolts, 0.0, 1.0);
        const double send = c.cvControlsOut ? fbkLevel * toneOut : toneOut;
        f.fbkSend = clampd(c.invertFbkOut ? -send : send, -12.0, 12.0);

        // FBK OUT is normalled to FBK IN's switching terminal: patching FBK IN
        // is what breaks the internal loop.
        const double ret = c.fbkInPatched ? fbkInVolts : send;
        const double banded = loopLp.lp(ret, kLoopLp) - loopHp.lp(ret, kLoopHp);
        const double intoLoop = c.cvControlsOut ? banded : fbkLevel * banded;
        delay.write(clampd(intoLoop, -12.0, 12.0));

        // ── X-FADE: clean against fed-back ──────────────────────────────────
        const double xf = clampd(
            c.xfade + c.xfadeCvAmt * xfadeCvV / kCvVolts, 0.0, 1.0);
        const double clean = c.xfadePostDrive ? driveOut : in;
        f.out = clampd((1.0 - xf) * clean + xf * toneOut, -kOutClip, kOutClip);

        // ── the HF warning ──────────────────────────────────────────────────
        const double hi = std::fabs(toneOut - hfHp.lp(toneOut, kHf));
        hfEnv += (hi / kClipVolts - hfEnv) * kHfEnv;
        f.hf = clampd(hfEnv, 0.0, 1.0);
        return f;
    }
};

}  // namespace umbrae
