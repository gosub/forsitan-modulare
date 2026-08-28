// raucus.cpp - VCV Rack 2 module
// raucus (Latin: "the stuffing of a cushion, flock, wool padding") is a
// block-level model of the four-transistor Electro-Harmonix Big Muff Pi,
// USA V3, the 1976-77 circuit: an input booster, two common-emitter stages
// clipped by antiparallel silicon diodes in their collector-base feedback,
// the passive two-branch tone network, and a recovery stage.
//
// The DSP core is in raucus_dsp.hpp and knows nothing about Rack. Circuit
// values and the measured stage gains come from ElectroSmash's analysis of
// the V3; see doc/raucus.md for what is and is not modelled.
//
// Two things here are not the block model the usual write-ups describe:
//
//   - the tone stack is the *solved* passive network, one biquad from a
//     nodal analysis of the whole loaded circuit, not two independent
//     first-order branches mixed together. That is what puts the notch at
//     1 kHz with the pot centred, walks it down to 260 Hz as the knob goes
//     treble-ward, and gets the 7 dB insertion loss for free.
//   - the feedback clipper is solved rather than approximated by a limiter
//     after the gain. The 1 uF capacitor in series with each diode pair
//     blocks DC and nothing else at audio, so the stage is memoryless and
//     the exact static solution fits in a table.
//
// Additions the pedal has no knob for: an input trim (a modular signal is
// some 25 dB hotter than a guitar), a bias trim (the starved, gated sound),
// a mids control (the tone-bypass mod), CV on the three real controls, a
// diode-type menu, and polyphony.
//
// Controls:
//   Knobs : SUSTAIN, TONE, MIDS, VOLUME
//   Trims : GAIN (input level), BIAS
//   In    : IN, SUS, TONE, VOL
//   Out   : OUT
//   Lights: clipping indicator, output level

#include "forsitan.hpp"
#include "raucus_dsp.hpp"
// the ChowDSP variable oversampler already vendored for guttur
#include "guttur/VariableOversampling.hpp"

struct Raucus : Module {
    enum ParamId {
        GAIN_PARAM,
        SUSTAIN_PARAM,
        BIAS_PARAM,
        TONE_PARAM,
        MIDS_PARAM,
        VOLUME_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        SUSTAIN_INPUT,
        TONE_INPUT,
        VOLUME_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        CLIP_LIGHT,
        AUDIO_LIGHT,
        LIGHTS_LEN
    };

    static const int kMaxChannels = 16;

    raucus::Voice voices[kMaxChannels];
    // The pedal's output coupling capacitor sits after the volume pot, and so
    // does this: the voice's own blocker is upstream of the makeup gain and
    // the soft ceiling below, and a ceiling compresses an asymmetric waveform
    // asymmetrically. That turns a signal with no offset into one with a
    // couple of hundred millivolts of it, and nothing downstream of the
    // voice was removing it.
    raucus::DCBlock outDc[kMaxChannels];
    VariableOversampling<> upsampler[kMaxChannels];
    // Smoothed controls, per channel because their CV may be polyphonic.
    float sustainZ[kMaxChannels] = {}, toneZ[kMaxChannels] = {}, volumeZ[kMaxChannels] = {};
    float toneSet[kMaxChannels] = {};    // tone value the biquad was built for
    float levelEnv = 0.f;
    bool smoothed = false;

    int osIndex = 2;                     // 2^osIndex, default 4x
    int lastOsIndex = -1;
    float lastSampleRate = 0.f;
    int diode = raucus::DIODE_SILICON;

    Raucus() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // Volts at the pedal's input for a 5 V Rack signal. A guitar delivers
        // a couple of hundred millivolts, which is where the circuit was
        // designed to sit; the default puts a nominal modular level there.
        configParam(GAIN_PARAM, 0.f, 1.f, 0.45f, "Input level", " V", 300.f, 0.01f);
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.65f, "Sustain", "%", 0.f, 100.f);
        configParam(BIAS_PARAM, -1.f, 1.f, 0.f, "Bias (starve the clippers)", "%", 0.f, 100.f);
        configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (bass to treble)", "%", 0.f, 100.f);
        configParam(MIDS_PARAM, 0.f, 1.f, 0.f, "Mids (fill the scoop)", "%", 0.f, 100.f);
        configParam(VOLUME_PARAM, 0.f, 1.f, 0.7f, "Volume", "%", 0.f, 100.f);
        configInput(AUDIO_INPUT, "Audio");
        configInput(SUSTAIN_INPUT, "Sustain CV");
        configInput(TONE_INPUT, "Tone CV");
        configInput(VOLUME_INPUT, "Volume CV");
        configOutput(AUDIO_OUTPUT, "Audio");
        configLight(CLIP_LIGHT, "Clipping");
        configLight(AUDIO_LIGHT, "Output level");
        configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
        // Build the diode tables now rather than on the first process() call.
        raucus::diodeTables();
    }

    void onReset() override {
        for (int c = 0; c < kMaxChannels; c++) {
            voices[c].reset();
            outDc[c].reset();
            toneSet[c] = -1.f;
        }
        levelEnv = 0.f;
        smoothed = false;
        lastOsIndex = -1;
    }

    void onSampleRateChange() override { lastOsIndex = -1; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "oversampling", json_integer(osIndex));
        json_object_set_new(root, "diode", json_integer(diode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "oversampling"))
            osIndex = clamp((int)json_integer_value(j), 0, 4);
        if (json_t* j = json_object_get(root, "diode"))
            diode = clamp((int)json_integer_value(j), 0, (int)raucus::DIODE_LIFTED);
        lastOsIndex = -1;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        if (osIndex != lastOsIndex || sr != lastSampleRate) {
            for (int c = 0; c < kMaxChannels; c++) {
                upsampler[c].setOversamplingIndex(osIndex);
                upsampler[c].reset(sr);
                voices[c].setRate(sr * (1 << osIndex));
                outDc[c].set(sr);              // this one runs at host rate
                outDc[c].reset();
                toneSet[c] = -1.f;
            }
            lastOsIndex = osIndex;
            lastSampleRate = sr;
        }

        const int ratio = 1 << osIndex;
        const float fsOs = sr * (float)ratio;
        const float inLevel = 0.01f * std::pow(300.f, params[GAIN_PARAM].getValue());
        const float bias = params[BIAS_PARAM].getValue() * 0.15f;   // volts at the stage
        const raucus::DiodeTable* table =
            (diode == raucus::DIODE_LIFTED) ? nullptr : &raucus::diodeTables()[diode];

        // 10 ms on the three pedal controls, so a swept CV or a dragged knob
        // does not step the tone stack's coefficients.
        const float a = clamp(1.f - std::exp(-1.f / (0.01f * sr)), 0.f, 1.f);

        const int channels = std::max(1, inputs[AUDIO_INPUT].getChannels());
        outputs[AUDIO_OUTPUT].setChannels(channels);

        float peak = 0.f, clipAmt = 0.f;
        for (int c = 0; c < channels; c++) {
            raucus::Voice::Params p;
            const float sustain = clamp(params[SUSTAIN_PARAM].getValue()
                + inputs[SUSTAIN_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
            const float tone = clamp(params[TONE_PARAM].getValue()
                + inputs[TONE_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
            const float volume = clamp(params[VOLUME_PARAM].getValue()
                + inputs[VOLUME_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
            if (!smoothed) { sustainZ[c] = sustain; toneZ[c] = tone; volumeZ[c] = volume; }
            sustainZ[c] += a * (sustain - sustainZ[c]);
            toneZ[c] += a * (tone - toneZ[c]);
            volumeZ[c] += a * (volume - volumeZ[c]);

            // Solving the network is ~40 flops; only redo it when the knob has
            // actually moved far enough to matter.
            if (std::fabs(toneZ[c] - toneSet[c]) > 2e-4f) {
                voices[c].tone.set(toneZ[c], fsOs);
                toneSet[c] = toneZ[c];
            }

            p.sustain = sustainZ[c];
            p.volume = volumeZ[c];
            p.mids = params[MIDS_PARAM].getValue();
            p.bias = bias;
            p.table = table;

            const float in = inputs[AUDIO_INPUT].getPolyVoltage(c) * 0.2f * inLevel;
            upsampler[c].upsample(in);
            float* buf = upsampler[c].getOSBuffer();
            for (int k = 0; k < ratio; k++) buf[k] = voices[c].process(buf[k], p);
            float y = upsampler[c].downsample();
            if (!std::isfinite(y)) {
                y = 0.f;
                voices[c].reset();
                outDc[c].reset();
                toneSet[c] = -1.f;
            }
            // The pedal's own gain structure leaves about half a volt peak at
            // the output jack, as the hardware does; bring that up to Rack's
            // nominal +-5 V at the default volume. The makeup is calibrated on
            // the stock silicon pair, so the LED and lifted settings really are
            // several times louder, as they are on a modded pedal - soft-limit
            // rather than square them off against Rack's rails.
            // The soft ceiling asymptotes at 10 V, so however loud the LED
            // and lifted settings get they approach Rack's rail rather than
            // squaring off against it.
            // 4.8 rather than 5: the soft ceiling asymptotes at twice this,
            // and the coupling below shifts the signal by whatever offset it
            // removes. Without that headroom the shifted signal meets the
            // hard clamp, and a hard clamp on an asymmetric waveform puts
            // the offset straight back.
            y = outDc[c].process(raucus::railClip(12.f * y, 4.8f, 4.8f));
            y = clamp(y, -10.f, 10.f);
            outputs[AUDIO_OUTPUT].setVoltage(y, c);
            peak = std::max(peak, std::fabs(y));
            clipAmt = std::max(clipAmt, voices[c].clipEnv);
        }
        smoothed = true;

        levelEnv += (peak * 0.2f - levelEnv) * 0.002f;
        lights[AUDIO_LIGHT].setBrightness(clamp(levelEnv * 1.4f, 0.f, 1.f));
        lights[CLIP_LIGHT].setBrightness(clamp(clipAmt * 1.6f, 0.f, 1.f));
    }
};

struct RaucusWidget : ModuleWidget {
    RaucusWidget(Raucus* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/raucus.svg")));

// @layout:begin raucus 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem GAIN_PARAM Trimpot 3.03 param "" 0.0
// @elem SUSTAIN_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem BIAS_PARAM Trimpot 3.03 param "" 0.0
// @elem TONE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIDS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VOLUME_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SUSTAIN_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TONE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOLUME_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLIP_LIGHT SmallLight 1.0 light "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_GAIN label 0.0 label "gain" 0.0 9.00 28.50
// @elem LABEL_SUSTAIN label 0.0 label "sustain" 0.0 25.40 33.50
// @elem LABEL_BIAS label 0.0 label "bias" 0.0 41.80 28.50
// @elem LABEL_TONE label 0.0 label "tone" 0.0 15.00 58.50
// @elem LABEL_MIDS label 0.0 label "mids" 0.0 35.80 58.50
// @elem LABEL_VOLUME label 0.0 label "volume" 0.0 25.40 78.50
// @elem LABEL_IN label 0.0 label "in" 0.0 8.15 97.50
// @elem LABEL_SUSCV label 0.0 label "sus" 0.0 19.65 97.50
// @elem LABEL_TONECV label 0.0 label "tone" 0.0 31.15 97.50
// @elem LABEL_VOLCV label 0.0 label "vol" 0.0 42.65 97.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 25.40 110.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 25.40 115.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<Trimpot>(mm2px(Vec(9.00f, 22.00f)), module, Raucus::GAIN_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.40f, 22.00f)), module, Raucus::SUSTAIN_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(41.80f, 22.00f)), module, Raucus::BIAS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.00f, 50.00f)), module, Raucus::TONE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(35.80f, 50.00f)), module, Raucus::MIDS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 70.00f)), module, Raucus::VOLUME_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.15f, 90.00f)), module, Raucus::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.65f, 90.00f)), module, Raucus::SUSTAIN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.15f, 90.00f)), module, Raucus::TONE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.65f, 90.00f)), module, Raucus::VOLUME_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.40f, 108.00f)), module, Raucus::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(33.40f, 14.00f)), module, Raucus::CLIP_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(30.40f, 105.00f)), module, Raucus::AUDIO_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Raucus* m = dynamic_cast<Raucus*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Clipping diodes",
            {"Silicon 1N4148 (stock)", "Germanium (earlier, softer)",
             "LED (louder, cleaner)", "Lifted (no diodes)"},
            [m]() { return m->diode; },
            [m](int i) { m->diode = i; }));
        menu->addChild(createIndexSubmenuItem("Oversampling",
            {"1× (rawest, lightest)", "2×", "4× (default)", "8×", "16× (cleanest)"},
            [m]() { return m->osIndex; },
            [m](int i) { m->osIndex = i; }));
    }
};

Model* modelRaucus = createModel<Raucus, RaucusWidget>("raucus");
