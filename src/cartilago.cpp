// cartilago.cpp - VCV Rack 2 module
// cartilago (Latin: "gristle, cartilage") is a modulator in the manner of the
// Gristleizer, the ETI magazine project by Roy Gwinn that Chris Carter built,
// modified and made a signature of Throbbing Gristle's sound. It is one LFO
// with four shapes driving either a FET attenuator (tremolo, and at audio
// rate a ragged ring modulator) or a resonant filter, with the modulation
// deliberately allowed to overshoot at both ends.
//
// This is a circuit-informed model, not a netlist transcription: no schematic
// revision was measured, and the published replicas differ from one another.
// What is modelled from the circuit rather than from taste:
//
//   - the attenuator is a JFET in a shunt divider, not a multiplier. Its
//     channel conductance sets the gain, so the "floor" of the tremolo is
//     Ron/(R+Ron) rather than silence, and because the channel sees the drain
//     swing as well as the gate the gain moves with the signal itself: the
//     second-harmonic grit that a clean VCA cannot produce.
//   - gate-drain capacitance injects the control edge straight into the audio
//     path (control feedthrough). This is the tick you hear on the pulse
//     setting with nothing plugged into the input, and it is kept.
//   - the control path is band-limited before it reaches the gate, which is
//     what turns that tick into a thump rather than a click.
//
// Deviations from the hardware, all deliberate:
//   - MOD input and SYNC input, neither of which the pedal has.
//   - the LFO shapes are band-limited (polyBLEP/polyBLAMP) so the audio-rate
//     settings ring-modulate instead of aliasing. Switchable off in the menu.
//   - the filter mode offers lowpass as well as the bandpass the replicas
//     are usually described as having.
//   - polyphonic: one FET and one filter per channel, one LFO for all of them.
//
// Controls:
//   Knobs : WAVE, RATE, DEPTH, BIAS, SHAPE, DRIVE, RES, LEVEL
//   Switch: MODE (VCA / VCF)
//   In    : IN, V/OCT, DEPTH, BIAS, MOD, SYNC
//   Out   : OUT, LFO
//   Lights: rate blink, one level LED per output

#include "forsitan.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

namespace cartilago {

static const int kMaxChannels = 16;

// ── JFET shunt attenuator ───────────────────────────────────────────────────
// Series R into a FET to ground. kRon is R/Ron, so the attenuator bottoms out
// at 1/(1+kRon) instead of zero: the Gristleizer never fully mutes.
static const float kRon = 20.f;
// How much of the drain swing the channel sees. A real JFET VCA cancels most
// of it by feeding half the drain voltage back to the gate; this one does not
// bother, which is where the grit comes from. It scales the conductance rather
// than offsetting it, so a channel the gate has pinched off stays off however
// hard the signal swings, and the distortion peaks in the middle of the sweep
// where the FET is both conducting and seeing a real voltage.
static const float kHalfDrain = 0.6f;
// Gate-drain capacitance times the load, in seconds: the control edge appears
// at the output scaled by tau * d(control)/dt. Sized so a full pulse edge
// lands the tick near -25 dB of full scale: a thump that keeps time under the
// tremolo, not a click that competes with it.
static const float kFeedTau = 3e-6f;
// The gate drive is band-limited by the circuit around it. This is what makes
// the pulse setting thump instead of click, and it is also what stops the
// feedthrough term from growing without bound at audio-rate modulation.
static const float kGateHz = 2000.f;

// ── cheap saturator, as in vespae ───────────────────────────────────────────
static inline float ftanh(float x) {
    x = clamp(x, -3.f, 3.f);
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// ── band-limited LFO ────────────────────────────────────────────────────────
// polyBLEP residual for a signed jump D: y += (D/2) * blep(t, dt).
static inline float blep(float t, float dt) {
    if (t < dt) { const float u = t / dt; return u + u - u * u - 1.f; }
    if (t > 1.f - dt) { const float u = (t - 1.f) / dt; return u * u + u + u + 1.f; }
    return 0.f;
}

// polyBLAMP, the integral of the above, for a slope discontinuity.
static inline float blamp(float t, float dt) {
    if (t < dt) { const float u = t / dt - 1.f; return -u * u * u / 3.f; }
    if (t > 1.f - dt) { const float u = (t - 1.f) / dt + 1.f; return u * u * u / 3.f; }
    return 0.f;
}

// Integrating the blep scaling gives 0.5 for a unit slope jump per sample, but
// the two-point residual is itself an approximation: sweeping it against the
// alias floor (test/cartilago_probe blamp) bottoms out at 0.60 for every
// shape and rate tried, 3 dB below what the analytic value gives. Take the
// measurement.
static const float kBlampScale = 0.60f;

struct Lfo {
    enum Wave { TRI, RAMP_UP, RAMP_DOWN, PULSE };
    float phase = 0.f;

    void reset() { phase = 0.f; }

    // dt = f / sampleRate, s = symmetry in (0,1)
    float process(float dt, float s, int wave, bool bandLimit) {
        dt = clamp(dt, 0.f, 0.45f);
        s = clamp(s, 0.02f, 0.98f);
        const float p = phase;
        float y;
        switch (wave) {
        case RAMP_UP:
            y = -1.f + 2.f * p;
            if (bandLimit) y -= blep(p, dt);
            break;
        case RAMP_DOWN:
            y = 1.f - 2.f * p;
            if (bandLimit) y += blep(p, dt);
            break;
        case PULSE: {
            y = (p < s) ? 1.f : -1.f;
            if (bandLimit) {
                float q = p - s;
                if (q < 0.f) q += 1.f;
                y += blep(p, dt) - blep(q, dt);
            }
            break;
        }
        default: {
            // Symmetry moves the peak, so this is one oscillator that runs
            // from a falling ramp through a triangle to a rising one.
            y = (p < s) ? (-1.f + 2.f * p / s) : (1.f - 2.f * (p - s) / (1.f - s));
            if (bandLimit) {
                const float ds = 2.f / s + 2.f / (1.f - s);
                float q = p - s;
                if (q < 0.f) q += 1.f;
                const float k = kBlampScale * dt * ds;
                y += k * blamp(p, dt) - k * blamp(q, dt);
            }
            break;
        }
        }
        phase += dt;
        if (phase >= 1.f) phase -= std::floor(phase);
        return clamp(y, -1.5f, 1.5f);
    }
};

// ── zero-delay-feedback state variable filter (Cytomic topology) ────────────
struct Svf {
    float ic1 = 0.f, ic2 = 0.f;

    void reset() { ic1 = ic2 = 0.f; }

    // g = tan(pi fc / fs), k = 1/Q. Bandpass in bp, lowpass in lp.
    void process(float x, float g, float k, float& bp, float& lp) {
        const float a1 = 1.f / (1.f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        bp = v1;
        lp = v2;
    }
};

// one-pole DC blocker, standing in for the output coupling capacitor
struct DCBlock {
    float xz = 0.f, yz = 0.f, r = 0.9995f;
    void setRate(float sr) { r = 1.f - 2.f * (float)M_PI * 12.f / sr; }
    void reset() { xz = yz = 0.f; }
    float process(float x) {
        yz = x - xz + r * yz;
        xz = x;
        return yz;
    }
};

// ── one audio channel: the FET, the filter and the coupling ─────────────────
struct Voice {
    Svf svf;
    DCBlock dc;

    void reset() { svf.reset(); dc.reset(); }

    // ctl01 in [0,1]: 0 = FET fully on (attenuating), 1 = pinched off (open).
    // In the triode region a JFET's channel conductance goes as
    // 2(Vgs - Vth) - Vds, so the drain swing itself modulates the gain: the
    // channel pinches on the positive half of the waveform and opens on the
    // negative one. Estimate Vds from the linear divider and use it once -
    // solving the loop properly would be a quadratic, and one explicit pass
    // is accurate where it matters (near the top of the sweep Vds ~ x) and
    // unconditionally stable where iterating is not.
    float vca(float x, float ctl01) {
        // Gate taper. Channel conductance is linear in gate voltage in the
        // triode region, so a linear control would put twenty of the
        // attenuator's twenty-six dB into the top twentieth of the sweep:
        // the divider's half-gain point is at cond = 1/kRon. Real gear hides
        // that in the pot taper and the FET's own threshold, neither of which
        // we have a value for, so this is a cube, which spreads the sweep to
        // roughly -26, -19, -11, -2 dB across the four quarters.
        const float u = clamp(1.f - ctl01, 0.f, 1.f);
        const float cond0 = u * u * u;
        const float vds = x / (1.f + kRon * cond0);
        const float cond = cond0 * clamp(1.f - kHalfDrain * vds, 0.f, 2.f);
        return x / (1.f + kRon * cond);
    }

    float vcf(float x, float g, float k, bool lowpass) {
        float bp, lp;
        svf.process(x, g, k, bp, lp);
        return lowpass ? lp : bp;
    }
};

} // namespace cartilago

struct Cartilago : Module {
    enum ParamId {
        WAVE_PARAM,
        RATE_PARAM,
        MODE_PARAM,
        DEPTH_PARAM,
        BIAS_PARAM,
        SHAPE_PARAM,
        DRIVE_PARAM,
        RES_PARAM,
        LEVEL_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        VOCT_INPUT,
        DEPTH_INPUT,
        BIAS_INPUT,
        MOD_INPUT,
        SYNC_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        LFO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        RATE_LIGHT,
        AUDIO_LIGHT,
        LFO_LIGHT,
        LIGHTS_LEN
    };

    cartilago::Lfo lfo;
    cartilago::Voice voices[cartilago::kMaxChannels];
    VariableOversampling<> upsampler[cartilago::kMaxChannels];
    float ctlBuf[16] = {};      // gate voltage at the oversampled rate
    float feedBuf[16] = {};     // control feedthrough, same rate
    float gateZ = 0.f;          // one-pole state of the gate drive
    float gatePrev = 0.f;       // for the feedthrough differentiator
    float lastLfo = 0.f;
    float levelEnv = 0.f, lfoEnv = 0.f;
    dsp::SchmittTrigger syncTrigger;

    int osIndex = 1;            // 2^osIndex, default 2x
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;
    bool bandLimit = true;      // polyBLEP/polyBLAMP on the LFO shapes
    bool feedthrough = true;    // the FET's gate-drain tick
    bool lowpassMode = false;   // VCF mode: bandpass (hardware) or lowpass

    Cartilago() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configSwitch(WAVE_PARAM, 0.f, 3.f, 0.f, "Waveform",
                     {"Triangle", "Ramp up", "Ramp down", "Pulse"});
        configParam(RATE_PARAM, -6.f, 7.f, 0.f, "Rate", " Hz", 2.f, 1.f);
        configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Mode", {"VCA", "VCF"});
        configParam(DEPTH_PARAM, 0.f, 1.25f, 0.75f, "Depth", "%", 0.f, 100.f);
        configParam(BIAS_PARAM, -1.f, 1.f, 0.f, "Bias", "%", 0.f, 100.f);
        configParam(SHAPE_PARAM, 0.02f, 0.98f, 0.5f, "Shape (symmetry)", "%", 0.f, 100.f);
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.5f, "Drive", " dB", 0.f, 40.f, -20.f);
        configParam(RES_PARAM, 0.f, 1.f, 0.45f, "Resonance", "%", 0.f, 100.f);
        configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level", "%", 0.f, 100.f);
        configInput(AUDIO_INPUT, "Audio");
        configInput(VOCT_INPUT, "1V/oct rate");
        configInput(DEPTH_INPUT, "Depth CV");
        configInput(BIAS_INPUT, "Bias CV");
        configInput(MOD_INPUT, "External modulation");
        configInput(SYNC_INPUT, "Sync (resets the LFO)");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(LFO_OUTPUT, "LFO");
        configLight(RATE_LIGHT, "Rate");
        configLight(AUDIO_LIGHT, "Audio level");
        configLight(LFO_LIGHT, "LFO level");
        configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
    }

    void onReset() override {
        lfo.reset();
        for (int c = 0; c < cartilago::kMaxChannels; c++) voices[c].reset();
        gateZ = gatePrev = lastLfo = 0.f;
        levelEnv = lfoEnv = 0.f;
        lastOsIndex = -1;
    }

    void onSampleRateChange() override { lastOsIndex = -1; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        json_object_set_new(root, "bandLimit", json_boolean(bandLimit));
        json_object_set_new(root, "feedthrough", json_boolean(feedthrough));
        json_object_set_new(root, "lowpassMode", json_boolean(lowpassMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            osIndex = clamp((int)json_integer_value(j), 0, 4);
        if (json_t* j = json_object_get(root, "bandLimit"))
            bandLimit = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "feedthrough"))
            feedthrough = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "lowpassMode"))
            lowpassMode = json_boolean_value(j);
        lastOsIndex = -1;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            for (int c = 0; c < cartilago::kMaxChannels; c++) {
                upsampler[c].setOversamplingIndex(osIndex);
                upsampler[c].reset(sr);
                voices[c].dc.setRate(sr * (1 << osIndex));
            }
            lastOsIndex = osIndex;
            lastSampleRate = sr;
        }

        const int ratio = 1 << osIndex;
        const float fsOs = sr * (float)ratio;

        // ── LFO rate ────────────────────────────────────────────────────────
        float octaves = params[RATE_PARAM].getValue();
        octaves += inputs[VOCT_INPUT].getVoltage();
        const float rate = clamp(dsp::exp2_taylor5(clamp(octaves, -8.f, 12.f)),
                                 0.002f, 0.45f * fsOs);
        const float dt = rate / fsOs;

        if (syncTrigger.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 1.f))
            lfo.reset();

        const int wave = (int)std::round(params[WAVE_PARAM].getValue());
        const float shape = params[SHAPE_PARAM].getValue();
        const float depth = clamp(params[DEPTH_PARAM].getValue()
                                  + inputs[DEPTH_INPUT].getVoltage() * 0.1f, 0.f, 1.25f);
        const float bias = clamp(params[BIAS_PARAM].getValue()
                                 + inputs[BIAS_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        const float mod = inputs[MOD_INPUT].getVoltage() * 0.2f;

        // ── the control path, at the oversampled rate ───────────────────────
        // One LFO for every channel, as in the pedal: it is one modulator.
        const float ga = clamp(2.f * (float)M_PI * cartilago::kGateHz / fsOs, 0.f, 1.f);
        for (int k = 0; k < ratio; k++) {
            const float raw = lfo.process(dt, shape, wave, bandLimit);
            lastLfo = raw;
            const float target = clamp(bias + depth * (raw + mod), -1.5f, 1.5f);
            gateZ += ga * (target - gateZ);
            ctlBuf[k] = gateZ;
            // i = C dV/dt through the gate-drain capacitance, into the load
            feedBuf[k] = cartilago::kFeedTau * (gateZ - gatePrev) * fsOs;
            gatePrev = gateZ;
        }

        // ── audio ───────────────────────────────────────────────────────────
        const bool vcf = params[MODE_PARAM].getValue() > 0.5f;
        const float drive = dsp::exp2_taylor5(
            10.f * params[DRIVE_PARAM].getValue() - 5.f);       // 1/32 .. 32
        // The filter's output saturator doubles as the resonance limiter. Its
        // drive follows the DRIVE knob so backing the knob off cleans the
        // filter up instead of leaving a fixed fuzz in the path; below unity
        // the small-signal gain is normalized back, so quiet is not also dull.
        const float post = 0.8f * std::pow(drive, 0.7f);
        // the 1.4 pays back the filter's own passband loss (input stage plus
        // 1/k at the default resonance), so the two modes sit at similar level
        const float postNorm = 1.4f / std::min(post, 1.f);
        const float level = params[LEVEL_PARAM].getValue();
        const float res = params[RES_PARAM].getValue();
        const float kres = 2.f - 1.96f * res;
        const float feedAmt = feedthrough ? 1.f : 0.f;

        const int channels = std::max(1, inputs[AUDIO_INPUT].getChannels());
        outputs[AUDIO_OUTPUT].setChannels(channels);

        float peak = 0.f;
        for (int c = 0; c < channels; c++) {
            const float in = inputs[AUDIO_INPUT].getPolyVoltage(c) * 0.2f;
            upsampler[c].upsample(in);
            float* buf = upsampler[c].getOSBuffer();
            for (int k = 0; k < ratio; k++) {
                const float x = cartilago::ftanh(buf[k] * drive);
                const float ctl01 = clamp(0.5f + 0.5f * ctlBuf[k], 0.f, 1.f);
                float y;
                if (!vcf) {
                    y = voices[c].vca(x, ctl01);
                } else {
                    // 45 Hz to 3.8 kHz, the sweep the replicas are described
                    // as covering; the same control drives it.
                    const float fc = clamp(45.f * dsp::exp2_taylor5(6.4f * ctl01),
                                           20.f, 0.45f * fsOs);
                    const float g = std::min(std::tan((float)M_PI * fc / fsOs), 4.f);
                    y = voices[c].vcf(x, g, kres, lowpassMode);
                    y = cartilago::ftanh(post * y) * postNorm;
                }
                y += feedAmt * feedBuf[k];
                buf[k] = voices[c].dc.process(y);
            }
            float y = upsampler[c].downsample();
            if (!std::isfinite(y)) { y = 0.f; voices[c].reset(); }
            y = clamp(5.f * level * y, -10.f, 10.f);
            outputs[AUDIO_OUTPUT].setVoltage(y, c);
            peak = std::max(peak, std::fabs(y));
        }

        outputs[LFO_OUTPUT].setVoltage(clamp(5.f * lastLfo, -10.f, 10.f));

        levelEnv += (peak * 0.2f - levelEnv) * 0.002f;
        lfoEnv += (std::fabs(lastLfo) - lfoEnv) * 0.002f;
        lights[RATE_LIGHT].setBrightness(clamp(0.5f + 0.5f * lastLfo, 0.f, 1.f));
        lights[AUDIO_LIGHT].setBrightness(clamp(levelEnv * 1.4f, 0.f, 1.f));
        lights[LFO_LIGHT].setBrightness(clamp(lfoEnv * 1.4f, 0.f, 1.f));
    }
};

struct CartilagoWidget : ModuleWidget {
    CartilagoWidget(Cartilago* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/cartilago.svg")));

// @layout:begin cartilago 60.96 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem WAVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RATE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem MODE_PARAM CKSS 2.3 param "" 0.0
// @elem DEPTH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BIAS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SHAPE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DRIVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DEPTH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem BIAS_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MOD_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SYNC_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LFO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RATE_LIGHT SmallLight 1.0 light "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem LFO_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_WAVE label 0.0 label "wave" 0.0 10.50 29.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 30.48 32.50
// @elem LABEL_MODE label 0.0 label "vca/vcf" 0.0 50.46 29.50
// @elem LABEL_DEPTH label 0.0 label "depth" 0.0 13.50 54.50
// @elem LABEL_BIAS label 0.0 label "bias" 0.0 30.48 54.50
// @elem LABEL_SHAPE label 0.0 label "shape" 0.0 47.46 54.50
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 13.50 76.50
// @elem LABEL_RES label 0.0 label "res" 0.0 30.48 76.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 47.46 76.50
// @elem LABEL_IN label 0.0 label "in" 0.0 5.88 95.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 18.18 95.50
// @elem LABEL_DEPTHCV label 0.0 label "depth" 0.0 30.48 95.50
// @elem LABEL_BIASCV label 0.0 label "bias" 0.0 42.78 95.50
// @elem LABEL_MOD label 0.0 label "mod" 0.0 55.08 95.50
// @elem LABEL_SYNC label 0.0 label "sync" 0.0 18.18 113.50
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 50.00 108.00
// @elem BOX_LFO panel_box 7.0 box "" 0.0 34.00 108.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 50.00 113.50
// @elem LABEL_LFOOUT label 0.0 label "lfo" 0.0 34.00 113.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.50f, 21.00f)), module, Cartilago::WAVE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(30.48f, 21.00f)), module, Cartilago::RATE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(50.46f, 21.00f)), module, Cartilago::MODE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 46.00f)), module, Cartilago::DEPTH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 46.00f)), module, Cartilago::BIAS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.46f, 46.00f)), module, Cartilago::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 68.00f)), module, Cartilago::DRIVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 68.00f)), module, Cartilago::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.46f, 68.00f)), module, Cartilago::LEVEL_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.88f, 88.00f)), module, Cartilago::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.18f, 88.00f)), module, Cartilago::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.48f, 88.00f)), module, Cartilago::DEPTH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.78f, 88.00f)), module, Cartilago::BIAS_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.08f, 88.00f)), module, Cartilago::MOD_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.18f, 106.00f)), module, Cartilago::SYNC_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(50.00f, 106.00f)), module, Cartilago::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(34.00f, 106.00f)), module, Cartilago::LFO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(38.50f, 13.00f)), module, Cartilago::RATE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(55.00f, 103.00f)), module, Cartilago::AUDIO_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(39.00f, 103.00f)), module, Cartilago::LFO_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Cartilago* m = dynamic_cast<Cartilago*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"1× (rawest, lightest)", "2× (default)", "4×", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex; },
            [m](int i) { m->osIndex = i; }));
        menu->addChild(createBoolPtrMenuItem("Band-limited LFO shapes", "", &m->bandLimit));
        menu->addChild(createBoolPtrMenuItem("FET control feedthrough (tick)", "", &m->feedthrough));
        menu->addChild(createBoolPtrMenuItem("VCF mode is lowpass", "", &m->lowpassMode));
    }
};

Model* modelCartilago = createModel<Cartilago, CartilagoWidget>("cartilago");
