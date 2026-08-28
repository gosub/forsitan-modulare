// scando.cpp - VCV Rack 2 module
// Scanned-synthesis oscillator (Verplank / Mathews / Shaw technique; the engine
// matches the classic Csound scansyn / Qu-Bit non-circular mass-spring string).
//   - a chain of masses-on-springs is a slowly-evolving wavetable
//   - a phase accumulator scans it at audio rate; pitch and timbre independent
//   - STRENGTH continuously drives the string with the hammer shape (free-run)
//   - EXCITE trigger snaps every mass to the hammer shape (a pluck)
//   - INJECT feeds an external audio signal in as excitation
//
// Controls:
//   Knobs : FREQ, FINE, MASS, STIFF, DAMP, CENTER, SHAPE, STRENGTH, RATE, IN LVL
//   In    : V/OCT, EXCITE (hammer hit), INJECT (audio), + CV for the timbre knobs
//   Out   : AUDIO
//   Light : LEVEL (output amplitude)

#include "forsitan.hpp"
#include "scando_engine.hpp"

using namespace scando;

static constexpr float kDrive    = 0.0008f;  // faint broadband bed (keeps it alive)
static constexpr float kPluckAmp = 3.0f;     // displacement applied on EXCITE
static constexpr float kStrength = 1.2f;     // continuous hammer-force scale
static constexpr float kInject   = 2.0f;     // audio-inject force scale
static constexpr float kOutGain  = 1.0f;     // drive into the output soft-clip

struct Scando : Module {
    enum ParamId {
        FREQ_PARAM,
        FINE_PARAM,
        MASS_PARAM,
        STIFF_PARAM,
        DAMP_PARAM,
        CENTER_PARAM,
        SHAPE_PARAM,
        STRENGTH_PARAM,
        RATE_PARAM,
        IN_LEVEL_PARAM,
        EXCITE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        VOCT_INPUT,
        EXCITE_INPUT,      // trigger: hammer the string to the current shape
        INJECT_INPUT,      // audio excitation
        MASS_CV_INPUT,
        STIFF_CV_INPUT,
        DAMP_CV_INPUT,
        CENTER_CV_INPUT,
        SHAPE_CV_INPUT,
        STRENGTH_CV_INPUT,
        RATE_CV_INPUT,
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
    DCBlocker     dc;        // centres the scanned signal before the limiter
    DCBlocker     dcOut;     // removes any soft-clip-induced DC at the output
    OutStage      out;

    float hammerBuf[kN];
    float forceBuf[kN];
    float lastShape = -1.f;
    float phase     = 0.f;     // scan phase in [0, kN)
    float physAccum = 0.f;     // fractional physics-tick accumulator
    float levelEnv  = 0.f;     // smoothed output amplitude for the LED

    Scando() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
        configParam(FINE_PARAM, -7.f, 7.f, 0.f, "Fine tune", " semitones");
        configParam(MASS_PARAM,     0.f, 1.f, 0.5f,  "Mass");
        configParam(STIFF_PARAM,    0.f, 1.f, 0.20f, "Stiffness");
        configParam(DAMP_PARAM,     0.f, 1.f, 0.70f, "Damping");
        configParam(CENTER_PARAM,   0.f, 1.f, 0.30f, "Centering");
        configParam(SHAPE_PARAM,    0.f, 1.f, 0.33f, "Hammer shape");
        configParam(STRENGTH_PARAM, 0.f, 1.f, 0.f,   "Strength");
        configParam(RATE_PARAM,     0.f, 1.f, 0.75f, "Update rate");
        configParam(IN_LEVEL_PARAM, 0.f, 1.f, 0.f,   "Inject level");
        configButton(EXCITE_PARAM, "Excite (hammer hit)");

        configInput(VOCT_INPUT,     "1V/oct pitch");
        configInput(EXCITE_INPUT,   "Excite (hammer hit) trigger");
        configInput(INJECT_INPUT,   "Inject audio");
        configInput(MASS_CV_INPUT,     "Mass CV");
        configInput(STIFF_CV_INPUT,    "Stiffness CV");
        configInput(DAMP_CV_INPUT,     "Damping CV");
        configInput(CENTER_CV_INPUT,   "Centering CV");
        configInput(SHAPE_CV_INPUT,    "Shape CV");
        configInput(STRENGTH_CV_INPUT, "Strength CV");
        configInput(RATE_CV_INPUT,     "Update rate CV");

        configOutput(AUDIO_OUTPUT, "Audio");
        configLight(LEVEL_LIGHT, "Output level");

        hammer.init();
        onReset();
    }

    void onReset() override {
        string.reset();
        dc.reset();
        dcOut.reset();
        out.reset();
        phase = 0.f;
        physAccum = 0.f;
        levelEnv = 0.f;
        lastShape = -1.f;
        // start at rest: silent until EXCITE, INJECT or STRENGTH drives the string
        hammer.build(params[SHAPE_PARAM].getValue(), hammerBuf);
    }

    // knob (0..1) plus its CV input (+-5V -> +-1.0), clamped to [0,1]
    float knobCV(int param, int input) {
        float v = params[param].getValue();
        if (inputs[input].isConnected())
            v += inputs[input].getVoltage() * 0.2f;
        return clamp(v, 0.f, 1.f);
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        // ── knobs (+ CV) -> engine parameters ───────────────────────────────
        const float massK     = knobCV(MASS_PARAM,     MASS_CV_INPUT);
        const float stiffK    = knobCV(STIFF_PARAM,    STIFF_CV_INPUT);
        const float dampK     = knobCV(DAMP_PARAM,     DAMP_CV_INPUT);
        const float centK     = knobCV(CENTER_PARAM,   CENTER_CV_INPUT);
        const float shapeK    = knobCV(SHAPE_PARAM,    SHAPE_CV_INPUT);
        const float strengthK = knobCV(STRENGTH_PARAM, STRENGTH_CV_INPUT);
        const float rateK     = knobCV(RATE_PARAM,     RATE_CV_INPUT);
        const float inLevel   = params[IN_LEVEL_PARAM].getValue();

        const float S = 0.20f + 0.70f * massK;        // integration step: heavy -> light
        const float K = 0.60f * stiffK;               // inter-mass stiffness
        const float C = 1.20f * centK;                // centering spring
        // damping: fast decay (top) down to slightly self-oscillating drone
        const float D = 0.006f * (1.f - dampK) - 0.0008f * dampK;
        string.setParams(K, C, D, S);

        // update rate: 500 Hz .. 8 kHz (exponential), strongly shapes evolution
        const float physHz = 500.f * std::pow(16.f, rateK);

        // rebuild the hammer shape only when it actually moves
        if (std::fabs(shapeK - lastShape) > 1e-4f) {
            hammer.build(shapeK, hammerBuf);
            lastShape = shapeK;
        }

        // ── EXCITE: hammer the string to the current shape (a pluck) ────────
        bool exciteTrig = exciteTrigger.process(inputs[EXCITE_INPUT].getVoltage(), 0.1f, 1.f);
        bool exciteBtn = exciteButton.process(params[EXCITE_PARAM].getValue() > 0.5f);
        if (exciteTrig || exciteBtn)
            string.setShape(hammerBuf, kPluckAmp);

        // ── continuous excitation forces ────────────────────────────────────
        const float inj  = inputs[INJECT_INPUT].isConnected()
                         ? inputs[INJECT_INPUT].getVoltage() * 0.1f : 0.f;
        const float drive = kStrength * strengthK + kInject * inLevel * inj;

        // faint broadband "life" rides along with STRENGTH only, so the string
        // stays at exact rest (silence) when nothing is driving it
        const float life = kDrive * strengthK;

        // ── advance the string physics at the chosen haptic rate ────────────
        physAccum += physHz / sr;
        while (physAccum >= 1.f) {
            physAccum -= 1.f;
            for (int i = 0; i < kN; ++i)
                forceBuf[i] = drive * hammerBuf[i] + life * rng.bipolar();
            string.update(forceBuf);
        }

        // ── scan the string shape at audio rate -> pitched output ───────────
        const float pitch = params[FREQ_PARAM].getValue()
                          + params[FINE_PARAM].getValue() * (1.f / 12.f)
                          + inputs[VOCT_INPUT].getVoltage();
        const float freq = dsp::FREQ_C4 * std::pow(2.f, pitch);
        phase += (float)kN * freq / sr;
        while (phase >= kN) phase -= kN;
        while (phase < 0.f) phase += kN;

        float s = dc.process(string.scan(phase));
        float y = dcOut.process(out.process(kOutGain * s));
        outputs[AUDIO_OUTPUT].setVoltage(y * 5.f);

        // ── output-level LED ────────────────────────────────────────────────
        levelEnv += (std::fabs(y) - levelEnv) * 0.001f;
        lights[LEVEL_LIGHT].setBrightness(levelEnv);
    }
};

struct ScandoWidget : ModuleWidget {
    ScandoWidget(Scando* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/scando.svg")));

// @layout:begin scando 81.28 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FINE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FREQ_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem SHAPE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem MASS_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem STIFF_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem STRENGTH_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem CENTER_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DAMP_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem IN_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem EXCITE_PARAM TL1105 2.6 param "" 0.0
// @elem EXCITE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem INJECT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem STIFF_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CENTER_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RATE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem STRENGTH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem MASS_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DAMP_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SHAPE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_FINE label 0.0 label "fine" 0.0 13.50 27.70
// @elem LABEL_FREQ label 0.0 label "freq" 0.0 40.64 29.50
// @elem LABEL_SHAPE label 0.0 label "shape" 0.0 67.78 27.70
// @elem LABEL_MASS label 0.0 label "mass" 0.0 13.50 46.20
// @elem LABEL_STIFF label 0.0 label "stiff" 0.0 40.64 46.20
// @elem LABEL_STRENGTH label 0.0 label "strength" 0.0 67.78 46.20
// @elem LABEL_CENTER label 0.0 label "center" 0.0 13.50 66.20
// @elem LABEL_DAMP label 0.0 label "damp" 0.0 40.64 66.20
// @elem LABEL_RATE label 0.0 label "rate" 0.0 67.78 66.20
// @elem LABEL_INLVL label 0.0 label "in lvl" 0.0 13.50 86.20
// @elem LABEL_EXCBTN label 0.0 label "excite" 0.0 40.64 86.20
// @elem LABEL_EXC label 0.0 label "exc" 0.0 8.00 93.20
// @elem LABEL_INJ label 0.0 label "inj" 0.0 21.00 93.20
// @elem LABEL_STIFFCV label 0.0 label "stiff" 0.0 34.00 93.20
// @elem LABEL_CNTRCV label 0.0 label "cntr" 0.0 47.00 93.20
// @elem LABEL_RATECV label 0.0 label "rate" 0.0 60.00 93.20
// @elem LABEL_STRGCV label 0.0 label "strg" 0.0 73.00 93.20
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 8.00 107.20
// @elem LABEL_MASSCV label 0.0 label "mass" 0.0 21.00 107.20
// @elem LABEL_DAMPCV label 0.0 label "damp" 0.0 34.00 107.20
// @elem LABEL_SHAPECV label 0.0 label "shape" 0.0 47.00 107.20
// @elem LABEL_OUT label 0.0 label "out" 0.0 73.00 120.35
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 40.64 122.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 73.00 114.35

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(71.12f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(71.12f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 18.00f)), module, Scando::FINE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(40.64f, 18.00f)), module, Scando::FREQ_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.78f, 18.00f)), module, Scando::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 38.00f)), module, Scando::MASS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.64f, 38.00f)), module, Scando::STIFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.78f, 38.00f)), module, Scando::STRENGTH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 58.00f)), module, Scando::CENTER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.64f, 58.00f)), module, Scando::DAMP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.78f, 58.00f)), module, Scando::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.50f, 78.00f)), module, Scando::IN_LEVEL_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(40.64f, 78.00f)), module, Scando::EXCITE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.00f, 99.00f)), module, Scando::EXCITE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(21.00f, 99.00f)), module, Scando::INJECT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(34.00f, 99.00f)), module, Scando::STIFF_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.00f, 99.00f)), module, Scando::CENTER_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.00f, 99.00f)), module, Scando::RATE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(73.00f, 99.00f)), module, Scando::STRENGTH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.00f, 113.00f)), module, Scando::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(21.00f, 113.00f)), module, Scando::MASS_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(34.00f, 113.00f)), module, Scando::DAMP_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.00f, 113.00f)), module, Scando::SHAPE_CV_INPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(78.00f, 109.50f)), module, Scando::LEVEL_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(73.00f, 113.00f)), module, Scando::AUDIO_OUTPUT));
        // @layout:end
    }
};

Model* modelScando = createModel<Scando, ScandoWidget>("scando");
