// viginti.cpp — VCV Rack 2 module
// viginti (Latin: "twenty") is the lowpass of the MS-20's later revisions: the
// OTA-based KORG35 Rev. 2 circuit, modelled as the nonlinear state-space
// system it is rather than as a filter with a waveshaper bolted to its output.
//
// Two RC stages buffered by OTAs, and around them a resonance feedback path
// through a non-inverting amplifier of gain ~4 that is clamped by three series
// diodes in each direction. Because the diodes sit inside the loop, the
// resonance is what distorts: quiet signals get a tall, clean peak, loud ones
// squash it and turn it gritty, and the filter is not equivalent to any fixed
// linear filter plus a static nonlinearity. That level dependence is the
// module.
//
// The model, the discretization and the stability bounds are from
//   M. Danish, S. Bilbao, M. Ducceschi, "Applications of Port Hamiltonian
//   Methods to Non-Iterative Stable Simulations of the KORG35 and MOOG 4-Pole
//   VCF", Proc. DAFx20in21, Vienna, 2021, section 3.1;
// the component values behind alpha and beta from that paper's own reference,
//   T. E. Stinchcombe, "A Study of the Korg MS10 & MS20 Filters", Aug. 2006.
// The DSP lives in src/viginti_dsp.hpp, with an RK4 integration of the same
// continuous model beside it as the reference the real-time scheme is measured
// against (test/viginti_probe, test/viginti_invariants).
//
// This is the lowpass section only. The MS-20's highpass is a different
// circuit and is not modelled here; see doc/viginti.md.
//
// Controls:
//   Knobs : CUTOFF, RES, DRIVE, LEVEL
//   Trim  : FM (attenuverter)
//   In    : IN (audio), V/OCT, FM, RES CV
//   Out   : OUT
//   Light : output level

#include "forsitan.hpp"
#include "viginti_dsp.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

struct Viginti : Module {
    enum ParamId {
        CUTOFF_PARAM,
        RES_PARAM,
        DRIVE_PARAM,
        LEVEL_PARAM,
        FM_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        VOCT_INPUT,
        FM_INPUT,
        RES_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId { AUDIO_OUTPUT, OUTPUTS_LEN };
    enum LightId { LEVEL_LIGHT, LIGHTS_LEN };

    static const int kMaxChannels = 16;

    viginti::Korg35Filter filter[kMaxChannels];
    VariableOversampling<> os[kMaxChannels];
    float levelEnv = 0.f;
    uint32_t noise = 0x2c9e7fu;

    int osIndex = 1;              // 2^osIndex, default 2x
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;
    // DRIVE and LEVEL only move when a hand does, and their pow() is the most
    // expensive thing in the control block.
    float lastDrive = -2.f, lastLevel = -2.f;
    double driveGain = 1.0, levelGain = 1.0;

    Viginti() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(CUTOFF_PARAM, 0.f, 1.f, 0.7f, "Cutoff", " Hz",
                    std::pow(2.f, (float)viginti::kFcOctaves),
                    (float)viginti::kFcMin);
        configParam(RES_PARAM, 0.f, 1.f, 0.3f, "Resonance", "%", 0.f, 100.f);
        configParam(DRIVE_PARAM, -1.f, 1.f, 0.f, "Drive", " dB", 0.f, 24.f);
        configParam(LEVEL_PARAM, -1.f, 1.f, 0.f, "Output level", " dB", 0.f, 24.f);
        configParam(FM_PARAM, -1.f, 1.f, 0.f, "FM amount", "%", 0.f, 100.f);
        configInput(AUDIO_INPUT, "Audio");
        configInput(VOCT_INPUT, "1V/oct cutoff");
        configInput(FM_INPUT, "Cutoff FM");
        configInput(RES_CV_INPUT, "Resonance CV");
        configOutput(AUDIO_OUTPUT, "Lowpass");
        configLight(LEVEL_LIGHT, "Output level");
        configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
    }

    void onReset() override {
        for (int c = 0; c < kMaxChannels; c++) filter[c].reset();
        levelEnv = 0.f;
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

    // xorshift. The circuit's own noise floor is what lets a self-oscillating
    // filter start from silence; the ideal model would sit at x = 0 forever.
    // Sized at about 8 uV at the circuit's input, which is inaudible until the
    // resonance is high enough to be oscillating anyway.
    float dither() {
        noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
        return 1e-4f * ((float)(noise & 0xffffff) / 8388608.f - 1.f);
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            for (int c = 0; c < kMaxChannels; c++) {
                os[c].setOversamplingIndex(osIndex);
                os[c].reset(sr);
                filter[c].setSampleRate(sr * (double)(1 << osIndex));
                filter[c].reset();
            }
            lastOsIndex = osIndex;
            lastSampleRate = sr;
        }

        const int ratio = 1 << osIndex;
        int channels = std::max(inputs[AUDIO_INPUT].getChannels(),
                                inputs[VOCT_INPUT].getChannels());
        channels = clamp(channels, 1, kMaxChannels);
        outputs[AUDIO_OUTPUT].setChannels(channels);

        const float drive = params[DRIVE_PARAM].getValue();
        if (drive != lastDrive) {
            lastDrive = drive;
            driveGain = std::pow(10.0, 24.0 * drive / 20.0);
        }
        const float level = params[LEVEL_PARAM].getValue();
        if (level != lastLevel) {
            lastLevel = level;
            levelGain = std::pow(10.0, 24.0 * level / 20.0);
        }

        const float cutoffKnob = params[CUTOFF_PARAM].getValue();
        const float resKnob = params[RES_PARAM].getValue();
        const float fmAmount = params[FM_PARAM].getValue();
        // Rack volts in, circuit volts out, both through the one documented
        // scale factor: at unity drive and level the module is a unity-gain
        // filter and nothing is normalised behind the user's back.
        const double inScale = viginti::kVoltScale * driveGain / viginti::kVref;
        const double outScale = viginti::kVref / viginti::kVoltScale * levelGain;

        float sum = 0.f;
        for (int c = 0; c < channels; c++) {
            // ── cutoff, per channel and per sample ──────────────────────────
            float octaves = (float)viginti::kFcOctaves * cutoffKnob
                          + inputs[VOCT_INPUT].getPolyVoltage(c)
                          + fmAmount * inputs[FM_INPUT].getPolyVoltage(c);
            const double fc = viginti::kFcMin
                * (double)dsp::exp2_taylor5(clamp(octaves, -6.f, 14.f));

            // ── resonance ───────────────────────────────────────────────────
            float r = resKnob;
            if (inputs[RES_CV_INPUT].isConnected())
                r += 0.1f * inputs[RES_CV_INPUT].getPolyVoltage(c);
            const double alpha = viginti::alphaFromKnob(r);

            const double u = inputs[AUDIO_INPUT].getPolyVoltage(c) * inScale;

            os[c].upsample((float)u + dither());
            float* buf = os[c].getOSBuffer();
            for (int k = 0; k < ratio; k++)
                buf[k] = (float)filter[c].processSample(buf[k], fc, alpha);
            float y = os[c].downsample();

            if (!std::isfinite(y)) { y = 0.f; filter[c].reset(); }
            const float volts = clamp((float)(y * outScale), -10.f, 10.f);
            outputs[AUDIO_OUTPUT].setVoltage(volts, c);
            sum += std::fabs(volts);
        }

        levelEnv += (sum / channels * 0.2f - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv * 1.4f, 0.f, 1.f));
    }
};

struct VigintiWidget : ModuleWidget {
    VigintiWidget(Viginti* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/viginti.svg")));

// @layout:begin viginti 40.64 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem CUTOFF_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FM_PARAM Trimpot 3.03 param "" 0.0
// @elem DRIVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RES_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 20.32 32.50
// @elem LABEL_RES label 0.0 label "res" 0.0 11.50 52.50
// @elem LABEL_FM label 0.0 label "fm" 0.0 29.14 52.50
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 11.50 73.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 29.14 73.50
// @elem LABEL_IN label 0.0 label "in" 0.0 8.00 94.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 20.32 94.50
// @elem LABEL_FMIN label 0.0 label "fm" 0.0 32.64 94.50
// @elem LABEL_RESCV label 0.0 label "res" 0.0 11.50 111.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 29.14 105.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 29.14 111.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(20.32f, 21.00f)), module, Viginti::CUTOFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 44.00f)), module, Viginti::RES_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(29.14f, 46.00f)), module, Viginti::FM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 65.00f)), module, Viginti::DRIVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.14f, 65.00f)), module, Viginti::LEVEL_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.00f, 87.00f)), module, Viginti::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 87.00f)), module, Viginti::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.64f, 87.00f)), module, Viginti::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.50f, 103.50f)), module, Viginti::RES_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(29.14f, 103.50f)), module, Viginti::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(34.14f, 100.50f)), module, Viginti::LEVEL_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Viginti* m = dynamic_cast<Viginti*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"1× (rawest, lightest)", "2× (default)", "4×", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex; },
            [m](int i) { m->osIndex = i; }));
    }
};

Model* modelViginti = createModel<Viginti, VigintiWidget>("viginti");
