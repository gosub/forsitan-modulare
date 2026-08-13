#include "forsitan.hpp"

// gradus — eight steps into one CV output.
//
// Eight rows, each one a value knob, a three-way mode switch and a trigger
// input. A trigger applies its own row to the running output value: add the
// knob, subtract the knob, or jump straight to it. Where cumuli ramps for as
// long as a gate is open, gradus moves only on an edge, and only by the
// amount dialled in, so the output walks a lattice of discrete values.
//
// What happens when several triggers land in the same sample is the whole
// design of the module:
//   - a jump beats every add and subtract in that sample
//   - among simultaneous jumps the lowest row on the panel wins, which is
//     the last one checked
//   - with no jump, every simultaneous add and subtract is applied
//
// The value is held internally in 0..10V exactly as cumuli holds its
// accumulator, and the bipolar option subtracts 5V on the way out. That is
// what keeps the knobs meaning the same thing in both ranges: a step is
// always its knob in volts, and a jump target is the knob read on the
// output's own scale.

// The knob is a step size in add/subtract, and an absolute target in jump.
// It reads as the second in bipolar, where the output is offset by -5V, so
// the tooltip has to follow the mode switch next to it.
struct GradusStepQuantity : ParamQuantity {
	int row = 0;
	int modeParam = 0;
	const bool* bipolar = nullptr;

	bool isTarget() {
		return module && (int) std::round(module->params[modeParam].getValue()) == 1;
	}
	float offset() {
		return (isTarget() && bipolar && *bipolar) ? 5.f : 0.f;
	}
	float getDisplayValue() override {
		return getValue() - offset();
	}
	void setDisplayValue(float v) override {
		setValue(v + offset());
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
		NUM_PARAMS
	};
	enum InputIds {
		TRIG1_INPUT, TRIG2_INPUT, TRIG3_INPUT, TRIG4_INPUT,
		TRIG5_INPUT, TRIG6_INPUT, TRIG7_INPUT, TRIG8_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		CV_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		LEVEL_LIGHT,
		NUM_LIGHTS
	};

	enum Rows { ROWS = 8 };
	// switch positions, numbered as the widget does: 0 is the bottom throw
	enum Mode { MODE_SUB, MODE_JUMP, MODE_ADD };

	// The running value, always 0..10V internally. Bipolar output is this
	// minus 5V, so the value survives a change of range unaltered.
	float value = 0.f;
	bool bipolar = false;

	dsp::SchmittTrigger trigger[ROWS];

	Gradus() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		for (int i = 0; i < ROWS; i++) {
			GradusStepQuantity* q = configParam<GradusStepQuantity>(
				STEP1_PARAM + i, 0.f, 10.f, 1.f, "", " V");
			q->row = i;
			q->modeParam = MODE1_PARAM + i;
			q->bipolar = &bipolar;
			configSwitch(MODE1_PARAM + i, 0.f, 2.f, 2.f,
				string::f("Row %d mode", i + 1),
				{"subtract", "jump", "add"});
			configInput(TRIG1_INPUT + i, string::f("Row %d trigger", i + 1));
		}
		configOutput(CV_OUTPUT, "CV");
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		value = 0.f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "value", json_real(value));
		json_object_set_new(root, "bipolar", json_boolean(bipolar));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "value"))
			value = math::clamp((float) json_number_value(j), 0.f, 10.f);
		if (json_t* j = json_object_get(root, "bipolar"))
			bipolar = json_boolean_value(j);
	}

	void process(const ProcessArgs& args) override {
		// One pass over the rows collects everything that fired this sample:
		// the sum of the relative moves, and the last jump seen. Reading top
		// to bottom and keeping the last is what gives the bottom row
		// precedence.
		int jumpRow = -1;
		float delta = 0.f;

		for (int i = 0; i < ROWS; i++) {
			if (!trigger[i].process(inputs[TRIG1_INPUT + i].getVoltage(), 0.1f, 1.f))
				continue;
			int mode = (int) std::round(params[MODE1_PARAM + i].getValue());
			if (mode == MODE_JUMP)
				jumpRow = i;
			else if (mode == MODE_ADD)
				delta += params[STEP1_PARAM + i].getValue();
			else
				delta -= params[STEP1_PARAM + i].getValue();
		}

		// A jump discards the relative moves of the same sample rather than
		// landing next to them: the row names an absolute value, and that is
		// where the output goes.
		if (jumpRow >= 0)
			value = params[STEP1_PARAM + jumpRow].getValue();
		else
			value += delta;

		value = math::clamp(value, 0.f, 10.f);

		float out = value - (bipolar ? 5.f : 0.f);
		outputs[CV_OUTPUT].setVoltage(out);
		lights[LEVEL_LIGHT].setBrightness(std::fabs(out) / (bipolar ? 5.f : 10.f));
	}
};


struct GradusWidget : ModuleWidget {
	GradusWidget(Gradus* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/gradus.svg")));

// @layout:begin gradus 40.64 128.5
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
// @elem MODE1_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE2_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE3_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE4_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE5_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE6_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE7_PARAM CKSSThree 2.3 param "" 0.0
// @elem MODE8_PARAM CKSSThree 2.3 param "" 0.0
// @elem TRIG1_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG2_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG3_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG4_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG5_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG6_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG7_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG8_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_STEP label 0.0 label "step" 0.0 12.00 10.00
// @elem LABEL_MODE label 0.0 label "mode" 0.0 22.50 10.00
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 33.00 10.00
// @elem LABEL_ROW1 label 0.0 label "1" 0.0 4.50 17.50
// @elem LABEL_ROW2 label 0.0 label "2" 0.0 4.50 29.00
// @elem LABEL_ROW3 label 0.0 label "3" 0.0 4.50 40.50
// @elem LABEL_ROW4 label 0.0 label "4" 0.0 4.50 52.00
// @elem LABEL_ROW5 label 0.0 label "5" 0.0 4.50 63.50
// @elem LABEL_ROW6 label 0.0 label "6" 0.0 4.50 75.00
// @elem LABEL_ROW7 label 0.0 label "7" 0.0 4.50 86.50
// @elem LABEL_ROW8 label 0.0 label "8" 0.0 4.50 98.00
// @elem BOX_CV panel_box 7.0 box "" 0.0 20.32 111.00
// @elem LABEL_CV label 0.0 label "out" 0.0 20.32 116.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 122.50

		addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
		addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 0.00f)))); // SCREW_TR
		addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
		addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 123.42f)))); // SCREW_BR
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 16.50f)), module, Gradus::STEP1_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 28.00f)), module, Gradus::STEP2_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 39.50f)), module, Gradus::STEP3_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 51.00f)), module, Gradus::STEP4_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 62.50f)), module, Gradus::STEP5_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 74.00f)), module, Gradus::STEP6_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 85.50f)), module, Gradus::STEP7_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.00f, 97.00f)), module, Gradus::STEP8_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 16.50f)), module, Gradus::MODE1_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 28.00f)), module, Gradus::MODE2_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 39.50f)), module, Gradus::MODE3_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 51.00f)), module, Gradus::MODE4_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 62.50f)), module, Gradus::MODE5_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 74.00f)), module, Gradus::MODE6_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 85.50f)), module, Gradus::MODE7_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(22.50f, 97.00f)), module, Gradus::MODE8_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 16.50f)), module, Gradus::TRIG1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 28.00f)), module, Gradus::TRIG2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 39.50f)), module, Gradus::TRIG3_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 51.00f)), module, Gradus::TRIG4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 62.50f)), module, Gradus::TRIG5_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 74.00f)), module, Gradus::TRIG6_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 85.50f)), module, Gradus::TRIG7_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.00f, 97.00f)), module, Gradus::TRIG8_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 109.00f)), module, Gradus::CV_OUTPUT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(25.32f, 106.00f)), module, Gradus::LEVEL_LIGHT));
		// @layout:end
	}

	void appendContextMenu(Menu* menu) override {
		Gradus* m = dynamic_cast<Gradus*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolMenuItem("Bipolar output (±5V)", "",
			[m]() { return m->bipolar; },
			[m](bool v) { m->bipolar = v; }));
	}
};


Model* modelGradus = createModel<Gradus, GradusWidget>("gradus");
