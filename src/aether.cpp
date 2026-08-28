// aether.cpp - VCV Rack 2 module
// aether (Latin: the upper air, the medium a signal was once thought to
// travel through) is a transmission line with something wrong with it, after
// Schlappi Engineering's Interstellar Radio.
//
// Audio goes into a synchronous voltage-to-frequency converter clocked by the
// CARRIER and leaves as a high frequency pulse train; a phase-locked loop
// whose own converter is clocked by the DEMODULATOR gets it back, or does not.
// Both clocks are voltage controlled and neither tracks 1V/oct. Turning the
// carrier down is turning the sample rate down. Turning the two clocks apart
// scales and offsets the recovered signal into the rails, which is the
// distortion. Ask the demodulator for a rate it cannot reach and the loop
// stops locking, which is the silence.
//
// With nothing patched the input jack is a DC bias the knob attenuates, the
// transmitter becomes an oscillator and the receiver chases it: that is the
// broken radio. With nothing patched in the CV inputs, the signal itself is
// normalled to both, so the CV knobs turn into audio-rate FM depth.
//
// The engine lives in src/aether_dsp.hpp, free of Rack headers so that
// test/aether_probe can hold it to the two laws it is built on. See
// doc/aether.md for the sources and for what is inference rather than
// documentation.
//
// Both signal outputs are a dry/wet against the input jack, so the module can
// sit in an effect send without a mixer beside it. The bottom row is that
// mix: cv, trimmer, out, error, trimmer, cv - a knob apiece would have left
// a fifth of a millimetre at the panel edges. One "mix" label names each
// trimmer and its jack, the way a knob names its own CV cluster.
//
// Controls:
//   Knobs : LEVEL, CARRIER, DEMOD, ERROR, TONE
//   Trim  : CARRIER CV, DEMOD CV (attenuators), OUT MIX, ERROR MIX
//   Switch: TYPE (the three phase comparators)
//   In    : IN, CARRIER CV, CARRIER CLK, DEMOD CV, DEMOD CLK,
//           OUT MIX CV, ERROR MIX CV (each attenuated by its own knob)
//   Out   : OUT, ERROR, TX (carrier clock), RX (demodulator clock)
//   Lights: output level, error level

#include "forsitan.hpp"
#include "aether_dsp.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

struct Aether : Module {
    enum ParamId {
        LEVEL_PARAM,
        CARRIER_PARAM,
        CARRIER_CV_PARAM,
        DEMOD_PARAM,
        DEMOD_CV_PARAM,
        ERROR_PARAM,
        TONE_PARAM,
        TYPE_PARAM,
        OUT_MIX_PARAM,
        ERROR_MIX_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        SIGNAL_INPUT,
        CARRIER_CV_INPUT,
        CARRIER_CLK_INPUT,
        DEMOD_CV_INPUT,
        DEMOD_CLK_INPUT,
        OUT_MIX_INPUT,
        ERROR_MIX_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SIGNAL_OUTPUT,
        ERROR_OUTPUT,
        CARRIER_OUTPUT,
        DEMOD_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId { OUT_LIGHT, ERROR_LIGHT, LIGHTS_LEN };

    aether::Engine engine;
    // The audio input gets a real interpolating upsample. The CV and clock
    // inputs get a straight line between one host sample and the next
    // instead: an antialiasing filter rings around a clock edge, and a clock
    // edge is the one thing here that has to land where it says it does.
    VariableOversampling<> osIn;
    VariableOversampling<> osOut[OUTPUTS_LEN];

    int osIndex = 2;              // 2^osIndex, default 4x
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;

    float prevCv[2] = {0.f, 0.f};
    float prevClk[2] = {0.f, 0.f};
    float outEnv = 0.f, errEnv = 0.f;

    Aether() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(LEVEL_PARAM, 0.f, 1.f, 0.5f, "Signal in level", "%", 0.f, 100.f);
        configParam(CARRIER_PARAM, 0.f, 1.f, 0.75f, "Carrier frequency", " Hz",
                    std::pow(2.f, (float)aether::kClkOctaves),
                    (float)aether::kClkMin);
        configParam(CARRIER_CV_PARAM, 0.f, 1.f, 0.f, "Carrier CV amount", "%", 0.f, 100.f);
        configParam(DEMOD_PARAM, 0.f, 1.f, 0.75f, "Demodulator frequency", " Hz",
                    std::pow(2.f, (float)aether::kClkOctaves),
                    (float)aether::kClkMin);
        configParam(DEMOD_CV_PARAM, 0.f, 1.f, 0.f, "Demodulator CV amount", "%", 0.f, 100.f);
        configParam(ERROR_PARAM, -1.f, 1.f, 0.f, "Error threshold", " V", 0.f, 5.f);
        configParam(TONE_PARAM, 0.f, 1.f, 0.7f, "Tone", " Hz",
                    (float)(aether::kToneMax / aether::kToneMin),
                    (float)aether::kToneMin);
        configParam(OUT_MIX_PARAM, 0.f, 1.f, 1.f, "Out dry/wet", "%", 0.f, 100.f);
        configParam(ERROR_MIX_PARAM, 0.f, 1.f, 1.f, "Error dry/wet", "%", 0.f, 100.f);
        configSwitch(TYPE_PARAM, 0.f, 2.f, 0.f, "Loop type",
                     {"1 - exclusive-or, locks to harmonics",
                      "2 - phase-frequency, quiet when unlocked",
                      "3 - set-reset latch"});
        configInput(SIGNAL_INPUT, "Signal");
        configInput(CARRIER_CV_INPUT, "Carrier CV");
        configInput(CARRIER_CLK_INPUT, "Carrier clock (replaces the internal one)");
        configInput(DEMOD_CV_INPUT, "Demodulator CV");
        configInput(DEMOD_CLK_INPUT, "Demodulator clock (replaces the internal one)");
        configInput(OUT_MIX_INPUT, "Out dry/wet CV (the knob attenuates it)");
        configInput(ERROR_MIX_INPUT, "Error dry/wet CV (the knob attenuates it)");
        configOutput(SIGNAL_OUTPUT, "Recovered signal");
        configOutput(ERROR_OUTPUT, "Error");
        configOutput(CARRIER_OUTPUT, "Carrier clock");
        configOutput(DEMOD_OUTPUT, "Demodulator clock");
        configLight(OUT_LIGHT, "Output level");
        configLight(ERROR_LIGHT, "Error level");
        configBypass(SIGNAL_INPUT, SIGNAL_OUTPUT);
    }

    void onReset() override {
        engine.reset();
        prevCv[0] = prevCv[1] = prevClk[0] = prevClk[1] = 0.f;
        outEnv = errEnv = 0.f;
        lastOsIndex = -1;
    }

    void onSampleRateChange() override { lastOsIndex = -1; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            osIndex = clamp((int)json_integer_value(j), 0, 4);
        lastOsIndex = -1;
    }

    // A dry/wet with no attenuverter: unpatched the knob is the amount, and
    // patched it becomes the CV's attenuator, so 0-10 V spans dry to wet at a
    // knob left where it already was.
    float mixAmount(int paramId, int inputId) {
        float m = params[paramId].getValue();
        if (inputs[inputId].isConnected())
            m *= inputs[inputId].getVoltage() * 0.1f;
        return clamp(m, 0.f, 1.f);
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            osIn.setOversamplingIndex(osIndex);
            osIn.reset(sr);
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

        aether::Engine::Controls c;
        c.inLevel = params[LEVEL_PARAM].getValue();
        c.carrierKnob = params[CARRIER_PARAM].getValue();
        c.carrierCvAmt = params[CARRIER_CV_PARAM].getValue();
        c.demodKnob = params[DEMOD_PARAM].getValue();
        c.demodCvAmt = params[DEMOD_CV_PARAM].getValue();
        c.tone = params[TONE_PARAM].getValue();
        c.errThresh = params[ERROR_PARAM].getValue();
        c.type = (int)std::round(params[TYPE_PARAM].getValue());
        c.inputPatched = inputs[SIGNAL_INPUT].isConnected();
        c.carrierCvPatched = inputs[CARRIER_CV_INPUT].isConnected();
        c.demodCvPatched = inputs[DEMOD_CV_INPUT].isConnected();
        c.extCarrier = inputs[CARRIER_CLK_INPUT].isConnected();
        c.extDemod = inputs[DEMOD_CLK_INPUT].isConnected();

        const float cvNow[2] = {inputs[CARRIER_CV_INPUT].getVoltage(),
                                inputs[DEMOD_CV_INPUT].getVoltage()};
        const float clkNow[2] = {inputs[CARRIER_CLK_INPUT].getVoltage(),
                                 inputs[DEMOD_CLK_INPUT].getVoltage()};

        osIn.upsample(inputs[SIGNAL_INPUT].getVoltage());
        float* inBuf = osIn.getOSBuffer();
        float* outBuf[OUTPUTS_LEN];
        for (int i = 0; i < OUTPUTS_LEN; i++) outBuf[i] = osOut[i].getOSBuffer();

        for (int k = 0; k < ratio; k++) {
            const float f = (float)(k + 1) / ratio;
            const double cvC = prevCv[0] + (cvNow[0] - prevCv[0]) * f;
            const double cvD = prevCv[1] + (cvNow[1] - prevCv[1]) * f;
            const double clkC = prevClk[0] + (clkNow[0] - prevClk[0]) * f;
            const double clkD = prevClk[1] + (clkNow[1] - prevClk[1]) * f;

            const aether::Engine::Frame fr =
                engine.process(c, inBuf[k], cvC, cvD, clkC, clkD);
            outBuf[SIGNAL_OUTPUT][k] = (float)fr.out;
            outBuf[ERROR_OUTPUT][k] = (float)fr.error;
            outBuf[CARRIER_OUTPUT][k] = (float)fr.carrierClk;
            outBuf[DEMOD_OUTPUT][k] = (float)fr.demodClk;
        }
        prevCv[0] = cvNow[0]; prevCv[1] = cvNow[1];
        prevClk[0] = clkNow[0]; prevClk[1] = clkNow[1];

        float v[OUTPUTS_LEN];
        for (int i = 0; i < OUTPUTS_LEN; i++) {
            v[i] = osOut[i].downsample();
            if (!std::isfinite(v[i])) { v[i] = 0.f; engine.reset(); }
        }

        // The dry side of both mixes is the signal at the jack, untouched by
        // LEVEL: an effect's dry path is the one that does nothing. With
        // nothing patched it is silence, so the broken radio only speaks at
        // the wet end.
        const float dry = inputs[SIGNAL_INPUT].getVoltage();
        v[SIGNAL_OUTPUT] = crossfade(dry, v[SIGNAL_OUTPUT],
                                     mixAmount(OUT_MIX_PARAM, OUT_MIX_INPUT));
        v[ERROR_OUTPUT] = crossfade(dry, v[ERROR_OUTPUT],
                                    mixAmount(ERROR_MIX_PARAM, ERROR_MIX_INPUT));

        for (int i = 0; i < OUTPUTS_LEN; i++)
            outputs[i].setVoltage(clamp(v[i], -10.f, 10.f));

        outEnv += (std::fabs(v[SIGNAL_OUTPUT]) * 0.2f - outEnv) * 0.002f;
        errEnv += (std::fabs(v[ERROR_OUTPUT]) * 0.2f - errEnv) * 0.002f;
        lights[OUT_LIGHT].setBrightness(clamp(outEnv * 1.6f, 0.f, 1.f));
        lights[ERROR_LIGHT].setBrightness(clamp(errEnv * 1.6f, 0.f, 1.f));
    }
};

struct AetherWidget : ModuleWidget {
    AetherWidget(Aether* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/aether.svg")));

// @layout:begin aether 71.12 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem SIGNAL_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TYPE_PARAM CKSSThree 2.3 param "" 0.0
// @elem TONE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CARRIER_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem ERROR_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DEMOD_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem CARRIER_CV_PARAM Trimpot 3.03 param "" 0.0
// @elem CARRIER_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DEMOD_CV_PARAM Trimpot 3.03 param "" 0.0
// @elem DEMOD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CARRIER_CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CARRIER_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem DEMOD_CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DEMOD_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SIGNAL_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem ERROR_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_LIGHT SmallLight 1.0 light "" 0.0
// @elem ERROR_LIGHT SmallLight 1.0 light "" 0.0
// @elem OUT_MIX_PARAM Trimpot 3.03 param "" 0.0
// @elem OUT_MIX_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ERROR_MIX_PARAM Trimpot 3.03 param "" 0.0
// @elem ERROR_MIX_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_IN label 0.0 label "in" 0.0 11.00 27.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 26.50 28.50
// @elem LABEL_TYPE label 0.0 label "type" 0.0 45.00 28.50
// @elem LABEL_TONE label 0.0 label "tone" 0.0 60.00 28.50
// @elem LABEL_CARRIER label 0.0 label "carrier" 0.0 18.00 57.50
// @elem LABEL_ERROR label 0.0 label "error" 0.0 35.56 52.50
// @elem LABEL_DEMOD label 0.0 label "demod" 0.0 53.00 57.50
// @elem LABEL_CCV label 0.0 label "cv" 0.0 22.50 71.50
// @elem LABEL_DCV label 0.0 label "cv" 0.0 57.50 71.50
// @elem LABEL_CCLK label 0.0 label "clk" 0.0 12.00 88.50
// @elem LABEL_DCLK label 0.0 label "clk" 0.0 59.00 88.50
// @elem BOX_TX panel_box 7.0 box "" 0.0 26.00 83.00
// @elem LABEL_TX label 0.0 label "tx" 0.0 26.00 88.50
// @elem BOX_RX panel_box 7.0 box "" 0.0 45.00 83.00
// @elem LABEL_RX label 0.0 label "rx" 0.0 45.00 88.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 27.50 106.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 27.50 111.50
// @elem BOX_ERR panel_box 7.0 box "" 0.0 43.50 106.00
// @elem LABEL_ERR label 0.0 label "error" 0.0 43.50 111.50
// @elem LABEL_OMIX label 0.0 label "mix" 0.0 11.60 111.50
// @elem LABEL_EMIX label 0.0 label "mix" 0.0 59.50 111.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 35.56 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(63.50f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(63.50f, 123.42f)))); // SCREW_BR
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.00f, 20.00f)), module, Aether::SIGNAL_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(26.50f, 20.00f)), module, Aether::LEVEL_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(45.00f, 20.00f)), module, Aether::TYPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60.00f, 20.00f)), module, Aether::TONE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(18.00f, 46.00f)), module, Aether::CARRIER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(35.56f, 44.00f)), module, Aether::ERROR_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(53.00f, 46.00f)), module, Aether::DEMOD_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(13.00f, 64.00f)), module, Aether::CARRIER_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.50f, 64.00f)), module, Aether::CARRIER_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(48.00f, 64.00f)), module, Aether::DEMOD_CV_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(57.50f, 64.00f)), module, Aether::DEMOD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.00f, 81.00f)), module, Aether::CARRIER_CLK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(26.00f, 81.00f)), module, Aether::CARRIER_OUTPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(59.00f, 81.00f)), module, Aether::DEMOD_CLK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(45.00f, 81.00f)), module, Aether::DEMOD_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.50f, 104.00f)), module, Aether::SIGNAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(43.50f, 104.00f)), module, Aether::ERROR_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(32.50f, 101.00f)), module, Aether::OUT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(48.50f, 101.00f)), module, Aether::ERROR_LIGHT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(15.90f, 104.00f)), module, Aether::OUT_MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.30f, 104.00f)), module, Aether::OUT_MIX_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(55.20f, 104.00f)), module, Aether::ERROR_MIX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(63.80f, 104.00f)), module, Aether::ERROR_MIX_INPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Aether* m = dynamic_cast<Aether*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"1× (rawest, lightest)", "2×", "4× (default)", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex; },
            [m](int i) { m->osIndex = i; }));
    }
};

Model* modelAether = createModel<Aether, AetherWidget>("aether");
