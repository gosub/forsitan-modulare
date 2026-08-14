#include "forsitan.hpp"

// gradus — eight steps into one CV output.
//
// Eight rows. Each row is a value knob, a two-way mode switch, and two
// triggers: one for the value as it stands and one for its negative, each
// with a button beside its input. A trigger applies its own row to the
// running output value, and the switch says how:
//
//   add  + trigger : value += knob      jump  + trigger : value = +knob
//   add  - trigger : value -= knob      jump  - trigger : value = -knob
//
// So one knob at 1V is four moves: up a volt, down a volt, straight to +1V,
// straight to -1V. Where cumuli ramps for as long as a gate is open, gradus
// moves only on an edge, and only by the amount dialled in, so the output
// walks a lattice of discrete values.
//
// What happens when several triggers land in the same sample is the whole
// design of the module:
//   - a jump beats every add in that sample
//   - among simultaneous jumps the last one in reading order wins: rows top
//     to bottom, and within a row the minus side after the plus side
//   - with no jump, every simultaneous add is applied, signs and all
//
// The output is clipped by the clip knob, the module's one global control:
// 0 to 10V, ±5V, ±10V, or nothing at all.

// The knob is a step size under the add switch and a target under jump, and
// the tooltip follows the switch next to it rather than leaving the reader
// to guess which one is on show.
struct GradusStepQuantity : ParamQuantity {
	int row = 0;
	int modeParam = 0;

	bool isTarget() {
		return module && (int) std::round(module->params[modeParam].getValue()) == 0;
	}
	std::string getLabel() override {
		return string::f("Row %d %s", row + 1, isTarget() ? "target" : "step");
	}
};


struct Gradus : Module {
	enum ParamIds {
		STEP1_PARAM, STEP2_PARAM, STEP3_PARAM, STEP4_PARAM,
		STEP5_PARAM, STEP6_PARAM, STEP7_PARAM, STEP8_PARAM,
		MODE1_PARAM, MODE2_PARAM, MODE3_PARAM, MODE4_PARAM,
		MODE5_PARAM, MODE6_PARAM, MODE7_PARAM, MODE8_PARAM,
		PLUS1_PARAM, PLUS2_PARAM, PLUS3_PARAM, PLUS4_PARAM,
		PLUS5_PARAM, PLUS6_PARAM, PLUS7_PARAM, PLUS8_PARAM,
		MINUS1_PARAM, MINUS2_PARAM, MINUS3_PARAM, MINUS4_PARAM,
		MINUS5_PARAM, MINUS6_PARAM, MINUS7_PARAM, MINUS8_PARAM,
		CLIP_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		PLUS1_INPUT, PLUS2_INPUT, PLUS3_INPUT, PLUS4_INPUT,
		PLUS5_INPUT, PLUS6_INPUT, PLUS7_INPUT, PLUS8_INPUT,
		MINUS1_INPUT, MINUS2_INPUT, MINUS3_INPUT, MINUS4_INPUT,
		MINUS5_INPUT, MINUS6_INPUT, MINUS7_INPUT, MINUS8_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		CV_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		LEVEL_LIGHT_GREEN,
		LEVEL_LIGHT_RED,
		NUM_LIGHTS
	};

	enum Rows { ROWS = 8 };
	// switch positions, numbered as the widget does: 0 is the bottom throw
	enum Mode { MODE_JUMP, MODE_ADD };
	// clip knob positions, narrowest reach first
	enum Clip { CLIP_UNI, CLIP_BI5, CLIP_BI10, CLIP_NONE };

	float value = 0.f;

	dsp::SchmittTrigger plusTrig[ROWS], minusTrig[ROWS];
	dsp::BooleanTrigger plusButton[ROWS], minusButton[ROWS];

	Gradus() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		for (int i = 0; i < ROWS; i++) {
			GradusStepQuantity* q = configParam<GradusStepQuantity>(
				STEP1_PARAM + i, 0.f, 10.f, 1.f, "", " V");
			q->row = i;
			q->modeParam = MODE1_PARAM + i;
			configSwitch(MODE1_PARAM + i, 0.f, 1.f, 1.f,
				string::f("Row %d mode", i + 1), {"jump", "add"});
			configButton(PLUS1_PARAM + i, string::f("Row %d plus", i + 1));
			configButton(MINUS1_PARAM + i, string::f("Row %d minus", i + 1));
			configInput(PLUS1_INPUT + i, string::f("Row %d plus trigger", i + 1));
			configInput(MINUS1_INPUT + i, string::f("Row %d minus trigger", i + 1));
		}
		configSwitch(CLIP_PARAM, 0.f, 3.f, (float) CLIP_BI10, "Clip",
			{"0 to 10 V", "±5 V", "±10 V", "no clip"});
		configOutput(CV_OUTPUT, "CV");
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		value = 0.f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "value", json_real(value));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "value")) {
			float v = (float) json_number_value(j);
			value = std::isfinite(v) ? v : 0.f;
		}
	}

	// not const: Param::getValue() is not
	float clipped(float v) {
		switch ((int) std::round(params[CLIP_PARAM].getValue())) {
			case CLIP_UNI:  return math::clamp(v, 0.f, 10.f);
			case CLIP_BI5:  return math::clamp(v, -5.f, 5.f);
			case CLIP_BI10: return math::clamp(v, -10.f, 10.f);
			default:        return v;
		}
	}

	void process(const ProcessArgs& args) override {
		// One pass over the rows collects everything that fired this sample:
		// the sum of the relative moves, and the last jump seen. Scanning in
		// reading order and keeping the last is what gives the bottom row
		// precedence, and a row's minus side precedence over its plus.
		bool jumped = false;
		float jumpTarget = 0.f;
		float delta = 0.f;

		for (int i = 0; i < ROWS; i++) {
			// Both sides are polled every sample whatever the other one did:
			// an edge detector only sees the rise it was there for the fall of.
			bool plus = plusTrig[i].process(inputs[PLUS1_INPUT + i].getVoltage(), 0.1f, 1.f);
			plus |= plusButton[i].process(params[PLUS1_PARAM + i].getValue() > 0.5f);
			bool minus = minusTrig[i].process(inputs[MINUS1_INPUT + i].getVoltage(), 0.1f, 1.f);
			minus |= minusButton[i].process(params[MINUS1_PARAM + i].getValue() > 0.5f);
			if (!plus && !minus)
				continue;

			float knob = params[STEP1_PARAM + i].getValue();
			bool jump = (int) std::round(params[MODE1_PARAM + i].getValue()) == MODE_JUMP;
			if (plus) {
				if (jump) { jumped = true; jumpTarget = knob; }
				else delta += knob;
			}
			if (minus) {
				if (jump) { jumped = true; jumpTarget = -knob; }
				else delta -= knob;
			}
		}

		// A jump discards the relative moves of the same sample rather than
		// landing next to them: the row names an absolute value, and that is
		// where the output goes.
		if (jumped)
			value = jumpTarget;
		else
			value += delta;

		value = clipped(value);
		// Unclipped, a long enough run of adds is the one way out of range.
		// This keeps a patch from ever holding an infinity.
		if (!std::isfinite(value))
			value = 0.f;

		outputs[CV_OUTPUT].setVoltage(value);
		float ref = ((int) std::round(params[CLIP_PARAM].getValue()) == CLIP_BI5) ? 5.f : 10.f;
		lights[LEVEL_LIGHT_GREEN].setBrightness(std::max(0.f, value) / ref);
		lights[LEVEL_LIGHT_RED].setBrightness(std::max(0.f, -value) / ref);
	}
};


struct GradusWidget : ModuleWidget {
	GradusWidget(Gradus* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/gradus.svg")));

// @layout:begin gradus 60.96 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem STEP1_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP3_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP4_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP5_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP6_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP7_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEP8_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MODE1_PARAM CKSS 2.3 param "" 0.0
// @elem MODE2_PARAM CKSS 2.3 param "" 0.0
// @elem MODE3_PARAM CKSS 2.3 param "" 0.0
// @elem MODE4_PARAM CKSS 2.3 param "" 0.0
// @elem MODE5_PARAM CKSS 2.3 param "" 0.0
// @elem MODE6_PARAM CKSS 2.3 param "" 0.0
// @elem MODE7_PARAM CKSS 2.3 param "" 0.0
// @elem MODE8_PARAM CKSS 2.3 param "" 0.0
// @elem PLUS1_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS2_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS3_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS4_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS5_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS6_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS7_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS8_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS1_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS2_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS3_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS4_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS5_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS6_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS7_PARAM TL1105 2.6 param "" 0.0
// @elem MINUS8_PARAM TL1105 2.6 param "" 0.0
// @elem PLUS1_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS2_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS3_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS4_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS5_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS6_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS7_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PLUS8_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS1_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS2_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS3_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS4_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS5_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS6_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS7_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MINUS8_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLIP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEVEL_LIGHT_GREEN SmallLight 1.0 light "" 0.0
// @elem LABEL_PLUS label 0.0 label "+" 0.0 31.25 11.50
// @elem LABEL_MINUS label 0.0 label "-" 0.0 50.75 11.50
// @elem LABEL_ROW1 label 0.0 label "1" 0.0 4.00 17.50
// @elem LABEL_ROW2 label 0.0 label "2" 0.0 4.00 29.00
// @elem LABEL_ROW3 label 0.0 label "3" 0.0 4.00 40.50
// @elem LABEL_ROW4 label 0.0 label "4" 0.0 4.00 52.00
// @elem LABEL_ROW5 label 0.0 label "5" 0.0 4.00 63.50
// @elem LABEL_ROW6 label 0.0 label "6" 0.0 4.00 75.00
// @elem LABEL_ROW7 label 0.0 label "7" 0.0 4.00 86.50
// @elem LABEL_ROW8 label 0.0 label "8" 0.0 4.00 98.00
// @elem LABEL_CLIP label 0.0 label "clip" 0.0 11.50 117.50
// @elem BOX_CV panel_box 7.0 box "" 0.0 30.48 111.00
// @elem LABEL_CV label 0.0 label "out" 0.0 30.48 116.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 122.50

		addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
		addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
		addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
		addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 16.50f)), module, Gradus::STEP1_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 28.00f)), module, Gradus::STEP2_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 39.50f)), module, Gradus::STEP3_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 51.00f)), module, Gradus::STEP4_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 62.50f)), module, Gradus::STEP5_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 74.00f)), module, Gradus::STEP6_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 85.50f)), module, Gradus::STEP7_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 97.00f)), module, Gradus::STEP8_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 16.50f)), module, Gradus::MODE1_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 28.00f)), module, Gradus::MODE2_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 39.50f)), module, Gradus::MODE3_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 51.00f)), module, Gradus::MODE4_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 62.50f)), module, Gradus::MODE5_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 74.00f)), module, Gradus::MODE6_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 85.50f)), module, Gradus::MODE7_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(20.50f, 97.00f)), module, Gradus::MODE8_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 16.50f)), module, Gradus::PLUS1_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 28.00f)), module, Gradus::PLUS2_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 39.50f)), module, Gradus::PLUS3_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 51.00f)), module, Gradus::PLUS4_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 62.50f)), module, Gradus::PLUS5_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 74.00f)), module, Gradus::PLUS6_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 85.50f)), module, Gradus::PLUS7_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.00f, 97.00f)), module, Gradus::PLUS8_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 16.50f)), module, Gradus::MINUS1_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 28.00f)), module, Gradus::MINUS2_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 39.50f)), module, Gradus::MINUS3_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 51.00f)), module, Gradus::MINUS4_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 62.50f)), module, Gradus::MINUS5_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 74.00f)), module, Gradus::MINUS6_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 85.50f)), module, Gradus::MINUS7_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(46.50f, 97.00f)), module, Gradus::MINUS8_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 16.50f)), module, Gradus::PLUS1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 28.00f)), module, Gradus::PLUS2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 39.50f)), module, Gradus::PLUS3_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 51.00f)), module, Gradus::PLUS4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 62.50f)), module, Gradus::PLUS5_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 74.00f)), module, Gradus::PLUS6_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 85.50f)), module, Gradus::PLUS7_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.50f, 97.00f)), module, Gradus::PLUS8_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 16.50f)), module, Gradus::MINUS1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 28.00f)), module, Gradus::MINUS2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 39.50f)), module, Gradus::MINUS3_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 51.00f)), module, Gradus::MINUS4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 62.50f)), module, Gradus::MINUS5_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 74.00f)), module, Gradus::MINUS6_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 85.50f)), module, Gradus::MINUS7_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 97.00f)), module, Gradus::MINUS8_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 109.00f)), module, Gradus::CLIP_PARAM));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.48f, 109.00f)), module, Gradus::CV_OUTPUT));
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(35.48f, 106.00f)), module, Gradus::LEVEL_LIGHT_GREEN));
		// @layout:end
	}
};


Model* modelGradus = createModel<Gradus, GradusWidget>("gradus");
