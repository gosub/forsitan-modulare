// crepitus.cpp — VCV Rack 2 module
// crepitus (Latin: "a crackling, a rustling, a clattering") is a material
// being worked. Not a foley box with a PAPER button: one point process whose
// events are impacts on a resonant object, and one knob — CRIT — that decides
// whether those events ignore each other or set each other off.
//
//   crit low   independent buckling events with power-law energies. Paper
//              being squeezed, gravel, rain on a roof.
//   crit ~ 1   each event tends to produce one more. Avalanches of every
//              size, which is what a crack front travelling along a line
//              sounds like. Tearing.
//   crit high  runaway, self-limited by how much object is left. Something
//              breaks, the rate collapses, the material recovers, it breaks
//              again.
//
// The engine (src/fracture_dsp.hpp) is shared with ruina, which is the same
// physics behind a different front end. It ports the SDT's crumpling and
// breaking models and joins them with a self-exciting (Hawkes) point process;
// see that header for the mechanism and doc/crepitus.md for the reasoning.
//
// Controls:
//   Knobs : DRIVE, CRIT, EN, FRAG, SIZE, HARD, PITCH, DECAY, MAT
//   In    : DRIVE, CRIT, V/OCT, STRIKE
//   Out   : OUT, EVENT
//   Lights: output level, events

#include "forsitan.hpp"
#include "fracture_dsp.hpp"

namespace crepitus {

// How many average events the intact object is worth, and how fast it knits
// itself back together, in units of the base event rate. Together these set
// where the process parks: with the rate scaled by integrity, equilibrium is
// integrity = R(1-crit)/(1 + R(1-crit)) below criticality and 1/crit above.
// R = 40 keeps the subcritical settings near intact (so DRIVE controls
// density, not loudness) while still letting a supercritical cascade dig a
// hole it takes a moment to fill.
static const double kBudget = 200.0;
static const double kRecovery = 40.0;

}  // namespace crepitus

struct Crepitus : Module {
    enum ParamId {
        DRIVE_PARAM,
        CRIT_PARAM,
        ENERGY_PARAM,
        FRAG_PARAM,
        SIZE_PARAM,
        HARD_PARAM,
        PITCH_PARAM,
        DECAY_PARAM,
        MAT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        DRIVE_INPUT,
        CRIT_INPUT,
        VOCT_INPUT,
        STRIKE_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        EVENT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        AUDIO_LIGHT,
        EVENT_LIGHT,
        LIGHTS_LEN
    };

    fracture::Engine engine;
    sdt::Material mat;
    dsp::PulseGenerator eventPulse;
    dsp::SchmittTrigger strikeTrigger;
    float lastSampleRate = 0.f;
    int ctlCount = 0;
    double lastF0 = -1.0, lastDecay = -1.0, lastMat = -1.0;
    float levelEnv = 0.f, eventEnv = 0.f;
    float outGain = 12000.f;
    bool limiter = true;

    Crepitus() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // Per instance, or two of these in a patch fire the same events at the
        // same samples.
        engine.rng = sdt::Rng(random::u32());
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.5f, "Drive (event rate)", " /s", 6000.f, 0.5f);
        configParam(CRIT_PARAM, 0.f, 3.f, 0.6f, "Criticality (branching ratio)");
        configParam(ENERGY_PARAM, 0.f, 1.f, 0.55f, "Event energy", " J", 100.f, 0.05f);
        configParam(FRAG_PARAM, 0.f, 1.f, 0.5f, "Fragmentation", "%", 0.f, 100.f);
        configParam(SIZE_PARAM, 0.f, 1.f, 0.8f, "Fragment size", "%", 33.f, 3.f);
        configParam(HARD_PARAM, 0.f, 1.f, 0.4f, "Hardness (contact stiffness)", "%", 0.f, 100.f);
        configParam(PITCH_PARAM, std::log2(30.f), std::log2(3000.f), std::log2(600.f),
                    "Pitch", " Hz", 2.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.3f, "Decay", "%", 0.f, 100.f);
        configParam(MAT_PARAM, 0.f, 1.f, 0.6f, "Material (rubber to glass)", "%", 0.f, 100.f);
        configInput(DRIVE_INPUT, "Drive CV");
        configInput(CRIT_INPUT, "Criticality CV");
        configInput(VOCT_INPUT, "1V/oct pitch");
        configInput(STRIKE_INPUT, "Strike (one event on a rising edge)");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(EVENT_OUTPUT, "Event trigger");
        configLight(AUDIO_LIGHT, "Output level");
        configLight(EVENT_LIGHT, "Events");
    }

    void onReset() override {
        engine.reset();
        engine.integrity = 1.0;
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
            engine.integrity = 1.0;
            lastSampleRate = args.sampleRate;
            lastF0 = lastDecay = lastMat = -1.0;
            ctlCount = 0;
        }

        const double drive = sdt::fclip(params[DRIVE_PARAM].getValue()
                                        + inputs[DRIVE_INPUT].getVoltage() * 0.1, 0.0, 1.0);
        const double baseRate = 0.5 * std::pow(6000.0, drive);

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
            // The avalanche timescale follows the process: eight events' worth
            // of time, so a slow drive gives long bursts you hear as bursts
            // and a fast one gives a dense roar.
            engine.setTau(sdt::fclip(8.0 / baseRate, 0.004, 0.4));
            engine.impact.setStiffness(1e5 * std::pow(5e4, params[HARD_PARAM].getValue()));
        }

        engine.baseRate = baseRate;
        engine.branching = sdt::fclip(params[CRIT_PARAM].getValue()
                                      + inputs[CRIT_INPUT].getVoltage() * 0.3, 0.0, 3.5);
        engine.crushingEnergy = 0.05 * std::pow(100.0, params[ENERGY_PARAM].getValue());
        engine.fragmentation = sdt::fclip(params[FRAG_PARAM].getValue(), 0.0, 1.0);
        engine.baseSize = 0.03 * std::pow(33.0, params[SIZE_PARAM].getValue());

        fracture::Result r = engine.process(true);

        // A strike from outside is an event the process did not choose, so it
        // costs the object the same integrity as five of its own.
        if (strikeTrigger.process(inputs[STRIKE_INPUT].getVoltage(), 0.1f, 1.f)) {
            const fracture::Result s = engine.strike(engine.crushingEnergy * 5.0 * engine.integrity,
                                                     engine.baseSize);
            r.audio += s.audio;
            r.event = true;
            engine.integrity = std::max(0.0, engine.integrity - 5.0 / crepitus::kBudget);
        }

        if (r.event) {
            engine.integrity = std::max(0.0, engine.integrity - 1.0 / crepitus::kBudget);
            eventPulse.trigger(1e-4f);
        }
        // The material knits itself back together as fast as it is being
        // worked, so DRIVE sets density rather than loudness.
        engine.integrity += (1.0 - engine.integrity) * baseRate * crepitus::kRecovery
                            / crepitus::kBudget * args.sampleTime;
        engine.integrity = sdt::fclip(engine.integrity, 0.0, 1.0);

        float out = (float)(r.audio * outGain);
        if (limiter) out = 10.f * std::tanh(out * 0.1f);
        out = clamp(out, -10.f, 10.f);
        outputs[AUDIO_OUTPUT].setVoltage(out);

        const bool ev = eventPulse.process(args.sampleTime);
        outputs[EVENT_OUTPUT].setVoltage(ev ? 10.f : 0.f);

        levelEnv += (std::fabs(out) * 0.2f - levelEnv) * 0.002f;
        eventEnv += ((ev ? 1.f : 0.f) - eventEnv) * 0.002f;
        lights[AUDIO_LIGHT].setBrightness(clamp(levelEnv * 1.6f, 0.f, 1.f));
        lights[EVENT_LIGHT].setBrightness(clamp(eventEnv * 8.f, 0.f, 1.f));
    }
};

struct CrepitusWidget : ModuleWidget {
    CrepitusWidget(Crepitus* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/crepitus.svg")));

// @layout:begin crepitus 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem DRIVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CRIT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ENERGY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FRAG_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SIZE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem HARD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MAT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DRIVE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CRIT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem STRIKE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem EVENT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem EVENT_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 12.70 30.50
// @elem LABEL_CRIT label 0.0 label "crit" 0.0 25.40 30.50
// @elem LABEL_ENERGY label 0.0 label "en" 0.0 38.10 30.50
// @elem LABEL_FRAG label 0.0 label "frag" 0.0 12.70 50.50
// @elem LABEL_SIZE label 0.0 label "size" 0.0 25.40 50.50
// @elem LABEL_HARD label 0.0 label "hard" 0.0 38.10 50.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 12.70 70.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 25.40 70.50
// @elem LABEL_MAT label 0.0 label "mat" 0.0 38.10 70.50
// @elem LABEL_DRIVECV label 0.0 label "drv" 0.0 8.15 91.50
// @elem LABEL_CRITCV label 0.0 label "crt" 0.0 19.65 91.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 31.15 91.50
// @elem LABEL_STRIKE label 0.0 label "hit" 0.0 42.65 91.50
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 15.00 106.00
// @elem BOX_EVENT panel_box 7.0 box "" 0.0 35.00 106.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 15.00 111.50
// @elem LABEL_EVENT label 0.0 label "ev" 0.0 35.00 111.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 22.00f)), module, Crepitus::DRIVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 22.00f)), module, Crepitus::CRIT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 22.00f)), module, Crepitus::ENERGY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 42.00f)), module, Crepitus::FRAG_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 42.00f)), module, Crepitus::SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 42.00f)), module, Crepitus::HARD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 62.00f)), module, Crepitus::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 62.00f)), module, Crepitus::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 62.00f)), module, Crepitus::MAT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.15f, 84.00f)), module, Crepitus::DRIVE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.65f, 84.00f)), module, Crepitus::CRIT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.15f, 84.00f)), module, Crepitus::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.65f, 84.00f)), module, Crepitus::STRIKE_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.00f, 104.00f)), module, Crepitus::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(35.00f, 104.00f)), module, Crepitus::EVENT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(20.00f, 101.00f)), module, Crepitus::AUDIO_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(40.00f, 101.00f)), module, Crepitus::EVENT_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Crepitus* m = dynamic_cast<Crepitus*>(module);
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
    }
};

Model* modelCrepitus = createModel<Crepitus, CrepitusWidget>("crepitus");
