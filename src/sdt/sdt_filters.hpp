// sdt_filters.hpp — the filter and delay primitives the SDT models are built
// from: one-pole, DC blocker, first-order allpass, the fractional delay line
// and the bidirectional waveguide section.
//
// Ported from SDTFilters.c (Sound Design Toolkit, GPL-3.0-or-later). Same
// difference equations; sample rate is per object rather than global, and the
// delay buffer is a std::vector sized once at construction.
#pragma once

#include <vector>

#include "sdt_common.hpp"

namespace sdt {

// y[n] = b0*x[n] - a1*y[n-1]
struct OnePole {
    double b0 = 1.0, a1 = 0.0, y1 = 0.0;

    void setFeedback(double f) {
        a1 = fclip(f, -1.0, 1.0);
        b0 = 1.0 - std::fabs(a1);
    }
    void lowpass(double hz, double sampleRate) {
        const double w = fclip(hz / sampleRate, 0.0, 0.5);
        a1 = -std::exp(-kTwoPi * w);
        b0 = 1.0 + a1;
    }
    void highpass(double hz, double sampleRate) {
        const double w = fclip(hz / sampleRate, 0.0, 0.5);
        a1 = std::exp(-kTwoPi * (0.5 - w));
        b0 = 1.0 - a1;
    }
    double process(double in) {
        y1 = b0 * in - a1 * y1;
        return y1;
    }
    void reset() { y1 = 0.0; }
};

// H(z) = g * (1 - z^-1) / (1 - fb*z^-1), with g the makeup gain that keeps
// Nyquist at unity.
struct DCFilter {
    double a_ = 1.0, g = 0.5, y = 0.0;

    void setFeedback(double f) {
        a_ = fclip(1.0 - f, kMicro, 1.0);
        g = 1.0 - a_ / 2.0;
    }
    void setFrequency(double hz, double sampleRate) {
        const double w = hz * kTwoPi / sampleRate;
        const double cosw = std::cos(w);
        setFeedback(cosw < kMicro ? 1.0 : (1.0 - std::fabs(std::sin(w))) / cosw);
    }
    double process(double in) {
        const double out = g * in - y;
        y = a_ * out + y;
        return out;
    }
    void reset() { y = 0.0; }
};

// First-order allpass, used as the fractional part of the delay line.
struct AllPass {
    double a = 0.0, x1 = 0.0, y1 = 0.0;

    void setFeedback(double f) { a = fclip(f, -1.0, 1.0); }
    double process(double in) {
        y1 = a * in + x1 - a * y1;
        x1 = in;
        return y1;
    }
    void reset() { x1 = y1 = 0.0; }
};

// Fractional delay: integer part by index, fractional part by allpass. The
// read pointer only moves at 16-sample boundaries and the two pointers
// crossfade, which is what lets the motor sweep its cylinder length every
// sample without crackling.
struct Delay {
    std::vector<double> buf;
    AllPass filters[2];
    double fade[16];
    double feedback = 0.0;
    long size = 1, head = 0, read[2] = {0, 0}, delay = 0;
    int count = 0, curr = 0;

    void init(long maxDelay) {
        if (maxDelay < 1) maxDelay = 1;
        size = maxDelay;
        buf.assign((size_t)size, 0.0);
        for (int i = 0; i < 16; i++) fade[i] = i < 5 ? 0.0 : 0.1 * (i - 5.0);
        head = read[0] = read[1] = 0;
        count = curr = 0;
        delay = 0;
        feedback = 0.0;
        filters[0].reset();
        filters[1].reset();
    }

    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0);
        head = 0;
        filters[0].reset();
        filters[1].reset();
    }

    double getDelay() const { return (double)delay + 0.618; }

    void setDelay(double f) {
        f = fclip(f, 0.618, (double)size);
        delay = (long)(f - 0.618);
        const double d = f - (double)delay;
        feedback = (1.0 - d) / (1.0 + d);
    }

    double process(double in) {
        buf[(size_t)head] = in;
        if (count == 0) {
            curr ^= 1;
            read[curr] = (size + head - delay) % size;
            filters[curr].setFeedback(feedback);
        }
        const int i = curr, j = curr ^ 1;
        const long ri = read[i], rj = read[j];
        const double yi = filters[i].process(buf[(size_t)ri]);
        const double yj = filters[j].process(buf[(size_t)rj]);
        const double gi = fade[count], gj = 1.0 - gi;
        const double out = gi * yi + gj * yj;
        head = (head + 1) % size;
        read[i] = (ri + 1) % size;
        read[j] = (rj + 1) % size;
        count = (count + 1) % 16;
        return out;
    }
};

// A tube section: forward and reverse delay lines with a reflection at each
// end. fwdFeed/revFeed gains are the reflection coefficients, and what is not
// reflected is transmitted (thruGain = 1 - |feedGain|).
struct Waveguide {
    Delay fwdDelay, revDelay;
    double fwdFeedGain = 0.0, revFeedGain = 0.0;
    double fwdThruGain = 1.0, revThruGain = 1.0;
    double fwdIn = 0.0, fwdFeed = 0.0, fwdThru = 0.0;
    double revIn = 0.0, revFeed = 0.0, revThru = 0.0;

    void init(long maxDelay) {
        fwdDelay.init(maxDelay);
        revDelay.init(maxDelay);
        fwdIn = fwdFeed = fwdThru = revIn = revFeed = revThru = 0.0;
    }
    void clear() {
        fwdDelay.clear();
        revDelay.clear();
        fwdIn = fwdFeed = fwdThru = revIn = revFeed = revThru = 0.0;
    }
    double getDelay() const { return fwdDelay.getDelay(); }
    void setDelay(double f) {
        fwdDelay.setDelay(f);
        revDelay.setDelay(f);
    }
    void setFwdFeedback(double f) {
        fwdFeedGain = fclip(f, -1.0, 1.0);
        fwdThruGain = 1.0 - std::fabs(fwdFeedGain);
    }
    void setRevFeedback(double f) {
        revFeedGain = fclip(f, -1.0, 1.0);
        revThruGain = 1.0 - std::fabs(revFeedGain);
    }
    double fwdOut() const { return fwdThru; }
    double revOut() const { return revThru; }

    void process(double fi, double ri) {
        fwdIn = fi + revFeedGain * revFeed;
        revIn = ri + fwdFeedGain * fwdFeed;
        fwdFeed = fwdDelay.process(fwdIn);
        revFeed = revDelay.process(revIn);
        fwdThru = fwdThruGain * fwdFeed;
        revThru = revThruGain * revFeed;
    }
};

}  // namespace sdt
