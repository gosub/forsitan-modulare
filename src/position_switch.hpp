#pragma once
// A three-position switch you can click straight to a position, instead of
// one that steps through its positions in a ring.
//
// Rack's app::Switch increments on every click and wraps at the top, which is
// exactly right for two positions - a click flips it - and irritating for
// three: going from "as is" to "invert" and back means passing through
// "randomize" and hearing it. So only the three-way switch is wrapped. Here
// the click lands where you pointed - top of the widget is the top position,
// bottom is the bottom one - like the switch it is drawn as.
//
// Right-click still opens the parameter's own menu, and the move goes on the
// undo stack exactly as Rack's does.

#include <rack.hpp>

namespace forsitan {

template <typename TBase>
struct PositionSwitch : TBase {
	float pressY = 0.f;

	// Note where the press landed and let the ordinary parameter handling
	// run: consuming the press here instead would still start a drag on this
	// widget, and Switch::onDragStart would then increment the value on top
	// of whatever we had set - every click landing one position too high.
	void onButton(const rack::event::Button& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT)
			pressY = e.pos.y;
		rack::app::ParamWidget::onButton(e);
	}

	// This is where Switch does its increment. Setting from the press
	// position instead is the whole point of the widget.
	void onDragStart(const rack::event::DragStart& e) override {
		if (this->momentary) {
			TBase::onDragStart(e);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_LEFT)
			setFromPos(pressY);
	}

	void onDragEnd(const rack::event::DragEnd& e) override {
		if (this->momentary)
			TBase::onDragEnd(e);
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

using CKSSThreePos = forsitan::PositionSwitch<rack::componentlibrary::CKSSThree>;
