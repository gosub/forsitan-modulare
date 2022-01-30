#ifndef CALLBACK_BUTTON_HPP__
#define CALLBACK_BUTTON_HPP__

#include "plugin.hpp"
//#include "window.hpp"
#include <functional>

template<typename T>
struct CallbackButton : SvgButton {
  static CallbackButton* create(const Vec& v,
				std::function<void (T*)> c,
				T* m,
				std::shared_ptr<rack::Svg> button_idle,
				std::shared_ptr<rack::Svg> button_clicked) {
    CallbackButton* cb = createWidget<CallbackButton<T> >(v);
    cb->callback = c;
    cb->module = m;
    cb->addFrame(button_idle);
    cb->addFrame(button_clicked);
    cb->shadow->opacity = -1.f;
    return cb;
  }

  CallbackButton() {}

  virtual void onButton(const ButtonEvent& e) override {
    SvgButton::onButton(e);
    if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
        e.consume(this);
        callback(module);
    }
  }

  std::function<void (T*)> callback;
  T* module;
};


#endif
