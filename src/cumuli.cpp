#include "forsitan.hpp"


struct Cumuli : Module {
	enum ParamIds {
		UPRATE_PARAM,
		UPGATE_PARAM,
		RESETGATE_PARAM,
		DOWNGATE_PARAM,
		DOWNRATE_PARAM,
		BIPOLAR_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		UP_INPUT,
		RESET_INPUT,
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
		configParam(UPGATE_PARAM, 0.f, 1.f, 0.f, "Rise manual gate");
		configParam(RESETGATE_PARAM, 0.f, 1.f, 0.f, "Reset manual gate");
		configParam(DOWNRATE_PARAM, -2.f, 2.f, 0.f, "Fall rate", "V/sec", 10);
		configParam(DOWNGATE_PARAM, 0.f, 1.f, 0.f, "Fall manual gate");
		configParam(BIPOLAR_PARAM, 0.f, 1.f, 0.f, "Bipolar switch");
		// input labels
		configInput(UP_INPUT, "Rise gate");
		configInput(RESET_INPUT, "Reset gate");
		configInput(DOWN_INPUT, "Fall gate");
		// output label
		configOutput(OUT_OUTPUT, "Envelope CV");
	}

	void process(const ProcessArgs& args) override {
		float upVperSec = 0;
		float downVperSec = 0;

		bool bipolar = params[BIPOLAR_PARAM].getValue();

		if(inputs[RESET_INPUT].getVoltage() + params[RESETGATE_PARAM].getValue() > 0.5) {
			// when output is bipolar, a -5V offset is added to the output
			// so to reach 0V on reset gate, we bring the accumulator to +5V
			// when output is monopolar, no offset is added, so a reset gate
			// sets the accumulator directly to 0V
			accumulator = bipolar ? 5.f : 0.f ;
		}

		if(inputs[UP_INPUT].getVoltage() + params[UPGATE_PARAM].getValue() > 0.5) {
			upVperSec = std::pow(10.f, params[UPRATE_PARAM].getValue());
			accumulator += upVperSec * args.sampleTime;
		}
		if(inputs[DOWN_INPUT].getVoltage() + params[DOWNGATE_PARAM].getValue() > 0.5) {
			downVperSec = std::pow(10.f, params[DOWNRATE_PARAM].getValue());
			accumulator -= downVperSec * args.sampleTime;
		}
		// internal value of the accumulator is always between 0 and + 10V
		accumulator = math::clamp(accumulator, 0.f, 10.f);
		// if output is set to bipolar, a -5V offset isaddded
		outputs[OUT_OUTPUT].setVoltage(accumulator - (bipolar ? 5.f : 0.f));
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

		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(12.7, 25.038)), module, Cumuli::UPRATE_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(12.7, 89.596)), module, Cumuli::DOWNRATE_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(7.792, 40.687)), module, Cumuli::UPGATE_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(7.792, 57.9)), module, Cumuli::RESETGATE_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(7.792, 73.495)), module, Cumuli::DOWNGATE_PARAM));
		addParam(createParam<CKSS>(mm2px(Vec(18.049, 105.435)), module, Cumuli::BIPOLAR_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.992, 40.687)), module, Cumuli::UP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.992, 57.9)), module, Cumuli::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.992, 73.495)), module, Cumuli::DOWN_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.467, 106.332)), module, Cumuli::OUT_OUTPUT));
	}
};


Model* cumuli = createModel<Cumuli, CumuliWidget>("cumuli");
