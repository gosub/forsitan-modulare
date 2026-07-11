// MMCCCXCIX.cpp — VCV Rack 2 module
// Features:
//   - PT2399 DSP core (schollz/onebitdelay port)
//   - Knobs: TIME, FEEDBACK, MIX, BRIGHTNESS, FB_LOOP_MIX
//   - CV inputs: TIME, FEEDBACK, MIX, FB_LOOP_MIX
//   - Feedback send/return loop with normalled bypass
//   - Compressor on output

#include "forsitan.hpp"
#include "MMCCCXCIX.h"

using namespace pt2399;

// ── helper: resistance from delay time ───────────────────────────────────────
// PT2399: delay_ms = 11.46 * R_kOhm + 29.7  → R = (delay_ms - 29.7) / 11.46
static inline float delayMsToR(float ms) {
    return clampf((clampf(ms, 35.f, 1175.f) - 29.7f) / 11.46f, 0.5f, 100.f);
}

// ─────────────────────────────────────────────────────────────────────────────
struct MMCCCXCIX : Module {

    enum ParamId {
        TIME_PARAM,         // delay time 35–600ms (knob)
        FEEDBACK_PARAM,     // feedback amount 0–1.8
        MIX_PARAM,          // dry/wet 0–1
        BRIGHTNESS_PARAM,   // input+output filter brightness 0–1
        FB_LOOP_MIX_PARAM,  // 0=internal fb, 1=return jack only
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        TIME_CV_INPUT,
        FEEDBACK_CV_INPUT,
        MIX_CV_INPUT,
        BRIGHTNESS_CV_INPUT,
        FB_LOOP_MIX_CV_INPUT,
        FB_RETURN_INPUT,    // feedback loop return (normalled: bypass)
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        FB_SEND_OUTPUT,     // feedback loop send
        OUTPUTS_LEN
    };
    enum LightId {
        FB_LOOP_ACTIVE_LIGHT,  // lit when return is connected
        LIGHTS_LEN
    };

    // Bits simulated per RAM clock: the delta-sigma loop runs at
    // 44000/delay * oversampling Hz, so CPU scales linearly with this.
    // 8 is audibly identical to the original hardcoded 16 (THD+N -46.0 vs
    // -46.4 dB, idle noise -68 vs -72 dB) at half the cost; selectable from
    // the context menu down to the raw chip rate.
    static constexpr int kDefaultOversampling = 8;

    PT2399Core   core{kDefaultOversampling};
    OnePoleCompressor comp;
    int oversampling = kDefaultOversampling;

    // smoothers for all modulatable parameters
    LinearSmoother smoothTime, smoothFb, smoothMix, smoothBright, smoothFbLoopMix;

    MMCCCXCIX() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        core.setFeedbackHighPassHz(80.f);  // fixed: models the physical cap

        configParam(TIME_PARAM,       0.f,  1.f,  0.15f, "Delay Time",    " ms", 0.f, 1140.f, 35.f);
        configParam(FEEDBACK_PARAM,   0.f,  1.8f, 0.4f,  "Feedback",      "%",   0.f, 100.f * (1.f/1.8f));
        configParam(MIX_PARAM,        0.f,  1.f,  0.5f,  "Dry/Wet");
        configParam(BRIGHTNESS_PARAM, 0.f,  1.f,  0.f,   "Brightness");
        configParam(FB_LOOP_MIX_PARAM,0.f,  1.f,  1.f,   "FB Loop Mix",   "%",   0.f, 100.f);

        configInput(AUDIO_INPUT,          "Audio");
        configInput(TIME_CV_INPUT,        "Time CV");
        configInput(FEEDBACK_CV_INPUT,    "Feedback CV");
        configInput(MIX_CV_INPUT,         "Mix CV");
        configInput(BRIGHTNESS_CV_INPUT,  "Brightness CV");
        configInput(FB_LOOP_MIX_CV_INPUT, "FB Loop Mix CV");
        configInput(FB_RETURN_INPUT,      "Feedback Loop Return");

        configOutput(AUDIO_OUTPUT,  "Audio");
        configOutput(FB_SEND_OUTPUT,"Feedback Loop Send");

        configLight(FB_LOOP_ACTIVE_LIGHT, "FB Loop Active");
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        const float sr = e.sampleRate;
        core.prepare(sr);
        comp.prepare(sr);
        comp.reset();
        smoothTime.reset(sr, 0.05f,
            timeKnobToMs(params[TIME_PARAM].getValue()));
        smoothFb.reset(sr, 0.05f,  params[FEEDBACK_PARAM].getValue());
        smoothMix.reset(sr, 0.02f, params[MIX_PARAM].getValue());
        smoothBright.reset(sr, 0.02f, params[BRIGHTNESS_PARAM].getValue());
        smoothFbLoopMix.reset(sr, 0.02f, params[FB_LOOP_MIX_PARAM].getValue());
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        core.reset();
        comp.reset();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(oversampling));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            oversampling = clamp((int)json_integer_value(j), 1, 16);
    }

    // ── time knob to milliseconds (quadratic curve: more resolution at low end)
    static float timeKnobToMs(float knob) {
        return 35.f + knob * knob * 1140.f;
    }

    void process(const ProcessArgs& args) override {
        // ── resolve TIME ────────────────────────────────────────────────────
        float timeKnob = params[TIME_PARAM].getValue();
        if (inputs[TIME_CV_INPUT].isConnected())
            timeKnob = clampf(timeKnob + inputs[TIME_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        const float targetMs = timeKnobToMs(timeKnob);
        smoothTime.setTarget(targetMs);
        const float delayMs = smoothTime.next();
        core.setDelayResistanceKOhm(delayMsToR(delayMs));

        // ── resolve FEEDBACK ────────────────────────────────────────────────
        float fbKnob = params[FEEDBACK_PARAM].getValue();
        if (inputs[FEEDBACK_CV_INPUT].isConnected())
            fbKnob = clampf(fbKnob + inputs[FEEDBACK_CV_INPUT].getVoltage() * 0.18f, 0.f, 1.8f);
        smoothFb.setTarget(fbKnob);
        core.setFeedback(smoothFb.next());

        // ── resolve MIX ─────────────────────────────────────────────────────
        float mixKnob = params[MIX_PARAM].getValue();
        if (inputs[MIX_CV_INPUT].isConnected())
            mixKnob = clampf(mixKnob + inputs[MIX_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        smoothMix.setTarget(mixKnob);
        const float mix = smoothMix.next();

        // ── resolve FB LOOP MIX ─────────────────────────────────────────────
        float fbLoopKnob = params[FB_LOOP_MIX_PARAM].getValue();
        if (inputs[FB_LOOP_MIX_CV_INPUT].isConnected())
            fbLoopKnob = clampf(fbLoopKnob + inputs[FB_LOOP_MIX_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        smoothFbLoopMix.setTarget(fbLoopKnob);
        const float fbLoopMix = smoothFbLoopMix.next();

        // ── resolve BRIGHTNESS ──────────────────────────────────────────────
        float brightKnob = params[BRIGHTNESS_PARAM].getValue();
        if (inputs[BRIGHTNESS_CV_INPUT].isConnected())
            brightKnob = clampf(brightKnob + inputs[BRIGHTNESS_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        smoothBright.setTarget(brightKnob);
        core.setBrightness(smoothBright.next());

        // ── static parameters ────────────────────────────────────────────────
        core.setOversampling(oversampling);   // early-outs when unchanged

        // ── feedback loop ────────────────────────────────────────────────────
        const bool returnConnected = inputs[FB_RETURN_INPUT].isConnected();

        const float returnSignal = returnConnected
            ? inputs[FB_RETURN_INPUT].getVoltage() * 0.1f  // V → normalized
            : 0.f;

        // ── audio ────────────────────────────────────────────────────────────
        const float input = inputs[AUDIO_INPUT].getVoltage() * 0.1f;  // V → normalized

        float wet = core.processSample(input, returnSignal, returnConnected, fbLoopMix);
        wet = comp.process(wet);

        // SEND output: the internal feedback signal in Eurorack volts
        outputs[FB_SEND_OUTPUT].setVoltage(core.getFeedbackSend() * 10.f);

        // DRY/WET mix
        const float dry = inputs[AUDIO_INPUT].getVoltage();
        outputs[AUDIO_OUTPUT].setVoltage(dry * (1.f - mix) + wet * 10.f * mix);

        // ── lights ───────────────────────────────────────────────────────────
        lights[FB_LOOP_ACTIVE_LIGHT].setBrightness(returnConnected ? 1.f : 0.f);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
struct MMCCCXCIXWidget : ModuleWidget {
    MMCCCXCIXWidget(MMCCCXCIX* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/MMCCCXCIX.svg")));

// @layout:begin MMCCCXCIX 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem TIME_PARAM RoundHugeBlackKnob 9.0 param "" 0.0
// @elem FEEDBACK_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem BRIGHTNESS_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FB_LOOP_MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TIME_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FEEDBACK_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BRIGHTNESS_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FB_LOOP_MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FB_SEND_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem FB_LOOP_ACTIVE_LIGHT SmallLight 1.5 light "" 0.0
// @elem FB_RETURN_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LABEL_TIME label 0.0 label "time" 0.0 15.27 33.03
// @elem LABEL_FEEDBACK label 0.0 label "fb" 0.0 10.22 52.84
// @elem LABEL_MIX label 0.0 label "wet" 0.0 40.52 52.84
// @elem LABEL_BRIGHT label 0.0 label "bright" 0.0 40.67 29.67
// @elem LABEL_FBLP label 0.0 label "fb mix" 0.0 25.56 52.84
// @elem LABEL_IN label 0.0 label "in" 0.0 10.21 119.16
// @elem LABEL_BRIGHTCV label 0.0 label "bright" 0.0 33.73 69.24
// @elem LABEL_TIMECV label 0.0 label "time" 0.0 17.82 69.04
// @elem LABEL_FBCV label 0.0 label "fb" 0.0 10.19 84.40
// @elem LABEL_MIXCV label 0.0 label "wet" 0.0 40.57 84.48
// @elem LABEL_FLPCV label 0.0 label "fb mix" 0.0 25.44 84.53
// @elem LABEL_SEND label 0.0 label "send" 0.0 14.99 102.00
// @elem LABEL_RETURN label 0.0 label "return" 0.0 35.23 102.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 40.34 119.10
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.46 123.37
// @elem BOX_SEND panel_box 7.0 box "" 0.0 14.86 95.80
// @elem BOX_OUT panel_box 7.0 box "" 0.0 40.14 113.10

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(15.27f, 20.06f)), module, MMCCCXCIX::TIME_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.22f, 43.64f)), module, MMCCCXCIX::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.56f, 43.64f)), module, MMCCCXCIX::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.67f, 20.44f)), module, MMCCCXCIX::BRIGHTNESS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.36f, 43.74f)), module, MMCCCXCIX::FB_LOOP_MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.08f, 111.52f)), module, MMCCCXCIX::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.80f, 61.52f)), module, MMCCCXCIX::TIME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.19f, 76.71f)), module, MMCCCXCIX::FEEDBACK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.54f, 76.66f)), module, MMCCCXCIX::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.33f, 61.52f)), module, MMCCCXCIX::BRIGHTNESS_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.44f, 76.63f)), module, MMCCCXCIX::FB_LOOP_MIX_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.79f, 94.46f)), module, MMCCCXCIX::FB_SEND_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(25.65f, 94.63f)), module, MMCCCXCIX::FB_LOOP_ACTIVE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.35f, 94.35f)), module, MMCCCXCIX::FB_RETURN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.34f, 111.85f)), module, MMCCCXCIX::AUDIO_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        MMCCCXCIX* m = dynamic_cast<MMCCCXCIX*>(module);
        if (!m) return;
        static const int factors[5] = {1, 2, 4, 8, 16};
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Delta-sigma oversampling",
            {"1× (raw chip, lightest)", "2×", "4×",
             "8× (default)", "16× (cleanest, heaviest)"},
            [m]() {
                for (int i = 0; i < 5; ++i)
                    if (m->oversampling <= factors[i]) return i;
                return 4;
            },
            [m](int i) { m->oversampling = factors[i]; }));
    }
};

Model* modelMMCCCXCIX = createModel<MMCCCXCIX, MMCCCXCIXWidget>("MMCCCXCIX");
