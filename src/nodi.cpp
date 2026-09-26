// nodi.cpp - VCV Rack 2 module
// nodi (Latin: "knots", plural of nodus) is a continuous-time sequencer, a
// clone of New Systems Instruments' Discrete Map with its A / B / C Expander
// built in. The engine lives in src/nodi/nodi.hpp, is specified by
// doc/design/nodi.md, and is measured by test/nodi_probe.
//
// Eight thresholds turn the motion of X into the activation of one of eight
// stages: a RISE stage when X rises past its threshold, a FALL stage when X
// falls past it. The active stage's slider is the f(X) output. X is normalled
// to an internal ramp, which makes it a step sequencer whose lower sliders
// place the steps in time; any other signal at X makes it follow that signal.
//
// Controls:
//   Clock   : RATE, SLOW / FAST, FM attenuator, N (SYNC/N count), LOOP / ONCE
//   Event   : eight threshold sliders (lit while X is above them), eight
//             RISE / OFF / FALL switches, POS / LEN
//   Map     : eight f(X) sliders (lit on the active stage), RANGE, DUR (gate
//             length)
//   Groups  : eight A / B / C switches, THR A / B / C attenuators
//   In      : V/O, FM, SYNC, /N, X (poly, normalled to RAMP), HI, LO, EXT,
//             +Y, THR A / B / C, A / B / C
//   Out     : RAMP, EOC, f(X), GATE, GATE A / B / C, COM
//   Menu    : Anti-aliasing, Setups (the manual's quick starts), and presets
//             and transforms for each bank of sliders and switches, one undo
//             step each

#include "forsitan.hpp"
#include "position_switch.hpp"
#include "nodi/nodi.hpp"
#include "nodi/shapes.hpp"

#include <utility>
#include <vector>

namespace {

const float kLightFlash = 0.05f;          // seconds a fired threshold stays lit
const int kLightDivision = 32;            // samples between light updates

float knobForHz(float hz, bool fast) {
    float lo = fast ? nodi::kFastMin : nodi::kSlowMin, hi = fast ? nodi::kFastMax : nodi::kSlowMax;
    return std::log(hz / lo) / std::log(hi / lo);
}

// The panel's switch values against the engine's: the widget numbers its
// throws from the bottom, the engine names them.
int directionOf(float v) {
    int i = (int)std::round(v);
    return i >= 2 ? nodi::RISE : i == 1 ? nodi::OFF : nodi::FALL;
}
int groupOf(float v) { return 2 - clamp((int)std::round(v), 0, 2); }
float directionParam(int d) { return d == nodi::RISE ? 2.f : d == nodi::OFF ? 1.f : 0.f; }
float groupParam(int g) { return (float)(2 - g); }
// A non-finite voltage reads as zero: a NaN at X would otherwise reach the
// event sort, and one at HI or LO every threshold.
float finite(float v) { return std::isfinite(v) ? v : 0.f; }

int rangeOf(float v) {
    int i = (int)std::round(v);
    return i >= 2 ? nodi::RANGE_BIPOLAR : i == 1 ? nodi::RANGE_FIVE : nodi::RANGE_HALF;
}

}  // namespace

struct Nodi : Module {
    enum ParamId {
        RATE_PARAM, FAST_PARAM, FM_PARAM, N_PARAM, LOOP_PARAM,
        MODE_PARAM, RANGE_PARAM, DUR_PARAM,
        VALUE1_PARAM, VALUE2_PARAM, VALUE3_PARAM, VALUE4_PARAM,
        VALUE5_PARAM, VALUE6_PARAM, VALUE7_PARAM, VALUE8_PARAM,
        THRESH1_PARAM, THRESH2_PARAM, THRESH3_PARAM, THRESH4_PARAM,
        THRESH5_PARAM, THRESH6_PARAM, THRESH7_PARAM, THRESH8_PARAM,
        DIR1_PARAM, DIR2_PARAM, DIR3_PARAM, DIR4_PARAM,
        DIR5_PARAM, DIR6_PARAM, DIR7_PARAM, DIR8_PARAM,
        GROUP1_PARAM, GROUP2_PARAM, GROUP3_PARAM, GROUP4_PARAM,
        GROUP5_PARAM, GROUP6_PARAM, GROUP7_PARAM, GROUP8_PARAM,
        THR_A_PARAM, THR_B_PARAM, THR_C_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        VOCT_INPUT, FM_INPUT, SYNC_INPUT, SYNCN_INPUT,
        X_INPUT, HI_INPUT, LO_INPUT, EXT_INPUT, Y_INPUT,
        THR_A_INPUT, THR_B_INPUT, THR_C_INPUT,
        SW_A_INPUT, SW_B_INPUT, SW_C_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        RAMP_OUTPUT, EOC_OUTPUT, FX_OUTPUT, GATE_OUTPUT,
        GATE_A_OUTPUT, GATE_B_OUTPUT, GATE_C_OUTPUT, COM_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        VALUE1_LIGHT, VALUE2_LIGHT, VALUE3_LIGHT, VALUE4_LIGHT,
        VALUE5_LIGHT, VALUE6_LIGHT, VALUE7_LIGHT, VALUE8_LIGHT,
        THRESH1_LIGHT, THRESH2_LIGHT, THRESH3_LIGHT, THRESH4_LIGHT,
        THRESH5_LIGHT, THRESH6_LIGHT, THRESH7_LIGHT, THRESH8_LIGHT,
        LIGHTS_LEN
    };

    nodi::Engine engine;
    nodi::Controls ctl;
    nodi::ChannelIn in[nodi::kMaxChannels];
    nodi::ChannelOut out[nodi::kMaxChannels];
    int antiAlias = nodi::AA_AUTO;

    dsp::ClockDivider lightDivider;
    float flash[nodi::kStages] = {};
    bool firedSince[nodi::kStages] = {};

    // RATE in Hz or as a period, whichever reads better, in the current range.
    struct RateQuantity : ParamQuantity {
        bool fast() const {
            return module && module->params[FAST_PARAM].getValue() > 0.5f;
        }
        std::string getDisplayValueString() override {
            float hz = nodi::rateHz(getValue(), fast());
            if (hz < 1.f) return string::f("%.3g s", 1.f / hz);
            return string::f("%.4g Hz", hz);
        }
        void setDisplayValueString(std::string s) override {
            float v = std::atof(s.c_str());
            if (v <= 0.f) return;
            bool seconds = s.find('s') != std::string::npos && s.find("Hz") == std::string::npos;
            float hz = seconds ? 1.f / v : v;
            setValue(clamp(knobForHz(hz, fast()), 0.f, 1.f));
        }
    };

    struct DurQuantity : ParamQuantity {
        std::string getDisplayValueString() override {
            float s = nodi::gateSeconds(getValue());
            if (s < 0.001f) return string::f("%.3g us", s * 1e6f);
            if (s < 1.f) return string::f("%.3g ms", s * 1e3f);
            return string::f("%.3g s", s);
        }
    };

    // An f(X) slider in volts, in the current RANGE.
    struct ValueQuantity : ParamQuantity {
        int range() const {
            return module ? rangeOf(module->params[RANGE_PARAM].getValue()) : nodi::RANGE_HALF;
        }
        std::string getDisplayValueString() override {
            return string::f("%.3f V", nodi::sliderVolts(getValue(), range()));
        }
        void setDisplayValueString(std::string s) override {
            setValue(nodi::voltsSlider(std::atof(s.c_str()), range()));
        }
    };

    // A threshold slider: a relative length in LENGTH, a voltage in POSIT.
    // (over the unpatched -5..+5 V space).
    struct ThreshQuantity : ParamQuantity {
        bool posit() const {
            return module && module->params[MODE_PARAM].getValue() > 0.5f;
        }
        std::string getDisplayValueString() override {
            if (posit()) return string::f("%.3f V", nodi::kLow + (nodi::kHigh - nodi::kLow) * getValue());
            return string::f("%.1f%% length", getValue() * 100.f);
        }
        void setDisplayValueString(std::string s) override {
            float v = std::atof(s.c_str());
            if (posit()) setValue(clamp((v - nodi::kLow) / (nodi::kHigh - nodi::kLow), 0.f, 1.f));
            else setValue(clamp(v / 100.f, 0.f, 1.f));
        }
    };

    Nodi() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam<RateQuantity>(RATE_PARAM, 0.f, 1.f, 0.5333f, "Rate");  // 0.25 Hz: two steps a second
        configSwitch(FAST_PARAM, 0.f, 1.f, 0.f, "Range",
                     {"Slow: 4 min to 9 Hz", "Fast: 4 Hz to 12 kHz"});
        configParam(FM_PARAM, 0.f, 1.f, 0.f, "FM amount", "%", 0.f, 100.f);
        configParam(N_PARAM, 1.f, 16.f, 1.f, "Sync count N");
        getParamQuantity(N_PARAM)->snapEnabled = true;
        configSwitch(LOOP_PARAM, 0.f, 1.f, 1.f, "Cycle", {"Once", "Loop"});
        configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Threshold sliders",
                     {"Length: relative lengths", "Posit.: positions"});
        configSwitch(RANGE_PARAM, 0.f, 2.f, 0.f, "f(X) range",
                     {"0 to 2.5 V", "0 to 5 V", "-5 to +5 V"});
        configParam<DurQuantity>(DUR_PARAM, 0.f, 1.f, 0.6f, "Gate length");
        // One literal call a control, which is what tools/audition/modspec.py
        // reads the ranges, defaults and switch labels from.
        configParam<ValueQuantity>(VALUE1_PARAM, 0.f, 1.f, 0.f, "Stage 1 f(X)");
        configParam<ValueQuantity>(VALUE2_PARAM, 0.f, 1.f, 0.f, "Stage 2 f(X)");
        configParam<ValueQuantity>(VALUE3_PARAM, 0.f, 1.f, 0.f, "Stage 3 f(X)");
        configParam<ValueQuantity>(VALUE4_PARAM, 0.f, 1.f, 0.f, "Stage 4 f(X)");
        configParam<ValueQuantity>(VALUE5_PARAM, 0.f, 1.f, 0.f, "Stage 5 f(X)");
        configParam<ValueQuantity>(VALUE6_PARAM, 0.f, 1.f, 0.f, "Stage 6 f(X)");
        configParam<ValueQuantity>(VALUE7_PARAM, 0.f, 1.f, 0.f, "Stage 7 f(X)");
        configParam<ValueQuantity>(VALUE8_PARAM, 0.f, 1.f, 0.f, "Stage 8 f(X)");
        configParam<ThreshQuantity>(THRESH1_PARAM, 0.f, 1.f, 0.5f, "Stage 1 threshold");
        configParam<ThreshQuantity>(THRESH2_PARAM, 0.f, 1.f, 0.5f, "Stage 2 threshold");
        configParam<ThreshQuantity>(THRESH3_PARAM, 0.f, 1.f, 0.5f, "Stage 3 threshold");
        configParam<ThreshQuantity>(THRESH4_PARAM, 0.f, 1.f, 0.5f, "Stage 4 threshold");
        configParam<ThreshQuantity>(THRESH5_PARAM, 0.f, 1.f, 0.5f, "Stage 5 threshold");
        configParam<ThreshQuantity>(THRESH6_PARAM, 0.f, 1.f, 0.5f, "Stage 6 threshold");
        configParam<ThreshQuantity>(THRESH7_PARAM, 0.f, 1.f, 0.5f, "Stage 7 threshold");
        configParam<ThreshQuantity>(THRESH8_PARAM, 0.f, 1.f, 0.5f, "Stage 8 threshold");
        configSwitch(DIR1_PARAM, 0.f, 2.f, 0.f, "Stage 1 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR2_PARAM, 0.f, 2.f, 2.f, "Stage 2 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR3_PARAM, 0.f, 2.f, 2.f, "Stage 3 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR4_PARAM, 0.f, 2.f, 2.f, "Stage 4 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR5_PARAM, 0.f, 2.f, 2.f, "Stage 5 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR6_PARAM, 0.f, 2.f, 2.f, "Stage 6 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR7_PARAM, 0.f, 2.f, 2.f, "Stage 7 direction", {"Fall", "Off", "Rise"});
        configSwitch(DIR8_PARAM, 0.f, 2.f, 2.f, "Stage 8 direction", {"Fall", "Off", "Rise"});
        configSwitch(GROUP1_PARAM, 0.f, 2.f, 2.f, "Stage 1 group", {"C", "B", "A"});
        configSwitch(GROUP2_PARAM, 0.f, 2.f, 1.f, "Stage 2 group", {"C", "B", "A"});
        configSwitch(GROUP3_PARAM, 0.f, 2.f, 0.f, "Stage 3 group", {"C", "B", "A"});
        configSwitch(GROUP4_PARAM, 0.f, 2.f, 2.f, "Stage 4 group", {"C", "B", "A"});
        configSwitch(GROUP5_PARAM, 0.f, 2.f, 1.f, "Stage 5 group", {"C", "B", "A"});
        configSwitch(GROUP6_PARAM, 0.f, 2.f, 0.f, "Stage 6 group", {"C", "B", "A"});
        configSwitch(GROUP7_PARAM, 0.f, 2.f, 2.f, "Stage 7 group", {"C", "B", "A"});
        configSwitch(GROUP8_PARAM, 0.f, 2.f, 1.f, "Stage 8 group", {"C", "B", "A"});
        configParam(THR_A_PARAM, 0.f, 1.f, 1.f, "Group A threshold CV amount", "%", 0.f, 100.f);
        configParam(THR_B_PARAM, 0.f, 1.f, 1.f, "Group B threshold CV amount", "%", 0.f, 100.f);
        configParam(THR_C_PARAM, 0.f, 1.f, 1.f, "Group C threshold CV amount", "%", 0.f, 100.f);

        configInput(VOCT_INPUT, "Rate V/oct");
        configInput(FM_INPUT, "Rate linear FM");
        configInput(SYNC_INPUT, "Sync (resets the ramp)");
        configInput(SYNCN_INPUT, "Sync after N (resets on the N-th edge)");
        configInput(X_INPUT, "X (normalled to the ramp)");
        configInput(HI_INPUT, "Above: top of the threshold space (+5 V)");
        configInput(LO_INPUT, "Below: bottom of the threshold space (-5 V)");
        configInput(EXT_INPUT, "Ext (cancels the stage)");
        configInput(Y_INPUT, "+Y (added to f(X))");
        configInput(THR_A_INPUT, "Group A threshold CV");
        configInput(THR_B_INPUT, "Group B threshold CV");
        configInput(THR_C_INPUT, "Group C threshold CV");
        configInput(SW_A_INPUT, "Switch A");
        configInput(SW_B_INPUT, "Switch B");
        configInput(SW_C_INPUT, "Switch C");
        configOutput(RAMP_OUTPUT, "Ramp");
        configOutput(EOC_OUTPUT, "End of cycle");
        configOutput(FX_OUTPUT, "f(X)");
        configOutput(GATE_OUTPUT, "Gate");
        configOutput(GATE_A_OUTPUT, "Group A gate");
        configOutput(GATE_B_OUTPUT, "Group B gate");
        configOutput(GATE_C_OUTPUT, "Group C gate");
        configOutput(COM_OUTPUT, "Switch common");
        for (int k = 0; k < nodi::kStages; k++) {
            configLight(VALUE1_LIGHT + k, string::f("Stage %d active", k + 1));
            configLight(THRESH1_LIGHT + k, string::f("X above stage %d", k + 1));
        }

        lightDivider.setDivision(kLightDivision);
        engine.setSampleRate(48000.f);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        engine.setSampleRate(e.sampleRate);
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        antiAlias = nodi::AA_AUTO;
        engine.reset();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "antiAlias", json_integer(antiAlias));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = json_object_get(root, "antiAlias");
        if (j) antiAlias = clamp((int)json_integer_value(j), 0, 2);
    }

    // Sets many parameters as one undo step.
    void setParams(const std::vector<std::pair<int, float>>& changes, const std::string& what) {
        history::ComplexAction* h = new history::ComplexAction;
        h->name = "nodi: " + what;
        for (const auto& c : changes) {
            float old = params[c.first].getValue();
            params[c.first].setValue(c.second);
            if (old == c.second) continue;
            history::ParamChange* pc = new history::ParamChange;
            pc->moduleId = id;
            pc->paramId = c.first;
            pc->oldValue = old;
            pc->newValue = c.second;
            h->push(pc);
        }
        if (APP && APP->history && !h->isEmpty()) APP->history->push(h);
        else delete h;
    }

    int range() { return rangeOf(params[RANGE_PARAM].getValue()); }
    bool lengthMode() { return params[MODE_PARAM].getValue() < 0.5f; }

    void readBank(int first, float* v) {
        for (int k = 0; k < nodi::kStages; k++) v[k] = params[first + k].getValue();
    }
    void bankChanges(int first, const float* v, std::vector<std::pair<int, float>>& c) {
        for (int k = 0; k < nodi::kStages; k++)
            c.push_back(std::make_pair(first + k, clamp(v[k], 0.f, 1.f)));
    }
    void readDirections(int* d) {
        for (int k = 0; k < nodi::kStages; k++) d[k] = directionOf(params[DIR1_PARAM + k].getValue());
    }
    void readGroups(int* g) {
        for (int k = 0; k < nodi::kStages; k++) g[k] = groupOf(params[GROUP1_PARAM + k].getValue());
    }

    static float uniform() { return random::uniform(); }

    void applyValuePreset(int p) {
        float v[nodi::kStages];
        nodi::valuePreset(p, range(), v, uniform);
        std::vector<std::pair<int, float>> c;
        bankChanges(VALUE1_PARAM, v, c);
        setParams(c, std::string("f(X) ") + nodi::valuePresetName(p));
    }
    void applyValueTransform(int t) {
        float v[nodi::kStages];
        readBank(VALUE1_PARAM, v);
        nodi::valueTransform(t, range(), v, uniform);
        std::vector<std::pair<int, float>> c;
        bankChanges(VALUE1_PARAM, v, c);
        setParams(c, std::string("f(X) ") + nodi::valueTransformName(t));
    }
    void applyThresholdPreset(int p) {
        float v[nodi::kStages];
        nodi::thresholdPreset(p, lengthMode(), v, uniform);
        std::vector<std::pair<int, float>> c;
        bankChanges(THRESH1_PARAM, v, c);
        setParams(c, std::string("thresholds ") + nodi::thresholdPresetName(p));
    }
    void applyThresholdTransform(int t) {
        float v[nodi::kStages];
        readBank(THRESH1_PARAM, v);
        nodi::thresholdTransform(t, v, uniform);
        std::vector<std::pair<int, float>> c;
        bankChanges(THRESH1_PARAM, v, c);
        setParams(c, std::string("thresholds ") + nodi::thresholdTransformName(t));
    }
    // Flips POS / LEN and rewrites the threshold sliders so every threshold
    // stays where it was (see positionsToLengths for when that is exact).
    void convertMode() {
        float v[nodi::kStages], w[nodi::kStages];
        readBank(THRESH1_PARAM, v);
        bool toPosit = lengthMode();
        if (toPosit) nodi::lengthsToPositions(v, w);
        else nodi::positionsToLengths(v, w);
        std::vector<std::pair<int, float>> c;
        bankChanges(THRESH1_PARAM, w, c);
        c.push_back(std::make_pair((int)MODE_PARAM, toPosit ? 1.f : 0.f));
        setParams(c, toPosit ? "convert to positions" : "convert to lengths");
    }
    void applyDirections(const int* d, const std::string& what) {
        std::vector<std::pair<int, float>> c;
        for (int k = 0; k < nodi::kStages; k++)
            c.push_back(std::make_pair(DIR1_PARAM + k, directionParam(d[k])));
        setParams(c, "directions " + what);
    }
    void applyGroups(const int* g, const std::string& what) {
        std::vector<std::pair<int, float>> c;
        for (int k = 0; k < nodi::kStages; k++)
            c.push_back(std::make_pair(GROUP1_PARAM + k, groupParam(g[k])));
        setParams(c, "groups " + what);
    }
    void applySetup(int id) {
        nodi::Setup s;
        nodi::setup(id, s);
        std::vector<std::pair<int, float>> c;
        c.push_back(std::make_pair((int)FAST_PARAM, s.fast ? 1.f : 0.f));
        c.push_back(std::make_pair((int)RATE_PARAM, clamp(knobForHz(s.hz, s.fast), 0.f, 1.f)));
        c.push_back(std::make_pair((int)LOOP_PARAM, s.once ? 0.f : 1.f));
        c.push_back(std::make_pair((int)MODE_PARAM, s.length ? 0.f : 1.f));
        c.push_back(std::make_pair((int)RANGE_PARAM,
                                   s.range == nodi::RANGE_BIPOLAR ? 2.f : s.range == nodi::RANGE_FIVE ? 1.f : 0.f));
        bankChanges(VALUE1_PARAM, s.value, c);
        bankChanges(THRESH1_PARAM, s.threshold, c);
        for (int k = 0; k < nodi::kStages; k++) {
            c.push_back(std::make_pair(DIR1_PARAM + k, directionParam(s.direction[k])));
            c.push_back(std::make_pair(GROUP1_PARAM + k, groupParam(s.group[k])));
        }
        setParams(c, std::string("setup ") + nodi::setupName(id));
    }

    void readControls() {
        ctl.rate = params[RATE_PARAM].getValue();
        ctl.fast = params[FAST_PARAM].getValue() > 0.5f;
        ctl.once = params[LOOP_PARAM].getValue() < 0.5f;
        ctl.voct = finite(inputs[VOCT_INPUT].getVoltage());
        ctl.fmAmount = params[FM_PARAM].getValue();
        ctl.fmCv = finite(inputs[FM_INPUT].getVoltage());
        ctl.syncN = (int)std::round(params[N_PARAM].getValue());
        ctl.length = params[MODE_PARAM].getValue() < 0.5f;
        ctl.range = rangeOf(params[RANGE_PARAM].getValue());
        ctl.gateLength = params[DUR_PARAM].getValue();
        ctl.antiAlias = antiAlias;
        for (int k = 0; k < nodi::kStages; k++) {
            ctl.value[k] = params[VALUE1_PARAM + k].getValue();
            ctl.threshold[k] = params[THRESH1_PARAM + k].getValue();
            ctl.direction[k] = directionOf(params[DIR1_PARAM + k].getValue());
            ctl.group[k] = groupOf(params[GROUP1_PARAM + k].getValue());
        }
        for (int g = 0; g < nodi::kGroups; g++) ctl.groupAmount[g] = params[THR_A_PARAM + g].getValue();
    }

    void process(const ProcessArgs& args) override {
        readControls();

        const bool internal = !inputs[X_INPUT].isConnected();
        const int channels = std::max(1, std::max(inputs[X_INPUT].getChannels(),
                                                  inputs[Y_INPUT].getChannels()));
        const bool hi = inputs[HI_INPUT].isConnected(), lo = inputs[LO_INPUT].isConnected();
        for (int c = 0; c < channels; c++) {
            nodi::ChannelIn& i = in[c];
            i.x = finite(inputs[X_INPUT].getPolyVoltage(c));
            i.above = hi ? finite(inputs[HI_INPUT].getPolyVoltage(c)) : nodi::kHigh;
            i.below = lo ? finite(inputs[LO_INPUT].getPolyVoltage(c)) : nodi::kLow;
            i.ext = finite(inputs[EXT_INPUT].getPolyVoltage(c));
            i.y = finite(inputs[Y_INPUT].getPolyVoltage(c));
            for (int g = 0; g < nodi::kGroups; g++) {
                i.groupCv[g] = finite(inputs[THR_A_INPUT + g].getPolyVoltage(c));
                i.sw[g] = finite(inputs[SW_A_INPUT + g].getPolyVoltage(c));
            }
        }

        engine.process(ctl, finite(inputs[SYNC_INPUT].getVoltage()),
                       finite(inputs[SYNCN_INPUT].getVoltage()),
                       internal, channels, in, out);

        outputs[RAMP_OUTPUT].setVoltage(engine.ramp);
        outputs[EOC_OUTPUT].setVoltage(engine.eoc ? 10.f : 0.f);
        for (int o = FX_OUTPUT; o <= COM_OUTPUT; o++) outputs[o].setChannels(channels);
        for (int c = 0; c < channels; c++) {
            const nodi::ChannelOut& o = out[c];
            outputs[FX_OUTPUT].setVoltage(o.fx, c);
            outputs[GATE_OUTPUT].setVoltage(o.gate ? 10.f : 0.f, c);
            for (int g = 0; g < nodi::kGroups; g++)
                outputs[GATE_A_OUTPUT + g].setVoltage(o.groupGate[g] ? 10.f : 0.f, c);
            outputs[COM_OUTPUT].setVoltage(o.com, c);
        }

        const nodi::Channel& ch = engine.ch[0];
        for (int k = 0; k < nodi::kStages; k++) firedSince[k] = firedSince[k] || ch.fired[k];
        if (lightDivider.process()) {
            const float dt = args.sampleTime * kLightDivision;
            for (int k = 0; k < nodi::kStages; k++) {
                if (firedSince[k]) flash[k] = 1.f;
                else flash[k] = std::max(0.f, flash[k] - dt / kLightFlash);
                firedSince[k] = false;
                lights[VALUE1_LIGHT + k].setBrightness(ch.active == k ? 1.f : 0.f);
                float above = ch.x >= ch.th[k] ? 0.25f : 0.f;
                lights[THRESH1_LIGHT + k].setBrightness(std::max(above, flash[k]));
            }
        }
    }
};

struct NodiWidget : ModuleWidget {
    NodiWidget(Nodi* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/nodi.svg")));

        // @layout:begin nodi 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem VALUE1_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE1_LIGHT
// @elem LABEL_STAGE1 label 0.0 label "1" 0.0 31.21 40.00
// @elem THRESH1_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH1_LIGHT
// @elem DIR1_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP1_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE2_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE2_LIGHT
// @elem LABEL_STAGE2 label 0.0 label "2" 0.0 39.71 40.00
// @elem THRESH2_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH2_LIGHT
// @elem DIR2_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP2_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE3_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE3_LIGHT
// @elem LABEL_STAGE3 label 0.0 label "3" 0.0 48.21 40.00
// @elem THRESH3_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH3_LIGHT
// @elem DIR3_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP3_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE4_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE4_LIGHT
// @elem LABEL_STAGE4 label 0.0 label "4" 0.0 56.71 40.00
// @elem THRESH4_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH4_LIGHT
// @elem DIR4_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP4_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE5_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE5_LIGHT
// @elem LABEL_STAGE5 label 0.0 label "5" 0.0 65.21 40.00
// @elem THRESH5_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH5_LIGHT
// @elem DIR5_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP5_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE6_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE6_LIGHT
// @elem LABEL_STAGE6 label 0.0 label "6" 0.0 73.71 40.00
// @elem THRESH6_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH6_LIGHT
// @elem DIR6_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP6_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE7_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE7_LIGHT
// @elem LABEL_STAGE7 label 0.0 label "7" 0.0 82.21 40.00
// @elem THRESH7_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH7_LIGHT
// @elem DIR7_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP7_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem VALUE8_PARAM VCVLightSlider 12.96 param "" 0.0 light=VALUE8_LIGHT
// @elem LABEL_STAGE8 label 0.0 label "8" 0.0 90.71 40.00
// @elem THRESH8_PARAM VCVLightSlider 12.96 param "" 0.0 light=THRESH8_LIGHT
// @elem DIR8_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem GROUP8_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem LABEL_RISE label 0.0 label "rise" 0.0 21.76 72.80
// @elem LABEL_FALL label 0.0 label "fall" 0.0 21.76 79.40
// @elem LABEL_GA label 0.0 label "a" 0.0 25.13 85.80
// @elem LABEL_GC label 0.0 label "c" 0.0 25.13 92.40
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LABEL_RATE label 0.0 label "rate" 0.0 6.75 22.50
// @elem FAST_PARAM CKSS 2.3 param "" 0.0
// @elem LABEL_FAST label 0.0 label "fast" 0.0 19.25 7.30
// @elem LABEL_SLOW label 0.0 label "slow" 0.0 19.25 22.20
// @elem FM_PARAM Trimpot 3.03 param "" 0.0
// @elem FM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_FM label 0.0 label "fm" 0.0 19.25 36.50
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_VOCT label 0.0 label "v/o" 0.0 6.75 51.50
// @elem SYNC_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_SYNC label 0.0 label "sync" 0.0 19.25 51.50
// @elem N_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LABEL_N label 0.0 label "n" 0.0 6.75 67.50
// @elem SYNCN_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_SYNCN label 0.0 label "/n" 0.0 19.25 66.50
// @elem LOOP_PARAM CKSS 2.3 param "" 0.0
// @elem LABEL_LOOP label 0.0 label "loop" 0.0 6.75 76.30
// @elem LABEL_ONCE label 0.0 label "once" 0.0 6.75 91.20
// @elem BOX_EOC panel_box 7.0 box "" 0.0 6.75 107.50 box=11x14
// @elem EOC_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_EOC label 0.0 label "eoc" 0.0 6.75 113.00
// @elem BOX_RAMP panel_box 7.0 box "" 0.0 19.25 107.50 box=11x14
// @elem RAMP_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_RAMP label 0.0 label "ramp" 0.0 19.25 113.00
// @elem MODE_PARAM CKSS 2.3 param "" 0.0
// @elem LABEL_POS label 0.0 label "pos" 0.0 102.67 7.30
// @elem LABEL_LEN label 0.0 label "len" 0.0 102.67 22.20
// @elem X_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_X label 0.0 label "x" 0.0 102.67 36.50
// @elem EXT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_EXT label 0.0 label "ext" 0.0 115.17 36.50
// @elem HI_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_HI label 0.0 label "hi" 0.0 102.67 51.50
// @elem LO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_LO label 0.0 label "lo" 0.0 115.17 51.50
// @elem Y_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_Y label 0.0 label "+y" 0.0 102.67 66.50
// @elem RANGE_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem LABEL_RANGE label 0.0 label "range" 0.0 115.17 67.20
// @elem BOX_GATE_A panel_box 7.0 box "" 0.0 102.67 76.50 box=11x14
// @elem GATE_A_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_GATE_A label 0.0 label "a" 0.0 102.67 82.00
// @elem BOX_GATE_B panel_box 7.0 box "" 0.0 115.17 76.50 box=11x14
// @elem GATE_B_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_GATE_B label 0.0 label "b" 0.0 115.17 82.00
// @elem BOX_GATE_C panel_box 7.0 box "" 0.0 102.67 92.00 box=11x14
// @elem GATE_C_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_GATE_C label 0.0 label "c" 0.0 102.67 97.50
// @elem BOX_FX panel_box 7.0 box "" 0.0 102.67 107.50 box=11x14
// @elem FX_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_FX label 0.0 label "f(x)" 0.0 102.67 113.00
// @elem BOX_GATE panel_box 7.0 box "" 0.0 115.17 107.50 box=11x14
// @elem GATE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_GATE label 0.0 label "gate" 0.0 115.17 113.00
// @elem DUR_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LABEL_DUR label 0.0 label "dur" 0.0 115.17 98.50
// @elem THR_A_PARAM Trimpot 3.03 param "" 0.0
// @elem THR_A_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_THR_A label 0.0 label "thr a" 0.0 37.71 107.00
// @elem THR_B_PARAM Trimpot 3.03 param "" 0.0
// @elem THR_B_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_THR_B label 0.0 label "thr b" 0.0 60.96 107.00
// @elem THR_C_PARAM Trimpot 3.03 param "" 0.0
// @elem THR_C_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_THR_C label 0.0 label "thr c" 0.0 84.21 107.00
// @elem SW_A_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_SW_A label 0.0 label "a" 0.0 33.20 121.00
// @elem SW_B_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_SW_B label 0.0 label "b" 0.0 45.20 121.00
// @elem SW_C_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LABEL_SW_C label 0.0 label "c" 0.0 76.72 121.00
// @elem BOX_COM panel_box 7.0 box "" 0.0 88.72 115.00 box=11x13
// @elem COM_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_COM label 0.0 label "com" 0.0 88.72 121.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(31.21f, 23.50f)), module, Nodi::VALUE1_PARAM, Nodi::VALUE1_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(31.21f, 54.50f)), module, Nodi::THRESH1_PARAM, Nodi::THRESH1_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(31.21f, 75.00f)), module, Nodi::DIR1_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(31.21f, 88.00f)), module, Nodi::GROUP1_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(39.71f, 23.50f)), module, Nodi::VALUE2_PARAM, Nodi::VALUE2_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(39.71f, 54.50f)), module, Nodi::THRESH2_PARAM, Nodi::THRESH2_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(39.71f, 75.00f)), module, Nodi::DIR2_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(39.71f, 88.00f)), module, Nodi::GROUP2_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(48.21f, 23.50f)), module, Nodi::VALUE3_PARAM, Nodi::VALUE3_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(48.21f, 54.50f)), module, Nodi::THRESH3_PARAM, Nodi::THRESH3_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(48.21f, 75.00f)), module, Nodi::DIR3_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(48.21f, 88.00f)), module, Nodi::GROUP3_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(56.71f, 23.50f)), module, Nodi::VALUE4_PARAM, Nodi::VALUE4_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(56.71f, 54.50f)), module, Nodi::THRESH4_PARAM, Nodi::THRESH4_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(56.71f, 75.00f)), module, Nodi::DIR4_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(56.71f, 88.00f)), module, Nodi::GROUP4_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(65.21f, 23.50f)), module, Nodi::VALUE5_PARAM, Nodi::VALUE5_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(65.21f, 54.50f)), module, Nodi::THRESH5_PARAM, Nodi::THRESH5_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(65.21f, 75.00f)), module, Nodi::DIR5_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(65.21f, 88.00f)), module, Nodi::GROUP5_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(73.71f, 23.50f)), module, Nodi::VALUE6_PARAM, Nodi::VALUE6_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(73.71f, 54.50f)), module, Nodi::THRESH6_PARAM, Nodi::THRESH6_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(73.71f, 75.00f)), module, Nodi::DIR6_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(73.71f, 88.00f)), module, Nodi::GROUP6_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(82.21f, 23.50f)), module, Nodi::VALUE7_PARAM, Nodi::VALUE7_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(82.21f, 54.50f)), module, Nodi::THRESH7_PARAM, Nodi::THRESH7_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(82.21f, 75.00f)), module, Nodi::DIR7_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(82.21f, 88.00f)), module, Nodi::GROUP7_PARAM));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(90.71f, 23.50f)), module, Nodi::VALUE8_PARAM, Nodi::VALUE8_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(90.71f, 54.50f)), module, Nodi::THRESH8_PARAM, Nodi::THRESH8_LIGHT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(90.71f, 75.00f)), module, Nodi::DIR8_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(90.71f, 88.00f)), module, Nodi::GROUP8_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(6.75f, 14.00f)), module, Nodi::RATE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(19.25f, 14.00f)), module, Nodi::FAST_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(6.75f, 29.00f)), module, Nodi::FM_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.25f, 29.00f)), module, Nodi::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.75f, 44.00f)), module, Nodi::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.25f, 44.00f)), module, Nodi::SYNC_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(6.75f, 59.00f)), module, Nodi::N_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.25f, 59.00f)), module, Nodi::SYNCN_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(6.75f, 83.00f)), module, Nodi::LOOP_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.75f, 105.50f)), module, Nodi::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(19.25f, 105.50f)), module, Nodi::RAMP_OUTPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(102.67f, 14.00f)), module, Nodi::MODE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.67f, 29.00f)), module, Nodi::X_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(115.17f, 29.00f)), module, Nodi::EXT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.67f, 44.00f)), module, Nodi::HI_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(115.17f, 44.00f)), module, Nodi::LO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.67f, 59.00f)), module, Nodi::Y_INPUT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(115.17f, 59.00f)), module, Nodi::RANGE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(102.67f, 74.50f)), module, Nodi::GATE_A_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(115.17f, 74.50f)), module, Nodi::GATE_B_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(102.67f, 90.00f)), module, Nodi::GATE_C_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(102.67f, 105.50f)), module, Nodi::FX_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(115.17f, 105.50f)), module, Nodi::GATE_OUTPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(115.17f, 90.00f)), module, Nodi::DUR_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.21f, 99.50f)), module, Nodi::THR_A_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.21f, 99.50f)), module, Nodi::THR_A_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(56.46f, 99.50f)), module, Nodi::THR_B_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.46f, 99.50f)), module, Nodi::THR_B_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(79.71f, 99.50f)), module, Nodi::THR_C_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.71f, 99.50f)), module, Nodi::THR_C_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.20f, 113.50f)), module, Nodi::SW_A_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.20f, 113.50f)), module, Nodi::SW_B_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(76.72f, 113.50f)), module, Nodi::SW_C_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(88.72f, 113.50f)), module, Nodi::COM_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Nodi* m = getModule<Nodi>();
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Anti-aliasing",
            {"Auto: steps closer than 2 ms", "Off", "On"}, &m->antiAlias));
        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Setups", "", [=](Menu* sub) {
            for (int i = 0; i < nodi::NUM_SETUPS; i++)
                sub->addChild(createMenuItem(nodi::setupName(i), "", [=]() { m->applySetup(i); }));
        }));
        menu->addChild(createSubmenuItem("f(X) sliders", "", [=](Menu* sub) {
            sub->addChild(createMenuLabel("Presets, in the current range"));
            for (int p = 0; p < nodi::NUM_VALUE_PRESETS; p++) {
                if (nodi::valuePresetStartsGroup(p)) sub->addChild(new MenuSeparator);
                sub->addChild(createMenuItem(nodi::valuePresetName(p), "",
                                             [=]() { m->applyValuePreset(p); }));
            }
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuLabel("Transforms"));
            for (int t = 0; t < nodi::NUM_VALUE_TRANSFORMS; t++) {
                if (nodi::valueTransformStartsGroup(t)) sub->addChild(new MenuSeparator);
                sub->addChild(createMenuItem(nodi::valueTransformName(t), "",
                                             [=]() { m->applyValueTransform(t); }));
            }
        }));
        menu->addChild(createSubmenuItem("Threshold sliders", "", [=](Menu* sub) {
            sub->addChild(createMenuLabel(m->lengthMode() ? "Rhythms, as lengths" : "Rhythms, as positions"));
            for (int p = 0; p < nodi::NUM_THRESHOLD_PRESETS; p++)
                sub->addChild(createMenuItem(nodi::thresholdPresetName(p), "",
                                             [=]() { m->applyThresholdPreset(p); }));
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuLabel("Transforms"));
            for (int t = 0; t < nodi::NUM_THRESHOLD_TRANSFORMS; t++)
                sub->addChild(createMenuItem(nodi::thresholdTransformName(t), "",
                                             [=]() { m->applyThresholdTransform(t); }));
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuItem(
                m->lengthMode() ? "Convert to positions, thresholds kept" : "Convert to lengths, thresholds kept",
                "", [=]() { m->convertMode(); }));
        }));
        menu->addChild(createSubmenuItem("Direction switches", "", [=](Menu* sub) {
            for (int p = 0; p < nodi::NUM_DIRECTION_PRESETS; p++)
                sub->addChild(createMenuItem(nodi::directionPresetName(p), "", [=]() {
                    int d[nodi::kStages];
                    nodi::directionPreset(p, d);
                    m->applyDirections(d, nodi::directionPresetName(p));
                }));
            sub->addChild(new MenuSeparator);
            for (int t = 0; t < nodi::NUM_SWITCH_TRANSFORMS; t++)
                sub->addChild(createMenuItem(nodi::directionTransformName(t), "", [=]() {
                    int d[nodi::kStages];
                    m->readDirections(d);
                    nodi::directionTransform(t, d);
                    m->applyDirections(d, nodi::directionTransformName(t));
                }));
        }));
        menu->addChild(createSubmenuItem("Group switches", "", [=](Menu* sub) {
            for (int p = 0; p < nodi::NUM_GROUP_PRESETS; p++)
                sub->addChild(createMenuItem(nodi::groupPresetName(p), "", [=]() {
                    int g[nodi::kStages];
                    nodi::groupPreset(p, g, Nodi::uniform);
                    m->applyGroups(g, nodi::groupPresetName(p));
                }));
            sub->addChild(new MenuSeparator);
            for (int t = 0; t < nodi::NUM_SWITCH_TRANSFORMS; t++)
                sub->addChild(createMenuItem(nodi::groupTransformName(t), "", [=]() {
                    int g[nodi::kStages];
                    m->readGroups(g);
                    nodi::groupTransform(t, g);
                    m->applyGroups(g, nodi::groupTransformName(t));
                }));
        }));
    }
};

Model* modelNodi = createModel<Nodi, NodiWidget>("nodi");
