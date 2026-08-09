// ruina.cpp — VCV Rack 2 module
// ruina (Latin: "a falling down, a collapse, a ruin") is the same fracture
// engine as crepitus with the other framing, and the more forsitan of the
// two: a CV loads an object, the object accumulates damage, creaks and
// crackles as it goes, and decides for itself when it has had enough.
//
// The point is the trigger, not the crash. Failure is drawn from a Weibull
// distribution, so the same gesture twice gives two different lifetimes: the
// patch can lean on the object but it cannot tell it when to give. BRIT is
// the Weibull shape — at the low end the object may fail at any moment, at
// the high end it fails almost exactly when it is due.
//
// While it holds, the acoustic emission is real physics too: the event rate
// climbs with the square of the accumulated damage, and the events start to
// correlate as failure approaches (the branching ratio of the process rises
// with damage), so a critically loaded object crackles in avalanches before
// it goes. That is how a strain gauge and a microphone see the same thing.
//
// Engine: src/fracture_dsp.hpp, shared with crepitus. See doc/ruina.md.
//
// Controls:
//   Knobs : LOAD, TOUGH, BRIT, EN, FRAG, HARD, PITCH, DECAY, MAT
//   In    : LOAD, TOUGH, RESET, V/OCT
//   Out   : OUT, BREAK, STRAIN
//   Lights: output level, break, strain

#include "forsitan.hpp"
#include "fracture_dsp.hpp"

namespace ruina {

// Damage is normalised so that the failure threshold has a mean near 1
// whatever the Weibull shape: gamma(1 + 1/k) is between 0.89 and 1 for every
// shape on the knob. So STRAIN can be read as "fraction of the way there"
// without leaking the draw the patch is not supposed to know.
static const double kBudget = 200.0;      // events the breaking object is worth
static const double kBreakRate = 4000.0;  // events/s during the collapse
static const double kBreakCrit = 2.2;     // branching ratio during the collapse
static const double kDone = 0.03;         // integrity at which the collapse is over

}  // namespace ruina

struct Ruina : Module {
    enum ParamId {
        LOAD_PARAM,
        TOUGH_PARAM,
        BRIT_PARAM,
        ENERGY_PARAM,
        FRAG_PARAM,
        HARD_PARAM,
        PITCH_PARAM,
        DECAY_PARAM,
        MAT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        LOAD_INPUT,
        TOUGH_INPUT,
        RESET_INPUT,
        VOCT_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        BREAK_OUTPUT,
        STRAIN_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        AUDIO_LIGHT,
        BREAK_LIGHT,
        STRAIN_LIGHT,
        LIGHTS_LEN
    };

    fracture::Engine engine;
    sdt::Material mat;
    sdt::Rng rng{20260815u};
    dsp::PulseGenerator breakPulse;
    dsp::SchmittTrigger resetTrigger;
    float lastSampleRate = 0.f;
    int ctlCount = 0;
    double lastF0 = -1.0, lastDecay = -1.0, lastMat = -1.0;
    double damage = 0.0;      // accumulated, normalised so failure is near 1
    double threshold = 1.0;   // this object's Weibull draw, never exposed
    bool failing = false;
    float levelEnv = 0.f, breakEnv = 0.f;
    float outGain = 12000.f;
    bool limiter = true;

    Ruina() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // Per instance, and it matters more here than anywhere: the failure
        // threshold is one draw from this stream, so a fixed seed would make
        // every object in every patch fail at exactly the same moment.
        engine.rng = sdt::Rng(random::u32());
        rng = sdt::Rng(random::u32());
        configParam(LOAD_PARAM, 0.f, 1.f, 0.4f, "Load", "%", 0.f, 100.f);
        configParam(TOUGH_PARAM, 0.f, 1.f, 0.5f, "Toughness (life at full load)", " s",
                    1000.f, 0.1f);
        configParam(BRIT_PARAM, 0.f, 1.f, 0.45f, "Brittleness (Weibull shape)", "", 40.f, 0.5f);
        configParam(ENERGY_PARAM, 0.f, 1.f, 0.55f, "Event energy", " J", 100.f, 0.05f);
        configParam(FRAG_PARAM, 0.f, 1.f, 0.5f, "Fragmentation", "%", 0.f, 100.f);
        configParam(HARD_PARAM, 0.f, 1.f, 0.5f, "Hardness (contact stiffness)", "%", 0.f, 100.f);
        configParam(PITCH_PARAM, std::log2(30.f), std::log2(3000.f), std::log2(400.f),
                    "Pitch", " Hz", 2.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.35f, "Decay", "%", 0.f, 100.f);
        configParam(MAT_PARAM, 0.f, 1.f, 0.7f, "Material (rubber to glass)", "%", 0.f, 100.f);
        configInput(LOAD_INPUT, "Load CV");
        configInput(TOUGH_INPUT, "Toughness CV");
        configInput(RESET_INPUT, "Reset (a new object, undamaged)");
        configInput(VOCT_INPUT, "1V/oct pitch");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(BREAK_OUTPUT, "Failure trigger");
        configOutput(STRAIN_OUTPUT, "Accumulated damage, 0-10 V");
        configLight(AUDIO_LIGHT, "Output level");
        configLight(BREAK_LIGHT, "Failure");
        configLight(STRAIN_LIGHT, "Damage");
    }

    // A fresh object: undamaged, and with a lifetime the patch cannot know.
    void newObject() {
        damage = 0.0;
        failing = false;
        engine.integrity = 1.0;
        drawThreshold();
    }

    void drawThreshold() {
        const double k = 0.5 * std::pow(40.0, params[BRIT_PARAM].getValue());
        // Weibull(shape k, scale 1). At k = 0.5 the object may go at any
        // moment; at k = 20 it goes when it is due, give or take a few
        // per cent.
        threshold = std::pow(-std::log(1.0 - rng.frand()), 1.0 / std::max(0.1, k));
        threshold = sdt::fclip(threshold, 0.02, 6.0);
    }

    void onReset() override {
        engine.reset();
        newObject();
        lastSampleRate = 0.f;
        lastF0 = lastDecay = lastMat = -1.0;
    }

    void onSampleRateChange() override { lastSampleRate = 0.f; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "outGain", json_real(outGain));
        json_object_set_new(root, "limiter", json_boolean(limiter));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "outGain"))
            outGain = clamp((float)json_real_value(j), 500.f, 200000.f);
        if (json_t* j = json_object_get(root, "limiter")) limiter = json_boolean_value(j);
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != lastSampleRate) {
            engine.init(args.sampleRate);
            newObject();
            lastSampleRate = args.sampleRate;
            lastF0 = lastDecay = lastMat = -1.0;
            ctlCount = 0;
        }

        if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) newObject();

        const double load = sdt::fclip(params[LOAD_PARAM].getValue()
                                       + inputs[LOAD_INPUT].getVoltage() * 0.1, 0.0, 1.0);
        const double toughKnob = sdt::fclip(params[TOUGH_PARAM].getValue()
                                            + inputs[TOUGH_INPUT].getVoltage() * 0.1, 0.0, 1.0);
        const double tough = 0.1 * std::pow(1000.0, toughKnob);   // seconds at full load

        if (ctlCount-- <= 0) {
            ctlCount = 15;
            const double matKnob = sdt::fclip(params[MAT_PARAM].getValue(), 0.0, 1.0);
            const double f0 = clamp(std::exp2(params[PITCH_PARAM].getValue()
                                              + inputs[VOCT_INPUT].getVoltage()),
                                    20.f, 9000.f);
            const double decay = 0.005 * std::pow(400.0, params[DECAY_PARAM].getValue());
            if (matKnob != lastMat) {
                sdt::blendMaterial(matKnob, mat);
                lastMat = matKnob;
                lastF0 = -1.0;
            }
            if (f0 != lastF0 || decay != lastDecay) {
                engine.setTone(mat, f0, decay);
                lastF0 = f0;
                lastDecay = decay;
            }
            engine.impact.setStiffness(1e5 * std::pow(5e4, params[HARD_PARAM].getValue()));
        }

        engine.crushingEnergy = 0.05 * std::pow(100.0, params[ENERGY_PARAM].getValue());
        engine.fragmentation = sdt::fclip(params[FRAG_PARAM].getValue(), 0.0, 1.0);
        engine.baseSize = 1.0;

        if (!failing) {
            // Damage accumulates as the cube of the load: a Basquin-style
            // power law, and the reason leaning on the object hard is not
            // twice as bad as leaning on it half as hard.
            damage += load * load * load / tough * args.sampleTime;

            // Acoustic emission. The rate climbs with the square of the
            // damage, and the events start to correlate as failure
            // approaches: a critically loaded object crackles in avalanches
            // before it goes.
            engine.baseRate = 600.0 * load * load * (0.05 + damage * damage);
            engine.branching = std::min(0.9, damage);
            engine.setTau(0.05);
            engine.integrity = 1.0;

            if (damage >= threshold) {
                failing = true;
                breakPulse.trigger(1e-3f);
                // The break itself, then the cascade behind it.
                engine.strike(engine.crushingEnergy * 30.0, 1.0);
                engine.baseRate = ruina::kBreakRate;
                engine.branching = ruina::kBreakCrit;
                engine.setTau(0.02);
            }
        } else {
            engine.baseRate = ruina::kBreakRate;
            engine.branching = ruina::kBreakCrit;
        }

        const fracture::Result r = engine.process(true);
        if (failing) {
            if (r.event)
                engine.integrity = std::max(0.0, engine.integrity - 1.0 / ruina::kBudget);
            // Once there is nothing left to break, hand the patch a new
            // object. Nothing is sounding by then: the rate scales with what
            // is left, so the cascade has already gone quiet.
            if (engine.integrity <= ruina::kDone) newObject();
        }

        float out = (float)(r.audio * outGain);
        if (limiter) out = 10.f * std::tanh(out * 0.1f);
        out = clamp(out, -10.f, 10.f);
        outputs[AUDIO_OUTPUT].setVoltage(out);

        const bool brk = breakPulse.process(args.sampleTime);
        outputs[BREAK_OUTPUT].setVoltage(brk ? 10.f : 0.f);
        // Damage against the nominal threshold, not the drawn one: the patch
        // watches it climb without being told when it will end.
        const float strain = clamp((float)damage * 10.f, 0.f, 10.f);
        outputs[STRAIN_OUTPUT].setVoltage(strain);

        levelEnv += (std::fabs(out) * 0.2f - levelEnv) * 0.002f;
        breakEnv += ((brk ? 1.f : 0.f) - breakEnv) * 0.002f;
        lights[AUDIO_LIGHT].setBrightness(clamp(levelEnv * 1.6f, 0.f, 1.f));
        lights[BREAK_LIGHT].setBrightness(clamp(breakEnv * 8.f, 0.f, 1.f));
        lights[STRAIN_LIGHT].setBrightness(clamp(strain * 0.1f, 0.f, 1.f));
    }
};

struct RuinaWidget : ModuleWidget {
    RuinaWidget(Ruina* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/ruina.svg")));

// @layout:begin ruina 60.96 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem LOAD_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TOUGH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BRIT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ENERGY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FRAG_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem HARD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MAT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LOAD_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TOUGH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem BREAK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem STRAIN_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem BREAK_LIGHT SmallLight 1.0 light "" 0.0
// @elem STRAIN_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_LOAD label 0.0 label "load" 0.0 12.70 33.50
// @elem LABEL_TOUGH label 0.0 label "tough" 0.0 32.00 30.50
// @elem LABEL_BRIT label 0.0 label "brit" 0.0 48.26 30.50
// @elem LABEL_ENERGY label 0.0 label "en" 0.0 12.70 52.50
// @elem LABEL_FRAG label 0.0 label "frag" 0.0 30.48 52.50
// @elem LABEL_HARD label 0.0 label "hard" 0.0 48.26 52.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 12.70 72.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 30.48 72.50
// @elem LABEL_MAT label 0.0 label "mat" 0.0 48.26 72.50
// @elem LABEL_LOADCV label 0.0 label "load" 0.0 9.50 91.50
// @elem LABEL_TOUGHCV label 0.0 label "tgh" 0.0 23.50 91.50
// @elem LABEL_RESET label 0.0 label "rst" 0.0 37.50 91.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 51.50 91.50
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 11.50 106.00
// @elem BOX_BREAK panel_box 7.0 box "" 0.0 30.48 106.00
// @elem BOX_STRAIN panel_box 7.0 box "" 0.0 49.46 106.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 11.50 111.50
// @elem LABEL_BREAK label 0.0 label "brk" 0.0 30.48 111.50
// @elem LABEL_STRAIN label 0.0 label "str" 0.0 49.46 111.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(12.70f, 22.00f)), module, Ruina::LOAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(32.00f, 22.00f)), module, Ruina::TOUGH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.26f, 22.00f)), module, Ruina::BRIT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 44.00f)), module, Ruina::ENERGY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 44.00f)), module, Ruina::FRAG_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.26f, 44.00f)), module, Ruina::HARD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 64.00f)), module, Ruina::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.48f, 64.00f)), module, Ruina::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.26f, 64.00f)), module, Ruina::MAT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.50f, 84.00f)), module, Ruina::LOAD_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(23.50f, 84.00f)), module, Ruina::TOUGH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.50f, 84.00f)), module, Ruina::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(51.50f, 84.00f)), module, Ruina::VOCT_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.50f, 104.00f)), module, Ruina::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.48f, 104.00f)), module, Ruina::BREAK_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(49.46f, 104.00f)), module, Ruina::STRAIN_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(16.50f, 101.00f)), module, Ruina::AUDIO_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(35.48f, 101.00f)), module, Ruina::BREAK_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(54.46f, 101.00f)), module, Ruina::STRAIN_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Ruina* m = dynamic_cast<Ruina*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Output gain",
            {"-12 dB", "-6 dB", "0 dB (default)", "+6 dB", "+12 dB"},
            [m]() {
                const float g[5] = {3000.f, 6000.f, 12000.f, 24000.f, 48000.f};
                int best = 2;
                for (int i = 0; i < 5; i++)
                    if (std::fabs(m->outGain - g[i]) < std::fabs(m->outGain - g[best])) best = i;
                return best;
            },
            [m](int i) {
                const float g[5] = {3000.f, 6000.f, 12000.f, 24000.f, 48000.f};
                m->outGain = g[i];
            }));
        menu->addChild(createBoolPtrMenuItem("Output limiter", "", &m->limiter));
        menu->addChild(createMenuItem("New object now", "", [m]() { m->newObject(); }));
    }
};

Model* modelRuina = createModel<Ruina, RuinaWidget>("ruina");
