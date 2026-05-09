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

    PT2399Core   core{16};
    OnePoleCompressor comp;

    // smoothers for all modulatable parameters
    LinearSmoother smoothTime, smoothFb, smoothMix, smoothBright, smoothFbLoopMix;

    MMCCCXCIX() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

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
        core.setFeedbackHighPassHz(80.f);  // fixed: models the physical cap

        // ── feedback loop ────────────────────────────────────────────────────
        const bool returnConnected = inputs[FB_RETURN_INPUT].isConnected();

        const float returnSignal = returnConnected
            ? inputs[FB_RETURN_INPUT].getVoltage() * 0.1f  // V → normalized
            : 0.f;

        // ── audio ────────────────────────────────────────────────────────────
        const float input = inputs[AUDIO_INPUT].getVoltage() * 0.1f;  // V → normalized

        float wet = core.processSample(input, returnSignal, returnConnected, fbLoopMix);
        wet = comp.process(wet);

        // SEND output: pre-HPF feedback signal in Eurorack volts
        outputs[FB_SEND_OUTPUT].setVoltage(core.getFeedbackPreHpf() * 10.f);

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
// @elem LABEL_TIME label 0.0 label "time" 0.0 14.25 32.83
// @elem LABEL_FEEDBACK label 0.0 label "feedback" 0.0 9.01 50.81
// @elem LABEL_MIX label 0.0 label "dry/wet" 0.0 40.52 50.42
// @elem LABEL_BRIGHT label 0.0 label "bright" 0.0 36.73 28.67
// @elem LABEL_FBLP label 0.0 label "fb mix" 0.0 25.36 50.30
// @elem LABEL_IN label 0.0 label "in" 0.0 12.41 114.56
// @elem LABEL_BRIGHTCV label 0.0 label "bright" 0.0 36.73 67.84
// @elem LABEL_TIMECV label 0.0 label "time" 0.0 17.02 67.84
// @elem LABEL_FBCV label 0.0 label "fb" 0.0 8.79 80.50
// @elem LABEL_MIXCV label 0.0 label "wet" 0.0 41.27 80.48
// @elem LABEL_FLPCV label 0.0 label "fb mix" 0.0 25.44 80.73
// @elem LABEL_SEND label 0.0 label "send" 0.0 12.09 98.18
// @elem LABEL_RETURN label 0.0 label "return" 0.0 37.03 98.47
// @elem LABEL_OUT label 0.0 label "out" 0.0 37.34 114.40
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 23.96 119.87
// @elem BOX_SEND panel_box 7.0 box "" 0.0 11.96 92.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 37.34 108.40

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(14.25f, 19.65f)), module, MMCCCXCIX::TIME_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9.01f, 42.04f)), module, MMCCCXCIX::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.71f, 41.91f)), module, MMCCCXCIX::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.73f, 20.39f)), module, MMCCCXCIX::BRIGHTNESS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.36f, 41.91f)), module, MMCCCXCIX::FB_LOOP_MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.28f, 107.02f)), module, MMCCCXCIX::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.90f, 59.92f)), module, MMCCCXCIX::TIME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.79f, 72.71f)), module, MMCCCXCIX::FEEDBACK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.14f, 73.06f)), module, MMCCCXCIX::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(36.73f, 59.92f)), module, MMCCCXCIX::BRIGHTNESS_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.44f, 72.93f)), module, MMCCCXCIX::FB_LOOP_MIX_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.09f, 91.86f)), module, MMCCCXCIX::FB_SEND_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(25.65f, 91.73f)), module, MMCCCXCIX::FB_LOOP_ACTIVE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.15f, 91.35f)), module, MMCCCXCIX::FB_RETURN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.34f, 107.65f)), module, MMCCCXCIX::AUDIO_OUTPUT));
        // @layout:end
    }
};

Model* modelMMCCCXCIX = createModel<MMCCCXCIX, MMCCCXCIXWidget>("MMCCCXCIX");
