// smoke_guttur — offline sanity checks for the guttur module.
// See smoke_harness.hpp for the shared scaffolding and CSV format.

#include "smoke_harness.hpp"
#include "../src/guttur.cpp"
#include <vector>
#include <complex>

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

// Is the engine wandering, or locked onto a fixed point?
//
// A Duffing oscillator either explores its attractor or settles into a
// periodic orbit, and guttur has large parameter regions that lock. A locked
// state passes every check above — alive, bounded, not dead, not railed —
// but it is a static drone, not gutter synthesis. Shipping the default in
// one of those regions is a real regression (it happened: muting bank B put
// the default there), so measure movement directly.
//
// Spectral flux: mean L1 distance between successive normalized magnitude
// spectra. A fixed tone holds its spectrum and scores ~0; a wandering one
// scores ~0.9. Zero-crossing statistics were tried first and do not
// discriminate at all (0.065 locked vs 0.070 wandering).
struct Flux {
    static const size_t N = 1024;
    std::vector<float> x;
    void add(float v) { x.push_back(v); }

    static void fft(std::vector<std::complex<double>>& a) {
        size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; i++) {
            size_t b = n >> 1;
            for (; j & b; b >>= 1) j ^= b;
            j ^= b;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t l = 2; l <= n; l <<= 1) {
            double an = -2 * M_PI / l;
            std::complex<double> wl(std::cos(an), std::sin(an));
            for (size_t i = 0; i < n; i += l) {
                std::complex<double> w(1);
                for (size_t k = 0; k < l / 2; k++) {
                    auto u = a[i + k], v = a[i + k + l / 2] * w;
                    a[i + k] = u + v; a[i + k + l / 2] = u - v; w *= wl;
                }
            }
        }
    }

    double value() const {
        std::vector<std::vector<double>> spec;
        for (size_t off = 0; off + N <= x.size(); off += N) {
            std::vector<std::complex<double>> buf(N);
            double e = 0;
            for (size_t i = 0; i < N; i++) {
                e += (double) x[off + i] * x[off + i];
                buf[i] = x[off + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / (N - 1)));
            }
            if (std::sqrt(e / N) < 0.01) continue;   // silent frame, no spectrum
            fft(buf);
            std::vector<double> p(N / 2);
            double tot = 0;
            for (size_t k = 1; k < N / 2; k++) { p[k] = std::abs(buf[k]); tot += p[k]; }
            if (tot <= 0) continue;
            for (size_t k = 1; k < N / 2; k++) p[k] /= tot;
            spec.push_back(p);
        }
        if (spec.size() < 2) return 0;
        double fl = 0;
        for (size_t i = 1; i < spec.size(); i++) {
            double d = 0;
            for (size_t k = 1; k < N / 2; k++) d += std::fabs(spec[i][k] - spec[i-1][k]);
            fl += d;
        }
        return fl / (spec.size() - 1);
    }
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
        Flux fx;
        for (long i = 0; i < (long)(20 * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Guttur::OUT_OUTPUT].getVoltage();
            s.add(v); fx.add(v);
            d.add(m.outputs[Guttur::DUFF_OUTPUT].getVoltage());
        }
        report("guttur", "nans", s.nans + d.nans, s.nans + d.nans == 0);
        report("guttur", "self_start_rms", s.rms(), s.rms() > 0.05);
        report("guttur", "dead_air_frac", s.deadFrac(), s.deadFrac() < 0.05);
        report("guttur", "rail_frac", s.railFrac(), s.railFrac() < 0.001);
        report("guttur", "peak", s.peak, s.peak <= 10.01f);
        report("guttur", "duff_alive", d.rms(), d.rms() > 0.01);
        report("guttur", "duff_bounded", d.peak, d.peak <= 10.01f);
        // the defaults must land in a wandering region, not a locked one
        double flux = fx.value();
        report("guttur", "default_not_static", flux, flux > 0.30);
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
    m.params[Guttur::GAINB_PARAM].setValue(1.f);
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

// The reported hard latch: with filters ON the module went permanently
// silent, and neither a parameter change, Randomize, nor Initialize brought
// it back -- only switching filters off, which bypasses the oversampler
// entirely. Cause: duffX is a raw Duffing state that transiently reaches
// ~1e46, the (float) cast into the oversampler yields inf, and the
// anti-imaging/anti-aliasing filters are recursive, so their state latched.
// AAFilter::reset() only rewrote coefficients and never cleared that state,
// so re-init could not recover either.
static void testGutturLatch() {
    // 1. the engine must survive an inf forced through the oversampler
    {
        Guttur m;
        long frame = 0;
        skip(m, frame, 1);
        m.engine.oversample.upsample(INFINITY);
        (void) m.engine.oversample.downsample();
        Meter after;
        run(m, frame, after, 3);
        report("guttur", "survives_inf_in_oversampler", after.rms(),
               after.nans == 0 && after.rms() > 0.05);
    }
    // 2. and a poisoned oversampler must be cleared by reset(), not just
    //    have its coefficients rewritten
    {
        VariableOversampling<> os;
        os.reset(48000.f);
        os.setOversamplingIndex(1);
        os.upsample(INFINITY);
        (void) os.downsample();
        os.reset(48000.f);            // must scrub the IIR state
        os.upsample(1.f);
        float y = os.downsample();
        report("guttur", "oversampler_reset_clears_state", y,
               std::isfinite(y));
    }
    // 3. duffX must never reach the oversampler as a non-finite float, at
    //    default settings or under the curated randomize ranges
    {
        Guttur m;
        long frame = 0;
        Meter all;
        auto draw = [](float lo, float hi) {
            return lo + rack::random::uniform() * (hi - lo);
        };
        for (int r = 0; r < 12; r++) {
            m.params[Guttur::DRIVE_PARAM].setValue(draw(0.5f, 2.5f));
            m.params[Guttur::GAINA_PARAM].setValue(draw(1.f, 2.f));
            m.params[Guttur::GAINB_PARAM].setValue(draw(1.f, 2.f));
            m.params[Guttur::LEVEL_PARAM].setValue(draw(1.4f, 3.f));
            m.params[Guttur::DAMP_PARAM].setValue(draw(0.f, 0.55f));
            m.params[Guttur::Q_PARAM].setValue(draw(0.f, 0.55f));
            m.params[Guttur::PITCH_PARAM].setValue(draw(0.15f, 1.f));
            m.params[Guttur::RATE_PARAM].setValue(draw(1.f, 10.f));
            m.params[Guttur::SMOOTH_PARAM].setValue(draw(0.f, 4.f));
            run(m, frame, all, 1);
        }
        // the module may legitimately be quiet in some of these states, but
        // it must never be latched: every sample finite, nothing railed
        report("guttur", "randomize_no_latch", all.nans + all.railed,
               all.nans == 0 && all.railFrac() < 0.02);
    }
}

SMOKE_MAIN(testGuttur, testGutturLatch)
