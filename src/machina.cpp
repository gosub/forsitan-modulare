// machina.cpp — VCV Rack 2 module
// machina (Latin: "machine, engine, contrivance") is an internal combustion
// engine. Not a sample and not a sawtooth through a filter: cylinders on a
// shared crank, each with an intake pipe, a chamber whose length breathes with
// the piston, and an extractor into a common exhaust, expansion chamber,
// muffler bank and tailpipe, all of it waveguides at the speed of sound.
//
// It earns its place in a rack by being a rhythm source that is not a clock.
// An engine at 12 Hz is a pulse train with physics: TRIG fires once per crank
// cycle, the cylinders are deliberately out of phase with each other, and the
// firing is not evenly spaced because ASYM says the crankshaft is not either.
//
// The engine is a port of SDTMotor from the Sound Design Toolkit
// (GPL-3.0-or-later, see src/sdt/). Two changes to the plumbing, no changes to
// the model: the waveguide bank is sized per role rather than one maximum
// delay for all 42 of them, and the pipe lengths are kept in metres so a
// sample-rate change can re-derive them.
//
// Controls:
//   Knobs : RPM, LOAD, CYL, DISP, COMP, SPARK, ASYM, EXH, MUFF, EXP, BACK
//   Switch: 4/2 stroke
//   In    : RPM, LOAD, DISP, EXH
//   Out   : MIX, IN, BLK, PIPE, TRIG
//   Lights: mix level, firing

#include "forsitan.hpp"
#include "sdt/sdt_motor.hpp"

struct Machina : Module {
    enum ParamId {
        RPM_PARAM,
        LOAD_PARAM,
        CYL_PARAM,
        STROKE_PARAM,
        DISP_PARAM,
        COMP_PARAM,
        SPARK_PARAM,
        ASYM_PARAM,
        EXH_PARAM,
        MUFF_PARAM,
        EXPAND_PARAM,
        BACK_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        RPM_INPUT,
        LOAD_INPUT,
        DISP_INPUT,
        EXH_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        MIX_OUTPUT,
        INTAKE_OUTPUT,
        BLOCK_OUTPUT,
        PIPE_OUTPUT,
        TRIG_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        MIX_LIGHT,
        TRIG_LIGHT,
        LIGHTS_LEN
    };

    sdt::Motor motor;
    dsp::PulseGenerator trigPulse;
    float lastSampleRate = 0.f;
    int ctlCount = 0;
    double lastPhase = 0.0;
    float levelEnv = 0.f, trigEnv = 0.f;
    // The three taps come out of the model at very different levels: the
    // block radiates most, the tailpipe least. These bring each one to
    // something a patch can use, and the mix uses the SDT help patch's own
    // balance (0.3 / 0.6 / 1.0) on top.
    float outGain = 1.f;
    bool limiter = true;
    // The lowpass on the intake air and on the block's radiation. See
    // sdt_motor.hpp: the SDT's nominal 20 Hz is never actually applied by the
    // C, and applied literally it silences both.
    float damping = 2500.f;

    static float intakeGain() { return 14.f; }
    static float blockGain() { return 3.5f; }
    static float pipeGain() { return 23.f; }
    static float mixGain() { return 6.f; }

    Machina() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        motor.rng = sdt::Rng(random::u32());
        configParam(RPM_PARAM, 0.f, 1.f, 0.25f, "Engine speed", " rpm", 60.f, 200.f);
        configParam(LOAD_PARAM, 0.f, 1.f, 0.15f, "Load (throttle)", "%", 0.f, 100.f);
        configSwitch(CYL_PARAM, 1.f, 12.f, 4.f, "Cylinders",
                     {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"});
        configSwitch(STROKE_PARAM, 0.f, 1.f, 0.f, "Cycle", {"Four stroke", "Two stroke"});
        configParam(DISP_PARAM, 0.f, 1.f, 0.35f, "Displacement", " cc", 0.f, 3000.f);
        configParam(COMP_PARAM, 5.f, 20.f, 10.f, "Compression ratio", ":1");
        configParam(SPARK_PARAM, 0.f, 1.f, 0.3f, "Spark duration", "%", 0.f, 100.f);
        configParam(ASYM_PARAM, 0.f, 1.f, 0.15f, "Crank asymmetry", "%", 0.f, 100.f);
        configParam(EXH_PARAM, 0.f, 1.f, 0.5f, "Exhaust length", " m", 0.f, 5.f);
        configParam(MUFF_PARAM, 0.f, 1.f, 0.5f, "Muffler", "%", 0.f, 100.f);
        configParam(EXPAND_PARAM, 0.f, 1.f, 0.f, "Expansion chamber", "%", 0.f, 100.f);
        configParam(BACK_PARAM, 0.f, 1.f, 0.f, "Backfire", "%", 0.f, 100.f);
        configInput(RPM_INPUT, "Engine speed CV");
        configInput(LOAD_INPUT, "Load CV");
        configInput(DISP_INPUT, "Displacement CV");
        configInput(EXH_INPUT, "Exhaust length CV");
        configOutput(MIX_OUTPUT, "Mix");
        configOutput(INTAKE_OUTPUT, "Intake");
        configOutput(BLOCK_OUTPUT, "Block vibrations");
        configOutput(PIPE_OUTPUT, "Tailpipe");
        configOutput(TRIG_OUTPUT, "Trigger, once per crank cycle");
        configLight(MIX_LIGHT, "Mix level");
        configLight(TRIG_LIGHT, "Firing");
    }

    void onReset() override { lastSampleRate = 0.f; }
    void onSampleRateChange() override { lastSampleRate = 0.f; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "outGain", json_real(outGain));
        json_object_set_new(root, "limiter", json_boolean(limiter));
        json_object_set_new(root, "damping", json_real(damping));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "outGain"))
            outGain = clamp((float)json_real_value(j), 0.05f, 20.f);
        if (json_t* j = json_object_get(root, "limiter")) limiter = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "damping"))
            damping = clamp((float)json_real_value(j), 20.f, 20000.f);
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != lastSampleRate || motor.damp != (double)damping) {
            motor.damp = damping;
            motor.dc = 25.0;
            motor.setSampleRate(args.sampleRate);
            lastSampleRate = args.sampleRate;
            ctlCount = 0;
        }

        // ── control rate ────────────────────────────────────────────────────
        // Everything here either reallocates nothing but does trigonometry, or
        // touches all twelve cylinders' delay lines.
        if (ctlCount-- <= 0) {
            ctlCount = 15;
            const double dispKnob = sdt::fclip(params[DISP_PARAM].getValue()
                                               + inputs[DISP_INPUT].getVoltage() * 0.1,
                                               0.0, 1.0);
            const double exhKnob = sdt::fclip(params[EXH_PARAM].getValue()
                                              + inputs[EXH_INPUT].getVoltage() * 0.1,
                                              0.0, 1.0);
            motor.setNCylinders((int)std::round(params[CYL_PARAM].getValue()));
            motor.setTwoStroke(params[STROKE_PARAM].getValue() > 0.5f);
            motor.setCylinderSize(20.0 + 2980.0 * dispKnob);
            motor.setCompressionRatio(params[COMP_PARAM].getValue());
            motor.setSparkTime(0.002 + 0.098 * params[SPARK_PARAM].getValue());
            motor.setAsymmetry(params[ASYM_PARAM].getValue());
            // The intake and extractor pipes scale with the exhaust: one knob
            // for "how much plumbing", because eleven pipe lengths is the
            // research instrument, not the module.
            motor.setExhaustSize(0.05 + 4.95 * exhKnob);
            motor.setIntakeSize(0.05 + 0.6 * exhKnob);
            motor.setExtractorSize(0.08 + 0.9 * exhKnob);
            motor.setMufflerSize(0.1 + 1.2 * exhKnob);
            motor.setOutletSize(0.01 + 0.3 * exhKnob);
            motor.setMufflerFeedback(params[MUFF_PARAM].getValue());
            motor.setExpansion(params[EXPAND_PARAM].getValue());
            motor.setBackfire(params[BACK_PARAM].getValue());
        }

        const double rpmKnob = sdt::fclip(params[RPM_PARAM].getValue()
                                          + inputs[RPM_INPUT].getVoltage() * 0.1, 0.0, 1.2);
        motor.setRpm(200.0 * std::pow(60.0, rpmKnob));
        motor.setThrottle(sdt::fclip(params[LOAD_PARAM].getValue()
                                     + inputs[LOAD_INPUT].getVoltage() * 0.1, 0.0, 1.0));

        double outs[3] = {0, 0, 0};
        motor.process(outs);
        for (int i = 0; i < 3; i++)
            if (!std::isfinite(outs[i])) outs[i] = 0.0;

        // One pulse per crank cycle: the phase wrapping is the engine's own
        // clock, and it is not evenly spaced once ASYM is up.
        if (motor.phase < lastPhase) trigPulse.trigger(1e-3f);
        lastPhase = motor.phase;

        auto emit = [&](int id, double v, float g) {
            float y = (float)(v * g) * outGain;
            if (limiter) y = 10.f * std::tanh(y * 0.1f);
            outputs[id].setVoltage(clamp(y, -10.f, 10.f));
            return y;
        };
        emit(INTAKE_OUTPUT, outs[0], intakeGain());
        emit(BLOCK_OUTPUT, outs[1], blockGain());
        emit(PIPE_OUTPUT, outs[2], pipeGain());
        const float mix = emit(MIX_OUTPUT,
                               0.3 * outs[0] + 0.6 * outs[1] + 1.0 * outs[2], mixGain());

        const bool trig = trigPulse.process(args.sampleTime);
        outputs[TRIG_OUTPUT].setVoltage(trig ? 10.f : 0.f);

        levelEnv += (std::fabs(mix) * 0.2f - levelEnv) * 0.002f;
        trigEnv += ((trig ? 1.f : 0.f) - trigEnv) * 0.004f;
        lights[MIX_LIGHT].setBrightness(clamp(levelEnv * 1.6f, 0.f, 1.f));
        lights[TRIG_LIGHT].setBrightness(clamp(trigEnv * 6.f, 0.f, 1.f));
    }
};

struct MachinaWidget : ModuleWidget {
    MachinaWidget(Machina* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/machina.svg")));

// @layout:begin machina 81.28 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem RPM_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem LOAD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CYL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STROKE_PARAM CKSS 2.3 param "" 0.0
// @elem DISP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem COMP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPARK_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ASYM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem EXH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MUFF_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem EXPAND_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BACK_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RPM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LOAD_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DISP_INPUT PJ301MPort 4.01 input "" 0.0
// @elem EXH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MIX_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem INTAKE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem BLOCK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem PIPE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem TRIG_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem MIX_LIGHT SmallLight 1.0 light "" 0.0
// @elem TRIG_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_RPM label 0.0 label "rpm" 0.0 15.00 33.50
// @elem LABEL_LOAD label 0.0 label "load" 0.0 36.00 30.50
// @elem LABEL_CYL label 0.0 label "cyl" 0.0 52.00 30.50
// @elem LABEL_STROKE label 0.0 label "4/2" 0.0 68.00 30.50
// @elem LABEL_DISP label 0.0 label "disp" 0.0 12.70 54.50
// @elem LABEL_COMP label 0.0 label "comp" 0.0 30.50 54.50
// @elem LABEL_SPARK label 0.0 label "spark" 0.0 48.30 54.50
// @elem LABEL_ASYM label 0.0 label "asym" 0.0 66.10 54.50
// @elem LABEL_EXH label 0.0 label "exh" 0.0 12.70 76.50
// @elem LABEL_MUFF label 0.0 label "muff" 0.0 30.50 76.50
// @elem LABEL_EXPAND label 0.0 label "exp" 0.0 48.30 76.50
// @elem LABEL_BACK label 0.0 label "back" 0.0 66.10 76.50
// @elem LABEL_RPMCV label 0.0 label "rpm" 0.0 19.64 95.50
// @elem LABEL_LOADCV label 0.0 label "load" 0.0 33.64 95.50
// @elem LABEL_DISPCV label 0.0 label "disp" 0.0 47.64 95.50
// @elem LABEL_EXHCV label 0.0 label "exh" 0.0 61.64 95.50
// @elem BOX_MIX panel_box 7.0 box "" 0.0 11.64 110.00 box=12.8x14.0
// @elem BOX_INTAKE panel_box 7.0 box "" 0.0 26.14 110.00 box=12.8x14.0
// @elem BOX_BLOCK panel_box 7.0 box "" 0.0 40.64 110.00 box=12.8x14.0
// @elem BOX_PIPE panel_box 7.0 box "" 0.0 55.14 110.00 box=12.8x14.0
// @elem BOX_TRIG panel_box 7.0 box "" 0.0 69.64 110.00 box=12.8x14.0
// @elem LABEL_MIX label 0.0 label "mix" 0.0 11.64 115.50
// @elem LABEL_INTAKE label 0.0 label "in" 0.0 26.14 115.50
// @elem LABEL_BLOCK label 0.0 label "blk" 0.0 40.64 115.50
// @elem LABEL_PIPE label 0.0 label "pipe" 0.0 55.14 115.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 69.64 115.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 40.64 124.00

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(73.66f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(73.66f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(15.00f, 22.00f)), module, Machina::RPM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.00f, 22.00f)), module, Machina::LOAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(52.00f, 22.00f)), module, Machina::CYL_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(68.00f, 22.00f)), module, Machina::STROKE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 46.00f)), module, Machina::DISP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.50f, 46.00f)), module, Machina::COMP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.30f, 46.00f)), module, Machina::SPARK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(66.10f, 46.00f)), module, Machina::ASYM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 68.00f)), module, Machina::EXH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(30.50f, 68.00f)), module, Machina::MUFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.30f, 68.00f)), module, Machina::EXPAND_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(66.10f, 68.00f)), module, Machina::BACK_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.64f, 88.00f)), module, Machina::RPM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.64f, 88.00f)), module, Machina::LOAD_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.64f, 88.00f)), module, Machina::DISP_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.64f, 88.00f)), module, Machina::EXH_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.64f, 108.00f)), module, Machina::MIX_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(26.14f, 108.00f)), module, Machina::INTAKE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.64f, 108.00f)), module, Machina::BLOCK_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55.14f, 108.00f)), module, Machina::PIPE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(69.64f, 108.00f)), module, Machina::TRIG_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(16.64f, 105.00f)), module, Machina::MIX_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(74.64f, 105.00f)), module, Machina::TRIG_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Machina* m = dynamic_cast<Machina*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Output gain",
            {"-12 dB", "-6 dB", "0 dB (default)", "+6 dB", "+12 dB"},
            [m]() {
                const float g[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
                int best = 2;
                for (int i = 0; i < 5; i++)
                    if (std::fabs(m->outGain - g[i]) < std::fabs(m->outGain - g[best])) best = i;
                return best;
            },
            [m](int i) {
                const float g[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
                m->outGain = g[i];
            }));
        menu->addChild(createBoolPtrMenuItem("Output limiter", "", &m->limiter));
        menu->addChild(createIndexSubmenuItem("Body damping",
            {"200 Hz (muffled)", "800 Hz", "2500 Hz (default)", "8000 Hz (raw)"},
            [m]() {
                const float d[4] = {200.f, 800.f, 2500.f, 8000.f};
                int best = 2;
                for (int i = 0; i < 4; i++)
                    if (std::fabs(m->damping - d[i]) < std::fabs(m->damping - d[best])) best = i;
                return best;
            },
            [m](int i) {
                const float d[4] = {200.f, 800.f, 2500.f, 8000.f};
                m->damping = d[i];
            }));
    }
};

Model* modelMachina = createModel<Machina, MachinaWidget>("machina");
