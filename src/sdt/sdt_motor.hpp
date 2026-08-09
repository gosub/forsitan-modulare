// sdt_motor.hpp — a combustion engine: cylinders on a shared crank phase,
// each with an intake pipe, a chamber whose length breathes with the piston,
// and an extractor into a common exhaust, expansion chamber, muffler bank and
// outlet. Three signals come out: what the intake sucks, what the block
// radiates, and what leaves the pipe.
//
// Ported from SDTMotor.c (Sound Design Toolkit, GPL-3.0-or-later).
//
// Changes from the C: the waveguide bank is sized per role instead of one
// maxDelay for all 42 of them (the cylinders need 0.5 m, the exhaust 6 m), the
// sample rate is per object, and the noise stream is per object.
#pragma once

#include "sdt_common.hpp"
#include "sdt_filters.hpp"

namespace sdt {

static const int kMaxCylinders = 12;
static const int kNMufflers = 4;

struct Motor {
    // Reflection coefficients of the fixed joints, straight from the C.
    static double metalFeed() { return 0.9; }
    static double mufflerFeed() { return 0.5; }
    static double jointFeed() { return 0.1; }
    static double airFeed() { return -0.5; }

    Waveguide intakes[kMaxCylinders], cylinders[kMaxCylinders], extractors[kMaxCylinders];
    Waveguide exhaust, mufflers[kNMufflers], outlet;
    OnePole air, walls;
    DCFilter intakeDC, vibrationsDC, outletDC;
    Rng rng{20260812u};

    double sampleRate = 48000.0;
    double rpm = 700.0, throttle = 0.0, phase = 0.0, step = 120.0;
    double cylinderSize = 500.0, compressionRatio = 10.0, sparkTime = 0.1;
    double asymmetry = 0.1, backfire = 0.0, backfireRate = 0.0;
    double revIntakes = 0.0, vibrations = 0.0, fwdExtractors = 0.0;
    double revMufflers = 0.0, fwdMufflers = 0.0, fwdOutlet = 0.0;
    // SDTMotor's own defaults are 20 Hz for both, but nothing in the C ever
    // applies them: SDTMotor_new leaves the one-poles at pass-through and the
    // DC blockers as plain first differences, and only a host that sets the
    // attributes calls update(). Applying 20 Hz literally lowpasses the intake
    // hiss and the block radiation down to nothing. The module sets its own.
    double damp = 2500.0, dc = 25.0;
    bool twoStrokeCycle = false, isRevvingDown = false, isBackfiring = false;
    int nCylinders = 4;

    // The sizes are kept in metres so a sample-rate change can re-derive the
    // delay lengths; the C keeps them in samples and cannot.
    double intakeSize = 0.25, extractorSize = 0.4, exhaustSize = 2.5;
    double mufflerSize = 0.5, outletSize = 0.01;
    double cylinderVolume = 400.0, expansion = 0.0, mufflerFeedback = 0.5;

    Motor() { setSampleRate(48000.0); }

    void setSampleRate(double sr) {
        sampleRate = sr;
        // Per-role capacity: the longest pipe each line ever has to hold.
        const long cylMax = (long)samplesInAir(0.5, sr) + 4;
        const long pipeMax = (long)samplesInAir(1.5, sr) + 4;
        const long bigMax = (long)samplesInAir(6.0, sr) + 4;
        for (int i = 0; i < kMaxCylinders; i++) {
            intakes[i].init(pipeMax);
            intakes[i].setRevFeedback(airFeed());
            cylinders[i].init(cylMax);
            extractors[i].init(pipeMax);
            extractors[i].setFwdFeedback(jointFeed());
        }
        exhaust.init(bigMax);
        exhaust.setRevFeedback(jointFeed());
        exhaust.setFwdFeedback(mufflerFeed());
        for (int i = 0; i < kNMufflers; i++) {
            mufflers[i].init(bigMax);
            mufflers[i].setRevFeedback(mufflerFeed());
            mufflers[i].setFwdFeedback(mufflerFeed());
        }
        outlet.init(pipeMax);
        outlet.setRevFeedback(mufflerFeed());
        outlet.setFwdFeedback(airFeed());
        // Reapply everything that was expressed in metres or in Hz.
        setIntakeSize(intakeSize);
        setExtractorSize(extractorSize);
        setExhaustSize(exhaustSize);
        setExpansion(expansion);
        setMufflerSize(mufflerSize);
        setMufflerFeedback(mufflerFeedback);
        setOutletSize(outletSize);
        setCylinderSize(cylinderVolume);
        updateFilters();
    }

    void updateFilters() {
        air.lowpass(damp, sampleRate);
        walls.lowpass(damp, sampleRate);
        intakeDC.setFrequency(dc, sampleRate);
        vibrationsDC.setFrequency(dc, sampleRate);
        outletDC.setFrequency(dc, sampleRate);
    }

    void setDamp(double f) {
        damp = f;
        updateFilters();
    }
    void setDc(double f) {
        dc = f;
        updateFilters();
    }

    void setRpm(double f) {
        f = std::max(0.0, f);
        if ((int)f < (int)rpm) {
            isRevvingDown = true;
        } else if ((int)f > (int)rpm) {
            backfireRate = backfire;
            isRevvingDown = false;
        } else {
            isRevvingDown = false;
        }
        rpm = f;
    }
    void setThrottle(double f) { throttle = fclip(f, 0.0, 1.0); }
    void setTwoStroke(bool two) {
        twoStrokeCycle = two;
        step = two ? 60.0 : 120.0;
    }
    void setNCylinders(int i) { nCylinders = iclip(i, 1, kMaxCylinders); }
    // f is a displacement in cc; the chamber length is its cube root.
    void setCylinderSize(double f) {
        cylinderVolume = std::max(0.0, f);
        cylinderSize = samplesInAir(0.01 * std::pow(cylinderVolume, 1.0 / 3.0), sampleRate);
    }
    void setCompressionRatio(double f) { compressionRatio = std::max(1.0, f); }
    void setSparkTime(double f) { sparkTime = fclip(f, kMicro, 1.0); }
    void setAsymmetry(double f) { asymmetry = fclip(f, 0.0, 1.0); }
    void setBackfire(double f) { backfire = f; }

    void setIntakeSize(double f) {
        intakeSize = f;
        const double s = samplesInAir(f, sampleRate);
        for (int i = 0; i < kMaxCylinders; i++)
            intakes[i].setDelay(s * (1.0 + spreadCoeff(i)));
    }
    void setExtractorSize(double f) {
        extractorSize = f;
        const double s = samplesInAir(f, sampleRate);
        for (int i = 0; i < kMaxCylinders; i++)
            extractors[i].setDelay(s * (1.0 + spreadCoeff(i)));
    }
    void setExhaustSize(double f) {
        exhaustSize = f;
        exhaust.setDelay(samplesInAir(f, sampleRate));
    }
    void setExpansion(double f) {
        expansion = fclip(f, 0.0, 1.0);
        for (int i = 0; i < kMaxCylinders; i++) extractors[i].setFwdFeedback(-expansion);
        exhaust.setRevFeedback(expansion);
    }
    void setMufflerSize(double f) {
        mufflerSize = f;
        const double s = samplesInAir(f, sampleRate);
        for (int i = 0; i < kNMufflers; i++)
            mufflers[i].setDelay(s * (0.7 + 0.6 * i / (kNMufflers - 1)));
    }
    void setMufflerFeedback(double f) {
        mufflerFeedback = fclip(f, 0.0, 1.0);
        exhaust.setFwdFeedback(mufflerFeedback);
        for (int i = 0; i < kNMufflers; i++) {
            mufflers[i].setRevFeedback(mufflerFeedback);
            mufflers[i].setFwdFeedback(mufflerFeedback);
        }
        outlet.setRevFeedback(mufflerFeedback);
    }
    void setOutletSize(double f) {
        outletSize = f;
        outlet.setDelay(samplesInAir(f, sampleRate));
    }

    // outs[0] intake, outs[1] block vibrations, outs[2] tailpipe.
    void process(double* outs) {
        revIntakes = 0.0;
        vibrations = 0.0;
        fwdExtractors = 0.0;
        for (int i = 0; i < nCylinders; i++) {
            const double position = (i + 0.5) / (double)nCylinders;
            const double asym =
                0.5 * asymmetry * std::sin(kTwoPi * position) / (double)nCylinders;
            const double ph = std::fmod(phase + position + asym, 1.0);
            const double spark =
                std::sin(kTwoPi * ph / sparkTime) * (ph < sparkTime ? 1.0 : 0.0) * throttle;
            double pressure, inValve, outValve;
            cycle(ph, &pressure, &inValve, &outValve);
            const double chamber =
                1.0 - (pressure * 0.5 + 0.5) * (1.0 - 1.0 / compressionRatio);
            const double inValveFeed = inValve * jointFeed() + (1.0 - inValve) * metalFeed();
            const double outValveFeed = outValve * jointFeed() + (1.0 - outValve) * metalFeed();
            cylinders[i].setDelay(cylinderSize * chamber);
            intakes[i].setFwdFeedback(inValveFeed);
            cylinders[i].setRevFeedback(inValveFeed);
            cylinders[i].setFwdFeedback(outValveFeed);
            extractors[i].setRevFeedback(outValveFeed);
            // intakes
            intakes[i].process(inValve * air.process(rng.white()), cylinders[i].revOut());
            revIntakes += intakes[i].revOut();
            // cylinders
            cylinders[i].process(spark + pressure + intakes[i].fwdOut(),
                                 extractors[i].revOut());
            vibrations += pressure + inValve + outValve + spark;
            // extractors
            extractors[i].process(cylinders[i].fwdOut(), exhaust.revOut() / nCylinders);
            fwdExtractors += extractors[i].fwdOut();
        }
        vibrations = walls.process(vibrations);
        exhaust.process(fwdExtractors, revMufflers);
        // backfiring
        const double bf =
            std::sin(kTwoPi * phase / sparkTime) * (phase < sparkTime ? 1.0 : 0.0);
        phase = phase + rpm / step / sampleRate;
        if (phase > 1.0) {
            isBackfiring = rng.frand() < backfireRate * (isRevvingDown ? 1.0 : 0.0);
            if (isBackfiring) backfireRate *= backfire;
        }
        phase = std::fmod(phase, 1.0);
        // mufflers
        revMufflers = 0.0;
        fwdMufflers = 0.0;
        for (int i = 0; i < kNMufflers; i++) {
            mufflers[i].process(
                exhaust.fwdOut() / kNMufflers + bf * (isBackfiring ? 1.0 : 0.0),
                outlet.revOut() / kNMufflers);
            revMufflers += mufflers[i].revOut();
            fwdMufflers += mufflers[i].fwdOut();
        }
        outlet.process(fwdMufflers, 0.0);
        fwdOutlet = outlet.fwdOut();
        outs[0] = intakeDC.process(revIntakes);
        outs[1] = vibrationsDC.process(vibrations);
        outs[2] = outletDC.process(fwdOutlet);
    }

    // ── internals ───────────────────────────────────────────────────────────
    // Pipes are staggered around the nominal length so the cylinders do not
    // all resonate at once: -1/12, +1/12, -2/12, +2/12 ...
    static double spreadCoeff(int i) {
        const int sign = (i % 2) * 2 - 1;
        return sign * std::floor(i / 2 + 1) / (double)kMaxCylinders;
    }

    void cycle(double ph, double* pressure, double* inValve, double* outValve) const {
        if (twoStrokeCycle) {
            const double w = kTwoPi * ph;
            *pressure = std::cos(w);
            *inValve = fclip(scale(*pressure, -0.33, -1.0, 0.0, 1.0, 1.0), 0.0, 1.0);
            *outValve = fclip(scale(*pressure, 0.34, -1.0, 0.0, 2.0, 1.0), 0.0, 1.0);
        } else {
            const double w = 2.0 * kTwoPi * ph;
            *pressure = std::cos(w);
            *inValve = std::sin(w) * (ph > 0.5 && ph < 0.75 ? 1.0 : 0.0);
            *outValve = -std::sin(w) * (ph > 0.25 && ph < 0.5 ? 1.0 : 0.0);
        }
    }
};

}  // namespace sdt
