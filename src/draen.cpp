// draen.cpp — VCV Rack 2 module
// dræn (Old English for "bee"; the etymological root of "drone") is a port of
// the dronecaster norns instrument: a bank of drone engines, each a small
// SuperCollider graph, played from two controls — fundamental (hz) and level
// (amp) — with a third control selecting the engine. Switching engines fades
// the current one down and the next one up, mirroring dronecaster's SynthSocket.
//
// Controls:
//   Knobs : HZ (fundamental), AMP (level), ENGINE (select)
//   In    : HZ CV (1V/oct or linear Hz, right-click), AMP CV, ENGINE CV
//   Out   : L, R (stereo)
//   Light : LEVEL (output amplitude)
//
// The drone engines and the reusable SC-UGEN DSP layer they are built on live in
// draen_engines.hpp / draen_ugens.hpp.

#include "forsitan.hpp"
#include "draen_engines.hpp"
#include "draen_alt_engines.hpp"

using namespace draen;

struct Draen : Module {
    enum ParamId {
        HZ_PARAM,
        AMP_PARAM,
        ENGINE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        HZ_CV_INPUT,
        AMP_CV_INPUT,
        ENGINE_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    enum HzMode { HZ_VOCT, HZ_LINEAR };

    // two banks of 37 engines: the dronecaster ports and the "hyf" originals
    std::vector<std::unique_ptr<DroneEngine>> banks[2];
    int bank = 0;          // bank the audio is currently on
    int bankRequest = 0;   // bank chosen in the menu (applied through the fade)

    // engine switching / fade state (sequential fade-down then fade-up)
    enum FadePhase { STEADY, FADE_OUT, FADE_IN };
    int   activeIdx = 0;
    int   cuedIdx   = 0;
    int   uiSelected = 0;      // engine the controls currently point at (for display)
    float fadeGain  = 1.f;
    FadePhase fadePhase = STEADY;
    bool firstFrame = true;   // adopt the patch's saved engine before making sound
    uint32_t seedCounter = 0x1u;
    float curSampleRate = 0.f;   // engines are (re)initialised when this changes

    // persisted settings
    int   hzMode   = HZ_VOCT;
    float fadeTime = 2.f;      // seconds for a full fade-down or fade-up

    float levelEnv = 0.f;

    Draen() {
        banks[0] = makeEngines();
        banks[1] = makeAltEngines();
        int n = (int)banks[0].size();   // both banks hold 37 engines

        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(HZ_PARAM, -5.f, 5.f, -1.25f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);  // default A2 (110 Hz)
        configParam(AMP_PARAM, 0.f, 1.f, 0.5f, "Level", "%", 0.f, 100.f);
        configParam(ENGINE_PARAM, 0.f, (float)(n - 1), 0.f, "Engine");
        paramQuantities[ENGINE_PARAM]->snapEnabled = true;

        configInput(HZ_CV_INPUT, "Frequency CV");
        configInput(AMP_CV_INPUT, "Level CV");
        configInput(ENGINE_CV_INPUT, "Engine select CV");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configLight(LEVEL_LIGHT, "Output level");
        // engines are initialised lazily on the first process() call, once the
        // real sample rate is known (delay-line engines need it to size buffers)
    }

    uint32_t nextSeed() { return seedCounter = seedCounter * 1664525u + 1013904223u; }

    void onReset() override {
        activeIdx = cuedIdx = 0;
        fadeGain = 1.f;
        fadePhase = STEADY;
        levelEnv = 0.f;
        if (!banks[bank].empty() && curSampleRate > 0.f)
            banks[bank][activeIdx]->init(nextSeed(), curSampleRate);
    }

    const char* engineName(int i) const {
        // show the requested bank immediately; the audio catches up via the fade
        const auto& b = banks[clamp(bankRequest, 0, 1)];
        if (i < 0 || i >= (int)b.size()) return "";
        return b[i]->name();
    }

    // knob + CV -> selected engine index
    int selectEngine() {
        int n = (int)banks[bank].size();
        if (n == 0) return 0;
        float p = params[ENGINE_PARAM].getValue();
        if (inputs[ENGINE_CV_INPUT].isConnected())
            p += inputs[ENGINE_CV_INPUT].getVoltage() * 0.1f * (n - 1);
        return clamp((int)std::lround(p), 0, n - 1);
    }

    // soft limiter: unity below ±1, gentle tanh compression above
    static float softLimit(float x) {
        if (x >  1.f) return  1.f + std::tanh(x - 1.f);
        if (x < -1.f) return -1.f + std::tanh(x + 1.f);
        return x;
    }

    void process(const ProcessArgs& args) override {
        int n = (int)banks[bank].size();
        if (n == 0) { outputs[LEFT_OUTPUT].setVoltage(0.f); outputs[RIGHT_OUTPUT].setVoltage(0.f); return; }

        // (re)initialise the active engine when the sample rate changes (also the
        // first call): delay-line engines size their buffers from it
        if (args.sampleRate != curSampleRate) {
            curSampleRate = args.sampleRate;
            banks[bank][activeIdx]->init(nextSeed(), curSampleRate);
        }

        // ── controls ────────────────────────────────────────────────────────
        float pitch = params[HZ_PARAM].getValue();
        float hz;
        if (hzMode == HZ_VOCT) {
            pitch += inputs[HZ_CV_INPUT].getVoltage();
            hz = dsp::FREQ_C4 * std::pow(2.f, pitch);
        } else {
            hz = dsp::FREQ_C4 * std::pow(2.f, pitch);
            if (inputs[HZ_CV_INPUT].isConnected())
                hz += inputs[HZ_CV_INPUT].getVoltage() * 100.f;   // 100 Hz / V
        }
        hz = clamp(hz, 0.f, 12000.f);

        float amp = params[AMP_PARAM].getValue();
        if (inputs[AMP_CV_INPUT].isConnected())
            amp += inputs[AMP_CV_INPUT].getVoltage() * 0.1f;
        amp = clamp(amp, 0.f, 1.f);

        int target = selectEngine();
        uiSelected = target;

        // on the first frame (fresh add or patch load) start directly on the
        // selected engine and fade it in from silence, instead of fading out
        // from engine 0 first
        if (firstFrame) {
            firstFrame = false;
            bank = clamp(bankRequest, 0, 1);
            activeIdx = cuedIdx = target;
            banks[bank][activeIdx]->init(nextSeed(), curSampleRate);
            fadeGain = 0.f;
            fadePhase = FADE_IN;
        }

        // ── engine-switch fade state machine ────────────────────────────────
        float step = args.sampleTime / std::max(fadeTime, 0.01f);
        switch (fadePhase) {
            case STEADY:
                if (target != activeIdx || bankRequest != bank) { cuedIdx = target; fadePhase = FADE_OUT; }
                break;
            case FADE_OUT:
                cuedIdx = target;                       // latest selection wins
                fadeGain -= step;
                if (cuedIdx == activeIdx && bankRequest == bank) {   // returned home: abort switch
                    fadePhase = FADE_IN;
                } else if (fadeGain <= 0.f) {
                    fadeGain = 0.f;
                    bank = clamp(bankRequest, 0, 1);
                    activeIdx = cuedIdx;
                    banks[bank][activeIdx]->init(nextSeed(), curSampleRate);
                    fadePhase = FADE_IN;
                }
                break;
            case FADE_IN:
                fadeGain += step;
                if (fadeGain >= 1.f) { fadeGain = 1.f; fadePhase = STEADY; }
                if (target != activeIdx || bankRequest != bank) { cuedIdx = target; fadePhase = FADE_OUT; }
                break;
        }

        // ── render active engine ────────────────────────────────────────────
        float l = 0.f, r = 0.f;
        banks[bank][activeIdx]->process(hz, amp, args.sampleTime, l, r);
        l *= fadeGain; r *= fadeGain;

        outputs[LEFT_OUTPUT].setVoltage(5.f * softLimit(l));
        outputs[RIGHT_OUTPUT].setVoltage(5.f * softLimit(r));

        // ── output-level LED ────────────────────────────────────────────────
        float mag = std::max(std::fabs(l), std::fabs(r));
        levelEnv += (mag - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "hzMode", json_integer(hzMode));
        json_object_set_new(root, "fadeTime", json_real(fadeTime));
        json_object_set_new(root, "bank", json_integer(bankRequest));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "hzMode")) hzMode = json_integer_value(j);
        if (json_t* j = json_object_get(root, "fadeTime")) fadeTime = json_real_value(j);
        if (json_t* j = json_object_get(root, "bank")) {
            bankRequest = clamp((int)json_integer_value(j), 0, 1);
            bank = bankRequest;   // loading a patch lands directly on the saved bank
        }
    }
};

// Engine picker, shared by the display's right-click menu and the panel menu:
// 37 detents is a lot of knob turning when you already know which drone you
// want. Lists the requested bank, so it follows a bank switch immediately.
static void appendEngineItems(Menu* menu, Draen* m) {
    int n = (int)m->banks[clamp(m->bankRequest, 0, 1)].size();
    for (int i = 0; i < n; i++) {
        menu->addChild(createCheckMenuItem(m->engineName(i), "",
            [m, i]() { return m->uiSelected == i; },
            [m, i]() { m->params[Draen::ENGINE_PARAM].setValue((float)i); }));
    }
}

// ── engine-name display (runtime NanoVG text; panel SVG can't hold <text>) ────
struct EngineDisplay : TransparentWidget {
    Draen* module = nullptr;

    // Right-click the name to choose an engine by name. Every other button
    // falls through to the module widget, so the display is still somewhere
    // you can grab to drag the module around.
    void onButton(const ButtonEvent& e) override {
        if (!module || e.action != GLFW_PRESS
            || e.button != GLFW_MOUSE_BUTTON_RIGHT || (e.mods & RACK_MOD_MASK))
            return;
        e.consume(this);
        Menu* menu = createMenu();
        menu->addChild(createMenuLabel("engine"));
        // the bank lives here too: it decides which 37 names are on offer,
        // and this menu covers the panel one while the pointer is over the display
        menu->addChild(createIndexPtrSubmenuItem("Bank",
            {"dræn (dronecaster ports)", "hyf (original instruments)"},
            &module->bankRequest));
        menu->addChild(new MenuSeparator);
        appendEngineItems(menu, module);
    }

    // dark rounded panel on the normal layer (always visible)
    void draw(const DrawArgs& args) override {
        Rect r = box.zeroPos();
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGB(0x11, 0x11, 0x11));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 0.8f);
        nvgStroke(args.vg);
    }

    // glowing engine name on the self-illuminating layer
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        std::shared_ptr<Font> font =
            APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font) return;

        const char* txt = module ? module->engineName(module->uiSelected) : "draen";
        Rect r = box.zeroPos();
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 11.f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGB(0xff, 0xd5, 0x00));   // accent yellow
        nvgText(args.vg, r.getCenter().x, r.getCenter().y, txt, NULL);
    }
};

struct DraenWidget : ModuleWidget {
    DraenWidget(Draen* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/draen.svg")));

// @layout:begin draen 40.64 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem ENGINE_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem HZ_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem AMP_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem ENGINE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem HZ_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AMP_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_ENGINE label 0.0 label "engine" 0.0 20.32 24.30
// @elem LABEL_ENGCV label 0.0 label "eng cv" 0.0 20.32 46.00
// @elem LABEL_HZ label 0.0 label "hz" 0.0 11.50 66.00
// @elem LABEL_AMP label 0.0 label "amp" 0.0 29.14 66.00
// @elem LABEL_HZCV label 0.0 label "cv" 0.0 11.50 84.50
// @elem LABEL_AMPCV label 0.0 label "cv" 0.0 29.14 84.50
// @elem LABEL_OUTL label 0.0 label "L" 0.0 11.50 114.00
// @elem LABEL_OUTR label 0.0 label "R" 0.0 29.14 114.00
// @elem BOX_OUTL panel_box 7.0 box "" 0.0 11.50 108.50
// @elem BOX_OUTR panel_box 7.0 box "" 0.0 29.14 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(20.32f, 34.00f)), module, Draen::ENGINE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 74.00f)), module, Draen::HZ_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.14f, 74.00f)), module, Draen::AMP_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 52.00f)), module, Draen::ENGINE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.50f, 90.00f)), module, Draen::HZ_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.14f, 90.00f)), module, Draen::AMP_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.50f, 106.50f)), module, Draen::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(29.14f, 106.50f)), module, Draen::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(20.32f, 106.50f)), module, Draen::LEVEL_LIGHT));
        // @layout:end

        // engine-name display (runtime widget, not part of the panel SVG)
        EngineDisplay* disp = createWidget<EngineDisplay>(mm2px(Vec(4.32f, 12.50f)));
        disp->box.size = mm2px(Vec(32.00f, 8.00f));
        disp->module = module;
        addChild(disp);
    }

    void appendContextMenu(Menu* menu) override {
        Draen* m = dynamic_cast<Draen*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Engine", m->engineName(m->uiSelected),
            [m](Menu* sub) { appendEngineItems(sub, m); }));
        menu->addChild(createIndexPtrSubmenuItem("Engine bank",
            {"dr\u00e6n (dronecaster ports)", "hyf (original instruments)"}, &m->bankRequest));
        menu->addChild(createIndexPtrSubmenuItem("Hz CV input",
            {"1V/oct", "Linear (100 Hz/V)"}, &m->hzMode));
        menu->addChild(createIndexSubmenuItem("Fade time",
            {"0.25 s", "0.5 s", "1 s", "2 s", "4 s", "8 s"},
            [m]() {
                float t = m->fadeTime;
                if (t < 0.375f) return 0;
                if (t < 0.75f)  return 1;
                if (t < 1.5f)   return 2;
                if (t < 3.f)    return 3;
                if (t < 6.f)    return 4;
                return 5;
            },
            [m](int i) {
                static const float times[] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f};
                m->fadeTime = times[clamp(i, 0, 5)];
            }));
    }
};

Model* modelDraen = createModel<Draen, DraenWidget>("draen");
