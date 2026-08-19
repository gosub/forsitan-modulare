// aether_dsp.hpp — the broken transmission line, free of Rack headers so
// test/aether_probe can measure it directly.
//
// The circuit, as its manual describes it: audio goes into a *synchronous*
// voltage-to-frequency converter clocked by the CARRIER, comes out as a high
// frequency pulse train, and is recovered by a phase-locked loop whose own
// oscillator is the same kind of converter, clocked by the DEMODULATOR. Both
// clocks are voltage controlled and neither tracks 1V/oct. Everything the
// module does — aliasing, bitcrush-like breakup, FM, dropouts, the standalone
// oscillator with nothing patched — falls out of those two clocks disagreeing.
//
// Two things follow from that architecture and they are the whole model:
//
//   * A synchronous charge-balance V/F converter *is* a first order
//     delta-sigma modulator whose clock is a knob. Its pulse rate is
//     f_tx = d * f_carrier with density d proportional to the input, and it
//     tops out at f_carrier/2 because the one-shot holds the reference
//     current for a whole clock period (AD652 data sheet, bipolar mode: zero
//     input sits at f_clock/4). Turning CARRIER FREQ down really is turning
//     the sample rate down, which is why the manual says so.
//
//   * The receiver locks its own converter to the incoming pulse rate, so at
//     lock d_rx * f_demod = d_tx * f_carrier, and the loop's control voltage
//     — which is the audio output — comes out as
//
//         v_out = R * (1 + v_in) - 1,     R = f_carrier / f_demod
//
//     Matched clocks (R = 1) return the input, as the manual promises. Any
//     other ratio scales and offsets it into the rails, which is the
//     distortion. A demodulator clock too slow to reach the incoming rate
//     cannot lock at all and there is no output, which the manual also says.
//
// The phase comparator is switchable (TYPE) and the three positions behave
// the way the three classic CD4046 comparators do, which is how the Sound On
// Sound review describes them: 1 and 3 put out something whether or not the
// loop is locked and will happily lock to a harmonic, 2 is edge triggered and
// goes quiet when it loses lock. So: XOR, phase-frequency detector, RS latch.
//
// Modelling decisions that are inference rather than documentation, called
// out here because they are the parts most likely to be wrong:
//
//   * the pulse trains are halved by a toggle flip-flop before the phase
//     comparator. Without it the trains are narrow pulses, XOR degenerates
//     into OR, and the loop runs away from lock instead of towards it. The
//     divider is what every 4046 design does with a duty cycle it does not
//     control, and it preserves the frequency information the loop needs.
//   * the clock ranges (20 Hz to 320 kHz) and the loop filter corner behind
//     TONE (60 Hz to 12 kHz). The manual gives neither.
//
// Timing is event driven: clock ticks are scheduled in continuous time and
// the loop filter is integrated between them, so a clock is free to run far
// above the sample rate — the aliasing the module is made of comes from the
// modelled carrier, not from the host's sample grid. Everything else runs on
// the (oversampled) audio grid.
//
// See doc/aether.md for the sources.

#pragma once

#include <algorithm>
#include <cmath>

namespace aether {

inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// ── ranges ──────────────────────────────────────────────────────────────────
// The clocks span 20 Hz to 327.68 kHz. Noon on the knob is 2.5 kHz, so the
// half of the travel that behaves like a sample rate gets half the knob.
static const double kClkMin = 20.0;
static const double kClkOctaves = 14.0;
static const double kClkMax = 400000.0;
// Octaves per volt at a fully open CV attenuator: +-5 V covers +-5 octaves.
static const double kClkOctPerVolt = 1.0;

// TONE is one passive pole, sitting in the loop and on the output both.
static const double kToneMin = 60.0;
static const double kToneMax = 12000.0;
// Fixed poles: the output buffer's bandwidth and its coupling capacitor.
static const double kOutPoleHz = 40000.0;
static const double kDcBlockHz = 5.0;

// Voltages. The unpatched SIGNAL IN jack is a +5 V bias the knob attenuates,
// which is what makes the module an oscillator with nothing plugged in.
static const double kDcBias = 5.0;
static const double kInVolts = 5.0;      // input volts for full scale density
static const double kOutVolts = 5.0;
static const double kClkVolts = 5.0;
static const double kErrVolts = 5.0;

// 2^x to about six digits, so the two clocks can be exponential without a
// pow() per oversampled frame.
inline double fastExp2(double x) {
    x = clampd(x, -60.0, 60.0);
    const double xi = std::floor(x);
    const double f = x - xi;
    const double p = 1.0 + f * (0.6931472 + f * (0.2402265 + f * (0.0555041
                   + f * (0.0096181 + f * 0.0013330))));
    return p * std::ldexp(1.0, (int)xi);
}

// ── synchronous charge-balance V/F converter ────────────────────────────────
// One tick of the clock: integrate the input, and if the integrator has
// crossed a full reference charge, emit a pulse and take that charge back
// out. Bipolar, so a zero input sits at a quarter of the clock rate. The
// one-shot cannot fire twice in a row, which is what caps the pulse rate at
// half the clock; the integrator is clamped so an over-range input winds it
// up only so far before it saturates rather than running away.
struct Svfc {
    double acc = 0.0;
    bool prevPulse = false;

    void reset() { acc = 0.0; prevPulse = false; }

    // u is the input normalised to +-1 full scale; returns the pulse bit.
    bool tick(double u) {
        const double d = 0.25 * (1.0 + clampd(u, -1.0, 1.0));   // 0 .. 0.5
        acc += d;
        bool pulse = false;
        if (acc >= 1.0 && !prevPulse) {
            acc -= 1.0;
            pulse = true;
        }
        acc = clampd(acc, -1.0, 2.0);
        prevPulse = pulse;
        return pulse;
    }
};

// ── phase comparators ───────────────────────────────────────────────────────
// Fed by the two pulse trains after a divide-by-two, so both are square. All
// three are wired so that the incoming train running ahead pushes the loop
// filter up, which is the direction that speeds the local oscillator up.
enum PdType { PD_XOR = 0, PD_PFD = 1, PD_RS = 2 };

struct PhaseComparator {
    bool a = false, b = false;      // divided trains: a = received, b = local
    bool up = false, down = false;  // phase-frequency detector state
    bool rs = false;                // RS latch state

    void reset() { a = b = up = down = rs = false; }

    void edgeA() {                  // a pulse arrived from the transmitter
        a = !a;
        if (a) {                    // rising edge of the divided train
            up = true;
            if (down) { up = false; down = false; }
            rs = true;
        }
    }
    void edgeB() {                  // the local oscillator emitted one
        b = !b;
        if (b) {
            down = true;
            if (up) { up = false; down = false; }
            rs = false;
        }
    }

    double out(int type) const {
        switch (type) {
            case PD_PFD: return up ? 1.0 : (down ? -1.0 : 0.0);
            case PD_RS:  return rs ? 1.0 : -1.0;
            default:     return (a != b) ? 1.0 : -1.0;
        }
    }
};

// ── polyBLEP, for the two clock outputs ─────────────────────────────────────
inline double polyBlep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// ── the engine ──────────────────────────────────────────────────────────────
struct Engine {
    // Set once per host sample, held across the oversampled frames.
    struct Controls {
        double inLevel = 0.5;        // SIGNAL IN attenuator, 0..1
        double carrierKnob = 0.7;    // 0..1
        double carrierCvAmt = 0.0;   // 0..1
        double demodKnob = 0.7;
        double demodCvAmt = 0.0;
        double tone = 0.5;           // 0..1
        double errThresh = 0.0;      // -1..1
        int type = PD_XOR;
        bool inputPatched = false;
        bool extCarrier = false;
        bool extDemod = false;
    };

    struct Frame {
        double out = 0.0;        // recovered audio, volts
        double error = 0.0;      // comparator across input and output, volts
        double carrierClk = 0.0; // volts
        double demodClk = 0.0;   // volts
        double signalIn = 0.0;   // post-attenuator input, volts
    };

    // ── state ──
    double sampleRate = 192000.0;   // the oversampled rate
    double dt = 1.0 / 192000.0;

    Svfc tx, rx;
    PhaseComparator pd;
    double loop = 0.0;              // loop filter state = the control voltage
    double toneOut = 0.0;           // the same pole again, on the output
    double outPole = 0.0;
    double dcState = 0.0;
    double carrierTimer = 0.0, demodTimer = 0.0;   // seconds to the next tick
    double carrierPeriod = 1.0, demodPeriod = 1.0;
    double fCarrier = 1000.0, fDemod = 1000.0;
    double extPrev[2] = {0.0, 0.0};
    bool errState = false;
    // cached control-rate derivations
    double tau = 1.0 / (2.0 * M_PI * 1000.0);
    double carrierOct = 0.0, demodOct = 0.0;
    double lastTone = -1.0, lastCarrierKnob = -1.0, lastDemodKnob = -1.0;

    void setSampleRate(double sr) {
        sampleRate = sr;
        dt = 1.0 / sr;
    }

    void reset() {
        tx.reset(); rx.reset(); pd.reset();
        loop = toneOut = outPole = dcState = 0.0;
        lastTone = lastCarrierKnob = lastDemodKnob = -1.0;
        carrierTimer = demodTimer = 0.0;
        extPrev[0] = extPrev[1] = 0.0;
        errState = false;
    }

    // The knob's own octaves are hoisted out by the caller: only the CV part
    // changes from one oversampled frame to the next.
    static double clockHz(double octaves) {
        return clampd(kClkMin * fastExp2(clampd(octaves, -12.0, 24.0)),
                      0.05, kClkMax);
    }
    static double clockHz(double knob, double cvVolts, double amount) {
        return clockHz(kClkOctaves * clampd(knob, 0.0, 1.0)
                       + kClkOctPerVolt * amount * cvVolts);
    }

    // 1 - exp(-x) to third order: the loop filter is advanced by whatever
    // fraction of a sample sits between two clock ticks, so the coefficient
    // cannot be precomputed. x stays small because a segment is never longer
    // than one oversampled sample.
    static double poleStep(double x) {
        if (x <= 0.0) return 0.0;
        if (x > 0.35) return 1.0 - std::exp(-x);
        return x * (1.0 - x * (0.5 - x * (1.0 / 6.0)));
    }

    // One oversampled frame. All voltages are Rack volts.
    Frame process(const Controls& c, double inVolts,
                  double carrierCv, double demodCv,
                  double extCarrierV, double extDemodV) {
        Frame f;

        // ── input stage ─────────────────────────────────────────────────────
        const double src = c.inputPatched ? inVolts : kDcBias;
        const double sig = clampd(c.inLevel, 0.0, 1.0) * src;
        f.signalIn = sig;
        const double u = clampd(sig / kInVolts, -1.0, 1.0);

        // Unpatched CV inputs are fed by the signal itself: the CV knobs
        // become audio-rate exponential FM depth. An external clock takes its
        // side's CV out of the picture entirely, as the manual notes.
        if (c.tone != lastTone) {
            lastTone = c.tone;
            const double toneHz = kToneMin * std::pow(kToneMax / kToneMin,
                                                      clampd(c.tone, 0.0, 1.0));
            tau = 1.0 / (2.0 * M_PI * toneHz);
        }
        if (c.carrierKnob != lastCarrierKnob) {
            lastCarrierKnob = c.carrierKnob;
            carrierOct = kClkOctaves * clampd(c.carrierKnob, 0.0, 1.0);
        }
        if (c.demodKnob != lastDemodKnob) {
            lastDemodKnob = c.demodKnob;
            demodOct = kClkOctaves * clampd(c.demodKnob, 0.0, 1.0);
        }
        fCarrier = c.extCarrier ? fCarrier
                 : clockHz(carrierOct + kClkOctPerVolt * c.carrierCvAmt * carrierCv);
        fDemod = c.extDemod ? fDemod
               : clockHz(demodOct + kClkOctPerVolt * c.demodCvAmt * demodCv);
        carrierPeriod = 1.0 / fCarrier;
        demodPeriod = 1.0 / fDemod;
        // A clock that has just sped up must not sit out the old, longer
        // period before its next tick.
        if (carrierTimer > carrierPeriod) carrierTimer = carrierPeriod;
        if (demodTimer > demodPeriod) demodTimer = demodPeriod;

        // ── external clocks: one zero crossing per frame, timed inside it ───
        double extCarrierAt = -1.0, extDemodAt = -1.0;
        if (c.extCarrier) {
            extCarrierAt = crossing(extPrev[0], extCarrierV);
            extPrev[0] = extCarrierV;
        }
        if (c.extDemod) {
            extDemodAt = crossing(extPrev[1], extDemodV);
            extPrev[1] = extDemodV;
        }

        // ── event loop over the frame ───────────────────────────────────────
        double rem = dt;
        for (int guard = 0; guard < 4096; guard++) {
            const double tC = c.extCarrier ? extCarrierAt : carrierTimer;
            const double tD = c.extDemod ? extDemodAt : demodTimer;
            double tNext = rem;
            int which = -1;
            if (tC >= 0.0 && tC <= tNext) { tNext = tC; which = 0; }
            if (tD >= 0.0 && tD < tNext) { tNext = tD; which = 1; }
            if (which < 0) break;

            advance(tNext, tau, c.type);
            rem -= tNext;
            if (c.extCarrier) { if (extCarrierAt >= 0.0) extCarrierAt -= tNext; }
            else carrierTimer -= tNext;
            if (c.extDemod) { if (extDemodAt >= 0.0) extDemodAt -= tNext; }
            else demodTimer -= tNext;

            if (which == 0) {
                if (c.extCarrier) extCarrierAt = -1.0;
                else carrierTimer += carrierPeriod;
                if (tx.tick(u)) pd.edgeA();
            } else {
                if (c.extDemod) extDemodAt = -1.0;
                else demodTimer += demodPeriod;
                if (rx.tick(loop)) pd.edgeB();
            }
        }
        advance(rem, tau, c.type);
        if (!c.extCarrier) carrierTimer -= rem;
        if (!c.extDemod) demodTimer -= rem;
        if (carrierTimer <= 0.0) carrierTimer = carrierPeriod;
        if (demodTimer <= 0.0) demodTimer = demodPeriod;

        // ── output path: the same pole again, the buffer, the coupling cap ──
        const double k = poleStep(dt / tau);
        toneOut += (loop - toneOut) * k;
        outPole += (toneOut - outPole) * poleStep(dt * 2.0 * M_PI * kOutPoleHz);
        dcState += (outPole - dcState) * poleStep(dt * 2.0 * M_PI * kDcBlockHz);
        f.out = clampd((outPole - dcState) * kOutVolts, -10.0, 10.0);

        // ── ERROR: a comparator across input and output ─────────────────────
        // Runs on the oversampled grid with no band-limiting of its own: the
        // edge lands within one oversampled sample of where it belongs, and
        // what that jitter leaves behind is well under the noise this output
        // is made of.
        f.error = comparator(f.out - sig - c.errThresh * kErrVolts);

        // ── the two clock outputs ───────────────────────────────────────────
        f.carrierClk = c.extCarrier ? 0.0
                     : square(carrierTimer, carrierPeriod) * kClkVolts;
        f.demodClk = c.extDemod ? 0.0
                   : square(demodTimer, demodPeriod) * kClkVolts;
        return f;
    }

    // What the probe measures: the distance between the loop's control
    // voltage and the one that would hold the local oscillator at the
    // incoming pulse rate. Zero is locked.
    double lockError(double u) const {
        const double dTx = 0.25 * (1.0 + clampd(u, -1.0, 1.0));
        const double want = 4.0 * dTx * fCarrier / fDemod - 1.0;
        return loop - clampd(want, -1.0, 1.0);
    }

private:
    // Integrate the loop filter across a stretch of time with the phase
    // comparator held at whatever it is now.
    void advance(double seconds, double tau, int type) {
        if (seconds <= 0.0) return;
        const double target = pd.out(type);
        loop += (target - loop) * poleStep(seconds / tau);
        loop = clampd(loop, -1.0, 1.0);
    }

    // Time inside this frame at which the external clock crossed zero going
    // up, or -1 if it did not.
    double crossing(double prev, double now) const {
        if (!(prev < 0.0 && now >= 0.0)) return -1.0;
        const double denom = now - prev;
        const double frac = denom > 1e-12 ? (-prev) / denom : 0.0;
        return clampd(frac, 0.0, 1.0) * dt;
    }

    // A 50% square from a countdown timer, band-limited while it can be.
    double square(double timer, double period) const {
        const double phase = clampd(1.0 - timer / period, 0.0, 1.0);
        const double inc = dt / period;
        double s = phase < 0.5 ? 1.0 : -1.0;
        if (inc < 0.4) {
            s += polyBlep(phase, inc);
            double p2 = phase + 0.5;
            if (p2 >= 1.0) p2 -= 1.0;
            s -= polyBlep(p2, inc);
        }
        return s;
    }

    double comparator(double x) {
        const double hyst = 0.02;
        errState = errState ? (x > -hyst) : (x > hyst);
        return (errState ? 1.0 : -1.0) * kErrVolts;
    }
};

}  // namespace aether
