#include "forsitan.hpp"
#include "citadel/dsp.hpp"
#include "citadel/modulation.hpp"
#include "position_switch.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

// artifex — nine stereo effects sharing one feedback loop.
//
// After the Bastl Instruments Citadel FX Wizard, the other face of the
// hardware vates is built after: three knobs that mean something different in
// every mode, a filter inside the feedback path, a stereo detune, and the
// same pattern generator and LFO vates has (src/citadel/modulation.hpp).
// The hardware's SHIFT layer is unpacked into real controls. See
// doc/artifex.md.

namespace {

const char* kModeNames[9] = {
	"delay", "flanger", "freezer", "panner", "crusher",
	"slicer", "pitcher", "replayer", "shifter",
};

}   // namespace

struct Artifex : Module {
	enum ParamId {
		FXMODE_PARAM,
		FXMODE_ATT_PARAM,
		TIME_PARAM,
		TIME_ATT_PARAM,
		FBK_PARAM,
		FBK_ATT_PARAM,
		AMT_PARAM,
		AMT_ATT_PARAM,
		FILTER_PARAM,
		STEREO_PARAM,
		TRIG_PARAM,
		TEMPO_PARAM,
		SYNC_PARAM,
		RATE_PARAM,
		LFO_ATT_PARAM,
		GSW_PARAM,
		CSW_PARAM,
		RHYTHM_PARAM,
		GAIN_PARAM,
		LEVEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		LEFT_INPUT,
		RIGHT_INPUT,
		FXMODE_INPUT,
		FREE_INPUT,
		STEP_INPUT,
		FBK_INPUT,
		AMT_INPUT,
		FILTER_INPUT,
		STEREO_INPUT,
		TRIG_INPUT,
		CLK_INPUT,
		LFO_INPUT,
		LFO_RESET_INPUT,
		PAT_RESET_INPUT,
		G_INPUT,
		C_INPUT,
		RHYTHM_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		ENV_OUTPUT,
		TRI_OUTPUT,
		PULSE_OUTPUT,
		SAW_OUTPUT,
		GATE_OUTPUT,
		CV_OUTPUT,
		CLK_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LEFT_LIGHT,
		RIGHT_LIGHT,
		IN_L_LIGHT,
		IN_R_LIGHT,
		LIGHTS_LEN
	};

	// ── settings ─────────────────────────────────────────────────────────────
	bool honourExternalClock = true;
	// Which voltage window the pattern inputs read. The hardware's is 0-5 V
	// logic — below 1.6 V inverts — which in Rack means a gate resting at 0 V
	// inverts the pattern continuously until it goes high. The default here
	// is the Rack reading: zero is neutral, positive randomizes, negative
	// inverts.
	bool hardwareCvWindow = false;
	// Mode changes from CV land on the next step of the clock, as on the
	// hardware, so a modulated mode change does not chop a sound in half.
	bool quantizeModeChanges = true;
	bool monoInput = false;

	// ── state ────────────────────────────────────────────────────────────────
	citadel::Modulation modul;
	int mode = 0;                 // the mode actually running
	int aimedMode = 0;            // what the knob and CV ask for
	float envFollow = 0.f;

	// What the displays show. Plain char buffers written by the audio thread
	// only when the text changes; the widget reads them from the UI thread.
	char uiModeText[32] = "1 delay";
	char uiTimeText[32] = "";
	int uiModeShown = -1;

	dsp::SchmittTrigger trigIn;
	dsp::BooleanTrigger trigButton;

	Artifex() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configSwitch(FXMODE_PARAM, 0.f, 8.f, 0.f, "FX mode",
		             {"delay", "flanger", "freezer", "panner", "crusher",
		              "slicer", "pitcher", "replayer", "shifter"});
		getParamQuantity(FXMODE_PARAM)->snapEnabled = true;
		configParam(FXMODE_ATT_PARAM, -1.f, 1.f, 0.f, "FX mode CV", "%", 0.f, 100.f);

		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Time");
		configParam(TIME_ATT_PARAM, -1.f, 1.f, 0.f, "Time CV", "%", 0.f, 100.f);
		configParam(FBK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
		configParam(FBK_ATT_PARAM, -1.f, 1.f, 0.f, "Feedback CV", "%", 0.f, 100.f);
		configParam(AMT_PARAM, 0.f, 1.f, 0.5f, "Amount", "%", 0.f, 100.f);
		configParam(AMT_ATT_PARAM, -1.f, 1.f, 0.f, "Amount CV", "%", 0.f, 100.f);

		// Bipolar around an open centre: lowpass to the left, highpass to the
		// right, and it sits in the feedback path only.
		configParam(FILTER_PARAM, -1.f, 1.f, 0.f, "Filter");
		configParam(STEREO_PARAM, 0.f, 1.f, 0.f, "Stereo", "%", 0.f, 100.f);
		configButton(TRIG_PARAM, "Trigger");

		configParam(TEMPO_PARAM, 30.f, 300.f, 120.f, "Tempo", " BPM");
		configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "LFO clock", {"free", "sync"});
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "LFO rate");
		configParam(LFO_ATT_PARAM, -1.f, 1.f, 0.f, "LFO rate CV", "%", 0.f, 100.f);

		configParam(RHYTHM_PARAM, 0.f, 31.f, 0.f, "Rhythm");
		getParamQuantity(RHYTHM_PARAM)->snapEnabled = true;
		configSwitch(GSW_PARAM, 0.f, 2.f, 1.f, "Gate pattern", {"invert", "as is", "randomize"});
		configSwitch(CSW_PARAM, 0.f, 2.f, 1.f, "CV pattern", {"invert", "as is", "randomize"});

		// The feedback loop reacts to how hard it is driven, so the input
		// gain is a control and not a menu item — as on the hardware, where
		// it is the first thing the manual tells you to set.
		configParam(GAIN_PARAM, 0.f, 4.f, 1.f, "Input gain", " dB", -10.f, 20.f);
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");

		configInput(LEFT_INPUT, "Left audio");
		configInput(RIGHT_INPUT, "Right audio");
		configInput(FXMODE_INPUT, "FX mode");
		configInput(FREE_INPUT, "Time (free)");
		configInput(STEP_INPUT, "Time (stepped by the clock)");
		configInput(FBK_INPUT, "Feedback");
		configInput(AMT_INPUT, "Amount");
		configInput(FILTER_INPUT, "Filter");
		configInput(STEREO_INPUT, "Stereo");
		configInput(TRIG_INPUT, "Trigger");
		configInput(CLK_INPUT, "Clock");
		configInput(LFO_INPUT, "LFO rate");
		configInput(LFO_RESET_INPUT, "LFO reset");
		configInput(PAT_RESET_INPUT, "Pattern reset");
		configInput(G_INPUT, "Gate pattern");
		configInput(C_INPUT, "CV pattern");
		configInput(RHYTHM_INPUT, "Rhythm");

		configOutput(LEFT_OUTPUT, "Left audio");
		configOutput(RIGHT_OUTPUT, "Right audio");
		configOutput(ENV_OUTPUT, "Envelope follower");
		configOutput(TRI_OUTPUT, "LFO triangle");
		configOutput(PULSE_OUTPUT, "LFO pulse");
		configOutput(SAW_OUTPUT, "LFO saw");
		configOutput(GATE_OUTPUT, "Pattern gate");
		configOutput(CV_OUTPUT, "Pattern CV");
		configOutput(CLK_OUTPUT, "Clock");

		configBypass(LEFT_INPUT, LEFT_OUTPUT);
		configBypass(RIGHT_INPUT, RIGHT_OUTPUT);
	}

	// A three-position switch, normalled to the input beside it.
	int patternMode(int switchParam, int inputId) {
		if (!inputs[inputId].isConnected())
			return (int)std::round(params[switchParam].getValue());
		float v = inputs[inputId].getVoltage();
		if (hardwareCvWindow)
			return v > 3.2f ? 2 : (v < 1.6f ? 0 : 1);
		return v > 1.f ? 2 : (v < -1.f ? 0 : 1);
	}

	// Ten volts covers the nine modes and wraps, the same rule vates uses for
	// bank and sample: an LFO sweeps the whole machine untrimmed, and a small
	// attenuverter setting picks between neighbours instead.
	int modeSelect() {
		int base = (int)std::round(params[FXMODE_PARAM].getValue());
		float cv = inputs[FXMODE_INPUT].getVoltage() * params[FXMODE_ATT_PARAM].getValue();
		int i = base + (int)std::floor(cv * 0.1f * (9.f - 1e-3f));
		i %= 9;
		if (i < 0)
			i += 9;
		return i;
	}

	void process(const ProcessArgs& args) override {
		// ── the shared modulation section ────────────────────────────────────
		citadel::ModIn min;
		min.dt = args.sampleTime;
		min.bpm = params[TEMPO_PARAM].getValue();
		min.clkVoltage = inputs[CLK_INPUT].getVoltage();
		min.honourExternal = honourExternalClock;
		min.patResetVoltage = inputs[PAT_RESET_INPUT].getVoltage();
		min.rhythm = (int)std::round(params[RHYTHM_PARAM].getValue());
		if (inputs[RHYTHM_INPUT].isConnected())
			min.rhythm = citadel::rhythmSelect(min.rhythm, inputs[RHYTHM_INPUT].getVoltage(), 1.f);
		min.gateMode = patternMode(GSW_PARAM, G_INPUT);
		min.cvMode = patternMode(CSW_PARAM, C_INPUT);
		min.lfoRateKnob = params[RATE_PARAM].getValue();
		min.lfoRateMod = inputs[LFO_INPUT].getVoltage() * 0.2f * params[LFO_ATT_PARAM].getValue();
		min.lfoSynced = params[SYNC_PARAM].getValue() > 0.5f;
		min.lfoResetVoltage = inputs[LFO_RESET_INPUT].getVoltage();
		modul.process(min);

		// ── mode ─────────────────────────────────────────────────────────────
		aimedMode = modeSelect();
		bool knobMode = !inputs[FXMODE_INPUT].isConnected()
		                || params[FXMODE_ATT_PARAM].getValue() == 0.f;
		if (aimedMode != mode && (!quantizeModeChanges || knobMode || modul.stepped))
			mode = aimedMode;

		// ── input ────────────────────────────────────────────────────────────
		float gain = params[GAIN_PARAM].getValue();
		float inL = inputs[LEFT_INPUT].getVoltage() * gain;
		float inR = inputs[RIGHT_INPUT].isConnected()
		            ? inputs[RIGHT_INPUT].getVoltage() * gain : inL;
		if (monoInput) {
			float m = 0.5f * (inL + inR);
			inL = inR = m;
		}

		// envelope follower on the input, 0-10 V
		float rect = std::max(std::fabs(inL), std::fabs(inR));
		float coef = rect > envFollow ? 0.002f : 0.0002f;
		envFollow += (rect - envFollow) * coef;

		// ── output ───────────────────────────────────────────────────────────
		float level = params[LEVEL_PARAM].getValue();
		float outL = inL * level;
		float outR = inR * level;
		outputs[LEFT_OUTPUT].setVoltage(clamp(outL, -10.f, 10.f));
		outputs[RIGHT_OUTPUT].setVoltage(clamp(outR, -10.f, 10.f));
		lights[LEFT_LIGHT].setBrightnessSmooth(std::fabs(outL) * 0.2f, args.sampleTime);
		lights[RIGHT_LIGHT].setBrightnessSmooth(std::fabs(outR) * 0.2f, args.sampleTime);
		// the input lamps go red when the gain stage clips, as on the hardware
		lights[IN_L_LIGHT].setBrightnessSmooth(std::fabs(inL) > 9.f ? 1.f : 0.f, args.sampleTime);
		lights[IN_R_LIGHT].setBrightnessSmooth(std::fabs(inR) > 9.f ? 1.f : 0.f, args.sampleTime);

		outputs[ENV_OUTPUT].setVoltage(clamp(envFollow, 0.f, 10.f));
		outputs[TRI_OUTPUT].setVoltage(modul.tri * 10.f);
		outputs[PULSE_OUTPUT].setVoltage(modul.lfoRising ? 10.f : 0.f);
		outputs[SAW_OUTPUT].setVoltage(modul.lfoPhase * 10.f);
		outputs[CLK_OUTPUT].setVoltage(modul.clock() ? 10.f : 0.f);
		outputs[GATE_OUTPUT].setVoltage(modul.gate() ? 10.f : 0.f);
		outputs[CV_OUTPUT].setVoltage(modul.cv());

		if (mode != uiModeShown) {
			uiModeShown = mode;
			std::snprintf(uiModeText, sizeof(uiModeText), "%d %s", mode + 1, kModeNames[mode]);
		}
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		honourExternalClock = true;
		hardwareCvWindow = false;
		quantizeModeChanges = true;
		monoInput = false;
		mode = aimedMode = 0;
		uiModeShown = -1;
		modul.resetSequence();
		modul.loadedRhythm = -1;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "externalClock", json_boolean(honourExternalClock));
		json_object_set_new(root, "hardwareCvWindow", json_boolean(hardwareCvWindow));
		json_object_set_new(root, "quantizeModeChanges", json_boolean(quantizeModeChanges));
		json_object_set_new(root, "monoInput", json_boolean(monoInput));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "externalClock"))
			honourExternalClock = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "hardwareCvWindow"))
			hardwareCvWindow = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "quantizeModeChanges"))
			quantizeModeChanges = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "monoInput"))
			monoInput = json_boolean_value(j);
	}
};

// ── the displays ──────────────────────────────────────────────────────────────
// Left names the mode, right reads the time parameter in whatever unit the
// mode uses. Both read a char buffer the audio thread fills, and both open a
// picker on right-click.
struct ArtifexDisplay : Widget {
	Artifex* module = nullptr;
	bool isMode = true;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.0);
		nvgFillColor(args.vg, nvgRGB(0x11, 0x11, 0x11));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x44, 0x44, 0x44));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		std::shared_ptr<Font> font = APP->window->loadFont(
			asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 13.0);
		nvgFillColor(args.vg, nvgRGB(0xff, 0xd5, 0x00));
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		const char* text = "artifex";
		if (module)
			text = isMode ? module->uiModeText : module->uiTimeText;
		nvgText(args.vg, 5.0, box.size.y * 0.5f, text, NULL);
	}

	void onButton(const event::Button& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT && module) {
			e.consume(this);
			showMenu();
			return;
		}
		Widget::onButton(e);
	}

	void showMenu() {
		Menu* menu = createMenu();
		if (isMode) {
			menu->addChild(createMenuLabel("FX mode"));
			Artifex* m = module;
			for (int i = 0; i < 9; i++)
				menu->addChild(createCheckMenuItem(
					string::f("%d %s", i + 1, kModeNames[i]), "",
					[=]() { return m->mode == i; },
					[=]() {
						m->params[Artifex::FXMODE_PARAM].setValue((float)i);
						m->mode = m->aimedMode = i;
					}));
		}
		else {
			menu->addChild(createMenuLabel("Rhythm"));
			Artifex* m = module;
			for (int i = 0; i < 32; i++)
				menu->addChild(createCheckMenuItem(
					string::f("%d", i + 1), "",
					[=]() { return (int)std::round(m->params[Artifex::RHYTHM_PARAM].getValue()) == i; },
					[=]() { m->params[Artifex::RHYTHM_PARAM].setValue((float)i); }));
		}
	}
};

struct ArtifexWidget : ModuleWidget {
	ArtifexWidget(Artifex* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/artifex.svg")));

// @layout:begin artifex 142.24 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FXMODE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TIME_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem FBK_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem AMT_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem FXMODE_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FXMODE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TIME_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FREE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem STEP_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FBK_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FBK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AMT_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem AMT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FILTER_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FILTER_INPUT PJ301MPort 4.01 input "" 0.0
// @elem STEREO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEREO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG_PARAM TL1105 2.6 param "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TEMPO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SYNC_PARAM CKSS 2.3 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LFO_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem LFO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LFO_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRI_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem PULSE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SAW_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem GSW_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem G_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CSW_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem C_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PAT_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem GATE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RHYTHM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RHYTHM_INPUT PJ301MPort 4.01 input "" 0.0
// @elem GAIN_PARAM Trimpot 3.03 param "" 0.0
// @elem LEFT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RIGHT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEFT_LIGHT SmallLight 1.0 light "" 0.0
// @elem RIGHT_LIGHT SmallLight 1.0 light "" 0.0
// @elem IN_L_LIGHT SmallLight 1.0 light "" 0.0
// @elem IN_R_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_FXMODE label 0.0 label "fx mode" 0.0 17.78 37.50
// @elem LABEL_TIME label 0.0 label "time" 0.0 53.34 40.50
// @elem LABEL_FBK label 0.0 label "feedback" 0.0 88.90 40.50
// @elem LABEL_AMT label 0.0 label "amount" 0.0 124.46 40.50
// @elem LABEL_FREE label 0.0 label "free" 0.0 53.34 54.50
// @elem LABEL_STEP label 0.0 label "step" 0.0 63.84 54.50
// @elem LABEL_FILTER label 0.0 label "filter" 0.0 17.78 73.50
// @elem LABEL_STEREO label 0.0 label "stereo" 0.0 53.34 73.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 88.90 73.50
// @elem LABEL_TEMPO label 0.0 label "tempo" 0.0 118.96 73.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 129.96 73.50
// @elem LABEL_SYNC label 0.0 label "sync" 0.0 8.00 77.30
// @elem LABEL_FREERUN label 0.0 label "free" 0.0 8.00 92.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 21.00 91.00
// @elem LABEL_LFO_RESET label 0.0 label "reset" 0.0 71.12 90.00
// @elem BOX_ENV panel_box 7.0 box "" 0.0 57.00 84.50
// @elem LABEL_ENV label 0.0 label "env" 0.0 57.00 90.00
// @elem BOX_TRI panel_box 7.0 box "" 0.0 92.00 84.50
// @elem LABEL_TRI label 0.0 label "tri" 0.0 92.00 90.00
// @elem BOX_PULSE panel_box 7.0 box "" 0.0 108.00 84.50
// @elem LABEL_PULSE label 0.0 label "pulse" 0.0 108.00 90.00
// @elem BOX_SAW panel_box 7.0 box "" 0.0 124.00 84.50
// @elem LABEL_SAW label 0.0 label "saw" 0.0 124.00 90.00
// @elem LABEL_G label 0.0 label "gate ptrn" 0.0 20.75 107.50
// @elem LABEL_C label 0.0 label "cv ptrn" 0.0 42.75 107.50
// @elem LABEL_PAT_RESET label 0.0 label "reset" 0.0 71.12 107.50
// @elem BOX_GATE panel_box 7.0 box "" 0.0 92.00 102.00
// @elem LABEL_GATE label 0.0 label "gate" 0.0 92.00 107.50
// @elem BOX_CV panel_box 7.0 box "" 0.0 108.00 102.00
// @elem LABEL_CV label 0.0 label "cv" 0.0 108.00 107.50
// @elem BOX_CLK_OUT panel_box 7.0 box "" 0.0 124.00 102.00
// @elem LABEL_CLK_OUT label 0.0 label "clk" 0.0 124.00 107.50
// @elem LABEL_RHYTHM label 0.0 label "rhythm" 0.0 18.50 126.50
// @elem LABEL_GAIN label 0.0 label "gain" 0.0 35.00 126.50
// @elem LABEL_IN label 0.0 label "in" 0.0 50.75 126.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 92.00 126.50
// @elem BOX_LEFT panel_box 7.0 box "" 0.0 108.00 120.00
// @elem LABEL_LEFT label 0.0 label "L" 0.0 108.00 125.50
// @elem BOX_RIGHT panel_box 7.0 box "" 0.0 124.00 120.00
// @elem LABEL_RIGHT label 0.0 label "R" 0.0 124.00 125.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 71.12 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(134.62f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(134.62f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(17.78f, 29.00f)), module, Artifex::FXMODE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(53.34f, 29.00f)), module, Artifex::TIME_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(88.90f, 29.00f)), module, Artifex::FBK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(124.46f, 29.00f)), module, Artifex::AMT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(12.78f, 47.00f)), module, Artifex::FXMODE_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.28f, 47.00f)), module, Artifex::FXMODE_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(42.84f, 47.00f)), module, Artifex::TIME_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.34f, 47.00f)), module, Artifex::FREE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(63.84f, 47.00f)), module, Artifex::STEP_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(83.90f, 47.00f)), module, Artifex::FBK_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.40f, 47.00f)), module, Artifex::FBK_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(119.46f, 47.00f)), module, Artifex::AMT_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(128.96f, 47.00f)), module, Artifex::AMT_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.28f, 65.00f)), module, Artifex::FILTER_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(23.28f, 65.00f)), module, Artifex::FILTER_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.84f, 65.00f)), module, Artifex::STEREO_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(58.84f, 65.00f)), module, Artifex::STEREO_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(84.15f, 65.00f)), module, Artifex::TRIG_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.65f, 65.00f)), module, Artifex::TRIG_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(118.96f, 65.00f)), module, Artifex::TEMPO_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(129.96f, 65.00f)), module, Artifex::CLK_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(8.00f, 84.00f)), module, Artifex::SYNC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(21.00f, 82.50f)), module, Artifex::RATE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.00f, 82.50f)), module, Artifex::LFO_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.00f, 82.50f)), module, Artifex::LFO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(71.12f, 82.50f)), module, Artifex::LFO_RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(57.00f, 82.50f)), module, Artifex::ENV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(92.00f, 82.50f)), module, Artifex::TRI_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108.00f, 82.50f)), module, Artifex::PULSE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(124.00f, 82.50f)), module, Artifex::SAW_OUTPUT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(16.00f, 98.00f)), module, Artifex::GSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.50f, 100.00f)), module, Artifex::G_INPUT));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(38.00f, 98.00f)), module, Artifex::CSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.50f, 100.00f)), module, Artifex::C_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(71.12f, 100.00f)), module, Artifex::PAT_RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(92.00f, 100.00f)), module, Artifex::GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108.00f, 100.00f)), module, Artifex::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(124.00f, 100.00f)), module, Artifex::CLK_OUTPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.00f, 118.00f)), module, Artifex::RHYTHM_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.00f, 118.00f)), module, Artifex::RHYTHM_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(35.00f, 118.00f)), module, Artifex::GAIN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.00f, 118.00f)), module, Artifex::LEFT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(56.50f, 118.00f)), module, Artifex::RIGHT_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(92.00f, 118.00f)), module, Artifex::LEVEL_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(108.00f, 118.00f)), module, Artifex::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(124.00f, 118.00f)), module, Artifex::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(113.00f, 115.00f)), module, Artifex::LEFT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(129.00f, 115.00f)), module, Artifex::RIGHT_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(50.00f, 115.00f)), module, Artifex::IN_L_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(61.50f, 115.00f)), module, Artifex::IN_R_LIGHT));
        // @layout:end

		ArtifexDisplay* modeDisp = new ArtifexDisplay;
		modeDisp->module = module;
		modeDisp->isMode = true;
		modeDisp->box.pos = mm2px(Vec(8.f, 9.5f));
		modeDisp->box.size = mm2px(Vec(60.f, 8.f));
		addChild(modeDisp);

		ArtifexDisplay* timeDisp = new ArtifexDisplay;
		timeDisp->module = module;
		timeDisp->isMode = false;
		timeDisp->box.pos = mm2px(Vec(74.24f, 9.5f));
		timeDisp->box.size = mm2px(Vec(60.f, 8.f));
		addChild(timeDisp);
	}

	void appendContextMenu(Menu* menu) override {
		Artifex* m = getModule<Artifex>();
		if (!m)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Mode changes wait for the clock", "",
		                                     &m->quantizeModeChanges));
		menu->addChild(createBoolPtrMenuItem("Sum the inputs to mono", "", &m->monoInput));
		menu->addChild(createBoolPtrMenuItem("Honour the external clock", "",
		                                     &m->honourExternalClock));
		menu->addChild(createBoolPtrMenuItem("Pattern inputs use the hardware window", "",
		                                     &m->hardwareCvWindow));
	}
};

Model* modelArtifex = createModel<Artifex, ArtifexWidget>("artifex");
