#include "forsitan.hpp"


Plugin* pluginInstance;


void init(Plugin* p) {
	pluginInstance = p;

	// Add modules here
	p->addModel(alea);
	p->addModel(interea);
	p->addModel(cumuli);
	p->addModel(deinde);
	p->addModel(pavo);
	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when your module is created to reduce startup times of Rack.
}
