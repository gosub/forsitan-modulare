// tundo.cpp — VCV Rack 2 module
// tundo (Latin: "I beat, I pound", from tundere, to strike repeatedly) is a
// parameterized digital drum voice built after the Noise Engineering
// Basimilus Iteritas Alter: six tonal oscillators plus noise, stacked into a
// modal spectrum, folded, and re-enveloped.
//
// Sources: Noise Engineering's own published manuals for the Basimilus
// Iteritas family (bi, bia, bia_german, bim), which document the engine in
// unusual depth. No firmware was consulted. "Basimilus", "Iteritas" and
// "Noise Engineering" are their trademarks and appear here only as
// attribution.
//
//   trig ─> six oscillators (ratios from harmonic to prime series)
//           + noise ─> Σ ─> attack env ─> infinifolder ─> final env ─> out
//
// Controls:
//   Knobs : PITCH, HARM, SPREAD, MORPH, FOLD, ATTACK, DECAY, each with its
//           own attenuverter and CV input
//   Switch: MODE (skin / liquid / metal), RANGE (bass / alto / treble),
//           both overridden by their CV input when one is patched
//   Button: HIT (strikes the voice, ORed with TRIG)
//   Out   : AUDIO, ENV (the voice's own final envelope, 0..10 V)
//
// The engine renders on its own clock, at a rate locked to a power-of-two
// multiple of the fundamental, so its alias images land on harmonics of the
// fundamental. That is the sound being cloned; the context menu offers a
// clean oversampled path for anyone who would rather not have it.

#include "forsitan.hpp"
#include "tundo_dsp.hpp"

// liquid-mode pitch envelope depth, in octaves, indexed by the menu item
static const float kLiquidDepths[4] = {1.f, 2.f, 3.f, 4.f};
// output swing: the last is the hardware's own
static const float kOutputVpp[3] = {5.f, 10.f, 14.f};

struct Tundo : Module {
    enum ParamId {
        PITCH_PARAM,
        HARM_PARAM,
        SPREAD_PARAM,
        MORPH_PARAM,
        FOLD_PARAM,
        ATTACK_PARAM,
        DECAY_PARAM,
        PITCH_ATT_PARAM,
        HARM_ATT_PARAM,
        SPREAD_ATT_PARAM,
        MORPH_ATT_PARAM,
        FOLD_ATT_PARAM,
        ATTACK_ATT_PARAM,
        DECAY_ATT_PARAM,
        MODE_PARAM,
        RANGE_PARAM,
        HIT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        PITCH_CV_INPUT,
        HARM_CV_INPUT,
        SPREAD_CV_INPUT,
        MORPH_CV_INPUT,
        FOLD_CV_INPUT,
        ATTACK_CV_INPUT,
        DECAY_CV_INPUT,
        MODE_CV_INPUT,
        RANGE_CV_INPUT,
        TRIG_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        ENV_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        HIT_LIGHT,
        LEVEL_LIGHT,
        ENV_LIGHT,
        LIGHTS_LEN
    };

    // one-pole parameter smoother
    struct Smooth {
        float current = 0.f, target = 0.f, b1 = 0.f;
        void setup(float initial, float freqHz, float rate) {
            current = target = initial;
            b1 = std::exp(-2.f * (float)M_PI * freqHz / rate);
        }
        inline float tick() {
            current = target + (current - target) * b1;
            return current;
        }
    };

    static constexpr int kControlDiv = 8;

    // attack knob: the CCW half is the pop plus noise, the CW half stretches
    // 0.5 ms to 2 s exponentially
    static constexpr float kMinAttackMs = 0.5f;
    static constexpr float kMaxAttackMs = 2000.f;
    static constexpr float kMinDecayMs = 5.f;
    static constexpr float kMaxDecayMs = 4000.f;

    tundo_dsp::Engine engine;
    tundo_dsp::Params p;
    Smooth smF0, smHarm, smSpread, smMorph, smFold, smAttack, smDecay, smNoise;
    dsp::SchmittTrigger trigTrigger, buttonTrigger;
    float sr = 0.f;
    int controlPhase = 0;
    float hitLight = 0.f;
    float levelEnv = 0.f;

    // context menu state
    bool cleanRate = false;
    bool quantize = true;
    bool extendedSpread = false;
    bool freeRun = false;
    int liquidDepth = 1;      // index into kLiquidDepths
    int outputLevel = 1;      // index into kOutputVpp

    Tundo() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(PITCH_PARAM, -3.f, 5.f, 0.f, "Pitch", " oct");
        configParam(HARM_PARAM, 0.f, 1.f, 0.3f, "Harmonic content", "%", 0.f, 100.f);
        configParam(SPREAD_PARAM, 0.f, 1.f, 0.f,
                    "Spread (harmonic series -> prime series)", "%", 0.f, 100.f);
        configParam(MORPH_PARAM, 0.f, 1.f, 0.f,
                    "Morph (sine -> triangle -> saw -> square)", "%", 0.f, 100.f);
        configParam(FOLD_PARAM, 0.f, 1.f, 0.f, "Fold (threshold, then pulses)",
                    "%", 0.f, 100.f);
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.5f, "Attack (noise <- pop -> slow)",
                    "%", 0.f, 100.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.35f, "Decay", " ms",
                    kMaxDecayMs / kMinDecayMs, kMinDecayMs);
        // the pitch attenuverter is the one that defaults open, so a patched
        // V/oct tracks exactly 1 V/oct without touching it
        configParam(PITCH_ATT_PARAM, -1.f, 1.f, 1.f, "Pitch CV (1 V/oct at 100%)",
                    "%", 0.f, 100.f);
        configParam(HARM_ATT_PARAM, -1.f, 1.f, 0.f, "Harmonic CV", "%", 0.f, 100.f);
        configParam(SPREAD_ATT_PARAM, -1.f, 1.f, 0.f, "Spread CV", "%", 0.f, 100.f);
        configParam(MORPH_ATT_PARAM, -1.f, 1.f, 0.f, "Morph CV", "%", 0.f, 100.f);
        configParam(FOLD_ATT_PARAM, -1.f, 1.f, 0.f, "Fold CV", "%", 0.f, 100.f);
        configParam(ATTACK_ATT_PARAM, -1.f, 1.f, 0.f, "Attack CV", "%", 0.f, 100.f);
        configParam(DECAY_ATT_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0.f, 100.f);
        configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Mode",
                     {"Skin", "Liquid", "Metal"});
        configSwitch(RANGE_PARAM, 0.f, 2.f, 0.f, "Range",
                     {"Bass", "Alto", "Treble"});
        configButton(HIT_PARAM, "Hit");
        configInput(PITCH_CV_INPUT, "Pitch (1 V/oct)");
        configInput(HARM_CV_INPUT, "Harmonic CV");
        configInput(SPREAD_CV_INPUT, "Spread CV");
        configInput(MORPH_CV_INPUT, "Morph CV");
        configInput(FOLD_CV_INPUT, "Fold CV");
        configInput(ATTACK_CV_INPUT, "Attack CV");
        configInput(DECAY_CV_INPUT, "Decay CV");
        configInput(MODE_CV_INPUT, "Mode CV (overrides the switch, 0..5 V)");
        configInput(RANGE_CV_INPUT, "Range CV (overrides the switch, 0..5 V)");
        configInput(TRIG_INPUT, "Trigger");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(ENV_OUTPUT, "Envelope (0..10 V)");
    }

    void initEngine(float sampleRate) {
        sr = sampleRate;
        engine.init(sr);
        smF0.setup(tundo_dsp::kBaseHz, 300.f, sr);
        smHarm.setup(0.3f, 30.f, sr);
        smSpread.setup(0.f, 30.f, sr);
        smMorph.setup(0.f, 30.f, sr);
        smFold.setup(0.f, 30.f, sr);
        smAttack.setup(0.5f, 30.f, sr);
        smDecay.setup(0.35f, 30.f, sr);
        smNoise.setup(0.f, 30.f, sr);
        controlPhase = 0;
        hitLight = 0.f;
        levelEnv = 0.f;
    }

    void onReset() override {
        sr = 0.f;               // force a re-init on the next process()
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "cleanRate", json_boolean(cleanRate));
        json_object_set_new(root, "quantize", json_boolean(quantize));
        json_object_set_new(root, "extendedSpread", json_boolean(extendedSpread));
        json_object_set_new(root, "freeRun", json_boolean(freeRun));
        json_object_set_new(root, "liquidDepth", json_integer(liquidDepth));
        json_object_set_new(root, "outputLevel", json_integer(outputLevel));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = json_object_get(root, "cleanRate");
        if (j) cleanRate = json_boolean_value(j);
        j = json_object_get(root, "quantize");
        if (j) quantize = json_boolean_value(j);
        j = json_object_get(root, "extendedSpread");
        if (j) extendedSpread = json_boolean_value(j);
        j = json_object_get(root, "freeRun");
        if (j) freeRun = json_boolean_value(j);
        j = json_object_get(root, "liquidDepth");
        if (j) liquidDepth = clamp((int)json_integer_value(j), 0, 3);
        j = json_object_get(root, "outputLevel");
        if (j) outputLevel = clamp((int)json_integer_value(j), 0, 2);
    }

    // knob + attenuverted CV, 5 V covering `span` of parameter range
    float knobCv(int param, int att, int input, float span) {
        float v = params[param].getValue();
        if (inputs[input].isConnected())
            v += inputs[input].getVoltage() * 0.2f * params[att].getValue() * span;
        return v;
    }

    // a patched CV overrides the switch entirely, splitting 0..5 V in three
    // the way the hardware does
    int switchOrCv(int param, int input) {
        if (inputs[input].isConnected()) {
            float v = inputs[input].getVoltage();
            if (v >= 3.3333f) return 2;
            if (v >= 1.6667f) return 1;
            return 0;
        }
        return (int)std::lround(params[param].getValue());
    }

    void updateControls() {
        using namespace tundo_dsp;

        p.mode = clamp(switchOrCv(MODE_PARAM, MODE_CV_INPUT), 0, 2);
        int range = clamp(switchOrCv(RANGE_PARAM, RANGE_CV_INPUT), 0, 2);

        // ---- pitch: 1 V/oct, plus the range switch's octave offset
        float oct = params[PITCH_PARAM].getValue() + 2.f * range;
        if (inputs[PITCH_CV_INPUT].isConnected())
            oct += inputs[PITCH_CV_INPUT].getVoltage() * params[PITCH_ATT_PARAM].getValue();
        smF0.target = clampf(kBaseHz * std::exp2(clamp(oct, -12.f, 12.f)),
                             kMinF0, kMaxF0);

        smHarm.target = clamp(knobCv(HARM_PARAM, HARM_ATT_PARAM, HARM_CV_INPUT, 1.f),
                              0.f, 1.f);
        smSpread.target = clamp(knobCv(SPREAD_PARAM, SPREAD_ATT_PARAM,
                                       SPREAD_CV_INPUT, 1.f), 0.f, 1.f);
        smMorph.target = clamp(knobCv(MORPH_PARAM, MORPH_ATT_PARAM,
                                      MORPH_CV_INPUT, 1.f), 0.f, 1.f);
        smFold.target = clamp(knobCv(FOLD_PARAM, FOLD_ATT_PARAM, FOLD_CV_INPUT, 1.f),
                              0.f, 1.f);

        float a = clamp(knobCv(ATTACK_PARAM, ATTACK_ATT_PARAM, ATTACK_CV_INPUT, 1.f),
                        0.f, 1.f);
        smAttack.target = a;
        smNoise.target = a <= 0.5f ? (0.5f - a) * 2.f : 0.f;

        smDecay.target = clamp(knobCv(DECAY_PARAM, DECAY_ATT_PARAM, DECAY_CV_INPUT, 1.f),
                               0.f, 1.f);

        // ---- menu-owned engine configuration
        p.liquidOct = kLiquidDepths[clamp(liquidDepth, 0, 3)];
        p.extendedSpread = extendedSpread;
        p.cleanRate = cleanRate;
        p.quantize = quantize;
    }

    // exponential maps, evaluated once per control block from the smoothed knobs
    void mapSlowParams() {
        p.harm = smHarm.current;
        p.spread = smSpread.current;
        float a = smAttack.current;
        p.attackMs = a <= 0.5f
            ? kMinAttackMs
            : kMinAttackMs * std::pow(kMaxAttackMs / kMinAttackMs, (a - 0.5f) * 2.f);
        float dk = smDecay.current;
        p.decayMs = kMinDecayMs * std::pow(kMaxDecayMs / kMinDecayMs, dk);
        // fully CW with free-run armed holds the envelopes open: tundo becomes
        // a drone, and TRIG only re-strikes the attack
        p.hold = freeRun && dk >= 0.995f;
    }

    void process(const ProcessArgs& args) override {
        if (sr != args.sampleRate)
            initEngine(args.sampleRate);

        if (controlPhase == 0)
            updateControls();

        p.f0 = smF0.tick();
        p.morph = smMorph.tick();
        p.fold = smFold.tick();
        p.noiseAmt = smNoise.tick();
        smHarm.tick();
        smSpread.tick();
        smAttack.tick();
        smDecay.tick();

        if (controlPhase == 0) {
            mapSlowParams();
            engine.updateControls(p);
        }
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        bool hit = trigTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.5f);
        if (buttonTrigger.process(params[HIT_PARAM].getValue(), 0.1f, 0.5f))
            hit = true;
        if (hit) {
            engine.trigger(p);
            hitLight = 1.f;
        }

        float audio, env;
        engine.process(p, audio, env);

        float half = kOutputVpp[clamp(outputLevel, 0, 2)] * 0.5f;
        float volts = clamp(audio * half, -10.f, 10.f);
        outputs[AUDIO_OUTPUT].setVoltage(volts);
        outputs[ENV_OUTPUT].setVoltage(clamp(env * 10.f, 0.f, 10.f));

        levelEnv += (std::fabs(audio) - levelEnv) * 0.002f;
        hitLight = std::max(0.f, hitLight - args.sampleTime * 8.f);
        lights[HIT_LIGHT].setBrightness(hitLight);
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv * 3.f, 0.f, 1.f));
        lights[ENV_LIGHT].setBrightness(clamp(env, 0.f, 1.f));
    }
};

struct TundoWidget : ModuleWidget {
    TundoWidget(Tundo* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/tundo.svg")));

// @layout:begin tundo 71.12 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem HARM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPREAD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MORPH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem HARM_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem SPREAD_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem MORPH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem PITCH_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HARM_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SPREAD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MORPH_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FOLD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ATTACK_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FOLD_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem ATTACK_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem DECAY_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FOLD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ATTACK_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DECAY_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HIT_PARAM TL1105 2.6 param "" 0.0
// @elem RANGE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RANGE_PARAM CKSSThree 2.3 param "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem HIT_LIGHT SmallLight 1.0 light "" 0.0
// @elem ENV_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 9.46 30.50
// @elem LABEL_HARM label 0.0 label "harm" 0.0 26.86 30.50
// @elem LABEL_SPREAD label 0.0 label "spread" 0.0 44.26 30.50
// @elem LABEL_MORPH label 0.0 label "morph" 0.0 61.66 30.50
// @elem LABEL_FOLD label 0.0 label "fold" 0.0 18.16 66.50
// @elem LABEL_ATTACK label 0.0 label "attack" 0.0 35.56 66.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 52.96 66.50
// @elem LABEL_MODE label 0.0 label "mode" 0.0 8.50 101.00
// @elem LABEL_MODECV label 0.0 label "cv" 0.0 19.50 101.00
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 30.56 101.00
// @elem LABEL_HIT label 0.0 label "hit" 0.0 40.56 101.00
// @elem LABEL_RANGECV label 0.0 label "cv" 0.0 51.62 101.00
// @elem LABEL_RANGE label 0.0 label "range" 0.0 62.62 101.00
// @elem BOX_ENV panel_box 7.0 box "" 0.0 27.81 111.00
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 43.31 111.00
// @elem LABEL_ENV label 0.0 label "env" 0.0 27.81 116.50
// @elem LABEL_AUDIO label 0.0 label "audio" 0.0 43.31 116.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 35.56 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(63.50f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(63.50f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9.46f, 22.00f)), module, Tundo::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(26.86f, 22.00f)), module, Tundo::HARM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(44.26f, 22.00f)), module, Tundo::SPREAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(61.66f, 22.00f)), module, Tundo::MORPH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(5.11f, 40.00f)), module, Tundo::PITCH_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(22.51f, 40.00f)), module, Tundo::HARM_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(39.91f, 40.00f)), module, Tundo::SPREAD_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(57.31f, 40.00f)), module, Tundo::MORPH_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.81f, 40.00f)), module, Tundo::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.21f, 40.00f)), module, Tundo::HARM_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(48.61f, 40.00f)), module, Tundo::SPREAD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.01f, 40.00f)), module, Tundo::MORPH_CV_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(18.16f, 58.00f)), module, Tundo::FOLD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(35.56f, 58.00f)), module, Tundo::ATTACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(52.96f, 58.00f)), module, Tundo::DECAY_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(13.81f, 76.00f)), module, Tundo::FOLD_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(31.21f, 76.00f)), module, Tundo::ATTACK_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(48.61f, 76.00f)), module, Tundo::DECAY_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.51f, 76.00f)), module, Tundo::FOLD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(39.91f, 76.00f)), module, Tundo::ATTACK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(57.31f, 76.00f)), module, Tundo::DECAY_CV_INPUT));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(8.50f, 92.50f)), module, Tundo::MODE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.50f, 92.50f)), module, Tundo::MODE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.56f, 92.50f)), module, Tundo::TRIG_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(40.56f, 92.50f)), module, Tundo::HIT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(51.62f, 92.50f)), module, Tundo::RANGE_CV_INPUT));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(62.62f, 92.50f)), module, Tundo::RANGE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.81f, 109.00f)), module, Tundo::ENV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(43.31f, 109.00f)), module, Tundo::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(43.50f, 89.50f)), module, Tundo::HIT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(32.81f, 106.00f)), module, Tundo::ENV_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(48.31f, 106.00f)), module, Tundo::LEVEL_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Tundo* module = getModule<Tundo>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Sample rate",
            {"Fundamental-locked (hardware)", "Clean (oversampled)"},
            [=]() { return module->cleanRate ? 1 : 0; },
            [=](int idx) { module->cleanRate = (idx == 1); }));
        menu->addChild(createBoolPtrMenuItem("16-bit quantization", "",
            &module->quantize));
        menu->addChild(createIndexSubmenuItem("Spread law",
            {"Harmonic -> prime", "Extended (unison -> harmonic -> prime)"},
            [=]() { return module->extendedSpread ? 1 : 0; },
            [=](int idx) { module->extendedSpread = (idx == 1); }));
        menu->addChild(createBoolPtrMenuItem("Free-run at full decay", "",
            &module->freeRun));
        menu->addChild(createIndexSubmenuItem("Liquid pitch depth",
            {"1 octave", "2 octaves", "3 octaves", "4 octaves"},
            [=]() { return module->liquidDepth; },
            [=](int idx) { module->liquidDepth = idx; }));
        menu->addChild(createIndexSubmenuItem("Output level",
            {"5 Vpp", "10 Vpp", "14 Vpp"},
            [=]() { return module->outputLevel; },
            [=](int idx) { module->outputLevel = idx; }));
    }
};

Model* modelTundo = createModel<Tundo, TundoWidget>("tundo");
