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
	p->addModel(limen);
	p->addModel(modelMMCCCXCIX);
	p->addModel(modelScando);
	p->addModel(modelPellicula);
	p->addModel(modelDraen);
	p->addModel(modelRete);
	p->addModel(modelUlulo);
	p->addModel(modelTabes);
	p->addModel(modelLustro);
	p->addModel(modelBulla);
	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when your module is created to reduce startup times of Rack.
}
