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
		configParam(CV_PARAM, 0.f, 1.f, 0.f, "");
		configParam(CASCADE_PARAM, 0.f, 1.f, 0.f, "");
	}

	void process(const ProcessArgs& args) override {
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
