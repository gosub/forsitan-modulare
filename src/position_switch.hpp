#pragma once
// A switch you can click straight to a position, instead of one that steps
// through its positions in a ring.
//
// Rack's app::Switch increments on every click and wraps at the top, which is
// fine for two positions and irritating for three: going from "as is" to
// "invert" and back means passing through "randomize" and hearing it. Here
// the click lands where you pointed — top of the widget is the top position,
// bottom is the bottom one — like the switch it is drawn as.
//
// Right-click still opens the parameter's own menu, and the move goes on the
// undo stack exactly as Rack's does.

#include <rack.hpp>

namespace forsitan {

template <typename TBase>
struct PositionSwitch : TBase {
	void onButton(const rack::event::Button& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT
		    && (e.mods & RACK_MOD_MASK) == 0) {
			setFromPos(e.pos.y);
			e.consume(this);
			return;
		}
		// everything else — right-click menu, ctrl-click, double-click — is
		// the ordinary parameter behaviour
		rack::app::ParamWidget::onButton(e);
	}

	void setFromPos(float y) {
		rack::engine::ParamQuantity* pq = this->getParamQuantity();
		if (!pq || this->box.size.y <= 0.f)
			return;
		float lo = pq->getMinValue();
		float hi = pq->getMaxValue();
		int n = (int)std::round(hi - lo) + 1;
		if (n < 2)
			return;
		float frac = rack::math::clamp(y / this->box.size.y, 0.f, 0.9999f);
		int idx = rack::math::clamp((int)(frac * n), 0, n - 1);
		float v = hi - (float)idx;      // the top of the widget is the top value
		float old = pq->getValue();
		if (v == old)
			return;
		pq->setImmediateValue(v);

		rack::history::ParamChange* h = new rack::history::ParamChange;
		h->name = "move switch";
		h->moduleId = this->module->id;
		h->paramId = this->paramId;
		h->oldValue = old;
		h->newValue = v;
		APP->history->push(h);
	}
};

}   // namespace forsitan

using CKSSPos = forsitan::PositionSwitch<rack::componentlibrary::CKSS>;
using CKSSThreePos = forsitan::PositionSwitch<rack::componentlibrary::CKSSThree>;
