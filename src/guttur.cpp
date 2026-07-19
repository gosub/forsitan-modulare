// guttur.cpp — VCV Rack 2 module
// guttur (Latin: "throat" — the guttural voice) is a port of Gutter
// Synthesis, Tom Mudd's chaotic resonator instrument: a forced, damped
// Duffing oscillator whose forcing loop runs *through* two banks of 24
// resonant bandpass biquads. The filters are not a post-effect — their
// summed output feeds back into the Duffing derivative, so oscillator
// and resonators form one coupled chaotic system.
//
//   duffX ─> [bank A: 24 BP biquads] ─┐
//        ─> [bank B: 24 BP biquads] ─┼─> finalY ──> OUT (×0.125)
//        ^                            │
//        └── Duffing step <── sin forcing (or audio IN) <┘
//            duffX' = (finalY + dx − duffX)/smooth, then distortion
//
// Ported from:
//  - gutterOsc.java, Tom Mudd (github.com/tommmmudd/guttersynthesis, GPL-3)
//  - guttersynth-sc, Mads Kjeldgaard & Scott Carver
//    (github.com/madskjeldgaard/guttersynth-sc, GPL-3), whose oversampling
//    headers are Jatin Chowdhury's, from ChowDSP-VCV (src/guttur/).
//
// Faithful quirks (they ARE the sound, see doc/guttur.md):
//  - biquad norm uses Q[filter] but a0/b2 use Q[bank] (Java Q[i] vs Q[j]);
//  - the Q array is shared between the two banks;
//  - the "lowpass" (new−old)/smooth is part differentiator, not a one-pole;
//  - output taps the filter sum pre-distortion (distortion shapes the
//    feedback state, not the output);
//  - filter tuning uses tan(pi·f/Fs) as in the Java. (The SC port passes
//    pi·f/Fs to a fasttan() that already multiplies by pi internally,
//    mistuning every filter a factor of pi up — not replicated.)
//
// The forcing sine runs at omega·dt·44100/(2·pi) Hz — TONE and RATE multiply
// into one frequency. Below ~10 Hz the banks stop being excited, duffX parks
// on a DC drift the bandpasses reject, and the module goes silent between
// surges; the defaults (omega 0.02, dt 5, ~700 Hz) match the originals.
//
// Rack-native additions: 20 factory banks (Tom Mudd's filters.txt) with
// glided morphing between them, PITCH (V/oct) and master Q macros, SPREAD
// per-filter scatter (seeded, survives save/load), runtime-switchable
// distortion, DUFF chaotic aux output, reset trigger, audio-input forcing
// auto-engaged by the IN cable, oversampled distortion (menu, default 2×).

#include "forsitan.hpp"
#include "guttur/VariableOversampling.hpp"
#include "guttur/guttur_banks.hpp"
#include <cmath>

namespace guttur_dsp {

// ---------------------------------------------------------------- utility

// SC zapgremlins semantics: NaN/inf, huge values and denormals go to 0.
inline double zapgremlins(double x) {
    double absx = std::fabs(x);
    return (absx > 1e-15 && absx < 1e15) ? x : 0.0;
}

// DC blocker, r = 0.995 (CCRMA), as in the SC port's dcblocker.h
struct Dcblocker {
    float xm1 = 0.f, ym1 = 0.f;
    float process(float x) {
        float y = x - xm1 + 0.995f * ym1;
        xm1 = x;
        ym1 = y;
        return y;
    }
    void reset() { xm1 = ym1 = 0.f; }
};

// ------------------------------------------------------------- distortion
// Types 0-4 as documented for the SC port (its enum had an accidental
// pass-through at index 2; we use the intended five).

// The musicdsp fast atan approximation. Note it is NOT monotonic: it peaks
// at x = 1/sqrt(0.28) = 1.890 and then decays back toward zero, so used as a
// waveshaper it folds rather than clips. Kept for the types that want that.
inline double fastatan(double x) { return x / (1.0 + 0.28 * (x * x)); }

inline double distortion(double v, int type) {
    switch (type) {
        case 0:   // hard clip
            return std::fmax(std::fmin(v, 1.0), -1.0);
        case 1:   // variable-hardness clip, shape = 3
            // musicdsp normalizes this by fastatan(shape) so that input 1
            // maps to output 1, which leaves a small-signal gain of
            // 3/fastatan(3) = 3.52 — 11 dB of extra gain injected straight
            // into the feedback loop. Normalize by the shape instead, for
            // unity gain at the origin, and use the exact atan so the curve
            // saturates instead of folding back at |v| > 0.63.
            return std::atan(v * 3.0) / 3.0;
        case 2:   // fast atan — folds back above |v| = 1.89 (see above)
            return fastatan(v);
        case 3: { // atan approximation (kvraudio)
            if (std::fabs(v) < 1e-12)
                return 0.0;   // the expression is 0/0 at v=0 (limit is 0)
            double w = v * 1.3;
            return 0.75 * (std::sqrt(w * w + 1.0) * 1.65 - 1.65) / v;
        }
        case 4: { // tanh approximation (kvraudio)
            // The rational approximation only tracks tanh over roughly
            // |v| <= 3; beyond that it turns around and grows as 0.1076*v,
            // so in the feedback loop it diverges (|finalY| hit 1e13 within
            // 100 ms and railed the output). Clamp to its valid range, where
            // it is monotonic and saturates at ~0.989.
            double w = std::fmax(std::fmin(v, 3.0), -3.0);
            return (0.1076 * w * w * w + 3.029 * w) / (w * w + 3.124);
        }
        case 5:   // exact atan, as Tom Mudd's Java used before the SC port
            return std::atan(v);   // swapped fastatan() in
        default:
            return v;
    }
}

// ---------------------------------------------------------------- engine
// Faithful transcription of GutterState + CalcCoeffs + next() from the SC
// port (variable names kept diffable), with per-filter gains added to mute
// the padded tail of short factory banks.

struct Engine {
    static const int bankCount = 2;
    static const int filterCount = BANK_FILTERS;

    double a0[2][BANK_FILTERS], a1[2][BANK_FILTERS], a2[2][BANK_FILTERS];
    double b1[2][BANK_FILTERS], b2[2][BANK_FILTERS];
    double filterFreqs[2][BANK_FILTERS];
    double Q[BANK_FILTERS];                  // shared across banks (quirk)
    double filterGain[2][BANK_FILTERS];      // 1 = active, 0 = padded slot
    double prevX1[2][BANK_FILTERS], prevX2[2][BANK_FILTERS];
    double prevY1[2][BANK_FILTERS], prevY2[2][BANK_FILTERS];
    double y[2][BANK_FILTERS];

    double gains[2];
    double Fs, singleGain;
    double smoothing;
    double duffX, duffY, dx, dy;
    double gamma, omega, c, t, dt;
    double tScale;   // 44100/Fs: keeps the sine forcing SR-invariant
    double finalY;
    bool filtersOn;
    bool enableAudioInput;
    int distType;

    VariableOversampling<> oversample;

    void resetDuff() {
        duffX = duffY = dx = dy = t = 0.0;
    }

    void init(double sampleRate) {
        Fs = sampleRate;
        tScale = 44100.0 / Fs;
        filtersOn = true;
        enableAudioInput = false;
        singleGain = 0.0;
        smoothing = 1.0;
        distType = 1;
        for (int bank = 0; bank < bankCount; bank++) {
            gains[bank] = 1.0;
            for (int f = 0; f < filterCount; f++) {
                filterFreqs[bank][f] = 100.0 + 10.0 * f;
                filterGain[bank][f] = 1.0;
                y[bank][f] = prevX1[bank][f] = prevX2[bank][f] = 0.0;
                prevY1[bank][f] = prevY2[bank][f] = 0.0;
            }
        }
        for (int f = 0; f < filterCount; f++)
            Q[f] = 30.0;
        resetDuff();
        gamma = 0.2;
        omega = 0.02;
        c = 0.01;
        dt = 5.0;
        calcCoeffs();
        oversample.reset((float) sampleRate);
    }

    void calcCoeffs() {
        for (int bank = 0; bank < bankCount; bank++) {
            for (int f = 0; f < filterCount; f++) {
                double K = std::tan(M_PI * filterFreqs[bank][f] / Fs);
                double norm = 1.0 / (1.0 + K / Q[f] + K * K);
                // quirk: a0/b2 use the BANK-indexed Q (Java Q[i]), norm the
                // filter-indexed one — kept faithfully
                a0[bank][f] = K / Q[bank] * norm;
                a1[bank][f] = 0.0;
                a2[bank][f] = -a0[bank][f];
                b1[bank][f] = 2.0 * (K * K - 1.0) * norm;
                b2[bank][f] = (1.0 - K / Q[bank] + K * K) * norm;
            }
        }
    }

    // One sample of the coupled system. Returns the audio out (unit range,
    // pre-distortion filter tap); duffX holds the chaotic aux after.
    float processSample(double inputSample) {
        finalY = 0.0;
        if (filtersOn) {
            for (int bank = 0; bank < bankCount; bank++) {
                for (int f = 0; f < filterCount; f++) {
                    y[bank][f] = a0[bank][f] * duffX
                               + a1[bank][f] * prevX1[bank][f]
                               + a2[bank][f] * prevX2[bank][f]
                               - b1[bank][f] * prevY1[bank][f]
                               - b2[bank][f] * prevY2[bank][f];
                    prevX2[bank][f] = zapgremlins(prevX1[bank][f]);
                    prevX1[bank][f] = zapgremlins(duffX);
                    prevY2[bank][f] = zapgremlins(prevY1[bank][f]);
                    prevY1[bank][f] = zapgremlins(y[bank][f]);
                    finalY += zapgremlins(y[bank][f] * gains[bank]
                                          * singleGain * filterGain[bank][f]);
                }
            }
        }
        else {   // filters disabled: pass the state directly
            finalY = duffX;
        }

        // Duffing derivative, forced by audio input or the internal sine
        if (enableAudioInput)
            dy = finalY - (finalY * finalY * finalY) - (c * duffY)
               + gamma * inputSample;
        else
            dy = finalY - (finalY * finalY * finalY) - (c * duffY)
               + gamma * std::sin(omega * t);

        duffY += dy;
        dx = duffY;
        // the "lowpass": part differentiator, part attenuator — verbatim
        duffX = (finalY + dx - duffX) / smoothing;

        float out;
        if (filtersOn) {
            // distortion shapes the feedback state, oversampled
            oversample.upsample((float) duffX);
            float* osBuffer = oversample.getOSBuffer();
            for (int k = 0; k < oversample.getOversamplingRatio(); k++)
                osBuffer[k] = (float) distortion(osBuffer[k], distType);
            duffX = oversample.downsample();
            // output the value from the filter, not from the distortion
            out = (float) (finalY * 0.125);
        }
        else {   // raw-Duffing mode: reset re-ignites ("snazzy clicks")
            duffX = std::fmax(std::fmin(duffX, 100.0), -100.0);
            if (std::fabs(duffX) > 99.0)
                resetDuff();
            out = (float) std::fmax(std::fmin(duffX * singleGain, 1.0), -1.0);
        }

        t += dt * tScale;

        if (!std::isfinite(duffX))
            resetDuff();
        return out;
    }
};

// audio-rate one-pole parameter smoother (replaces SC's SlopeSignal)
struct Smoothed {
    double value = 0.0, target = 0.0, coef = 1.0;
    void setup(double v, double tauS, double rate) {
        value = target = v;
        coef = 1.0 - std::exp(-1.0 / (tauS * rate));
    }
    double tick() {
        value += (target - value) * coef;
        return value;
    }
};

} // namespace guttur_dsp

struct Guttur : Module {
    enum ParamId {
        DRIVE_PARAM,
        TONE_PARAM,
        DAMP_PARAM,
        RATE_PARAM,
        SMOOTH_PARAM,
        DRIVE_ATT_PARAM,
        TONE_ATT_PARAM,
        DAMP_ATT_PARAM,
        RATE_ATT_PARAM,
        BANKA_PARAM,
        BANKB_PARAM,
        PITCH_PARAM,
        Q_PARAM,
        SPREAD_PARAM,
        GAINA_PARAM,
        GAINB_PARAM,
        LEVEL_PARAM,
        DIST_PARAM,
        FILT_PARAM,
        RESET_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        DRIVE_CV_INPUT,
        TONE_CV_INPUT,
        DAMP_CV_INPUT,
        RATE_CV_INPUT,
        BANKA_CV_INPUT,
        BANKB_CV_INPUT,
        PITCH_CV_INPUT,
        Q_CV_INPUT,
        GAINA_CV_INPUT,
        GAINB_CV_INPUT,
        AUDIO_INPUT,
        RESET_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_OUTPUT,
        DUFF_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        OUT_LIGHT,
        DUFF_LIGHT,
        LIGHTS_LEN
    };

    static constexpr int kControlDiv = 16;

    guttur_dsp::Engine engine;
    guttur_dsp::Dcblocker dcOut, dcDuff;
    guttur_dsp::Smoothed smGamma, smOmega, smC, smDt, smSmooth,
        smSingleGain, smGainA, smGainB;

    // bank/macro layer: slewed per-filter targets (§ the morphing glide)
    double freqTarget[2][guttur_dsp::BANK_FILTERS];
    double qTarget[guttur_dsp::BANK_FILTERS];
    double gainTarget[2][guttur_dsp::BANK_FILTERS];
    double glideCoef = 1.0;
    float glideMs = 100.f;

    // SPREAD scatter, deterministic from a serialized seed
    uint32_t spreadSeed = 1;
    double detune[2][guttur_dsp::BANK_FILTERS];
    double qScatter[guttur_dsp::BANK_FILTERS];

    int osIndex = 1;   // oversampling of the distortion stage, default 2×

    dsp::SchmittTrigger resetTrig;
    dsp::BooleanTrigger resetBtn;
    int controlPhase = 0;
    float sr = 0.f;    // engine (re)inits lazily when this diverges
    bool snapSlews = true;
    float outEnv = 0.f, duffEnv = 0.f;

    Guttur() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(DRIVE_PARAM, 0.f, 10.f, 0.2f, "Drive (gamma forcing)");
        // 0.5752575 puts omega at the original's 0.02; with dt = 5 that is a
        // ~700 Hz forcing sine. Below ~10 Hz the banks lose their excitation
        // and the whole engine falls silent between surges — see the header.
        configParam(TONE_PARAM, 0.f, 1.f, 0.5752575f, "Tone (forcing frequency)",
                    "", 1e4f, 1e-4f);
        configParam(DAMP_PARAM, 0.f, 1.f, 0.5f, "Damping (c)", "", 1e4f, 1e-4f);
        configParam(RATE_PARAM, 0.f, 10.f, 5.f, "Rate (dt time step)");
        configParam(SMOOTH_PARAM, 0.f, 5.f, 1.f, "Smooth (chaos lowpass)");
        configParam(DRIVE_ATT_PARAM, -1.f, 1.f, 0.f, "Drive CV amount", "%", 0.f, 100.f);
        configParam(TONE_ATT_PARAM, -1.f, 1.f, 0.f, "Tone CV amount", "%", 0.f, 100.f);
        configParam(DAMP_ATT_PARAM, -1.f, 1.f, 0.f, "Damping CV amount", "%", 0.f, 100.f);
        configParam(RATE_ATT_PARAM, -1.f, 1.f, 0.f, "Rate CV amount", "%", 0.f, 100.f);
        configParam(BANKA_PARAM, 1.f, 20.f, 6.f, "Bank A preset");
        configParam(BANKB_PARAM, 1.f, 20.f, 6.f, "Bank B preset");
        paramQuantities[BANKA_PARAM]->snapEnabled = true;
        paramQuantities[BANKB_PARAM]->snapEnabled = true;
        configParam(PITCH_PARAM, 0.f, 1.f, 0.812052f, "Pitch (freq multiplier)",
                    "x", 40.f, 0.05f);
        configParam(Q_PARAM, 0.f, 1.f, 0.430777f, "Resonance (master Q)",
                    "", 320.f, 2.5f);
        configParam(SPREAD_PARAM, 0.f, 1.f, 0.f, "Spread (per-filter scatter)",
                    "%", 0.f, 100.f);
        configParam(GAINA_PARAM, 0.f, 2.f, 1.f, "Bank A gain");
        configParam(GAINB_PARAM, 0.f, 2.f, 1.f, "Bank B gain");
        configParam(LEVEL_PARAM, 0.f, 3.5f, 1.4f, "Level (drive into the sum)");
        configSwitch(DIST_PARAM, 0.f, 5.f, 1.f, "Distortion",
                     {"Hard clip", "Soft clip", "Atan (folding)", "Atan approx",
                      "Tanh approx", "Atan (exact)"});
        configSwitch(FILT_PARAM, 0.f, 1.f, 1.f, "Filters",
                     {"Off (raw Duffing)", "On"});
        configButton(RESET_PARAM, "Reset chaos");
        configInput(DRIVE_CV_INPUT, "Drive CV");
        configInput(TONE_CV_INPUT, "Tone CV");
        configInput(DAMP_CV_INPUT, "Damping CV");
        configInput(RATE_CV_INPUT, "Rate CV");
        configInput(BANKA_CV_INPUT, "Bank A select CV (0-10V)");
        configInput(BANKB_CV_INPUT, "Bank B select CV (0-10V)");
        configInput(PITCH_CV_INPUT, "Pitch (1V/oct)");
        configInput(Q_CV_INPUT, "Resonance CV");
        configInput(GAINA_CV_INPUT, "Bank A gain CV");
        configInput(GAINB_CV_INPUT, "Bank B gain CV");
        configInput(AUDIO_INPUT, "Audio forcing (replaces the internal sine)");
        configInput(RESET_INPUT, "Reset trigger");
        configOutput(OUT_OUTPUT, "Audio");
        configOutput(DUFF_OUTPUT, "Duffing chaotic state (aux/mod)");
        configBypass(AUDIO_INPUT, OUT_OUTPUT);
        regenScatter();
    }

    void regenScatter() {
        // tiny LCG so the scatter is reproducible from the stored seed
        uint32_t s = spreadSeed;
        auto next = [&s]() {
            s = s * 1664525u + 1013904223u;
            return (double) (s >> 8) / 8388608.0 - 1.0;   // [-1, 1)
        };
        for (int b = 0; b < 2; b++)
            for (int f = 0; f < guttur_dsp::BANK_FILTERS; f++)
                detune[b][f] = next();
        for (int f = 0; f < guttur_dsp::BANK_FILTERS; f++)
            qScatter[f] = next();
    }

    void initEngine(float sampleRate) {
        sr = sampleRate;
        engine.init(sr);
        engine.oversample.setOversamplingIndex(osIndex);
        dcOut.reset();
        dcDuff.reset();
        double tau = 0.005;   // 5 ms, replaces SC's per-block SlopeSignal
        smGamma.setup(0.2, tau, sr);
        smOmega.setup(0.02, tau, sr);
        smC.setup(0.01, tau, sr);
        smDt.setup(5.0, tau, sr);
        smSmooth.setup(0.0, tau, sr);
        smSingleGain.setup(1.4, tau, sr);
        smGainA.setup(1.0, tau, sr);
        smGainB.setup(1.0, tau, sr);
        kickChaos();
        controlPhase = 0;
        snapSlews = true;
        setGlide(glideMs);
    }

    // The chaos does not self-start from exact zeros: with the forcing sine
    // at ~0.04 Hz the Duffing settles on a quiet fixed point and the
    // bandpass banks see DC. The SC port ignites because its parameter
    // slopes start from InitGutterState's values (omega 1.25, dt 1.0 —
    // audio-rate forcing) and glide to the user's settings within the first
    // block. Replicate that: start the smoothers at those init constants so
    // the first milliseconds sweep the forcing through the audio range.
    // Also fired by the RESET button/trigger — the "percussive re-ignition".
    void kickChaos() {
        smGamma.value = 0.1;
        smOmega.value = 1.25;
        smC.value = 0.3;
        smDt.value = 1.0;
    }

    void setGlide(float ms) {
        glideMs = ms;
        if (sr > 0.f)
            glideCoef = 1.0 - std::exp(-(double) kControlDiv / (sr * ms * 1e-3));
    }

    void onReset() override {
        sr = 0.f;   // force re-init on the next process()
    }

    void onRandomize(const RandomizeEvent& e) override {
        Module::onRandomize(e);
        spreadSeed = random::u32();
        regenScatter();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "spreadSeed", json_integer((json_int_t) spreadSeed));
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        json_object_set_new(root, "glideMs", json_real(glideMs));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "spreadSeed"))) {
            spreadSeed = (uint32_t) json_integer_value(j);
            regenScatter();
        }
        if ((j = json_object_get(root, "oversampling")))
            osIndex = clamp((int) json_integer_value(j), 0, 4);
        if ((j = json_object_get(root, "glideMs")))
            setGlide(json_real_value(j));
        engine.oversample.setOversamplingIndex(osIndex);
    }

    void setOversampling(int idx) {
        osIndex = clamp(idx, 0, 4);
        // buffers are preallocated; switching the index is audio-thread safe
        engine.oversample.setOversamplingIndex(osIndex);
    }

    int bankRow(int knobParam, int cvInput) {
        float v = params[knobParam].getValue() - 1.f
                + inputs[cvInput].getVoltage() * 1.9f;   // 0-10V spans the 20 rows
        return clamp((int) std::round(v), 0, guttur_dsp::BANK_ROWS - 1);
    }

    void updateControls() {
        using namespace guttur_dsp;

        // ---- chaos parameters: knob + attenuverted CV, then slewed
        float drive = params[DRIVE_PARAM].getValue()
                    + params[DRIVE_ATT_PARAM].getValue() * inputs[DRIVE_CV_INPUT].getVoltage();
        smGamma.target = clamp(drive, 0.f, 10.f);

        // tone/damp knobs live on an exponential 1e-4..1 map (4 decades)
        float tt = params[TONE_PARAM].getValue()
                 + params[TONE_ATT_PARAM].getValue() * inputs[TONE_CV_INPUT].getVoltage() * 0.1f;
        smOmega.target = 1e-4 * std::pow(10.0, 4.0 * clamp(tt, 0.f, 1.f));

        float dtp = params[DAMP_PARAM].getValue()
                  + params[DAMP_ATT_PARAM].getValue() * inputs[DAMP_CV_INPUT].getVoltage() * 0.1f;
        smC.target = 1e-4 * std::pow(10.0, 4.0 * clamp(dtp, 0.f, 1.f));

        float rate = params[RATE_PARAM].getValue()
                   + params[RATE_ATT_PARAM].getValue() * inputs[RATE_CV_INPUT].getVoltage();
        smDt.target = clamp(rate, 0.f, 10.f);

        smSmooth.target = params[SMOOTH_PARAM].getValue();
        smSingleGain.target = clamp(params[LEVEL_PARAM].getValue(), 0.f, 5.f);
        smGainA.target = clamp(params[GAINA_PARAM].getValue()
                               + inputs[GAINA_CV_INPUT].getVoltage() * 0.2f, 0.f, 2.f);
        smGainB.target = clamp(params[GAINB_PARAM].getValue()
                               + inputs[GAINB_CV_INPUT].getVoltage() * 0.2f, 0.f, 2.f);

        engine.filtersOn = params[FILT_PARAM].getValue() > 0.5f;
        engine.distType = (int) std::round(params[DIST_PARAM].getValue());
        engine.enableAudioInput = inputs[AUDIO_INPUT].isConnected();

        // ---- resonator banks: targets, then one-pole glide per filter
        int rowA = bankRow(BANKA_PARAM, BANKA_CV_INPUT);
        int rowB = bankRow(BANKB_PARAM, BANKB_CV_INPUT);
        int rows[2] = {rowA, rowB};

        double pitchMult = 0.05 * std::pow(40.0, (double) params[PITCH_PARAM].getValue());
        pitchMult *= std::pow(2.0, (double) inputs[PITCH_CV_INPUT].getVoltage());
        double qt = clamp(params[Q_PARAM].getValue()
                          + inputs[Q_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        double masterQ = 2.5 * std::pow(320.0, qt);
        double spread = params[SPREAD_PARAM].getValue();

        double fMax = 0.45 * engine.Fs;
        for (int b = 0; b < 2; b++) {
            const double* row = bankFreqs[rows[b]];
            int len = bankLen[rows[b]];
            for (int f = 0; f < BANK_FILTERS; f++) {
                double freq = row[f] * pitchMult * (1.0 + 0.15 * spread * detune[b][f]);
                freqTarget[b][f] = std::fmin(std::fmax(freq, 10.0), fMax);
                gainTarget[b][f] = (f < len) ? 1.0 : 0.0;
            }
        }
        for (int f = 0; f < BANK_FILTERS; f++) {
            double q = masterQ * (1.0 + 0.15 * spread * qScatter[f]);
            qTarget[f] = std::fmin(std::fmax(q, 0.5), 2000.0);
        }

        double k = snapSlews ? 1.0 : glideCoef;
        snapSlews = false;
        for (int b = 0; b < 2; b++) {
            for (int f = 0; f < BANK_FILTERS; f++) {
                engine.filterFreqs[b][f] += (freqTarget[b][f] - engine.filterFreqs[b][f]) * k;
                engine.filterGain[b][f] += (gainTarget[b][f] - engine.filterGain[b][f]) * k;
            }
        }
        for (int f = 0; f < BANK_FILTERS; f++)
            engine.Q[f] += (qTarget[f] - engine.Q[f]) * k;
        engine.calcCoeffs();

        // ---- reset (button or trigger): percussive re-ignition
        bool trig = resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
        bool btn = resetBtn.process(params[RESET_PARAM].getValue() > 0.5f);
        if (trig || btn) {
            engine.resetDuff();
            kickChaos();
        }
    }

    void process(const ProcessArgs& args) override {
        if (sr != args.sampleRate)
            initEngine(args.sampleRate);
        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        engine.gamma = smGamma.tick();
        engine.omega = smOmega.tick();
        engine.c = smC.tick();
        engine.dt = smDt.tick();
        engine.smoothing = 1.0 + smSmooth.tick();   // 0 = no smoothing
        engine.singleGain = smSingleGain.tick();
        engine.gains[0] = smGainA.tick();
        engine.gains[1] = smGainB.tick();

        float in = inputs[AUDIO_INPUT].getVoltage() * 0.2f;
        float out = engine.processSample(in);

        out = dcOut.process(out);
        outputs[OUT_OUTPUT].setVoltage(clamp(5.f * out, -10.f, 10.f));

        float duff = clamp((float) engine.duffX, -1.f, 1.f);
        duff = dcDuff.process(duff);
        outputs[DUFF_OUTPUT].setVoltage(clamp(5.f * duff, -10.f, 10.f));

        outEnv += (std::fabs(out) - outEnv) * 0.002f;
        duffEnv += (std::fabs(duff) - duffEnv) * 0.002f;
        lights[OUT_LIGHT].setBrightness(clamp(outEnv * 2.f, 0.f, 1.f));
        lights[DUFF_LIGHT].setBrightness(clamp(duffEnv, 0.f, 1.f));
    }
};

struct GutturWidget : ModuleWidget {
    GutturWidget(Guttur* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/guttur.svg")));

// @layout:begin guttur 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem DRIVE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TONE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DAMP_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem RATE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem SMOOTH_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DRIVE_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem TONE_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem DAMP_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem RATE_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem BANKA_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem BANKB_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem Q_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SPREAD_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem GAINA_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem GAINB_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DIST_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FILT_PARAM CKSS 2.3 param "" 0.0
// @elem RESET_PARAM TL1105 2.6 param "" 0.0
// @elem DRIVE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TONE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DAMP_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RATE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BANKA_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BANKB_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PITCH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem Q_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem GAINA_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem GAINB_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RESET_INPUT PJ301MPort 4.18 input "" 0.0
// @elem OUT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem DUFF_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem OUT_LIGHT SmallLight 1.5 light "" 0.0
// @elem DUFF_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 13.00 33.50
// @elem LABEL_TONE label 0.0 label "tone" 0.0 37.00 33.50
// @elem LABEL_DAMP label 0.0 label "damp" 0.0 61.00 33.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 85.00 33.50
// @elem LABEL_SMOOTH label 0.0 label "smooth" 0.0 109.00 33.50
// @elem LABEL_FILT label 0.0 label "filters" 0.0 109.00 53.70
// @elem LABEL_BANKA label 0.0 label "bank a" 0.0 13.00 71.50
// @elem LABEL_BANKB label 0.0 label "bank b" 0.0 37.00 71.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 61.00 71.50
// @elem LABEL_Q label 0.0 label "q" 0.0 85.00 71.50
// @elem LABEL_SPREAD label 0.0 label "spread" 0.0 109.00 71.50
// @elem LABEL_GAINA label 0.0 label "gain a" 0.0 13.00 102.50
// @elem LABEL_GAINB label 0.0 label "gain b" 0.0 37.00 102.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 61.00 102.50
// @elem LABEL_DIST label 0.0 label "dist" 0.0 85.00 102.50
// @elem LABEL_RESET label 0.0 label "reset" 0.0 109.00 101.00
// @elem LABEL_IN label 0.0 label "in" 0.0 61.00 117.00
// @elem LABEL_DUFF label 0.0 label "duff" 0.0 76.00 116.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 94.00 116.50
// @elem BOX_DUFF panel_box 7.0 box "" 0.0 76.00 111.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 94.00 111.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(13.00f, 22.00f)), module, Guttur::DRIVE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(37.00f, 22.00f)), module, Guttur::TONE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(61.00f, 22.00f)), module, Guttur::DAMP_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(85.00f, 22.00f)), module, Guttur::RATE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(109.00f, 22.00f)), module, Guttur::SMOOTH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(13.00f, 40.50f)), module, Guttur::DRIVE_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(37.00f, 40.50f)), module, Guttur::TONE_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(61.00f, 40.50f)), module, Guttur::DAMP_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(85.00f, 40.50f)), module, Guttur::RATE_ATT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.00f, 63.00f)), module, Guttur::BANKA_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.00f, 63.00f)), module, Guttur::BANKB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(61.00f, 63.00f)), module, Guttur::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(85.00f, 63.00f)), module, Guttur::Q_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(109.00f, 63.00f)), module, Guttur::SPREAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.00f, 94.00f)), module, Guttur::GAINA_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.00f, 94.00f)), module, Guttur::GAINB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(61.00f, 94.00f)), module, Guttur::LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(85.00f, 94.00f)), module, Guttur::DIST_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(109.00f, 45.50f)), module, Guttur::FILT_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(109.00f, 94.00f)), module, Guttur::RESET_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.00f, 50.00f)), module, Guttur::DRIVE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.00f, 50.00f)), module, Guttur::TONE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 50.00f)), module, Guttur::DAMP_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(85.00f, 50.00f)), module, Guttur::RATE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.00f, 79.00f)), module, Guttur::BANKA_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.00f, 79.00f)), module, Guttur::BANKB_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 79.00f)), module, Guttur::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(85.00f, 79.00f)), module, Guttur::Q_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.00f, 109.50f)), module, Guttur::GAINA_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.00f, 109.50f)), module, Guttur::GAINB_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 109.50f)), module, Guttur::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(109.00f, 109.50f)), module, Guttur::RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(94.00f, 109.00f)), module, Guttur::OUT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(76.00f, 109.00f)), module, Guttur::DUFF_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(99.00f, 106.00f)), module, Guttur::OUT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(81.00f, 106.00f)), module, Guttur::DUFF_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Guttur* module = getModule<Guttur>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Distortion oversampling",
            {"1x", "2x", "4x", "8x", "16x"},
            [=]() { return module->osIndex; },
            [=](int idx) { module->setOversampling(idx); }));

        static const float glides[4] = {20.f, 100.f, 500.f, 2000.f};
        menu->addChild(createIndexSubmenuItem("Bank glide",
            {"20 ms", "100 ms", "500 ms", "2 s"},
            [=]() {
                for (int i = 0; i < 4; i++)
                    if (std::fabs(module->glideMs - glides[i]) < 1.f)
                        return i;
                return 1;
            },
            [=](int idx) { module->setGlide(glides[idx]); }));
    }
};

Model* modelGuttur = createModel<Guttur, GutturWidget>("guttur");
