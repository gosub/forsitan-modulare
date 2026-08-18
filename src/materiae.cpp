// materiae.cpp — VCV Rack 2 module
// materiae (Latin: "of matter", genitive of materia — raw stuff, timber, the
// material a thing is made from) is a percussive voice built from two square
// waves and nothing else. All of its complexity comes from the relationship
// between them: how their frequencies sit, how they modulate each other, and
// which operator reads the pair.
//
//   trig ─> two naive squares ─> A<->B cross-modulation
//        ─> relationship operator ─> resonant filter ─> VCA ─> out
//
//   env 1 ─> amplitude
//   env 2 ─> pitch, the operator itself, cutoff (bipolar, per destination)
//
// Controls:
//   sources  : PITCH (+ V/oct), RATIO, SHAPE (pulse-width skew), GRID, DIV
//   relation : XMOD, TILT, DEST switch, RELATION, BLEND
//   body     : CUTOFF, RESO, LP/BP switch
//   time     : ATTACK, DECAY, CURVE (env 1), DECAY 2, CURVE 2, and three
//              bipolar trimpots routing env 2
//   in       : TRIG, V/OCT, and CV for ratio, xmod, relation, cutoff
//   out      : AUDIO, ENV 2
//
// See src/materiae_dsp.hpp for why the operator set is five and not eight,
// and why the logic core runs on a clock of its own.

#include "forsitan.hpp"
#include "materiae_dsp.hpp"

using namespace materiae_dsp;

static const float kOutputVpp[3] = {5.f, 10.f, 14.f};
static const float kPhaseOffsets[4] = {0.f, 0.25f, 0.5f, 0.75f};

static const char* kOpNames[kNumOps] = {"and", "sum", "ring", "flip", "noise"};

// display names for the ratio table, in the same order as kRatios
static const char* kRatioNames[kNumRatios] = {
    "1:4", "1:3", "1:2", "2:3", "3:4",
    "1:1", "1:1 detuned", "5:4", "4:3", "sqrt2",
    "3:2", "5:3", "7:4", "2:1", "5:2",
    "e", "3:1", "pi", "4:1"
};

struct Materiae;

// The ratio knob reads as a table index; show what it actually selects.
// Defined out of line below, because it has to ask the module which mode the
// knob is in.
struct RatioQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
};

// RELATION sits between operators as often as on one, and the crossfade is
// the point, so the tooltip names the pair and how far across it is.
struct RelationQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        float v = clamp(getValue(), 0.f, (float)(kNumOps - 1));
        int i = clamp((int)v, 0, kNumOps - 2);
        float f = v - (float)i;
        if (f < 0.02f) return kOpNames[i];
        if (f > 0.98f) return kOpNames[i + 1];
        return string::f("%s / %s %.0f%%", kOpNames[i], kOpNames[i + 1], f * 100.f);
    }
};

struct DivQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        int i = clamp((int)std::lround(getValue()), 0, kNumDiv - 1);
        return string::f("/%d", 1 << i);
    }
};

struct GridQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        float sr = APP->engine->getSampleRate();
        float t = clamp(getValue(), 0.f, 1.f);
        float top = std::log2(sr * kGridMaxMult), bot = std::log2(kGridMin);
        float hz = std::pow(2.f, top + (bot - top) * t);
        if (hz >= sr) return string::f("%.1fx clean (%.0f kHz)", hz / sr, hz * 0.001f);
        return string::f("%.1f kHz", hz * 0.001f);
    }
};

// The gain knob is a drive, so it reads in dB: past about half the knob the
// saturator is doing more than the level is.
struct GainQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        float k = clamp(getValue(), 0.f, 1.f);
        return string::f("%+.1f dB", k * 4.f * 6.0206f);
    }
};

struct CutoffQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        float t = clamp(getValue(), 0.f, 1.f);
        return string::f("%.0f Hz",
                         kMinCut * std::pow(2.f, t * std::log2(kMaxCut / kMinCut)));
    }
};

// the three time knobs share a shape: exponential over a fixed span
struct TimeQuantity : ParamQuantity {
    float base = 0.005f, span = 9.6f;
    std::string getDisplayValueString() override {
        float s = base * std::pow(2.f, clamp(getValue(), 0.f, 1.f) * span);
        return s < 1.f ? string::f("%.1f ms", s * 1000.f) : string::f("%.2f s", s);
    }
};

struct Materiae : Module {
    enum ParamId {
        PITCH_PARAM,
        RATIO_PARAM,
        SHAPE_PARAM,
        GRID_PARAM,
        DIV_PARAM,
        XMOD_PARAM,
        TILT_PARAM,
        DEST_PARAM,
        RELATION_PARAM,
        BLEND_PARAM,
        CUTOFF_PARAM,
        RESO_PARAM,
        FILTER_PARAM,
        GAIN_PARAM,
        DECAY2_PARAM,
        CURVE2_PARAM,
        ATTACK_PARAM,
        DECAY_PARAM,
        CURVE_PARAM,
        E2PITCH_PARAM,
        E2REL_PARAM,
        E2CUT_PARAM,
        HIT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        TRIG_INPUT,
        VOCT_INPUT,
        RATIO_CV_INPUT,
        XMOD_CV_INPUT,
        RELATION_CV_INPUT,
        CUTOFF_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        ENV2_OUTPUT,
        DRONE_OUTPUT,
        AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        HIT_LIGHT,
        ENV2_LIGHT,
        DRONE_LIGHT,
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    materiae_dsp::Engine engine;   // rack::engine::Engine is also in scope
    materiae_dsp::Params p;
    dsp::SchmittTrigger trigTrigger, buttonTrigger;

    // menu state, saved in the patch
    bool freeRun = false;
    bool env2Free = false;
    bool trackCutoff = false;
    bool freeRatio = false;
    bool useVelocity = true;
    int phaseIdx = 0;
    int outputLevel = 1;

    float e2Pitch = 0.f, e2Rel = 0.f, e2Cut = 0.f, voct = 0.f;
    float levelEnv = 0.f, droneEnv = 0.f, hitLight = 0.f;
    int velCount = 0;
    float velPeak = 0.f;
    int controlPhase = 0;
    static const int kControlDiv = 16;

    Materiae() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(PITCH_PARAM, -2.f, 7.f, 1.f, "Pitch", " oct");
        configParam<RatioQuantity>(RATIO_PARAM, 0.f, (float)(kNumRatios - 1), 10.f, "Ratio");
        configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape (pulse-width skew)");
        configParam<GridQuantity>(GRID_PARAM, 0.f, 1.f, 0.f, "Grid (logic-core rate)");
        configParam<DivQuantity>(DIV_PARAM, 0.f, (float)(kNumDiv - 1), 0.f, "Division");
        configParam(XMOD_PARAM, 0.f, 1.f, 0.f, "Cross-modulation", "%", 0.f, 100.f);
        configParam(TILT_PARAM, -1.f, 1.f, 0.f, "Tilt (A->B vs B->A)");
        configSwitch(DEST_PARAM, 0.f, 2.f, 0.f, "Modulation destination",
                     {"Frequency", "Amplitude", "Both"});
        configParam<RelationQuantity>(RELATION_PARAM, 0.f, (float)(kNumOps - 1), 1.f, "Relation");
        configParam(BLEND_PARAM, 0.f, 1.f, 1.f, "Blend (osc A -> operator)",
                    "%", 0.f, 100.f);
        configParam<CutoffQuantity>(CUTOFF_PARAM, 0.f, 1.f, 0.65f, "Cutoff");
        configParam(RESO_PARAM, 0.f, 1.f, 0.2f, "Resonance", "%", 0.f, 100.f);
        configSwitch(FILTER_PARAM, 0.f, 1.f, 0.f, "Filter", {"Lowpass", "Bandpass"});
        configParam<GainQuantity>(GAIN_PARAM, 0.f, 1.f, 0.f, "Gain");
        { auto* q = configParam<TimeQuantity>(DECAY2_PARAM, 0.f, 1.f, 0.35f, "Decay 2");
          q->base = 0.003f; q->span = 9.4f; }
        configParam(CURVE2_PARAM, -1.f, 1.f, 0.5f, "Curve 2");
        { auto* q = configParam<TimeQuantity>(ATTACK_PARAM, 0.f, 1.f, 0.f, "Attack");
          q->base = 0.0002f; q->span = 11.3f; }
        { auto* q = configParam<TimeQuantity>(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay");
          q->base = 0.005f; q->span = 9.6f; }
        configParam(CURVE_PARAM, -1.f, 1.f, 0.5f, "Curve");
        configParam(E2PITCH_PARAM, -1.f, 1.f, 0.f, "Env 2 -> pitch");
        configParam(E2REL_PARAM, -1.f, 1.f, 0.f, "Env 2 -> relation");
        configParam(E2CUT_PARAM, -1.f, 1.f, 0.f, "Env 2 -> cutoff");
        configButton(HIT_PARAM, "Hit");

        getParamQuantity(RATIO_PARAM)->snapEnabled = true;
        getParamQuantity(DIV_PARAM)->snapEnabled = true;

        configInput(TRIG_INPUT, "Trigger");
        configInput(VOCT_INPUT, "1V/octave");
        configInput(RATIO_CV_INPUT, "Ratio CV");
        configInput(XMOD_CV_INPUT, "Cross-modulation CV");
        configInput(RELATION_CV_INPUT, "Relation CV");
        configInput(CUTOFF_CV_INPUT, "Cutoff CV");
        configOutput(ENV2_OUTPUT, "Env 2");
        configOutput(DRONE_OUTPUT, "Drone (the voice before env 1)");
        configOutput(AUDIO_OUTPUT, "Audio");

        engine.setSampleRate(44100.f);
        engine.reset();
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        engine.setSampleRate(e.sampleRate);
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        engine.reset();
    }

    // everything that only needs to move at control rate
    void mapParams(float sampleRate) {
        voct = inputs[VOCT_INPUT].getVoltage();
        p.f0 = clampf(kBaseHz * fastExp2(params[PITCH_PARAM].getValue() + voct),
                      kMinF0, kMaxF0);

        float rIdx = params[RATIO_PARAM].getValue();
        if (inputs[RATIO_CV_INPUT].isConnected())
            rIdx += inputs[RATIO_CV_INPUT].getVoltage() * 0.2f
                  * (float)(kNumRatios - 1) * 0.5f;
        if (freeRatio) {
            // the knob's own span, read as a continuous ratio instead of an index
            float t = clampf(rIdx / (float)(kNumRatios - 1), 0.f, 1.f);
            p.ratio = 0.25f * fastExp2(t * 4.f);           // 0.25x .. 4x
        } else {
            int i = (int)std::lround(clampf(rIdx, 0.f, (float)(kNumRatios - 1)));
            p.ratio = kRatios[i];
        }

        p.shape = params[SHAPE_PARAM].getValue();

        // GRID: 4x the host rate down to 250 Hz, logarithmic
        float gk = clampf(params[GRID_PARAM].getValue(), 0.f, 1.f);
        float top = std::log2(sampleRate * kGridMaxMult);
        float bot = std::log2(kGridMin);
        p.gridRate = fastExp2(top + (bot - top) * gk);

        p.divShift = (int)std::lround(clampf(params[DIV_PARAM].getValue(),
                                             0.f, (float)(kNumDiv - 1)));

        p.xmod = clampf(params[XMOD_PARAM].getValue()
                      + inputs[XMOD_CV_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
        p.tilt = clampf(params[TILT_PARAM].getValue(), -1.f, 1.f);
        p.modDest = (int)params[DEST_PARAM].getValue();

        p.relation = clampf(params[RELATION_PARAM].getValue()
                          + inputs[RELATION_CV_INPUT].getVoltage() * 0.4f,
                          0.f, (float)(kNumOps - 1));
        p.blend = clampf(params[BLEND_PARAM].getValue(), 0.f, 1.f);

        // cutoff knob is exponential over the filter's whole range; its CV is
        // a straight volt-per-octave on top of it
        float ck = clampf(params[CUTOFF_PARAM].getValue(), 0.f, 1.f);
        p.cutoff = kMinCut * fastExp2(ck * std::log2(kMaxCut / kMinCut)
                                    + inputs[CUTOFF_CV_INPUT].getVoltage());
        p.cutoff = clampf(p.cutoff, kMinCut, kMaxCut);
        p.reso = clampf(params[RESO_PARAM].getValue(), 0.f, 1.f);
        p.filterMode = (int)params[FILTER_PARAM].getValue();
        // 0 to +24 dB into a saturator that asymptotes at unity: past the
        // knee the knob stops making it louder and starts making it dirtier
        p.gain = fastExp2(clampf(params[GAIN_PARAM].getValue(), 0.f, 1.f) * 4.f);

        p.attack = 0.0002f * fastExp2(params[ATTACK_PARAM].getValue() * 11.3f);
        p.decay = 0.005f * fastExp2(params[DECAY_PARAM].getValue() * 9.6f);
        p.curve = params[CURVE_PARAM].getValue();
        p.decay2 = 0.003f * fastExp2(params[DECAY2_PARAM].getValue() * 9.4f);
        p.curve2 = params[CURVE2_PARAM].getValue();

        e2Pitch = params[E2PITCH_PARAM].getValue();
        e2Rel = params[E2REL_PARAM].getValue();
        e2Cut = params[E2CUT_PARAM].getValue();

        // the drone tap has no envelope in front of it, so the engine cannot
        // be allowed to idle while anything is listening to it
        engine.alwaysRun = outputs[DRONE_OUTPUT].isConnected();
        engine.freeRun = freeRun;
        engine.env2Free = env2Free;
        engine.trackCutoff = trackCutoff;
        engine.phaseOffset = kPhaseOffsets[clamp(phaseIdx, 0, 3)];
    }

    void process(const ProcessArgs& args) override {
        // onSampleRateChange does not reach a module built outside Rack (the
        // offline harnesses), and this costs one compare a sample
        if (args.sampleRate != engine.sampleRate)
            engine.setSampleRate(args.sampleRate);
        if (controlPhase == 0) mapParams(args.sampleRate);
        if (++controlPhase >= kControlDiv) controlPhase = 0;

        float tv = inputs[TRIG_INPUT].getVoltage();
        bool hit = trigTrigger.process(tv, 0.1f, 1.5f);
        bool button = buttonTrigger.process(params[HIT_PARAM].getValue(), 0.1f, 0.5f);
        if (hit || button) {
            // Velocity is the trigger's own height, read over the 2 ms after
            // the edge: a 5 V trigger is already full, so only a deliberately
            // attenuated one plays quieter and the usual patch is unaffected.
            velPeak = button ? 5.f : tv;
            velCount = (int)(0.002f * args.sampleRate);
            engine.trigger(p, 1.f);
            hitLight = 1.f;
        }
        if (velCount > 0) {
            velPeak = std::max(velPeak, tv);
            velCount--;
            engine.velocity = useVelocity
                ? clampf(velPeak * 0.2f, 0.05f, 1.f) : 1.f;
        }

        float audio, drone, env2;
        engine.process(p, args.sampleTime, audio, drone, env2, e2Pitch, e2Rel,
                       e2Cut, voct);

        float half = kOutputVpp[clamp(outputLevel, 0, 2)] * 0.5f;
        outputs[AUDIO_OUTPUT].setVoltage(clamp(audio * half, -10.f, 10.f));
        outputs[DRONE_OUTPUT].setVoltage(clamp(drone * half, -10.f, 10.f));
        outputs[ENV2_OUTPUT].setVoltage(clamp(env2 * 10.f, 0.f, 10.f));

        levelEnv += (std::fabs(audio) - levelEnv) * 0.002f;
        droneEnv += (std::fabs(drone) - droneEnv) * 0.002f;
        hitLight = std::max(0.f, hitLight - args.sampleTime * 8.f);
        lights[HIT_LIGHT].setBrightness(hitLight);
        lights[ENV2_LIGHT].setBrightness(clamp(env2, 0.f, 1.f));
        lights[DRONE_LIGHT].setBrightness(clamp(droneEnv * 3.f, 0.f, 1.f));
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv * 3.f, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "freeRun", json_boolean(freeRun));
        json_object_set_new(root, "env2Free", json_boolean(env2Free));
        json_object_set_new(root, "trackCutoff", json_boolean(trackCutoff));
        json_object_set_new(root, "freeRatio", json_boolean(freeRatio));
        json_object_set_new(root, "useVelocity", json_boolean(useVelocity));
        json_object_set_new(root, "phaseIdx", json_integer(phaseIdx));
        json_object_set_new(root, "outputLevel", json_integer(outputLevel));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "freeRun"))) freeRun = json_boolean_value(j);
        if ((j = json_object_get(root, "env2Free"))) env2Free = json_boolean_value(j);
        if ((j = json_object_get(root, "trackCutoff"))) trackCutoff = json_boolean_value(j);
        if ((j = json_object_get(root, "freeRatio"))) freeRatio = json_boolean_value(j);
        if ((j = json_object_get(root, "useVelocity"))) useVelocity = json_boolean_value(j);
        if ((j = json_object_get(root, "phaseIdx"))) phaseIdx = (int)json_integer_value(j);
        if ((j = json_object_get(root, "outputLevel"))) outputLevel = (int)json_integer_value(j);
    }
};

// RatioQuantity needs the module to know which mode the knob is in, so its
// body waits until Materiae is a complete type.
std::string RatioQuantity::getDisplayValueString() {
    Materiae* m = dynamic_cast<Materiae*>(module);
    float v = getValue();
    if (m && m->freeRatio) {
        float t = clamp(v / (float)(kNumRatios - 1), 0.f, 1.f);
        return string::f("%.3fx", 0.25f * std::pow(2.f, t * 4.f));
    }
    int i = clamp((int)std::lround(v), 0, kNumRatios - 1);
    return kRatioNames[i];
}

struct MateriaeWidget : ModuleWidget {
    MateriaeWidget(Materiae* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/materiae.svg")));

// @layout:begin materiae 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RATIO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SHAPE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GRID_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DIV_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem XMOD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TILT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DEST_PARAM CKSSThree 2.3 param "" 0.0
// @elem RELATION_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BLEND_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CUTOFF_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RESO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FILTER_PARAM CKSS 2.3 param "" 0.0
// @elem GAIN_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem E2PITCH_PARAM Trimpot 3.03 param "" 0.0
// @elem E2REL_PARAM Trimpot 3.03 param "" 0.0
// @elem E2CUT_PARAM Trimpot 3.03 param "" 0.0
// @elem ATTACK_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CURVE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CURVE2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem HIT_PARAM TL1105 2.6 param "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RATIO_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem XMOD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RELATION_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CUTOFF_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ENV2_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem DRONE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem HIT_LIGHT SmallLight 1.0 light "" 0.0
// @elem ENV2_LIGHT SmallLight 1.0 light "" 0.0
// @elem DRONE_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 14.96 29.50
// @elem LABEL_RATIO label 0.0 label "ratio" 0.0 37.96 29.50
// @elem LABEL_SHAPE label 0.0 label "shape" 0.0 60.96 29.50
// @elem LABEL_GRID label 0.0 label "grid" 0.0 83.96 29.50
// @elem LABEL_DIV label 0.0 label "div" 0.0 106.96 29.50
// @elem LABEL_XMOD label 0.0 label "xmod" 0.0 14.96 53.50
// @elem LABEL_TILT label 0.0 label "tilt" 0.0 37.96 53.50
// @elem LABEL_DEST label 0.0 label "dest" 0.0 60.96 53.50
// @elem LABEL_RELATION label 0.0 label "relation" 0.0 83.96 53.50
// @elem LABEL_BLEND label 0.0 label "blend" 0.0 106.96 53.50
// @elem LABEL_E2 label 0.0 label "env 2 to" 0.0 106.96 63.00
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 14.96 77.50
// @elem LABEL_RESO label 0.0 label "reso" 0.0 37.96 77.50
// @elem LABEL_FILTER label 0.0 label "lp/bp" 0.0 60.96 77.50
// @elem LABEL_GAIN label 0.0 label "gain" 0.0 83.96 77.50
// @elem LABEL_E2PITCH label 0.0 label "pit" 0.0 98.96 75.50
// @elem LABEL_E2REL label 0.0 label "rel" 0.0 106.96 75.50
// @elem LABEL_E2CUT label 0.0 label "cut" 0.0 114.96 75.50
// @elem LABEL_ATTACK label 0.0 label "att" 0.0 14.96 98.50
// @elem LABEL_DECAY label 0.0 label "dec" 0.0 37.96 98.50
// @elem LABEL_CURVE label 0.0 label "crv" 0.0 60.96 98.50
// @elem LABEL_DECAY2 label 0.0 label "dec 2" 0.0 83.96 98.50
// @elem LABEL_CURVE2 label 0.0 label "crv 2" 0.0 106.96 98.50
// @elem LABEL_HIT label 0.0 label "hit" 0.0 7.00 112.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 17.00 112.50
// @elem LABEL_VOCT label 0.0 label "v/o" 0.0 26.76 112.50
// @elem LABEL_RATIOCV label 0.0 label "rat" 0.0 36.52 112.50
// @elem LABEL_XMODCV label 0.0 label "xmd" 0.0 46.28 112.50
// @elem LABEL_RELCV label 0.0 label "rel" 0.0 56.04 112.50
// @elem LABEL_CUTCV label 0.0 label "cut" 0.0 65.80 112.50
// @elem BOX_ENV2 panel_box 7.0 box "" 0.0 78.50 107.00
// @elem BOX_DRONE panel_box 7.0 box "" 0.0 94.00 107.00
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 109.50 107.00
// @elem LABEL_ENV2 label 0.0 label "env" 0.0 78.50 112.50
// @elem LABEL_DRONE label 0.0 label "drone" 0.0 94.00 112.50
// @elem LABEL_AUDIO label 0.0 label "out" 0.0 109.50 112.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.96f, 21.00f)), module, Materiae::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.96f, 21.00f)), module, Materiae::RATIO_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60.96f, 21.00f)), module, Materiae::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(83.96f, 21.00f)), module, Materiae::GRID_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(106.96f, 21.00f)), module, Materiae::DIV_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.96f, 45.00f)), module, Materiae::XMOD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.96f, 45.00f)), module, Materiae::TILT_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(60.96f, 45.00f)), module, Materiae::DEST_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(83.96f, 45.00f)), module, Materiae::RELATION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(106.96f, 45.00f)), module, Materiae::BLEND_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.96f, 69.00f)), module, Materiae::CUTOFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.96f, 69.00f)), module, Materiae::RESO_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(60.96f, 69.00f)), module, Materiae::FILTER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(83.96f, 69.00f)), module, Materiae::GAIN_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(98.96f, 69.00f)), module, Materiae::E2PITCH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(106.96f, 69.00f)), module, Materiae::E2REL_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(114.96f, 69.00f)), module, Materiae::E2CUT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(14.96f, 90.00f)), module, Materiae::ATTACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.96f, 90.00f)), module, Materiae::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60.96f, 90.00f)), module, Materiae::CURVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(83.96f, 90.00f)), module, Materiae::DECAY2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(106.96f, 90.00f)), module, Materiae::CURVE2_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(7.00f, 105.00f)), module, Materiae::HIT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.00f, 105.00f)), module, Materiae::TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(26.76f, 105.00f)), module, Materiae::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(36.52f, 105.00f)), module, Materiae::RATIO_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(46.28f, 105.00f)), module, Materiae::XMOD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(56.04f, 105.00f)), module, Materiae::RELATION_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.80f, 105.00f)), module, Materiae::CUTOFF_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.50f, 105.00f)), module, Materiae::ENV2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(94.00f, 105.00f)), module, Materiae::DRONE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(109.50f, 105.00f)), module, Materiae::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(10.00f, 102.00f)), module, Materiae::HIT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(83.50f, 102.00f)), module, Materiae::ENV2_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(99.00f, 102.00f)), module, Materiae::DRONE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(114.50f, 102.00f)), module, Materiae::LEVEL_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Materiae* module = getModule<Materiae>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Oscillator phase on trigger",
            {"Reset (repeatable hits)", "Free-running (every hit differs)"},
            [=]() { return module->freeRun ? 1 : 0; },
            [=](int idx) { module->freeRun = (idx == 1); }));
        menu->addChild(createIndexSubmenuItem("Osc B phase offset",
            {"0 deg", "90 deg", "180 deg", "270 deg"},
            [=]() { return module->phaseIdx; },
            [=](int idx) { module->phaseIdx = idx; }));
        menu->addChild(createIndexSubmenuItem("Env 2 on retrigger",
            {"Retrigger", "Free (finish the running one)"},
            [=]() { return module->env2Free ? 1 : 0; },
            [=](int idx) { module->env2Free = (idx == 1); }));
        menu->addChild(createIndexSubmenuItem("Ratio knob",
            {"Table (19 steps)", "Free (0.25x - 4x)"},
            [=]() { return module->freeRatio ? 1 : 0; },
            [=](int idx) { module->freeRatio = (idx == 1); }));
        menu->addChild(createBoolPtrMenuItem("Cutoff tracks pitch", "",
            &module->trackCutoff));
        menu->addChild(createBoolPtrMenuItem("Velocity from trigger height", "",
            &module->useVelocity));
        menu->addChild(createIndexSubmenuItem("Output level",
            {"5 Vpp", "10 Vpp", "14 Vpp"},
            [=]() { return module->outputLevel; },
            [=](int idx) { module->outputLevel = idx; }));
    }
};

Model* modelMateriae = createModel<Materiae, MateriaeWidget>("materiae");
