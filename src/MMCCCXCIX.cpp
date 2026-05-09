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
    LinearSmoother smoothTime, smoothFb, smoothMix, smoothFbLoopMix;

    MMCCCXCIX() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(TIME_PARAM,       0.f,  1.f,  0.15f, "Delay Time",    " ms", 0.f, 1140.f, 35.f);
        configParam(FEEDBACK_PARAM,   0.f,  1.8f, 0.4f,  "Feedback",      "%",   0.f, 100.f * (1.f/1.8f));
        configParam(MIX_PARAM,        0.f,  1.f,  0.5f,  "Dry/Wet");
        configParam(BRIGHTNESS_PARAM, 0.f,  1.f,  0.f,   "Brightness");
        configParam(FB_LOOP_MIX_PARAM,0.f,  1.f,  1.f,   "FB Loop Mix",   "%",   0.f, 100.f);

        configInput(AUDIO_INPUT,         "Audio");
        configInput(TIME_CV_INPUT,        "Time CV");
        configInput(FEEDBACK_CV_INPUT,    "Feedback CV");
        configInput(MIX_CV_INPUT,         "Mix CV");
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

        // ── static parameters (no CV, low update cost) ──────────────────────
        core.setBrightness(params[BRIGHTNESS_PARAM].getValue());
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
// @elem FEEDBACK_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem MIX_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem BRIGHTNESS_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FB_LOOP_MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TIME_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FEEDBACK_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FB_LOOP_MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FB_SEND_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem FB_LOOP_ACTIVE_LIGHT SmallLight 1.5 light "" 0.0
// @elem FB_RETURN_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LABEL_TIME label 0.0 label "time" 0.0 25.40 13.00
// @elem LABEL_FEEDBACK label 0.0 label "feedback" 0.0 14.20 32.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 36.60 32.50
// @elem LABEL_BRIGHT label 0.0 label "bright" 0.0 14.20 50.50
// @elem LABEL_FBLP label 0.0 label "fblp" 0.0 36.60 50.50
// @elem LABEL_IN label 0.0 label "in" 0.0 10.00 70.50
// @elem LABEL_TIMECV label 0.0 label "cv" 0.0 25.00 70.50
// @elem LABEL_FBCV label 0.0 label "cv" 0.0 40.00 70.50
// @elem LABEL_MIXCV label 0.0 label "cv" 0.0 10.00 82.50
// @elem LABEL_FLPCV label 0.0 label "cv" 0.0 25.00 82.50
// @elem LABEL_SEND label 0.0 label "send" 0.0 15.00 97.50
// @elem LABEL_RETURN label 0.0 label "return" 0.0 35.00 97.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 25.40 112.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 7.00 120.00

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(25.40f, 18.00f)), module, MMCCCXCIX::TIME_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(14.20f, 40.00f)), module, MMCCCXCIX::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(36.60f, 40.00f)), module, MMCCCXCIX::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.20f, 58.00f)), module, MMCCCXCIX::BRIGHTNESS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.60f, 58.00f)), module, MMCCCXCIX::FB_LOOP_MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 76.00f)), module, MMCCCXCIX::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.00f, 76.00f)), module, MMCCCXCIX::TIME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.00f, 76.00f)), module, MMCCCXCIX::FEEDBACK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 88.00f)), module, MMCCCXCIX::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.00f, 88.00f)), module, MMCCCXCIX::FB_LOOP_MIX_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.00f, 103.00f)), module, MMCCCXCIX::FB_SEND_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(25.40f, 103.00f)), module, MMCCCXCIX::FB_LOOP_ACTIVE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.00f, 103.00f)), module, MMCCCXCIX::FB_RETURN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.40f, 116.00f)), module, MMCCCXCIX::AUDIO_OUTPUT));
        // @layout:end
    }
};

Model* modelMMCCCXCIX = createModel<MMCCCXCIX, MMCCCXCIXWidget>("MMCCCXCIX");
