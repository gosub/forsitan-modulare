// umbrae.cpp — VCV Rack 2 module
// umbrae (Latin: shadows, and of shadow) is an audio feedback instrument
// after Bastl Instruments and Casper Electronics' Dark Matter. The name is
// the designer's: Casper describes feedback as "the negative space around a
// sound, like a sonic shadow. A dark counterpart."
//
// A signal goes into an overdriven input VCA, through a two-band tone section
// whose boosts saturate, and out of a crossfader. Around the tone section
// runs a feedback loop with a VCA of its own, and that loop is the module:
// below unity it colours whatever passes through, above unity it howls at a
// pitch the circuit picks — low if the bass fader is up, around three
// kilohertz if the treble one is. An envelope follower watches the input and
// is normalled to the feedback and crossfade CV inputs, so what you play
// ducks and gates its own feedback.
//
// The loop can be taken outside: patching FBK IN breaks the internal one, so
// the send and return jacks put a delay, a reverb or a filter inside the
// howl. The polarity switch is there because half the modules you might put
// in the loop invert, and an inverted loop does not feed back.
//
// The engine lives in src/umbrae_dsp.hpp, free of Rack headers so that
// test/umbrae_probe can hold it to the measurement that matters: the
// oscillation pitch is a property of the modelled circuit and does not move
// with the sample rate. That is also why the oversampling menu starts at 4x
// — see doc/umbrae.md, which also separates what the hardware's manual
// documents from what had to be inferred.
//
// Controls:
//   Faders : DRIVE, BASS, TREBLE, FBK, X-FADE
//   Knobs  : BASS BOOST, TREBLE BOOST
//   Trim   : DRIVE CV (attenuator), FBK CV, X-FADE CV (attenuverters)
//   Switch : HYPER, DYNAMICS sense and decay, EXT FBK polarity and loop, SRC
//   In     : IN, BASS CV, TREBLE CV, DRIVE CV, FBK CV, X-FADE CV, RETURN
//   Out    : OUT, ENV, SEND
//   Lights : output level, envelope, HF warning

#include "forsitan.hpp"
#include "umbrae_dsp.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

struct Umbrae : Module {
    enum ParamId {
        DRIVE_PARAM,
        BASS_PARAM,
        TREBLE_PARAM,
        FBK_PARAM,
        XFADE_PARAM,
        BASS_BOOST_PARAM,
        TREBLE_BOOST_PARAM,
        DRIVE_CV_PARAM,
        FBK_CV_PARAM,
        XFADE_CV_PARAM,
        HYPER_PARAM,
        SENSE_PARAM,
        DECAY_PARAM,
        POLARITY_PARAM,
        LOOP_PARAM,
        SRC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        SIGNAL_INPUT,
        BASS_CV_INPUT,
        TREBLE_CV_INPUT,
        DRIVE_CV_INPUT,
        FBK_CV_INPUT,
        XFADE_CV_INPUT,
        RETURN_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        XFADE_OUTPUT,
        DYNAMICS_OUTPUT,
        SEND_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId { OUT_LIGHT, ENV_LIGHT, HF_LIGHT, LIGHTS_LEN };

    umbrae::Engine engine;
    VariableOversampling<> osIn, osReturn;
    VariableOversampling<> osOut[OUTPUTS_LEN];

    // The modelled loop delay needs at least two oversampled samples to
    // interpolate between, so 4x is the floor rather than a preference.
    static const int kMinOsIndex = 2;
    int osIndex = kMinOsIndex;
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;
    bool envLowpass = true;        // the back-panel jumper

    float prevCv[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
    float outEnv = 0.f;

    Umbrae() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.5f, "Drive", "%", 0.f, 100.f);
        configParam(BASS_PARAM, 0.f, 1.f, 0.5f, "Bass", "%", 0.f, 100.f);
        configParam(TREBLE_PARAM, 0.f, 1.f, 0.5f, "Treble", "%", 0.f, 100.f);
        configParam(FBK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
        configParam(XFADE_PARAM, 0.f, 1.f, 0.f, "Crossfade, clean to fed back",
                    "%", 0.f, 100.f);
        configParam(BASS_BOOST_PARAM, 0.f, 1.f, 0.f, "Bass boost", "%", 0.f, 100.f);
        configParam(TREBLE_BOOST_PARAM, 0.f, 1.f, 0.f, "Treble boost", "%", 0.f, 100.f);
        configParam(DRIVE_CV_PARAM, 0.f, 1.f, 0.f, "Drive CV amount", "%", 0.f, 100.f);
        configParam(FBK_CV_PARAM, -1.f, 1.f, 0.f, "Feedback CV amount", "%", 0.f, 100.f);
        configParam(XFADE_CV_PARAM, -1.f, 1.f, 0.f, "Crossfade CV amount", "%", 0.f, 100.f);
        configSwitch(HYPER_PARAM, 0.f, 1.f, 0.f, "Hyper drive",
                     {"off", "on — x7 into the tone section"});
        configSwitch(SENSE_PARAM, 0.f, 1.f, 0.f, "Envelope listens",
                     {"to the input", "after the drive stage"});
        configSwitch(DECAY_PARAM, 0.f, 1.f, 0.f, "Envelope decay",
                     {"short", "long"});
        configSwitch(POLARITY_PARAM, 0.f, 1.f, 0.f, "Send polarity",
                     {"normal", "inverted"});
        configSwitch(LOOP_PARAM, 0.f, 1.f, 1.f, "Feedback VCA sits on",
                     {"the return", "the send"});
        configSwitch(SRC_PARAM, 0.f, 1.f, 0.f, "Clean side of the crossfade",
                     {"the input", "after the drive stage"});
        configInput(SIGNAL_INPUT, "Signal");
        configInput(BASS_CV_INPUT, "Bass boost CV");
        configInput(TREBLE_CV_INPUT, "Treble boost CV");
        configInput(DRIVE_CV_INPUT, "Drive CV");
        configInput(FBK_CV_INPUT, "Feedback CV (normalled to the envelope)");
        configInput(XFADE_CV_INPUT, "Crossfade CV (normalled to the envelope)");
        configInput(RETURN_INPUT, "Feedback return (breaks the internal loop)");
        configOutput(XFADE_OUTPUT, "Crossfade");
        configOutput(DYNAMICS_OUTPUT, "Envelope");
        configOutput(SEND_OUTPUT, "Feedback send");
        configLight(OUT_LIGHT, "Output level");
        configLight(ENV_LIGHT, "Envelope");
        configLight(HF_LIGHT, "High frequency warning");
        configBypass(SIGNAL_INPUT, XFADE_OUTPUT);
    }

    void onReset() override {
        engine.reset();
        for (int i = 0; i < 5; i++) prevCv[i] = 0.f;
        outEnv = 0.f;
        envLowpass = true;
        lastOsIndex = -1;
    }

    void onSampleRateChange() override { lastOsIndex = -1; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        json_object_set_new(root, "envLowpass", json_boolean(envLowpass));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            osIndex = clamp((int)json_integer_value(j), kMinOsIndex, 4);
        if (json_t* j = json_object_get(root, "envLowpass"))
            envLowpass = json_boolean_value(j);
        lastOsIndex = -1;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            osIn.setOversamplingIndex(osIndex);
            osIn.reset(sr);
            osReturn.setOversamplingIndex(osIndex);
            osReturn.reset(sr);
            for (int i = 0; i < OUTPUTS_LEN; i++) {
                osOut[i].setOversamplingIndex(osIndex);
                osOut[i].reset(sr);
            }
            engine.setSampleRate((double)sr * (double)(1 << osIndex));
            engine.reset();
            lastOsIndex = osIndex;
            lastSampleRate = sr;
        }
        const int ratio = 1 << osIndex;

        umbrae::Engine::Controls c;
        c.drive = params[DRIVE_PARAM].getValue();
        c.bass = params[BASS_PARAM].getValue();
        c.treble = params[TREBLE_PARAM].getValue();
        c.fbk = params[FBK_PARAM].getValue();
        c.xfade = params[XFADE_PARAM].getValue();
        c.bassBoost = params[BASS_BOOST_PARAM].getValue();
        c.trebleBoost = params[TREBLE_BOOST_PARAM].getValue();
        c.driveCvAmt = params[DRIVE_CV_PARAM].getValue();
        c.fbkCvAmt = params[FBK_CV_PARAM].getValue();
        c.xfadeCvAmt = params[XFADE_CV_PARAM].getValue();
        c.hyperDrive = params[HYPER_PARAM].getValue() > 0.5f;
        c.dynPostDrive = params[SENSE_PARAM].getValue() > 0.5f;
        c.dynLongDecay = params[DECAY_PARAM].getValue() > 0.5f;
        c.xfadePostDrive = params[SRC_PARAM].getValue() > 0.5f;
        c.invertFbkOut = params[POLARITY_PARAM].getValue() > 0.5f;
        c.cvControlsOut = params[LOOP_PARAM].getValue() > 0.5f;
        c.dynLowpass = envLowpass;
        c.inputPatched = inputs[SIGNAL_INPUT].isConnected();
        c.fbkInPatched = inputs[RETURN_INPUT].isConnected();
        c.fbkCvPatched = inputs[FBK_CV_INPUT].isConnected();
        c.xfadeCvPatched = inputs[XFADE_CV_INPUT].isConnected();

        // CV is interpolated rather than filtered: these carry audio often
        // enough (the manual patches an oscillator into FBK CV to tune the
        // howl) but they land on VCA control ports, where an antialiasing
        // filter's ringing would be a worse artifact than the imaging.
        const float cvNow[5] = {inputs[DRIVE_CV_INPUT].getVoltage(),
                                inputs[FBK_CV_INPUT].getVoltage(),
                                inputs[XFADE_CV_INPUT].getVoltage(),
                                inputs[BASS_CV_INPUT].getVoltage(),
                                inputs[TREBLE_CV_INPUT].getVoltage()};

        osIn.upsample(inputs[SIGNAL_INPUT].getVoltage());
        osReturn.upsample(inputs[RETURN_INPUT].getVoltage());
        float* inBuf = osIn.getOSBuffer();
        float* retBuf = osReturn.getOSBuffer();
        float* outBuf[OUTPUTS_LEN];
        for (int i = 0; i < OUTPUTS_LEN; i++) outBuf[i] = osOut[i].getOSBuffer();

        umbrae::Engine::Frame fr;
        for (int k = 0; k < ratio; k++) {
            const float t = (float)(k + 1) / ratio;
            const double driveCv = prevCv[0] + (cvNow[0] - prevCv[0]) * t;
            const double fbkCv = prevCv[1] + (cvNow[1] - prevCv[1]) * t;
            const double xfadeCv = prevCv[2] + (cvNow[2] - prevCv[2]) * t;
            const double bassCv = prevCv[3] + (cvNow[3] - prevCv[3]) * t;
            const double trebCv = prevCv[4] + (cvNow[4] - prevCv[4]) * t;

            fr = engine.process(c, inBuf[k], driveCv, fbkCv, xfadeCv,
                                bassCv, trebCv, retBuf[k]);
            outBuf[XFADE_OUTPUT][k] = (float)fr.out;
            outBuf[DYNAMICS_OUTPUT][k] = (float)fr.dynamics;
            outBuf[SEND_OUTPUT][k] = (float)fr.fbkSend;
        }
        for (int i = 0; i < 5; i++) prevCv[i] = cvNow[i];

        float v[OUTPUTS_LEN];
        for (int i = 0; i < OUTPUTS_LEN; i++) {
            v[i] = osOut[i].downsample();
            if (!std::isfinite(v[i])) { v[i] = 0.f; engine.reset(); }
        }
        outputs[XFADE_OUTPUT].setVoltage(clamp(v[XFADE_OUTPUT], -10.f, 10.f));
        // the envelope is a control voltage: it is not band limited, and
        // filtering it would only add overshoot to something that is meant to
        // be a clean 0 to +5 V ramp
        outputs[DYNAMICS_OUTPUT].setVoltage(clamp((float)fr.dynamics, 0.f, 10.f));
        outputs[SEND_OUTPUT].setVoltage(clamp(v[SEND_OUTPUT], -10.f, 10.f));

        outEnv += (std::fabs(v[XFADE_OUTPUT]) * 0.2f - outEnv) * 0.002f;
        lights[OUT_LIGHT].setBrightness(clamp(outEnv * 1.6f, 0.f, 1.f));
        lights[ENV_LIGHT].setBrightness(clamp((float)fr.dynamics * 0.2f, 0.f, 1.f));
        lights[HF_LIGHT].setBrightness(clamp((float)fr.hf * 2.5f, 0.f, 1.f));
    }
};

struct UmbraeWidget : ModuleWidget {
    UmbraeWidget(Umbrae* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/umbrae.svg")));

// @layout:begin umbrae 101.6 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem SIGNAL_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HYPER_PARAM CKSS 2.3 param "" 0.0
// @elem BASS_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TREBLE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SENSE_PARAM CKSS 2.3 param "" 0.0
// @elem DECAY_PARAM CKSS 2.3 param "" 0.0
// @elem POLARITY_PARAM CKSS 2.3 param "" 0.0
// @elem LOOP_PARAM CKSS 2.3 param "" 0.0
// @elem DRIVE_CV_PARAM Trimpot 3.03 param "" 0.0
// @elem DRIVE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem BASS_BOOST_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TREBLE_BOOST_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FBK_CV_PARAM Trimpot 3.03 param "" 0.0
// @elem FBK_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem XFADE_CV_PARAM Trimpot 3.03 param "" 0.0
// @elem XFADE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DRIVE_PARAM VCVSlider 12.96 param "" 0.0
// @elem BASS_PARAM VCVSlider 12.96 param "" 0.0
// @elem TREBLE_PARAM VCVSlider 12.96 param "" 0.0
// @elem FBK_PARAM VCVSlider 12.96 param "" 0.0
// @elem XFADE_PARAM VCVSlider 12.96 param "" 0.0
// @elem RETURN_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SEND_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem DYNAMICS_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem XFADE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SRC_PARAM CKSS 2.3 param "" 0.0
// @elem OUT_LIGHT SmallLight 1.0 light "" 0.0
// @elem ENV_LIGHT SmallLight 1.0 light "" 0.0
// @elem HF_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_IN label 0.0 label "in" 0.0 9.00 24.50
// @elem LABEL_HYPER label 0.0 label "hyper" 0.0 20.00 25.50
// @elem LABEL_BASSCV label 0.0 label "bass" 0.0 33.00 24.50
// @elem LABEL_TREBCV label 0.0 label "treb" 0.0 45.00 24.50
// @elem LABEL_DYN label 0.0 label "dynamics" 0.0 62.00 25.50
// @elem LABEL_EXT label 0.0 label "ext fbk" 0.0 84.00 25.50
// @elem LABEL_DRIVECV label 0.0 label "cv" 0.0 16.30 47.50
// @elem LABEL_BASSBOOST label 0.0 label "boost" 0.0 30.80 48.50
// @elem LABEL_TREBBOOST label 0.0 label "boost" 0.0 49.80 48.50
// @elem LABEL_FBKCV label 0.0 label "cv" 0.0 73.30 47.50
// @elem LABEL_XFADECV label 0.0 label "cv" 0.0 92.30 47.50
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 11.80 83.00
// @elem LABEL_BASS label 0.0 label "bass" 0.0 30.80 83.00
// @elem LABEL_TREBLE label 0.0 label "treble" 0.0 49.80 83.00
// @elem LABEL_FBK label 0.0 label "fbk" 0.0 68.80 83.00
// @elem LABEL_XFADE label 0.0 label "x-fade" 0.0 87.80 83.00
// @elem LABEL_RETURN label 0.0 label "return" 0.0 10.00 112.50
// @elem BOX_SEND panel_box 7.0 box "" 0.0 30.00 107.00
// @elem LABEL_SEND label 0.0 label "send" 0.0 30.00 112.50
// @elem BOX_ENV panel_box 7.0 box "" 0.0 52.00 107.00
// @elem LABEL_ENV label 0.0 label "env" 0.0 52.00 112.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 74.00 107.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 74.00 112.50
// @elem LABEL_SRC label 0.0 label "src" 0.0 92.00 113.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 50.80 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 123.42f)))); // SCREW_BR
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.00f, 17.00f)), module, Umbrae::SIGNAL_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(20.00f, 17.00f)), module, Umbrae::HYPER_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 17.00f)), module, Umbrae::BASS_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.00f, 17.00f)), module, Umbrae::TREBLE_CV_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(58.00f, 17.00f)), module, Umbrae::SENSE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(66.00f, 17.00f)), module, Umbrae::DECAY_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(80.00f, 17.00f)), module, Umbrae::POLARITY_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(88.00f, 17.00f)), module, Umbrae::LOOP_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(6.80f, 40.00f)), module, Umbrae::DRIVE_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.30f, 40.00f)), module, Umbrae::DRIVE_CV_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.80f, 40.00f)), module, Umbrae::BASS_BOOST_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(49.80f, 40.00f)), module, Umbrae::TREBLE_BOOST_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(63.80f, 40.00f)), module, Umbrae::FBK_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(73.30f, 40.00f)), module, Umbrae::FBK_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(82.80f, 40.00f)), module, Umbrae::XFADE_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(92.30f, 40.00f)), module, Umbrae::XFADE_CV_INPUT));
        addParam(createParamCentered<VCVSlider>(mm2px(Vec(11.80f, 66.00f)), module, Umbrae::DRIVE_PARAM));
        addParam(createParamCentered<VCVSlider>(mm2px(Vec(30.80f, 66.00f)), module, Umbrae::BASS_PARAM));
        addParam(createParamCentered<VCVSlider>(mm2px(Vec(49.80f, 66.00f)), module, Umbrae::TREBLE_PARAM));
        addParam(createParamCentered<VCVSlider>(mm2px(Vec(68.80f, 66.00f)), module, Umbrae::FBK_PARAM));
        addParam(createParamCentered<VCVSlider>(mm2px(Vec(87.80f, 66.00f)), module, Umbrae::XFADE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 105.00f)), module, Umbrae::RETURN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.00f, 105.00f)), module, Umbrae::SEND_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(52.00f, 105.00f)), module, Umbrae::DYNAMICS_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(74.00f, 105.00f)), module, Umbrae::XFADE_OUTPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(92.00f, 105.00f)), module, Umbrae::SRC_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(79.00f, 102.00f)), module, Umbrae::OUT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(57.00f, 102.00f)), module, Umbrae::ENV_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(35.00f, 102.00f)), module, Umbrae::HF_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Umbrae* m = dynamic_cast<Umbrae*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        // The hardware has a jumper on the back for this.
        menu->addChild(createBoolPtrMenuItem("Envelope detector lowpass",
                                             "", &m->envLowpass));
        // 1x and 2x are missing on purpose: the loop's propagation delay is
        // modelled as a duration, and below 4x it is shorter than one sample.
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"4× (default)", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex - Umbrae::kMinOsIndex; },
            [m](int i) { m->osIndex = i + Umbrae::kMinOsIndex; }));
    }
};

Model* modelUmbrae = createModel<Umbrae, UmbraeWidget>("umbrae");
