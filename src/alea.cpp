#include <random>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "forsitan.hpp"
// Not pulled in by rack.hpp, and the only way to a tag's name: the IDs
// themselves are explicitly not part of the ABI.
#include <tag.hpp>
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

// The tags offered in the "excluded tags" submenu, canonical spelling as in
// Rack's own tag list. Anything not here is never filtered on.
const std::vector<std::string> filterTags = {
    "Blank", "Controller", "Expander", "External", "MIDI", "Utility", "Visual"
};

// A maker, not a plugin: one author shipping several plugins under one brand
// gets one ticket in the brand draw, which is what "by brand" means to a human.
std::string brandOf(plugin::Plugin* p) {
    if (!p->brand.empty()) return p->brand;
    if (!p->name.empty()) return p->name;
    return p->slug;
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

	// Draw a brand first, then a module inside it, so a plugin with 100
	// modules is no likelier than a plugin with one.
	bool weightByBrand = false;
	// Canonical tag names, not tag IDs: the IDs are not part of the ABI.
	std::set<std::string> excludedTags = {"Blank", "Expander", "External", "MIDI"};

	Alea() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	}

	// Names resolved to this Rack's own IDs, once per draw.
	std::set<int> excludedTagIds() {
		std::set<int> ids;
		for (const std::string& t : excludedTags) {
			int id = tag::findId(t);
			if (id >= 0) ids.insert(id);
		}
		return ids;
	}

	bool accepts(Model* m, const std::set<int>& excluded) {
		// Hidden models are the ones a maker deprecated: loadable from an old
		// patch, never offered in the browser, and never a good surprise here.
		if (m->hidden) return false;
		for (int tagId : m->tagIds) {
			if (excluded.count(tagId)) return false;
		}
		return true;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "weightByBrand", json_boolean(weightByBrand));
		json_t* tagsJ = json_array();
		for (const std::string& t : excludedTags) {
			json_array_append_new(tagsJ, json_string(t.c_str()));
		}
		json_object_set_new(rootJ, "excludedTags", tagsJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* weightJ = json_object_get(rootJ, "weightByBrand");
		if (weightJ) weightByBrand = json_boolean_value(weightJ);
		json_t* tagsJ = json_object_get(rootJ, "excludedTags");
		if (json_is_array(tagsJ)) {
			excludedTags.clear();
			size_t i;
			json_t* tagJ;
			json_array_foreach(tagsJ, i, tagJ) {
				const char* s = json_string_value(tagJ);
				if (s) excludedTags.insert(s);
			}
		}
	}
};


namespace {

void CreateRandomModule(Alea* alea) {
	// NULL in the module browser, where the button is only a picture.
	if (!alea) return;
	const std::set<int> excluded = alea->excludedTagIds();

	if (alea->weightByBrand) {
		std::map<std::string, std::vector<Model*>> byBrand;
		for (plugin::Plugin* p : rack::plugin::plugins) {
			for (Model* m : p->models) {
				if (alea->accepts(m, excluded)) byBrand[brandOf(p)].push_back(m);
			}
		}
		if (byBrand.empty()) return;
		const std::vector<Model*>& models =
			select_randomly(byBrand.begin(), byBrand.end())->second;
		CreateModule(*select_randomly(models.begin(), models.end()));
		return;
	}

	std::vector<Model*> models;
	for (plugin::Plugin* p : rack::plugin::plugins) {
		for (Model* m : p->models) {
			if (alea->accepts(m, excluded)) models.push_back(m);
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

	void appendContextMenu(Menu* menu) override {
		Alea* m = dynamic_cast<Alea*>(module);
		if (!m) return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("The pool"));

		menu->addChild(createBoolPtrMenuItem(
			"Even odds per brand", "", &m->weightByBrand));

		menu->addChild(createSubmenuItem("Excluded tags", "", [=](Menu* sub) {
			for (const std::string& t : filterTags) {
				sub->addChild(createCheckMenuItem(t, "",
					[=]() { return m->excludedTags.count(t) > 0; },
					[=]() {
						if (m->excludedTags.count(t)) m->excludedTags.erase(t);
						else m->excludedTags.insert(t);
					}));
			}
		}));
	}
};


Model* alea = createModel<Alea, AleaWidget>("alea");
