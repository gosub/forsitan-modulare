// rete.cpp — VCV Rack 2 module
// rete (Latin for "net") is a feedback integrator network, a topology described
// by Nathan Ho (https://nathan.ho.name/posts/feedback-integrator-networks/),
// itself inspired by Giorgio Sancristoforo's Bentō. Eight signals run through
// leaky integrators into a fixed random 8x8 mixing matrix, then DC-blocking
// highpass filters, then hard clippers, and feed back with a one-sample delay.
// The result self-oscillates anywhere between steady tones and dense chaos.
//
// The block order (integrate -> mix -> highpass -> clip) is Ho's; he arrived
// at it by trial and error and it is what keeps the loop bounded and alive.
// Per his interface advice, the matrix itself stays fixed (re-rolled by the
// RND button/trigger, seed saved with the patch) and playability comes from
// the eight per-node gain knobs + CVs.
//
// Controls:
//   Knobs : G1..G8 (node gains), LEAK (integrator leak), SCALE (matrix drive),
//           IN LVL (excite input level)
//   In    : G1..G8 CV, IN (excite audio), RND trigger
//   Out   : L, R (nodes spread across the stereo field), POLY (all 8 nodes)
//   Light : LEVEL (output amplitude)

#include "forsitan.hpp"

static constexpr int kNodes = 8;

struct Rete : Module {
    enum ParamId {
        G1_PARAM, G2_PARAM, G3_PARAM, G4_PARAM,
        G5_PARAM, G6_PARAM, G7_PARAM, G8_PARAM,
        LEAK_PARAM,
        SCALE_PARAM,
        IN_LEVEL_PARAM,
        RND_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        G1_CV_INPUT, G2_CV_INPUT, G3_CV_INPUT, G4_CV_INPUT,
        G5_CV_INPUT, G6_CV_INPUT, G7_CV_INPUT, G8_CV_INPUT,
        EXCITE_INPUT,
        RND_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        POLY_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    // network state
    float matrix[kNodes][kNodes] = {};
    float integ[kNodes] = {};    // leaky integrator states
    float hpX[kNodes] = {};      // LeakDC previous input
    float hpY[kNodes] = {};      // LeakDC previous output
    float fb[kNodes] = {};       // clipper outputs, fed back next sample

    // stereo spread gains (equal power, node i at position i/(kNodes-1))
    float panL[kNodes], panR[kNodes];

    uint32_t seed = 0;
    uint32_t noiseState = 0x9d2c5680u;
    float curSampleRate = 0.f;
    float srRatio = 1.f;         // 48000 / sampleRate, for leak coefficients
    float levelEnv = 0.f;
    dsp::SchmittTrigger rndTrigger;
    dsp::BooleanTrigger rndButton;

    Rete() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        for (int i = 0; i < kNodes; i++) {
            configParam(G1_PARAM + i, 0.f, 1.f, 0.5f,
                        string::f("Node %d gain", i + 1), "%", 0.f, 100.f);
            configInput(G1_CV_INPUT + i, string::f("Node %d gain CV", i + 1));
        }
        configParam(LEAK_PARAM, 0.f, 1.f, 0.5f, "Integrator leak");
        configParam(SCALE_PARAM, 0.f, 1.f, 0.67f, "Matrix drive");
        configParam(IN_LEVEL_PARAM, 0.f, 1.f, 0.f, "Excite level", "%", 0.f, 100.f);
        configButton(RND_PARAM, "Randomize matrix");
        configInput(EXCITE_INPUT, "Excite audio");
        configInput(RND_INPUT, "Randomize matrix trigger");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configOutput(POLY_OUTPUT, "Nodes (8-channel polyphonic)");
        configLight(LEVEL_LIGHT, "Output level");

        for (int i = 0; i < kNodes; i++) {
            float p = (float)i / (kNodes - 1);
            panR[i] = std::sqrt(p);
            panL[i] = std::sqrt(1.f - p);
        }

        seed = random::u32();
        reseed(seed);
    }

    // xorshift32 on a local copy so matrix generation is reproducible from seed
    static uint32_t nextRand(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    static float bipolar(uint32_t& s) {
        return (nextRand(s) >> 8) * (2.f / 16777216.f) - 1.f;
    }

    // build the matrix from a seed and kick the integrators off equilibrium
    void reseed(uint32_t s) {
        seed = s ? s : 0x1u;
        uint32_t r = seed;
        for (int i = 0; i < kNodes; i++)
            for (int j = 0; j < kNodes; j++)
                matrix[i][j] = bipolar(r);
        for (int i = 0; i < kNodes; i++) {
            integ[i] = 0.01f * bipolar(r);
            hpX[i] = hpY[i] = 0.f;
            fb[i] = 0.f;
        }
    }

    void onReset() override {
        reseed(random::u32());
        levelEnv = 0.f;
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != curSampleRate) {
            curSampleRate = args.sampleRate;
            srRatio = 48000.f / args.sampleRate;
        }

        if (rndButton.process(params[RND_PARAM].getValue() > 0.5f)
            || rndTrigger.process(inputs[RND_INPUT].getVoltage(), 0.1f, 1.f))
            reseed(random::u32());

        // ── controls ────────────────────────────────────────────────────────
        // integrator leak: knob maps 1-coef exponentially 1e-1 .. 1e-5 (at 48k)
        float leakK = params[LEAK_PARAM].getValue();
        float leak48 = 1.f - std::pow(10.f, -1.f - 4.f * leakK);
        float leak = std::pow(leak48, srRatio);
        // LeakDC pole (Ho's first-order highpass), ~0.995 reference at 48k
        float dcR = std::pow(0.995f, srRatio);
        // matrix drive: 1 .. 1000, exponential
        float scale = std::pow(10.f, 3.f * params[SCALE_PARAM].getValue());

        float gain[kNodes];
        for (int i = 0; i < kNodes; i++) {
            float g = params[G1_PARAM + i].getValue();
            if (inputs[G1_CV_INPUT + i].isConnected())
                g += inputs[G1_CV_INPUT + i].getVoltage() * 0.2f;
            gain[i] = clamp(g, 0.f, 1.f) * scale;
        }

        float excite = inputs[EXCITE_INPUT].getVoltage() * 0.1f
                     * params[IN_LEVEL_PARAM].getValue();

        // ── one network tick ────────────────────────────────────────────────
        // tiny noise floor keeps the zero state from being a dead equilibrium
        float life = 1e-5f * bipolar(noiseState);

        float gi[kNodes];
        for (int i = 0; i < kNodes; i++) {
            // excite injected with alternating sign so it doesn't just move
            // every node identically
            float in = fb[i] + (i & 1 ? -excite : excite) + life;
            integ[i] = leak * integ[i] + in;
            gi[i] = gain[i] * integ[i];
        }

        float outL = 0.f, outR = 0.f;
        for (int i = 0; i < kNodes; i++) {
            const float* row = matrix[i];
            float m = 0.f;
            for (int j = 0; j < kNodes; j++)
                m += row[j] * gi[j];
            // LeakDC highpass
            float y = m - hpX[i] + dcR * hpY[i];
            hpX[i] = m;
            hpY[i] = y;
            if (!std::isfinite(y)) { y = 0.f; hpX[i] = hpY[i] = 0.f; integ[i] = 0.f; }
            // clipper bounds the loop
            float c = clamp(y, -1.f, 1.f);
            fb[i] = c;
            outL += c * panL[i];
            outR += c * panR[i];
            outputs[POLY_OUTPUT].setVoltage(5.f * c, i);
        }
        outputs[POLY_OUTPUT].setChannels(kNodes);

        // sum of panL^2 == kNodes/2; worst-case correlated sum stays near ±5V
        float comp = 5.f / (kNodes / 2.f);
        outputs[LEFT_OUTPUT].setVoltage(outL * comp);
        outputs[RIGHT_OUTPUT].setVoltage(outR * comp);

        float mag = std::max(std::fabs(outL), std::fabs(outR)) / std::sqrt((float)kNodes);
        levelEnv += (mag - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seed", json_integer((json_int_t)seed));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "seed"))
            reseed((uint32_t)json_integer_value(j));
    }
};

struct ReteWidget : ModuleWidget {
    ReteWidget(Rete* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/rete.svg")));

// @layout:begin rete 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem G1_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G2_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G3_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G4_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G5_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G6_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G7_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G8_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem G1_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G2_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G3_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G4_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G5_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G6_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G7_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G8_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEAK_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SCALE_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem IN_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem EXCITE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RND_PARAM TL1105 2.0 param "" 0.0
// @elem RND_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem POLY_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_GAINS label 0.0 label "node gains" 0.0 25.40 12.50
// @elem LABEL_LEAK label 0.0 label "leak" 0.0 11.00 82.50
// @elem LABEL_SCALE label 0.0 label "scale" 0.0 25.40 82.50
// @elem LABEL_INLVL label 0.0 label "lvl" 0.0 39.80 82.50
// @elem LABEL_IN label 0.0 label "in" 0.0 9.70 98.50
// @elem LABEL_RND label 0.0 label "rnd" 0.0 25.40 98.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 41.10 98.50
// @elem LABEL_OUTL label 0.0 label "L" 0.0 9.70 114.00
// @elem LABEL_OUTR label 0.0 label "R" 0.0 25.40 114.00
// @elem LABEL_POLY label 0.0 label "poly" 0.0 41.10 114.00
// @elem BOX_OUTL panel_box 7.0 box "" 0.0 9.70 108.50
// @elem BOX_OUTR panel_box 7.0 box "" 0.0 25.40 108.50
// @elem BOX_POLY panel_box 7.0 box "" 0.0 41.10 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(7.40f, 20.00f)), module, Rete::G1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(19.40f, 20.00f)), module, Rete::G2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(31.40f, 20.00f)), module, Rete::G3_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(43.40f, 20.00f)), module, Rete::G4_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(7.40f, 43.50f)), module, Rete::G5_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(19.40f, 43.50f)), module, Rete::G6_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(31.40f, 43.50f)), module, Rete::G7_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(43.40f, 43.50f)), module, Rete::G8_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.40f, 30.50f)), module, Rete::G1_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.40f, 30.50f)), module, Rete::G2_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.40f, 30.50f)), module, Rete::G3_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.40f, 30.50f)), module, Rete::G4_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.40f, 54.00f)), module, Rete::G5_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.40f, 54.00f)), module, Rete::G6_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.40f, 54.00f)), module, Rete::G7_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.40f, 54.00f)), module, Rete::G8_CV_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.00f, 71.00f)), module, Rete::LEAK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.40f, 71.00f)), module, Rete::SCALE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(39.80f, 71.00f)), module, Rete::IN_LEVEL_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.70f, 91.00f)), module, Rete::EXCITE_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(25.40f, 91.00f)), module, Rete::RND_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.10f, 91.00f)), module, Rete::RND_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(9.70f, 106.50f)), module, Rete::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.40f, 106.50f)), module, Rete::RIGHT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(41.10f, 106.50f)), module, Rete::POLY_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(29.20f, 102.70f)), module, Rete::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelRete = createModel<Rete, ReteWidget>("rete");
