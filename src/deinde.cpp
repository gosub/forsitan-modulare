#include "forsitan.hpp"


struct Deinde : Module {
	enum ParamIds {
		CV_PARAM,
		CASCADE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		CVIN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT1_OUTPUT,
		OUT2_OUTPUT,
		OUT3_OUTPUT,
		OUT4_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	Deinde() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(CV_PARAM, 0.f, 1.f, 1.f, "Cascade CV attenuator");
		configParam(CASCADE_PARAM, 0.f, 10.f, 0.f, "Manual cascade");
	}

	void process(const ProcessArgs& args) override {
		float cascade = inputs[CVIN_INPUT].getVoltage() * params[CV_PARAM].getValue() + params[CASCADE_PARAM].getValue();
		float out1 = clamp(cascade * 4.f, 0.f, 10.f);
		float out2 = clamp(cascade * 4.f - 10.f, 0.f, 10.f);
		float out3 = clamp(cascade * 4.f - 20.f, 0.f, 10.f);
		float out4 = clamp(cascade * 4.f - 30.f, 0.f, 10.f);
		outputs[OUT1_OUTPUT].setVoltage(out1);
		outputs[OUT2_OUTPUT].setVoltage(out2);
		outputs[OUT3_OUTPUT].setVoltage(out3);
		outputs[OUT4_OUTPUT].setVoltage(out4);
	}
};


struct DeindeWidget : ModuleWidget {
	DeindeWidget(Deinde* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/deinde.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(29.543, 21.35)), module, Deinde::CV_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(29.543, 46.743)), module, Deinde::CASCADE_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.275, 21.35)), module, Deinde::CVIN_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.106, 46.743)), module, Deinde::OUT1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.106, 64.795)), module, Deinde::OUT2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.106, 82.847)), module, Deinde::OUT3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.106, 100.9)), module, Deinde::OUT4_OUTPUT));
	}
};


Model* deinde = createModel<Deinde, DeindeWidget>("deinde");
