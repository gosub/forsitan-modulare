#include "forsitan.hpp"


struct Interea : Module {
	enum ParamIds {
		FREQ_PARAM,
		HARMON_PARAM,
		VOICING_PARAM,
		INVERSION_PARAM,
		QUALITY_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		VOLTOCT_INPUT,
		VOICING_INPUT,
		INVERSION_INPUT,
		QUALITY_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		ROOT_OUTPUT,
		_3RD_OUTPUT,
		_5TH_OUTPUT,
		_7TH_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		HARMON_LIGHT,
		SPREAD_LIGHT,
		DROP3_LIGHT,
		DROP2_LIGHT,
		CLOSE_LIGHT,
		THIRD_LIGHT,
		SECOND_LIGHT,
		FIRST_LIGHT,
		ROOT_LIGHT,
		HALFDIM_LIGHT,
		DOM7_LIGHT,
		MIN7_LIGHT,
		MAJ7_LIGHT,
		NUM_LIGHTS
	};

	Interea() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(FREQ_PARAM, -5.f, 5.f, 0.f, "Frequency", " Hz", 2, dsp::FREQ_C4);
		configParam(HARMON_PARAM, 0.f, 1.f, 0.f, "Harmonize");
		configParam(VOICING_PARAM, 0.f, 4.f, 0.f, "Voicing");
		configParam(INVERSION_PARAM, 0.f, 4.f, 0.f, "Inversion");
		configParam(QUALITY_PARAM, 0.f, 4.f, 0.f, "Quality");
	}

	void process(const ProcessArgs& args) override {
	}
};


struct IntereaWidget : ModuleWidget {
	IntereaWidget(Interea* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/interea.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(13.145, 19.454)), module, Interea::FREQ_PARAM));
		addParam(createParamCentered<LEDButton>(mm2px(Vec(41.967, 19.454)), module, Interea::HARMON_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(22.273, 43.029)), module, Interea::VOICING_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(22.273, 64.725)), module, Interea::INVERSION_PARAM));
		addParam(createParamCentered<Rogan2PWhite>(mm2px(Vec(22.273, 86.421)), module, Interea::QUALITY_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(26.715, 26.928)), module, Interea::VOLTOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.177, 48.095)), module, Interea::VOICING_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.177, 69.791)), module, Interea::INVERSION_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.177, 91.487)), module, Interea::QUALITY_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.177, 106.478)), module, Interea::ROOT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(19.659, 106.478)), module, Interea::_3RD_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.141, 106.478)), module, Interea::_5TH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(42.623, 106.478)), module, Interea::_7TH_OUTPUT));

		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(41.967, 19.454)), module, Interea::HARMON_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 37.879)), module, Interea::SPREAD_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 41.583)), module, Interea::DROP3_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 45.287)), module, Interea::DROP2_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 48.991)), module, Interea::CLOSE_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 59.575)), module, Interea::THIRD_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 63.279)), module, Interea::SECOND_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 66.983)), module, Interea::FIRST_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 70.687)), module, Interea::ROOT_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 81.27)), module, Interea::HALFDIM_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 84.975)), module, Interea::DOM7_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 88.679)), module, Interea::MIN7_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(33.056, 92.383)), module, Interea::MAJ7_LIGHT));
	}
};


Model* interea = createModel<Interea, IntereaWidget>("interea");