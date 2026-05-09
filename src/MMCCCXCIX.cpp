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

        // Screws (1HP inset, matching other forsitan modules)
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2*RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2*RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // ── TIME (big knob, centre) ──────────────────────────────────────────
        addParam(createParamCentered<RoundHugeBlackKnob>(
            mm2px(Vec(25.4f, 18.f)),
            module, MMCCCXCIX::TIME_PARAM));

        // ── FEEDBACK + MIX ────────────────────────────────────────────────────
        addParam(createParamCentered<RoundBigBlackKnob>(
            mm2px(Vec(14.2f, 40.f)),
            module, MMCCCXCIX::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(
            mm2px(Vec(36.6f, 40.f)),
            module, MMCCCXCIX::MIX_PARAM));

        // ── BRIGHTNESS + FB LOOP MIX ─────────────────────────────────────────
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(14.2f, 58.f)),
            module, MMCCCXCIX::BRIGHTNESS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(36.6f, 58.f)),
            module, MMCCCXCIX::FB_LOOP_MIX_PARAM));

        // ── CV inputs row 1: AUDIO IN, TIME CV, FEEDBACK CV ──────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(10.f, 76.f)), module, MMCCCXCIX::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(25.f, 76.f)), module, MMCCCXCIX::TIME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(40.f, 76.f)), module, MMCCCXCIX::FEEDBACK_CV_INPUT));

        // ── CV inputs row 2: MIX CV, FB LOOP MIX CV ──────────────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(10.f, 88.f)), module, MMCCCXCIX::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(25.f, 88.f)), module, MMCCCXCIX::FB_LOOP_MIX_CV_INPUT));

        // ── Feedback loop ─────────────────────────────────────────────────────
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(15.f, 103.f)), module, MMCCCXCIX::FB_SEND_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(25.4f, 103.f)), module, MMCCCXCIX::FB_LOOP_ACTIVE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(35.f, 103.f)), module, MMCCCXCIX::FB_RETURN_INPUT));

        // ── Audio output ──────────────────────────────────────────────────────
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(25.4f, 116.f)), module, MMCCCXCIX::AUDIO_OUTPUT));
    }
};

Model* modelMMCCCXCIX = createModel<MMCCCXCIX, MMCCCXCIXWidget>("MMCCCXCIX");
