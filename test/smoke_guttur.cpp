// smoke_guttur — offline sanity checks for the guttur module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/guttur.cpp"

// A bounded, non-NaN output is not enough for a feedback module: a diverged
// loop rails at the clamp and reads as "bounded", and an engine that has
// fallen into a silent fixed point still passes an RMS check if the window
// includes its opening transient. Measure both failure modes directly.
struct Meter {
    long n = 0, nans = 0, railed = 0;
    double sum2 = 0;
    float peak = 0;
    // dead air: 50 ms blocks whose rms is below -60 dBFS (0.01 V of 5 V)
    long blocks = 0, quiet = 0;
    double bsum2 = 0;
    long bn = 0;
    static const long kBlock = (long)(0.05 * 48000);

    void add(float v) {
        if (!std::isfinite(v)) { nans++; v = 0.f; }
        if (std::fabs(v) >= 9.99f) railed++;
        sum2 += (double) v * v;
        peak = std::max(peak, std::fabs(v));
        n++;
        bsum2 += (double) v * v;
        if (++bn == kBlock) {
            if (std::sqrt(bsum2 / bn) < 0.01) quiet++;
            blocks++; bsum2 = 0; bn = 0;
        }
    }
    double rms() const { return n ? std::sqrt(sum2 / n) : 0.0; }
    double deadFrac() const { return blocks ? (double) quiet / blocks : 1.0; }
    double railFrac() const { return n ? (double) railed / n : 0.0; }
};

// advance without measuring (settle after a parameter change)
static void skip(Guttur& m, long& frame, double secs) {
    for (long i = 0; i < (long)(secs * SR); i++) m.process(makeArgs(frame++));
}

static void run(Guttur& m, long& frame, Meter& l, double secs) {
    for (long i = 0; i < (long)(secs * SR); i++) {
        m.process(makeArgs(frame++));
        l.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
    }
}

static void testGuttur() {
    {
        // Defaults must run *continuously*: the forcing sine is ~700 Hz, so
        // the resonator banks stay excited. When it was left sub-audio the
        // engine was silent 85% of the time and only the ignition transient
        // and slow re-ignitions were audible.
        Guttur m;
        long frame = 0;
        skip(m, frame, 1);   // discard the ignition transient
        Meter s, d;
        for (long i = 0; i < (long)(20 * SR); i++) {
            m.process(makeArgs(frame++));
            s.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
            d.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
        }
        report("guttur", "nans", s.nans + d.nans, s.nans + d.nans == 0);
        report("guttur", "self_start_rms", s.rms(), s.rms() > 0.05);
        report("guttur", "dead_air_frac", s.deadFrac(), s.deadFrac() < 0.05);
        report("guttur", "rail_frac", s.railFrac(), s.railFrac() < 0.001);
        report("guttur", "peak", s.peak, s.peak <= 10.01f);
        report("guttur", "duff_alive", d.rms(), d.rms() > 0.01);
        report("guttur", "duff_bounded", d.peak, d.peak <= 10.01f);
    }

    Guttur m;
    long frame = 0;
    skip(m, frame, 2);

    // reset trigger: state must clear and the sound re-ignite, no NaNs
    m.inputs[Guttur::RESET_INPUT].channels = 1;
    m.inputs[Guttur::RESET_INPUT].setVoltage(10.f);
    for (int i = 0; i < 64; i++) m.process(makeArgs(frame++));
    m.inputs[Guttur::RESET_INPUT].setVoltage(0.f);
    Meter r;
    run(m, frame, r, 4);
    report("guttur", "reset_reignites", r.rms(), r.nans == 0 && r.rms() > 0.05);

    // hostile: everything cranked + spread + fastest glide preset sweeps
    m.params[Guttur::DRIVE_PARAM].setValue(10.f);
    m.params[Guttur::RATE_PARAM].setValue(10.f);
    m.params[Guttur::Q_PARAM].setValue(1.f);
    m.params[Guttur::LEVEL_PARAM].setValue(3.5f);
    m.params[Guttur::SPREAD_PARAM].setValue(1.f);
    m.params[Guttur::GAINA_PARAM].setValue(2.f);
    m.params[Guttur::GAINB_PARAM].setValue(2.f);
    Meter h;
    for (int i = 0; i < (int)(10 * SR); i++) {
        if (i % (int)SR == 0) {
            // stomp through the factory banks while everything is hot
            m.params[Guttur::BANKA_PARAM].setValue(1.f + (i / (int)SR) * 2.f);
            m.params[Guttur::BANKB_PARAM].setValue(20.f - (i / (int)SR) * 2.f);
        }
        m.process(makeArgs(frame++));
        h.add(m.outputs[Guttur::OUT_OUTPUT].getVoltage());
        h.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
    }
    report("guttur", "hostile_nans", h.nans, h.nans == 0);
    report("guttur", "hostile_bounded", h.peak, h.peak <= 10.01f);
    // even flat out the loop must not diverge and sit on the clamp
    report("guttur", "hostile_rail_frac", h.railFrac(), h.railFrac() < 0.05);

    // raw-Duffing mode ("snazzy clicks"): bounded by the unit clip, alive
    m.params[Guttur::FILT_PARAM].setValue(0.f);
    m.params[Guttur::LEVEL_PARAM].setValue(1.4f);
    skip(m, frame, 1);
    Meter raw;
    run(m, frame, raw, 4);
    report("guttur", "raw_nans", raw.nans, raw.nans == 0);
    report("guttur", "raw_alive", raw.rms(), raw.rms() > 0.01);
    report("guttur", "raw_bounded", raw.peak, raw.peak <= 10.01f);

    // Every distortion type must keep the feedback loop stable. The tanh
    // approximation is unbounded past |v| = 3 and used to diverge here,
    // railing the output at a full-scale square wave -- which a peak-only
    // check happily passes, so assert on rail fraction and dead air too.
    m.params[Guttur::FILT_PARAM].setValue(1.f);
    m.params[Guttur::DRIVE_PARAM].setValue(0.2f);
    m.params[Guttur::RATE_PARAM].setValue(5.f);
    m.params[Guttur::Q_PARAM].setValue(0.430777f);
    m.params[Guttur::SPREAD_PARAM].setValue(0.f);
    m.params[Guttur::GAINA_PARAM].setValue(1.f);
    m.params[Guttur::GAINB_PARAM].setValue(0.f);
    for (int type = 0; type <= 5; type++) {
        m.params[Guttur::DIST_PARAM].setValue((float) type);
        skip(m, frame, 1);   // settle after the switch
        Meter d;
        run(m, frame, d, 3);
        char name[64];
        snprintf(name, sizeof name, "dist%d_stable", type);
        report("guttur", name, d.railFrac(),
               d.nans == 0 && d.peak <= 10.01f && d.railFrac() < 0.01
                   && d.deadFrac() < 0.05);
    }
}

SMOKE_MAIN(testGuttur)
