#include "forsitan.hpp"


struct Cumuli : Module {
	enum ParamIds {
		UPRATE_PARAM,
		DOWNRATE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		UP_INPUT,
		DOWN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	float accumulator = 0.f;

	Cumuli() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(UPRATE_PARAM, -2.f, 2.f, 0.f, "Rise rate", "V/sec", 10);
		configParam(DOWNRATE_PARAM, -2.f, 2.f, 0.f, "Fall rate", "V/sec", 10);
		// input labels
		configInput(UP_INPUT, "Rise gate");
		configInput(DOWN_INPUT, "Fall gate");
		// output label
		configOutput(OUT_OUTPUT, "Envelope CV");
	}

	void process(const ProcessArgs& args) override {
		float upVperSec = 0;
		float downVperSec = 0;

		if(inputs[UP_INPUT].getVoltage() > 0.5) {
			upVperSec = std::pow(10.f, params[UPRATE_PARAM].getValue());
			accumulator += upVperSec * args.sampleTime;
		}
		if(inputs[DOWN_INPUT].getVoltage() > 0.5) {
			downVperSec = std::pow(10.f, params[DOWNRATE_PARAM].getValue());
			accumulator -= downVperSec * args.sampleTime;
		}
		accumulator = math::clamp(accumulator, 0.f, 10.f);
		outputs[OUT_OUTPUT].setVoltage(accumulator);
	}
};


struct CumuliWidget : ModuleWidget {
	CumuliWidget(Cumuli* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/cumuli.svg")));

		addChild(createWidget<ScrewSilver>(Vec(0, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(12.7, 27.154)), module, Cumuli::UPRATE_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(12.7, 85.892)), module, Cumuli::DOWNRATE_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.7, 44.391)), module, Cumuli::UP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.7, 68.203)), module, Cumuli::DOWN_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.7, 106.332)), module, Cumuli::OUT_OUTPUT));
	}
};


Model* cumuli = createModel<Cumuli, CumuliWidget>("cumuli");
