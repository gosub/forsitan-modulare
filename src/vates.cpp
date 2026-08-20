#include "forsitan.hpp"

// vates — stereo sample player with a pattern generator underneath.
//
// After the Bastl Instruments Citadel Wave Bard: you do not draw a rhythm,
// you modulate sample selection and let the rhythm fall out. The hardware's
// two modifier buttons (SHIFT and BANK, which give every knob two or three
// jobs) are unpacked here into real controls, since holding one thing while
// turning another is a gesture a mouse does badly.
//
// Six banks of eight samples are generated from a seed — drums, objects,
// grains, micro, tones, air — one sample per kind the generator knows, so a
// knob position always means the same role. User kits follow, read from the
// kits folder shared with pellicula. See doc/vates.md.
//
// NOTE: this is the panel and the parameter surface. The engine (sample
// generation, playback, envelope, FX, LFO, pattern generator) lands next.

struct Vates : Module {
	enum ParamId {
		BANK_PARAM,
		BANK_ATT_PARAM,
		SAMPLE_PARAM,
		SAMPLE_ATT_PARAM,
		MODE_PARAM,
		TRIG_PARAM,
		PITCH_PARAM,
		PITCH_ATT_PARAM,
		LENGTH_PARAM,
		LENGTH_ATT_PARAM,
		FILTER_PARAM,
		FX_PARAM,
		SYNC_PARAM,
		RATE_PARAM,
		LFO_ATT_PARAM,
		TEMPO_PARAM,
		RHYTHM_PARAM,
		GSW_PARAM,
		CSW_PARAM,
		LEVEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		BANK_INPUT,
		SAMPLE_INPUT,
		TRIG_INPUT,
		FREE_INPUT,
		NOTE_INPUT,
		LENGTH_INPUT,
		FILTER_INPUT,
		FX_INPUT,
		LFO_INPUT,
		LFO_RESET_INPUT,
		CLK_INPUT,
		G_INPUT,
		C_INPUT,
		PAT_RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENV_OUTPUT,
		TRI_OUTPUT,
		PULSE_OUTPUT,
		CLK_OUTPUT,
		GATE_OUTPUT,
		CV_OUTPUT,
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LEFT_LIGHT,
		RIGHT_LIGHT,
		LIGHTS_LEN
	};

	Vates() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(BANK_PARAM, 0.f, 1.f, 0.f, "Bank");
		configParam(BANK_ATT_PARAM, -1.f, 1.f, 0.f, "Bank CV", "%", 0.f, 100.f);
		configParam(SAMPLE_PARAM, 0.f, 1.f, 0.f, "Sample");
		configParam(SAMPLE_ATT_PARAM, -1.f, 1.f, 0.f, "Sample CV", "%", 0.f, 100.f);
		configSwitch(MODE_PARAM, 0.f, 1.f, 1.f, "Sample modulation", {"cue", "play"});
		configButton(TRIG_PARAM, "Trigger");
		configParam(PITCH_PARAM, -2.f, 2.f, 0.f, "Pitch", " oct");
		configParam(PITCH_ATT_PARAM, -1.f, 1.f, 0.f, "Pitch CV", "%", 0.f, 100.f);
		configParam(LENGTH_PARAM, -1.f, 1.f, 0.f, "Length");
		configParam(LENGTH_ATT_PARAM, -1.f, 1.f, 0.f, "Length CV", "%", 0.f, 100.f);
		configParam(FILTER_PARAM, -1.f, 1.f, 0.f, "Filter");
		configParam(FX_PARAM, -1.f, 1.f, 0.f, "FX");
		configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "LFO clock", {"free", "sync"});
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "LFO rate");
		configParam(LFO_ATT_PARAM, -1.f, 1.f, 0.f, "LFO rate CV", "%", 0.f, 100.f);
		configParam(TEMPO_PARAM, 30.f, 300.f, 120.f, "Tempo", " BPM");
		configParam(RHYTHM_PARAM, 0.f, 31.f, 0.f, "Rhythm");
		getParamQuantity(RHYTHM_PARAM)->snapEnabled = true;
		configSwitch(GSW_PARAM, 0.f, 2.f, 1.f, "Gate pattern", {"invert", "as is", "randomize"});
		configSwitch(CSW_PARAM, 0.f, 2.f, 1.f, "CV pattern", {"invert", "as is", "randomize"});
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");

		configInput(BANK_INPUT, "Bank");
		configInput(SAMPLE_INPUT, "Sample");
		configInput(TRIG_INPUT, "Trigger");
		configInput(FREE_INPUT, "Free pitch (unquantized)");
		configInput(NOTE_INPUT, "Note pitch (quantized, latched at trigger)");
		configInput(LENGTH_INPUT, "Length");
		configInput(FILTER_INPUT, "Filter");
		configInput(FX_INPUT, "FX");
		configInput(LFO_INPUT, "LFO rate");
		configInput(LFO_RESET_INPUT, "LFO reset");
		configInput(CLK_INPUT, "Clock");
		configInput(G_INPUT, "Gate pattern modifier");
		configInput(C_INPUT, "CV pattern modifier");
		configInput(PAT_RESET_INPUT, "Pattern reset");

		configOutput(ENV_OUTPUT, "Envelope");
		configOutput(TRI_OUTPUT, "LFO triangle");
		configOutput(PULSE_OUTPUT, "LFO pulse");
		configOutput(CLK_OUTPUT, "Clock");
		configOutput(GATE_OUTPUT, "Pattern gate");
		configOutput(CV_OUTPUT, "Pattern CV");
		configOutput(LEFT_OUTPUT, "Left");
		configOutput(RIGHT_OUTPUT, "Right");
	}

	void process(const ProcessArgs& args) override {
		// engine to follow
		outputs[LEFT_OUTPUT].setVoltage(0.f);
		outputs[RIGHT_OUTPUT].setVoltage(0.f);
	}
};

struct VatesWidget : ModuleWidget {
	VatesWidget(Vates* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/vates.svg")));

// @layout:begin vates 132.08 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem BANK_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem SAMPLE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem PITCH_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem LENGTH_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem BANK_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem BANK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SAMPLE_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem SAMPLE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PITCH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FREE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem NOTE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LENGTH_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem LENGTH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MODE_PARAM CKSS 2.3 param "" 0.0
// @elem TRIG_PARAM TL1105 2.6 param "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FILTER_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FILTER_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FX_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SYNC_PARAM CKSS 2.3 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LFO_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem LFO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem LFO_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRI_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem PULSE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem TEMPO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RHYTHM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GSW_PARAM CKSSThree 2.3 param "" 0.0
// @elem G_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CSW_PARAM CKSSThree 2.3 param "" 0.0
// @elem C_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PAT_RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem GATE_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LEFT_LIGHT SmallLight 1.0 light "" 0.0
// @elem RIGHT_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_BANK label 0.0 label "bank" 0.0 16.50 40.50
// @elem LABEL_SAMPLE label 0.0 label "sample" 0.0 49.50 40.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 82.50 40.50
// @elem LABEL_LENGTH label 0.0 label "length" 0.0 115.50 40.50
// @elem LABEL_FREE label 0.0 label "free" 0.0 82.50 54.50
// @elem LABEL_NOTE label 0.0 label "note" 0.0 93.00 54.50
// @elem LABEL_CUE label 0.0 label "cue" 0.0 34.00 57.50
// @elem LABEL_MODE label 0.0 label "play" 0.0 34.00 72.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 20.25 71.50
// @elem LABEL_FILTER label 0.0 label "filter" 0.0 49.50 72.50
// @elem LABEL_FX label 0.0 label "fx" 0.0 82.50 72.50
// @elem BOX_ENV panel_box 7.0 box "" 0.0 115.50 66.00
// @elem LABEL_ENV label 0.0 label "env" 0.0 115.50 71.50
// @elem LABEL_SYNC label 0.0 label "sync" 0.0 10.00 74.80
// @elem LABEL_FREERUN label 0.0 label "free" 0.0 10.00 90.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 21.00 90.50
// @elem LABEL_LFO_RESET label 0.0 label "reset" 0.0 51.50 89.50
// @elem BOX_TRI panel_box 7.0 box "" 0.0 70.00 84.00
// @elem LABEL_TRI label 0.0 label "tri" 0.0 70.00 89.50
// @elem BOX_PULSE panel_box 7.0 box "" 0.0 89.00 84.00
// @elem LABEL_PULSE label 0.0 label "pulse" 0.0 89.00 89.50
// @elem LABEL_TEMPO label 0.0 label "tempo" 0.0 105.50 90.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 116.50 89.50
// @elem LABEL_GSW label 0.0 label "G" 0.0 24.00 95.50
// @elem LABEL_CSW label 0.0 label "C" 0.0 44.00 95.50
// @elem LABEL_RHYTHM label 0.0 label "rhythm" 0.0 11.50 110.50
// @elem LABEL_G label 0.0 label "G in" 0.0 33.50 109.50
// @elem LABEL_C label 0.0 label "C in" 0.0 53.50 109.50
// @elem LABEL_PAT_RESET label 0.0 label "reset" 0.0 65.00 109.50
// @elem BOX_GATE panel_box 7.0 box "" 0.0 80.00 104.00
// @elem LABEL_GATE label 0.0 label "gate" 0.0 80.00 109.50
// @elem BOX_CV panel_box 7.0 box "" 0.0 96.00 104.00
// @elem LABEL_CV label 0.0 label "cv" 0.0 96.00 109.50
// @elem BOX_CLK_OUT panel_box 7.0 box "" 0.0 112.00 104.00
// @elem LABEL_CLK_OUT label 0.0 label "clk" 0.0 112.00 109.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 25.00 126.50
// @elem BOX_LEFT panel_box 7.0 box "" 0.0 85.00 120.00
// @elem LABEL_LEFT label 0.0 label "L" 0.0 85.00 125.50
// @elem BOX_RIGHT panel_box 7.0 box "" 0.0 105.00 120.00
// @elem LABEL_RIGHT label 0.0 label "R" 0.0 105.00 125.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 66.04 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(16.50f, 29.00f)), module, Vates::BANK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(49.50f, 29.00f)), module, Vates::SAMPLE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(82.50f, 29.00f)), module, Vates::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(115.50f, 29.00f)), module, Vates::LENGTH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(11.50f, 47.00f)), module, Vates::BANK_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(21.00f, 47.00f)), module, Vates::BANK_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(44.50f, 47.00f)), module, Vates::SAMPLE_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.00f, 47.00f)), module, Vates::SAMPLE_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(72.00f, 47.00f)), module, Vates::PITCH_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(82.50f, 47.00f)), module, Vates::FREE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.00f, 47.00f)), module, Vates::NOTE_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(110.50f, 47.00f)), module, Vates::LENGTH_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(120.00f, 47.00f)), module, Vates::LENGTH_INPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(34.00f, 64.00f)), module, Vates::MODE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(15.50f, 64.00f)), module, Vates::TRIG_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.00f, 64.00f)), module, Vates::TRIG_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(49.50f, 64.00f)), module, Vates::FILTER_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.50f, 64.00f)), module, Vates::FILTER_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(82.50f, 64.00f)), module, Vates::FX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.50f, 64.00f)), module, Vates::FX_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(115.50f, 64.00f)), module, Vates::ENV_OUTPUT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(10.00f, 82.00f)), module, Vates::SYNC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(21.00f, 82.00f)), module, Vates::RATE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(32.00f, 82.00f)), module, Vates::LFO_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.50f, 82.00f)), module, Vates::LFO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(51.50f, 82.00f)), module, Vates::LFO_RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(70.00f, 82.00f)), module, Vates::TRI_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(89.00f, 82.00f)), module, Vates::PULSE_OUTPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(105.50f, 82.00f)), module, Vates::TEMPO_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.50f, 82.00f)), module, Vates::CLK_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 102.00f)), module, Vates::RHYTHM_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(24.00f, 102.00f)), module, Vates::GSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.50f, 102.00f)), module, Vates::G_INPUT));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(44.00f, 102.00f)), module, Vates::CSW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.50f, 102.00f)), module, Vates::C_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.00f, 102.00f)), module, Vates::PAT_RESET_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(80.00f, 102.00f)), module, Vates::GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(96.00f, 102.00f)), module, Vates::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(112.00f, 102.00f)), module, Vates::CLK_OUTPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.00f, 118.00f)), module, Vates::LEVEL_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(85.00f, 118.00f)), module, Vates::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(105.00f, 118.00f)), module, Vates::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(90.00f, 115.00f)), module, Vates::LEFT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(110.00f, 115.00f)), module, Vates::RIGHT_LIGHT));
        // @layout:end
	}
};


Model* modelVates = createModel<Vates, VatesWidget>("vates");
