// lustro.cpp — VCV Rack 2 module
// lustro (Latin: "I traverse, I survey") is a scanned filter: the sibling of
// scando. The same vibrating mass-spring string (scando_engine.hpp), instead
// of being scanned as a wavetable, becomes the control surface of a resonant
// filterbank. Sixteen bandpass filters process the input audio; each band's
// gain is the displacement of the string at one of sixteen points along its
// length. Pluck the string and the spectrum blooms and decays; drive it
// continuously and the filterbank breathes with the physics. Displacement is
// bipolar, so bands can invert phase, adding comb-like movement.
//
// Controls:
//   Knobs : STIFF, DAMP, RATE (string physics), SHAPE, STRENGTH (hammer),
//           RES (filter Q), BASE (lowest band), SPREAD (octaves), MIX
//   In    : DAMP CV, RATE CV, BASE CV, SPREAD CV, EXCITE trigger, IN (audio)
//   Out   : OUT
//   Light : LEVEL (wet amplitude)

#include "forsitan.hpp"
#include "scando_engine.hpp"

using namespace scando;

static constexpr int kBands = 16;
static constexpr float kPluckAmp = 3.0f;
static constexpr float kStrength = 1.2f;
static constexpr float kDrive    = 0.0008f;

struct Lustro : Module {
    enum ParamId {
        STIFF_PARAM,
        DAMP_PARAM,
        RATE_PARAM,
        SHAPE_PARAM,
        STRENGTH_PARAM,
        RES_PARAM,
        BASE_PARAM,
        SPREAD_PARAM,
        MIX_PARAM,
        EXCITE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        DAMP_CV_INPUT,
        RATE_CV_INPUT,
        BASE_CV_INPUT,
        SPREAD_CV_INPUT,
        EXCITE_INPUT,
        AUDIO_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    ScannedString string;
    HammerTable   hammer;
    Rng           rng;
    dsp::SchmittTrigger exciteTrigger;
    dsp::BooleanTrigger exciteButton;

    // Chamberlin state-variable bandpass bank
    float svfLow[kBands] = {};
    float svfBand[kBands] = {};

    float hammerBuf[kN];
    float forceBuf[kN];
    float lastShape = -1.f;
    float physAccum = 0.f;
    float levelEnv = 0.f;

    Lustro() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(STIFF_PARAM,    0.f, 1.f, 0.20f, "Stiffness");
        configParam(DAMP_PARAM,     0.f, 1.f, 0.70f, "Damping");
        configParam(RATE_PARAM,     0.f, 1.f, 0.75f, "Update rate");
        configParam(SHAPE_PARAM,    0.f, 1.f, 0.33f, "Hammer shape");
        configParam(STRENGTH_PARAM, 0.f, 1.f, 0.f,   "Strength");
        configParam(RES_PARAM,      0.f, 1.f, 0.5f,  "Resonance");
        configParam(BASE_PARAM,     0.f, 1.f, 0.4f,  "Base frequency");
        configParam(SPREAD_PARAM,   0.f, 1.f, 0.6f,  "Spread (octaves)");
        configParam(MIX_PARAM,      0.f, 1.f, 1.f,   "Dry/wet mix", "%", 0.f, 100.f);
        configButton(EXCITE_PARAM, "Excite (hammer hit)");
        configInput(DAMP_CV_INPUT,   "Damping CV");
        configInput(RATE_CV_INPUT,   "Update rate CV");
        configInput(BASE_CV_INPUT,   "Base frequency CV");
        configInput(SPREAD_CV_INPUT, "Spread CV");
        configInput(EXCITE_INPUT,    "Excite (hammer hit) trigger");
        configInput(AUDIO_INPUT,     "Audio");
        configOutput(AUDIO_OUTPUT,   "Audio");
        configLight(LEVEL_LIGHT,     "Wet level");

        hammer.init();
        onReset();
    }

    void onReset() override {
        string.reset();
        for (int b = 0; b < kBands; b++) svfLow[b] = svfBand[b] = 0.f;
        physAccum = 0.f;
        levelEnv = 0.f;
        lastShape = -1.f;
        hammer.build(params[SHAPE_PARAM].getValue(), hammerBuf);
    }

    float knobCV(int param, int input) {
        float v = params[param].getValue();
        if (inputs[input].isConnected())
            v += inputs[input].getVoltage() * 0.2f;
        return clamp(v, 0.f, 1.f);
    }

    // unity below +-1, gentle tanh compression above (as draen)
    static float softLimit(float x) {
        if (x >  1.f) return  1.f + std::tanh(x - 1.f);
        if (x < -1.f) return -1.f + std::tanh(x + 1.f);
        return x;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        // ── string physics (same mappings as scando, mass/centering fixed) ──
        const float stiffK = params[STIFF_PARAM].getValue();
        const float dampK  = knobCV(DAMP_PARAM, DAMP_CV_INPUT);
        const float rateK  = knobCV(RATE_PARAM, RATE_CV_INPUT);
        const float shapeK = params[SHAPE_PARAM].getValue();
        const float strengthK = params[STRENGTH_PARAM].getValue();

        const float S = 0.55f;                        // fixed medium mass
        const float K = 0.60f * stiffK;
        const float C = 0.36f;                        // fixed centering
        const float D = 0.006f * (1.f - dampK) - 0.0008f * dampK;
        string.setParams(K, C, D, S);

        const float physHz = 500.f * std::pow(16.f, rateK);

        if (std::fabs(shapeK - lastShape) > 1e-4f) {
            hammer.build(shapeK, hammerBuf);
            lastShape = shapeK;
        }

        bool exciteTrig = exciteTrigger.process(inputs[EXCITE_INPUT].getVoltage(), 0.1f, 1.f);
        bool exciteBtn = exciteButton.process(params[EXCITE_PARAM].getValue() > 0.5f);
        if (exciteTrig || exciteBtn)
            string.setShape(hammerBuf, kPluckAmp);

        const float drive = kStrength * strengthK;
        const float life  = kDrive * strengthK;

        physAccum += physHz / sr;
        while (physAccum >= 1.f) {
            physAccum -= 1.f;
            for (int i = 0; i < kN; ++i)
                forceBuf[i] = drive * hammerBuf[i] + life * rng.bipolar();
            string.update(forceBuf);
        }

        // ── filterbank ──────────────────────────────────────────────────────
        // base 40 Hz .. 2 kHz (exp), spread 1 .. 6 octaves across the 16 bands
        const float baseK   = knobCV(BASE_PARAM, BASE_CV_INPUT);
        const float spreadK = knobCV(SPREAD_PARAM, SPREAD_CV_INPUT);
        const float f0      = 40.f * std::pow(50.f, baseK);
        const float octaves = 1.f + 5.f * spreadK;
        // resonance: q 2 .. 40 (exp); Chamberlin damping = 1/q
        const float q1 = 1.f / (2.f * std::pow(20.f, params[RES_PARAM].getValue()));

        const float in = inputs[AUDIO_INPUT].getVoltage() * 0.2f;

        float wet = 0.f;
        for (int b = 0; b < kBands; b++) {
            float fc = f0 * std::pow(2.f, octaves * b / (float)(kBands - 1));
            fc = std::min(fc, 0.22f * sr);
            float f1 = 2.f * std::sin((float)M_PI * fc / sr);
            // Chamberlin SVF tick
            svfLow[b] += f1 * svfBand[b];
            float high = in - svfLow[b] - q1 * svfBand[b];
            svfBand[b] += f1 * high;
            if (!std::isfinite(svfBand[b]) || !std::isfinite(svfLow[b]))
                svfLow[b] = svfBand[b] = 0.f;
            // band gain: string displacement at this point (bipolar!)
            int node = 1 + b * (kN - 3) / (kBands - 1);   // skip pinned ends
            float g = clamp(string.x[node] * 0.6f, -1.5f, 1.5f);
            wet += g * svfBand[b];
        }
        wet = softLimit(wet * 0.4f);

        const float mix = params[MIX_PARAM].getValue();
        float out = in * (1.f - mix) + wet * mix;
        outputs[AUDIO_OUTPUT].setVoltage(5.f * out);

        levelEnv += (std::fabs(wet) - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }
};

struct LustroWidget : ModuleWidget {
    LustroWidget(Lustro* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/lustro.svg")));

// @layout:begin lustro 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem STIFF_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DAMP_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SHAPE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem STRENGTH_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem BASE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SPREAD_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DAMP_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RATE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BASE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPREAD_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem EXCITE_PARAM TL1105 2.6 param "" 0.0
// @elem EXCITE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_STIFF label 0.0 label "stiff" 0.0 10.40 28.50
// @elem LABEL_DAMP label 0.0 label "damp" 0.0 25.40 28.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 40.40 28.50
// @elem LABEL_SHAPE label 0.0 label "shape" 0.0 10.40 45.50
// @elem LABEL_STRENGTH label 0.0 label "force" 0.0 25.40 45.50
// @elem LABEL_RES label 0.0 label "res" 0.0 40.40 45.50
// @elem LABEL_BASE label 0.0 label "base" 0.0 10.40 62.50
// @elem LABEL_SPREAD label 0.0 label "spread" 0.0 25.40 62.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 40.40 62.50
// @elem LABEL_DAMPCV label 0.0 label "damp" 0.0 7.40 80.50
// @elem LABEL_RATECV label 0.0 label "rate" 0.0 19.40 80.50
// @elem LABEL_BASECV label 0.0 label "base" 0.0 31.40 80.50
// @elem LABEL_SPREADCV label 0.0 label "sprd" 0.0 43.40 80.50
// @elem LABEL_EXCBTN label 0.0 label "excite" 0.0 10.40 98.50
// @elem LABEL_EXCITE label 0.0 label "exc" 0.0 25.40 98.50
// @elem LABEL_IN label 0.0 label "in" 0.0 10.40 114.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 40.40 114.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 40.40 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 20.00f)), module, Lustro::STIFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 20.00f)), module, Lustro::DAMP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 20.00f)), module, Lustro::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 37.00f)), module, Lustro::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 37.00f)), module, Lustro::STRENGTH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 37.00f)), module, Lustro::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 54.00f)), module, Lustro::BASE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 54.00f)), module, Lustro::SPREAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 54.00f)), module, Lustro::MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.40f, 73.00f)), module, Lustro::DAMP_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.40f, 73.00f)), module, Lustro::RATE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.40f, 73.00f)), module, Lustro::BASE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.40f, 73.00f)), module, Lustro::SPREAD_CV_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(10.40f, 91.00f)), module, Lustro::EXCITE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 91.00f)), module, Lustro::EXCITE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.40f, 106.50f)), module, Lustro::AUDIO_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.40f, 106.50f)), module, Lustro::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(45.40f, 103.50f)), module, Lustro::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelLustro = createModel<Lustro, LustroWidget>("lustro");
