// radix.cpp - VCV Rack 2 module
// radix (Latin: "root") is a chaotic 8-bit source: an integer machine on its
// own clock, whose program is three stepped knobs with CV. It is in the family
// of Dirty Electronics' Radical22 and is built from doc/design/radix.md, not
// from that module's firmware; the engine lives in src/radix/radix.hpp and is
// measured by test/radix_probe.
//
// SRC picks where the modulator byte comes from, LAW how it turns into the
// accumulator's increment, TABLE how the accumulator is read: 5 x 5 x 6 = 150
// programs. Each knob's CV adds 1 V per step, rounded, so a slow ramp walks
// the positions in order and a sequencer picks them.
//
// Nothing on the output is band-limited: the engine clock reaches the host
// through a zero-order hold and aliases, which is the sound. The one thing
// taken off is DC. Under SELF the phase can stall in one part of the table
// and sit there, and a module that parks a -5 V offset on its output is a
// module that thumps whatever it feeds.
//
// Controls:
//   Knobs : RATE, PARAM, CLOCK, BITS, GRIT, SRC, LAW, TABLE
//   In    : V/OCT, PARAM CV, CLOCK CV, SRC CV, LAW CV, TABLE CV, IN
//   Out   : OUT (+-5 V), CV OUT (0-10 V, the rungler on the accumulator)
//   Menu  : the TEXT table's string, "Clock moves pitch"

#include "forsitan.hpp"
#include "radix/radix.hpp"

#include <atomic>

namespace {

// CLOCK knob: 0..1 onto kClockMin..kClockMax, exponential
const float kClockSpan = radix::kClockMax / radix::kClockMin;   // 960

float clockKnobFor(float hz) {
    return std::log(hz / radix::kClockMin) / std::log(kClockSpan);
}

}  // namespace

struct Radix : Module {
    enum ParamId {
        RATE_PARAM,
        PARAM_PARAM,
        CLOCK_PARAM,
        BITS_PARAM,
        GRIT_PARAM,
        SRC_PARAM,
        LAW_PARAM,
        TABLE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        VOCT_INPUT,
        PARAM_CV_INPUT,
        CLOCK_CV_INPUT,
        SRC_CV_INPUT,
        LAW_CV_INPUT,
        TABLE_CV_INPUT,
        AUDIO_INPUT,
        INPUTS_LEN
    };
    enum OutputId { AUDIO_OUTPUT, CV_OUTPUT, OUTPUTS_LEN };
    enum LightId { LIGHTS_LEN };

    radix::Engine engine;
    radix::Params p;
    float dcx = 0.f, dcy = 0.f;
    float dcCoef = 0.f;

    // The TEXT string is edited on the UI thread and read on the audio one:
    // the table is built into `staging` there and copied across here.
    std::string text = "forsitan radix";
    radix::Tables staging;
    std::atomic<bool> textPending{false};

    Radix() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(RATE_PARAM, -4.f, 4.f, 0.f, "Rate", " Hz", 2.f, dsp::FREQ_C4);
        configParam(PARAM_PARAM, 0.f, 255.f, 128.f, "Param");
        configParam(CLOCK_PARAM, 0.f, 1.f, clockKnobFor(radix::kRefClock),
                    "Clock", " Hz", kClockSpan, radix::kClockMin);
        configParam(BITS_PARAM, 1.f, 16.f, 16.f, "Bits");
        paramQuantities[BITS_PARAM]->snapEnabled = true;
        configParam(GRIT_PARAM, 0.f, 1.f, 0.f, "Grit", "%", 0.f, 100.f);
        configSwitch(SRC_PARAM, 0.f, radix::NUM_SRC - 1, 0.f, "Source",
                     {"Param", "Table walk", "Self", "Counters", "Input"});
        configSwitch(LAW_PARAM, 0.f, radix::NUM_LAW - 1, 0.f, "Law",
                     {"Add", "Multiply", "Shift", "XOR", "Sync"});
        configSwitch(TABLE_PARAM, 0.f, radix::NUM_TABLE - 1, 0.f, "Table",
                     {"Sine", "Saw", "Pulse", "Noise", "Bits", "Text"});
        configInput(VOCT_INPUT, "1V/oct rate");
        configInput(PARAM_CV_INPUT, "Param CV (1 V = 25.5)");
        configInput(CLOCK_CV_INPUT, "Clock CV (1V/oct)");
        configInput(SRC_CV_INPUT, "Source CV (1 V per step)");
        configInput(LAW_CV_INPUT, "Law CV (1 V per step)");
        configInput(TABLE_CV_INPUT, "Table CV (1 V per step)");
        configInput(AUDIO_INPUT, "Audio / feedback");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(CV_OUTPUT, "Chaos CV");
        onSampleRateChange();
    }

    void onReset() override {
        engine.reset();
        dcx = dcy = 0.f;
        p.clockMovesPitch = false;
        setText("forsitan radix");
    }

    void onSampleRateChange() override {
        float sr = APP ? APP->engine->getSampleRate() : 48000.f;
        engine.setSampleRate(sr);
        // 2 Hz: below the bottom of RATE, so a 16 Hz pulse keeps its shape
        dcCoef = 1.f - 2.f * (float)M_PI * 2.f / sr;
    }

    void setText(const std::string& s) {
        text = s;
        staging.setText(s);
        textPending = true;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "text", json_string(text.c_str()));
        json_object_set_new(root, "clockMovesPitch", json_boolean(p.clockMovesPitch));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "text"))
            setText(json_string_value(j) ? json_string_value(j) : "");
        if (json_t* j = json_object_get(root, "clockMovesPitch"))
            p.clockMovesPitch = json_boolean_value(j);
    }

    int stepped(int param, int input, int n) {
        float v = params[param].getValue() + inputs[input].getVoltage();
        return clamp((int)std::lround(v), 0, n - 1);
    }

    void process(const ProcessArgs& args) override {
        if (textPending.exchange(false))
            std::copy(staging.text, staging.text + 256, engine.tables.text);

        float oct = params[RATE_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
        p.freq = dsp::FREQ_C4 * dsp::exp2_taylor5(clamp(oct, -8.f, 7.f));

        float clockOct = params[CLOCK_PARAM].getValue() * std::log2(kClockSpan)
                       + inputs[CLOCK_CV_INPUT].getVoltage();
        p.clock = radix::kClockMin * dsp::exp2_taylor5(clamp(clockOct, 0.f, std::log2(kClockSpan)));

        p.param = clamp(params[PARAM_PARAM].getValue()
                        + 25.5f * inputs[PARAM_CV_INPUT].getVoltage(), 0.f, 255.f);
        p.bits = (int)params[BITS_PARAM].getValue();
        p.grit = params[GRIT_PARAM].getValue();
        p.src = stepped(SRC_PARAM, SRC_CV_INPUT, radix::NUM_SRC);
        p.law = stepped(LAW_PARAM, LAW_CV_INPUT, radix::NUM_LAW);
        p.table = stepped(TABLE_PARAM, TABLE_CV_INPUT, radix::NUM_TABLE);
        // a NaN at IN would become a byte of whatever lrint makes of it
        float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
        p.in = std::isfinite(in) ? in : 0.f;

        float cv;
        float y = engine.process(p, cv);

        dcy = y - dcx + dcCoef * dcy;
        dcx = y;
        outputs[AUDIO_OUTPUT].setVoltage(clamp(5.f * dcy, -10.f, 10.f));
        outputs[CV_OUTPUT].setVoltage(10.f * cv);
        (void)args;
    }
};

struct RadixWidget : ModuleWidget {
    RadixWidget(Radix* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/radix.svg")));

// @layout:begin radix 60.96 128.5 notitle svg=tools/panels/gen_radix_panel.py
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PARAM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CLOCK_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GRIT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SRC_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LAW_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TABLE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BITS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PARAM_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLOCK_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SRC_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LAW_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TABLE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.00f, 22.00f)), module, Radix::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.69f, 19.09f)), module, Radix::PARAM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.39f, 16.18f)), module, Radix::CLOCK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.69f, 97.09f)), module, Radix::GRIT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.00f, 62.00f)), module, Radix::SRC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.69f, 59.09f)), module, Radix::LAW_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(50.39f, 56.18f)), module, Radix::TABLE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.00f, 100.00f)), module, Radix::BITS_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.29f, 32.76f)), module, Radix::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.98f, 29.85f)), module, Radix::PARAM_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(39.68f, 26.94f)), module, Radix::CLOCK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.29f, 110.76f)), module, Radix::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.29f, 72.76f)), module, Radix::SRC_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.98f, 69.85f)), module, Radix::LAW_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.68f, 66.94f)), module, Radix::TABLE_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.98f, 107.85f)), module, Radix::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(39.68f, 104.94f)), module, Radix::CV_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Radix* m = dynamic_cast<Radix*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Clock moves pitch", "",
                                             &m->p.clockMovesPitch));

        struct TextTableField : ui::TextField {
            Radix* module;
            void onSelectKey(const event::SelectKey& e) override {
                if (e.action == GLFW_PRESS &&
                    (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
                    module->setText(text);
                    getAncestorOfType<ui::MenuOverlay>()->requestDelete();
                    e.consume(this);
                }
                if (!e.getTarget())
                    ui::TextField::onSelectKey(e);
            }
        };
        menu->addChild(createMenuLabel("Text table (Enter to apply)"));
        auto* tf = new TextTableField;
        tf->module = m;
        tf->text = m->text;
        tf->box.size.x = 200.f;
        menu->addChild(tf);
    }
};

Model* modelRadix = createModel<Radix, RadixWidget>("radix");
