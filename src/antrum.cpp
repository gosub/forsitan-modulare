// antrum.cpp - VCV Rack 2 module
// antrum (Latin: "cave, grotto") is a feedback delay network reverb built
// after the Make Noise / SoundHack Erbe-Verb: a reverb meant to be played,
// with every parameter modulatable at control or audio rate and a size
// control that runs from a coffin to the heavens without changing algorithm.
//
// Sources:
//   - Tom Erbe, "Building the Erbe-Verb: Extending the Feedback Delay
//     Network Reverb for Modular Synthesizer Use", ICMC 2015 - the topology,
//     the 0..0.8 allpass diffusion, the Chebyshev saturation before the
//     matrix, the two modulation types and the energy CV output.
//   - The Erbe-Verb manual (Make Noise) - control ranges and behaviour.
//   - davemollen's dm-Reverb (GPL-3.0) - the block layout, the delay time
//     ratios, the early reflection taps and the analog tilt filter model.
//
//   in ─> pre-delay (forward or reversed) ─> 4-line FDN ─> tilt ─> mix ─> out
//
// Controls:
//   Knobs : SIZE, PRE-DELAY, DECAY, ABSORB, DEPTH, SPEED, TILT, MIX,
//           each with its own attenuverter and CV input
//   Button: REVERSE (also a gate input)
//   In    : L, R (R normalled to L), CLK (pre-delay + speed clock sync)
//   Out   : L, R, CV (the network's own energy, 0..10 V)
//
// DEPTH is bipolar: CCW is cyclic modulation (multiphase sine vibrato in the
// delay lines), CW is ergodic modulation (grain clouds scattering the room
// dimensions), and the last stretch CW fades in the octave-up shimmer, the
// way the hardware does it (the context menu can unlink shimmer instead).
// ABSORB folds diffusion and damping into one knob, as the hardware does:
// the first third adds diffusion, the rest closes the absorption filters.

#include "forsitan.hpp"
#include "antrum_dsp.hpp"

// fixed shimmer amounts for the unlinked context-menu modes; index 0 is the
// default, where shimmer rides the top of the DEPTH knob instead
static const float kShimmerFixed[5] = {0.f, 0.f, 0.25f, 0.5f, 1.f};

// clock sync ratios, 1/1 in the middle (Erbe-Verb manual)
static const float kSyncRatios[15] = {
    1.f / 12.f, 1.f / 8.f, 1.f / 6.f, 1.f / 4.f, 1.f / 3.f, 1.f / 2.f,
    2.f / 3.f, 1.f, 3.f / 2.f, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f
};

struct Antrum : Module {
    enum ParamId {
        SIZE_PARAM,
        PREDELAY_PARAM,
        DECAY_PARAM,
        ABSORB_PARAM,
        DEPTH_PARAM,
        SPEED_PARAM,
        TILT_PARAM,
        MIX_PARAM,
        SIZE_ATT_PARAM,
        PREDELAY_ATT_PARAM,
        DECAY_ATT_PARAM,
        ABSORB_ATT_PARAM,
        DEPTH_ATT_PARAM,
        SPEED_ATT_PARAM,
        TILT_ATT_PARAM,
        MIX_ATT_PARAM,
        REVERSE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        LEFT_INPUT,
        RIGHT_INPUT,
        CLOCK_INPUT,
        REVERSE_INPUT,
        SIZE_CV_INPUT,
        PREDELAY_CV_INPUT,
        DECAY_CV_INPUT,
        ABSORB_CV_INPUT,
        DEPTH_CV_INPUT,
        SPEED_CV_INPUT,
        TILT_CV_INPUT,
        MIX_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        CV_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        SPEED_LIGHT,
        REVERSE_LIGHT,
        CV_LIGHT,
        LEVEL_L_LIGHT,
        LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    // one-pole parameter smoother
    struct Smooth {
        float current = 0.f, target = 0.f, b1 = 0.f;
        void setup(float initial, float freqHz, float rate) {
            current = target = initial;
            b1 = std::exp(-2.f * (float)M_PI * freqHz / rate);
        }
        inline float tick() {
            current = target + (current - target) * b1;
            return current;
        }
    };

    static constexpr int kControlDiv = 8;

    // Shimmer normally rides the top of the DEPTH knob, the way the hardware
    // does it, so the octave-up voice always comes with maximum grain
    // scatter. Unlinking it fixes shimmer at a set amount instead, which is
    // the only way to get a clean shimmer over an unmodulated room.
    static const int kShimmerModes = 5;

    antrum_dsp::Engine engine;
    antrum_dsp::Params p;
    // size and pre-delay smooth fast enough to keep audio-rate sweeps alive;
    // the feedback parameters are deliberately sluggish
    Smooth smSize, smPredelay, smReverse, smDecay, smDiffuse, smDamp,
        smCyclic, smErgodic, smShimmer, smSpeed, smTilt, smMix;
    dsp::SchmittTrigger clockTrigger, buttonTrigger;
    bool reverseLatched = false;
    int shimmerMode = 0;          // 0 = folded into depth, see kShimmerFixed
    float clockPeriod = 0.f;      // seconds between clock edges, 0 = unknown
    float clockTimer = 0.f;
    float sr = 0.f;
    int controlPhase = 0;
    float levelEnvL = 0.f, levelEnvR = 0.f;

    Antrum() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // exponential displays: value = multiplier * base^knob
        configParam(SIZE_PARAM, 0.f, 1.f, 0.706f, "Size", " ms",
                    antrum_dsp::kMaxSizeMs, antrum_dsp::kMinSizeMs);
        configParam(PREDELAY_PARAM, 0.f, 1.f, 0.f, "Pre-delay", " ms",
                    antrum_dsp::kMaxPredelayMs / antrum_dsp::kMinPredelayMs,
                    antrum_dsp::kMinPredelayMs);
        configParam(DECAY_PARAM, 0.f, 1.2f, 0.75f, "Decay", "%", 0.f, 100.f);
        configParam(ABSORB_PARAM, 0.f, 1.f, 0.5f, "Absorb (diffusion, then damping)",
                    "%", 0.f, 100.f);
        configParam(DEPTH_PARAM, -1.f, 1.f, 0.f, "Depth (cyclic <-> ergodic)",
                    "%", 0.f, 100.f);
        configParam(SPEED_PARAM, 0.f, 1.f, 0.25f, "Speed", " Hz", 512.f, 0.5f);
        configParam(TILT_PARAM, -1.f, 1.f, 0.f, "Tilt (low <-> high)", "%", 0.f, 100.f);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
        configParam(SIZE_ATT_PARAM, -1.f, 1.f, 0.f, "Size CV", "%", 0.f, 100.f);
        configParam(PREDELAY_ATT_PARAM, -1.f, 1.f, 0.f, "Pre-delay CV", "%", 0.f, 100.f);
        configParam(DECAY_ATT_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0.f, 100.f);
        configParam(ABSORB_ATT_PARAM, -1.f, 1.f, 0.f, "Absorb CV", "%", 0.f, 100.f);
        configParam(DEPTH_ATT_PARAM, -1.f, 1.f, 0.f, "Depth CV", "%", 0.f, 100.f);
        configParam(SPEED_ATT_PARAM, -1.f, 1.f, 0.f, "Speed CV", "%", 0.f, 100.f);
        configParam(TILT_ATT_PARAM, -1.f, 1.f, 0.f, "Tilt CV", "%", 0.f, 100.f);
        configParam(MIX_ATT_PARAM, -1.f, 1.f, 0.f, "Mix CV", "%", 0.f, 100.f);
        configButton(REVERSE_PARAM, "Reverse the pre-delay buffer");
        configInput(LEFT_INPUT, "Left audio");
        configInput(RIGHT_INPUT, "Right audio (normalled to left)");
        configInput(CLOCK_INPUT, "Clock (syncs pre-delay and speed)");
        configInput(REVERSE_INPUT, "Reverse gate");
        configInput(SIZE_CV_INPUT, "Size CV");
        configInput(PREDELAY_CV_INPUT, "Pre-delay CV");
        configInput(DECAY_CV_INPUT, "Decay CV");
        configInput(ABSORB_CV_INPUT, "Absorb CV");
        configInput(DEPTH_CV_INPUT, "Depth CV");
        configInput(SPEED_CV_INPUT, "Speed CV");
        configInput(TILT_CV_INPUT, "Tilt CV");
        configInput(MIX_CV_INPUT, "Mix CV");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configOutput(CV_OUTPUT, "Reverb energy (0..10 V)");
        configBypass(LEFT_INPUT, LEFT_OUTPUT);
        configBypass(RIGHT_INPUT, RIGHT_OUTPUT);
    }

    void initEngine(float sampleRate) {
        sr = sampleRate;
        engine.init(sr);
        smSize.setup(80.f, 300.f, sr);
        smPredelay.setup(antrum_dsp::kMinPredelayMs, 300.f, sr);
        smReverse.setup(0.f, 12.f, sr);
        smDecay.setup(0.9f, 20.f, sr);
        smDiffuse.setup(0.4f, 20.f, sr);
        smDamp.setup(0.25f, 20.f, sr);
        smCyclic.setup(0.f, 20.f, sr);
        smErgodic.setup(0.f, 20.f, sr);
        smShimmer.setup(0.f, 20.f, sr);
        smSpeed.setup(2.f, 20.f, sr);
        smTilt.setup(0.5f, 20.f, sr);
        smMix.setup(0.5f, 20.f, sr);
        controlPhase = 0;
        clockPeriod = 0.f;
        clockTimer = 0.f;
    }

    void onReset() override {
        sr = 0.f;               // force a re-init on the next process()
        reverseLatched = false;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "reverse", json_boolean(reverseLatched));
        json_object_set_new(root, "shimmerMode", json_integer(shimmerMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = json_object_get(root, "reverse");
        if (j) reverseLatched = json_boolean_value(j);
        j = json_object_get(root, "shimmerMode");
        if (j) shimmerMode = clamp((int)json_integer_value(j), 0, kShimmerModes - 1);
    }

    // knob + attenuverted CV, 5 V covering `span` of parameter range
    float knobCv(int param, int att, int input, float span) {
        float v = params[param].getValue();
        if (inputs[input].isConnected())
            v += inputs[input].getVoltage() * 0.2f * params[att].getValue() * span;
        return v;
    }

    static int ratioIndex(float t) {
        int i = (int)std::lround(clamp(t, 0.f, 1.f) * 14.f);
        return clamp(i, 0, 14);
    }

    void updateControls() {
        using namespace antrum_dsp;

        bool synced = inputs[CLOCK_INPUT].isConnected() && clockPeriod > 0.f;

        // ---- size: 1..500 ms, exponential
        float t = clamp(knobCv(SIZE_PARAM, SIZE_ATT_PARAM, SIZE_CV_INPUT, 1.f), 0.f, 1.f);
        smSize.target = kMinSizeMs * std::pow(kMaxSizeMs / kMinSizeMs, t);

        // ---- pre-delay: 7..500 ms free-running, clock ratios when synced
        t = clamp(knobCv(PREDELAY_PARAM, PREDELAY_ATT_PARAM, PREDELAY_CV_INPUT, 1.f),
                  0.f, 1.f);
        if (synced) {
            // CCW multiplies the clock period, CW divides it
            float ratio = kSyncRatios[ratioIndex(1.f - t)];
            smPredelay.target = clamp(clockPeriod * 1000.f * ratio,
                                      1.f, kMaxSyncPredelayMs);
        }
        else {
            smPredelay.target = kMinPredelayMs
                * std::pow(kMaxPredelayMs / kMinPredelayMs, t);
        }

        // ---- decay: up to 120% reflection gain
        smDecay.target = clamp(knobCv(DECAY_PARAM, DECAY_ATT_PARAM, DECAY_CV_INPUT, 1.2f),
                               0.f, 1.2f);

        // ---- absorb: diffusion over the first third, then damping
        float absorb = clamp(knobCv(ABSORB_PARAM, ABSORB_ATT_PARAM, ABSORB_CV_INPUT, 1.f),
                             0.f, 1.f);
        smDiffuse.target = std::min(absorb * 3.f, 1.f) * 0.8f;
        smDamp.target = std::max(absorb - 1.f / 3.f, 0.f) * 1.490214f;

        // ---- depth: cyclic below centre, ergodic above, shimmer at the top
        float depth = clamp(knobCv(DEPTH_PARAM, DEPTH_ATT_PARAM, DEPTH_CV_INPUT, 1.f),
                            -1.f, 1.f);
        float d2 = depth * depth;
        smCyclic.target = depth < 0.f ? d2 * kMaxCyclicMs : 0.f;
        smErgodic.target = depth > 0.f ? d2 * kErgodicFrac * smSize.target : 0.f;
        smShimmer.target = shimmerMode == 0 ? clamp((depth - 0.8f) * 5.f, 0.f, 1.f)
                                            : kShimmerFixed[shimmerMode];

        // ---- speed: 0.5..256 Hz, or a ratio of the clock
        t = clamp(knobCv(SPEED_PARAM, SPEED_ATT_PARAM, SPEED_CV_INPUT, 1.f), 0.f, 1.f);
        if (synced) {
            float ratio = kSyncRatios[ratioIndex(t)];
            smSpeed.target = clamp(ratio / clockPeriod, 0.02f, 9000.f);
        }
        else {
            smSpeed.target = 0.5f * std::pow(512.f, t);
        }

        // ---- tilt: bipolar, squared for a flat centre
        t = clamp(knobCv(TILT_PARAM, TILT_ATT_PARAM, TILT_CV_INPUT, 1.f), -1.f, 1.f);
        smTilt.target = t * std::fabs(t) * 0.5f + 0.5f;

        // ---- mix
        smMix.target = clamp(knobCv(MIX_PARAM, MIX_ATT_PARAM, MIX_CV_INPUT, 1.f),
                             0.f, 1.f);

        // ---- reverse: button latches, gate is momentary
        bool gate = inputs[REVERSE_INPUT].getVoltage() >= 1.5f;
        smReverse.target = (reverseLatched || gate) ? 1.f : 0.f;
    }

    void process(const ProcessArgs& args) override {
        if (sr != args.sampleRate)
            initEngine(args.sampleRate);

        if (buttonTrigger.process(params[REVERSE_PARAM].getValue(), 0.1f, 0.5f))
            reverseLatched = !reverseLatched;

        // ---- clock tracking for the sync ratios
        clockTimer += args.sampleTime;
        if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.2f, 1.5f)) {
            if (clockTimer > 0.001f && clockTimer < 10.f)
                clockPeriod = clockTimer;
            clockTimer = 0.f;
        }
        if (clockTimer > 10.f)
            clockPeriod = 0.f;

        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        p.sizeMs = smSize.tick();
        p.predelayMs = smPredelay.tick();
        p.reverse = smReverse.tick();
        p.decay = smDecay.tick();
        p.diffuse = smDiffuse.tick();
        p.damp = smDamp.tick();
        p.cyclicMs = smCyclic.tick();
        p.ergodicMs = smErgodic.tick();
        p.shimmer = smShimmer.tick();
        p.speedHz = smSpeed.tick();
        p.mix = smMix.tick();
        float tilt = smTilt.tick();
        if (controlPhase == 1)
            engine.tilt.setTilt(tilt);

        float inL = inputs[LEFT_INPUT].getVoltage() * 0.2f;
        float inR = inputs[RIGHT_INPUT].isConnected()
                  ? inputs[RIGHT_INPUT].getVoltage() * 0.2f : inL;

        float outL, outR;
        engine.process(inL, inR, p, outL, outR);

        outputs[LEFT_OUTPUT].setVoltage(clamp(5.f * outL, -10.f, 10.f));
        outputs[RIGHT_OUTPUT].setVoltage(clamp(5.f * outR, -10.f, 10.f));

        float energy = engine.energy();
        outputs[CV_OUTPUT].setVoltage(clamp(energy * 10.f, 0.f, 10.f));

        levelEnvL += (std::fabs(outL) - levelEnvL) * 0.002f;
        levelEnvR += (std::fabs(outR) - levelEnvR) * 0.002f;
        lights[LEVEL_L_LIGHT].setBrightness(clamp(levelEnvL * 2.f, 0.f, 1.f));
        lights[LEVEL_R_LIGHT].setBrightness(clamp(levelEnvR * 2.f, 0.f, 1.f));
        lights[CV_LIGHT].setBrightness(clamp(energy, 0.f, 1.f));
        lights[SPEED_LIGHT].setBrightness(engine.lfo.x < 0.5f ? 1.f : 0.f);
        // lit while reversing, flickering once per buffer pass
        bool rev = smReverse.target > 0.5f;
        lights[REVERSE_LIGHT].setBrightness(
            rev ? (engine.reverse.phasor.x < 0.5f ? 1.f : 0.25f) : 0.f);
    }
};

struct AntrumWidget : ModuleWidget {
    AntrumWidget(Antrum* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/antrum.svg")));

// @layout:begin antrum 101.6 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem SIZE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem PREDELAY_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DECAY_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem ABSORB_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DEPTH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPEED_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TILT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SIZE_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem PREDELAY_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem DECAY_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem ABSORB_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem DEPTH_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem SPEED_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem TILT_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem MIX_ATT_PARAM Trimpot 2.5 param "" 0.0
// @elem REVERSE_PARAM TL1105 2.6 param "" 0.0
// @elem SIZE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PREDELAY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DECAY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem ABSORB_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DEPTH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPEED_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TILT_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RIGHT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REVERSE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem SPEED_LIGHT SmallLight 1.5 light "" 0.0
// @elem REVERSE_LIGHT SmallLight 1.5 light "" 0.0
// @elem CV_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_SIZE label 0.0 label "size" 0.0 14.80 35.50
// @elem LABEL_PREDELAY label 0.0 label "pre-delay" 0.0 38.80 35.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 62.80 35.50
// @elem LABEL_ABSORB label 0.0 label "absorb" 0.0 86.80 35.50
// @elem LABEL_DEPTH label 0.0 label "depth" 0.0 14.80 74.50
// @elem LABEL_SPEED label 0.0 label "speed" 0.0 38.80 74.50
// @elem LABEL_TILT label 0.0 label "tilt" 0.0 62.80 74.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 86.80 74.50
// @elem LABEL_INL label 0.0 label "in l" 0.0 6.50 112.50
// @elem LABEL_INR label 0.0 label "in r" 0.0 17.50 112.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 28.50 112.50
// @elem LABEL_GATE label 0.0 label "gate" 0.0 39.50 112.50
// @elem LABEL_REV label 0.0 label "rev" 0.0 49.50 112.00
// @elem LABEL_CV label 0.0 label "cv" 0.0 61.40 112.50
// @elem LABEL_L label 0.0 label "L" 0.0 77.00 112.50
// @elem LABEL_R label 0.0 label "R" 0.0 92.60 112.50
// @elem BOX_CV panel_box 7.0 box "" 0.0 61.40 107.00
// @elem BOX_L panel_box 7.0 box "" 0.0 77.00 107.00
// @elem BOX_R panel_box 7.0 box "" 0.0 92.60 107.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 50.80 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(14.80f, 24.00f)), module, Antrum::SIZE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(38.80f, 24.00f)), module, Antrum::PREDELAY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(62.80f, 24.00f)), module, Antrum::DECAY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(86.80f, 24.00f)), module, Antrum::ABSORB_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(9.80f, 42.00f)), module, Antrum::SIZE_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.80f, 42.00f)), module, Antrum::PREDELAY_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(57.80f, 42.00f)), module, Antrum::DECAY_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(81.80f, 42.00f)), module, Antrum::ABSORB_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.30f, 42.00f)), module, Antrum::SIZE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.30f, 42.00f)), module, Antrum::PREDELAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(67.30f, 42.00f)), module, Antrum::DECAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(91.30f, 42.00f)), module, Antrum::ABSORB_CV_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.80f, 66.00f)), module, Antrum::DEPTH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.80f, 66.00f)), module, Antrum::SPEED_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(62.80f, 66.00f)), module, Antrum::TILT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(86.80f, 66.00f)), module, Antrum::MIX_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(9.80f, 81.00f)), module, Antrum::DEPTH_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.80f, 81.00f)), module, Antrum::SPEED_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(57.80f, 81.00f)), module, Antrum::TILT_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(81.80f, 81.00f)), module, Antrum::MIX_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.30f, 81.00f)), module, Antrum::DEPTH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.30f, 81.00f)), module, Antrum::SPEED_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(67.30f, 81.00f)), module, Antrum::TILT_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(91.30f, 81.00f)), module, Antrum::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.50f, 105.00f)), module, Antrum::LEFT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.50f, 105.00f)), module, Antrum::RIGHT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.50f, 105.00f)), module, Antrum::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(39.50f, 105.00f)), module, Antrum::REVERSE_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(49.50f, 105.00f)), module, Antrum::REVERSE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(61.40f, 105.00f)), module, Antrum::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(77.00f, 105.00f)), module, Antrum::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(92.60f, 105.00f)), module, Antrum::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(44.30f, 60.50f)), module, Antrum::SPEED_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(52.50f, 102.00f)), module, Antrum::REVERSE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(66.40f, 102.00f)), module, Antrum::CV_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(82.00f, 102.00f)), module, Antrum::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(97.60f, 102.00f)), module, Antrum::LEVEL_R_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Antrum* module = getModule<Antrum>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Shimmer",
            {"Folded into depth", "Off", "25%", "50%", "100%"},
            [=]() { return module->shimmerMode; },
            [=](int idx) { module->shimmerMode = idx; }));
    }
};

Model* modelAntrum = createModel<Antrum, AntrumWidget>("antrum");
