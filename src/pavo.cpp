#include "forsitan.hpp"


struct Pavo : Module {
	enum ParamIds {
		SPREAD_PARAM,
		CENTER_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		POLYIN_INPUT,
		SPREAD_INPUT,
		CENTER_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	Pavo() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(SPREAD_PARAM, 0.f, 10.f, 5.f, "Spread", "%", 0, 10);
		configParam(CENTER_PARAM, -5.f, 5.f, 0.f, "Center", "%", 0, 20);
	}

	// TODO:
	// [X] SPREAD INPUT
	// [X] CENTER INPUT
	// [-] SPREAD CLAMP
	// [-] CENTER CLAMP
	// [X] POSITION CLAMP
	// [ ] LEVEL COMPENSATION
	// [ ] EQUAL POWER PANNING
	// [X] SINGLE CHANNEL CASE
	// [X] REDO POSITION CALCULATION
	// [ ] FACTOR OUT PANNING FUNCTION

	void process(const ProcessArgs& args) override {
		float outL = 0.f, outR = 0.f;
		int channels = inputs[POLYIN_INPUT].getChannels();
		// spread input voltage is 0V-10V, internal value is [0.0, 1.0]
		float spread = (inputs[SPREAD_INPUT].getVoltage() + params[SPREAD_PARAM].getValue()) / 10.f;
		// spread input voltage is ±5V, internal value is [-0.5, 0.5]
		float center = (inputs[CENTER_INPUT].getVoltage() + params[CENTER_PARAM].getValue()) / 10.f;
		float pos_delta = channels == 1 ? 0.f : 1.f/(channels-1);
		float pos_offset = channels == 1 ? 0.f : -0.5f;
		for (int c = 0; c < channels; c++) {
			// calculate position within [-0.5, 0.5]
			float position = ((c * pos_delta) + pos_offset) * spread + center;
			// normalize and clamp position to [0.0, 1.0]
			position = position + 0.5f;
			position = clamp(position, 0.f, 1.f);
			// get input of channel c
			float input = inputs[POLYIN_INPUT].getVoltage(c);
			// pan in the stereo field
			outR += input * position;
			outL += input * (1.f - position);
		}
		outputs[LEFT_OUTPUT].setVoltage(outL);
		outputs[RIGHT_OUTPUT].setVoltage(outR);
	}
};


struct PavoWidget : ModuleWidget {
	PavoWidget(Pavo* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/pavo.svg")));

		addChild(createWidget<ScrewSilver>(Vec(0, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(7.938, 49.909)), module, Pavo::SPREAD_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(7.938, 76.896)), module, Pavo::CENTER_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.7, 24.283)), module, Pavo::POLYIN_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.579, 52.081)), module, Pavo::SPREAD_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.579, 79.069)), module, Pavo::CENTER_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(7.408, 105.273)), module, Pavo::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(17.992, 105.273)), module, Pavo::RIGHT_OUTPUT));
	}
};


Model* pavo = createModel<Pavo, PavoWidget>("pavo");
