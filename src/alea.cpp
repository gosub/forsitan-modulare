#include <random>
#include <iterator>
#include <vector>
#include "forsitan.hpp"
#include "callback_button.hpp"


namespace {

void CreateModule(Model* model) {
    engine::Module* module = model->createModule();
    APP->engine->addModule(module);
    ModuleWidget* moduleWidget = model->createModuleWidget(module);
    APP->scene->rack->addModuleAtMouse(moduleWidget);
    // Load template preset
    moduleWidget->loadTemplate();

    // history::ModuleAdd
    history::ModuleAdd* h = new history::ModuleAdd;
    h->name = "create module";
    // This serializes the module so redoing returns to the current state.
    h->setModule(moduleWidget);
    APP->history->push(h);
}

template<typename Iter, typename RandomGenerator>
Iter select_randomly(Iter start, Iter end, RandomGenerator& g) {
    std::uniform_int_distribution<> dis(0, std::distance(start, end) - 1);
    std::advance(start, dis(g));
    return start;
}

template<typename Iter>
Iter select_randomly(Iter start, Iter end) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return select_randomly(start, end, gen);
}

} // namespace


struct Alea : Module {
	enum ParamIds {
		NUM_PARAMS
	};
	enum InputIds {
		NUM_INPUTS
	};
	enum OutputIds {
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	Alea() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	}

	bool accepts(Model* m) {
		// Hidden models are the ones a maker deprecated: loadable from an old
		// patch, never offered in the browser, and never a good surprise here.
		if (m->hidden) return false;
		return true;
	}
};


namespace {

void CreateRandomModule(Alea* alea) {
	// NULL in the module browser, where the button is only a picture.
	if (!alea) return;

	std::vector<Model*> models;
	for (plugin::Plugin* p : rack::plugin::plugins) {
		for (Model* m : p->models) {
			if (alea->accepts(m)) models.push_back(m);
		}
	}
	if (models.empty()) return;
	CreateModule(*select_randomly(models.begin(), models.end()));
}

} // namespace


typedef CallbackButton<Alea> CB;


struct AleaWidget : ModuleWidget {

	AleaWidget(Alea* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/alea.svg")));

		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        std::shared_ptr<rack::Svg> die = APP->window->loadSvg(asset::plugin(pluginInstance, "res/buttons/die.svg"));
        std::shared_ptr<rack::Svg> die_negative = APP->window->loadSvg(asset::plugin(pluginInstance, "res/buttons/die-negative.svg"));
        addChild(CB::create(Vec(7.5, 128), [](Alea* m){CreateRandomModule(m);}, module, die, die_negative));
	}
};


Model* alea = createModel<Alea, AleaWidget>("alea");
