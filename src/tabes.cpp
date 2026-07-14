// tabes.cpp — VCV Rack 2 module
// tabes (Latin: "wasting away, decay") is a disintegration looper. Record a
// phrase; on every pass the tape ages: high frequencies dull, mild
// saturation compresses, hiss creeps in, the level sags, and dropouts appear
// more and more often as the loop wears out. Wow/flutter warbles the
// playback. Basinski's Disintegration Loops, as a module.
//
// The degradation is done tape-style: the play head reads the loop, and the
// write head re-records a slightly worse copy in place, so loss accumulates
// pass over pass. A pristine copy of the original recording is kept; SPLICE
// swaps in fresh tape (back to pass zero).
//
// Controls:
//   Knobs : DECAY (loss per pass), WOW (wow/flutter depth)
//   Btns  : REC (toggle recording), SPLICE (restore pristine recording)
//   In    : IN (audio), REC gate, SPLICE trigger, DECAY CV
//   Out   : OUT (audio), AGE (0.1V per pass, clamps at 10V), EOC (trigger)
//   Light : REC (on while recording)

#include "forsitan.hpp"

static constexpr float kMaxSeconds = 30.f;

struct Tabes : Module {
    enum ParamId {
        DECAY_PARAM,
        WOW_PARAM,
        REC_PARAM,
        SPLICE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        REC_GATE_INPUT,
        SPLICE_TRIG_INPUT,
        DECAY_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        AGE_OUTPUT,
        EOC_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        REC_LIGHT,
        EOC_LIGHT,
        OUT_LIGHT,
        LIGHTS_LEN
    };

    std::vector<float> tape;      // the aging loop
    std::vector<float> pristine;  // the recording as it was made
    int loopLen = 0;              // samples in the loop (0 = empty)
    int playPos = 0;
    int recPos = 0;
    bool recording = false;
    bool gateWasHigh = false;
    int age = 0;                  // completed passes since last splice

    float lpState = 0.f;          // per-pass write-head lowpass
    int xfadeTotal = 0;           // seam declick: head/input blend length
    int xfadeRemain = 0;          // samples of blend left (first pass only)
    float declick = 0.f;          // additive bridge over source switches
    float prevOut = 0.f;
    float declickCoef = 0.f;
    bool lastRecording = false;
    float wowPhase = 0.f, flutterPhase = 0.f;
    float dropEnv = 1.f;          // smoothed dropout gain
    int dropTimer = 0;
    enum MonitorMode { MONITOR_WHILE_REC, MONITOR_ALWAYS, MONITOR_NEVER };
    int monitorMode = MONITOR_WHILE_REC;   // context menu

    float curSampleRate = 0.f;
    uint32_t noiseState = 0x6c078965u;
    dsp::BooleanTrigger recButton, spliceButton;
    dsp::SchmittTrigger spliceTrigger;
    dsp::PulseGenerator eocPulse;
    float eocFlash = 0.f;
    float outEnv = 0.f;

    Tabes() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.4f, "Decay per pass");
        configParam(WOW_PARAM, 0.f, 1.f, 0.3f, "Wow/flutter");
        configButton(REC_PARAM, "Record");
        configButton(SPLICE_PARAM, "Splice (restore pristine tape)");
        configInput(AUDIO_INPUT, "Audio");
        configInput(REC_GATE_INPUT, "Record gate");
        configInput(SPLICE_TRIG_INPUT, "Splice trigger");
        configInput(DECAY_CV_INPUT, "Decay CV");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(AGE_OUTPUT, "Age (0.1V per pass)");
        configOutput(EOC_OUTPUT, "End of loop trigger");
        configLight(REC_LIGHT, "Recording");
        configLight(EOC_LIGHT, "End of loop");
        configLight(OUT_LIGHT, "Output level");
    }

    void onReset() override {
        loopLen = 0;
        playPos = recPos = 0;
        recording = false;
        age = 0;
        lpState = 0.f;
        xfadeRemain = 0;
        declick = 0.f;
        prevOut = 0.f;
        lastRecording = false;
        wowPhase = flutterPhase = 0.f;
        dropEnv = 1.f;
        dropTimer = 0;
        eocFlash = 0.f;
        outEnv = 0.f;
        std::fill(tape.begin(), tape.end(), 0.f);
    }

    float noise() {
        uint32_t& s = noiseState;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }

    // uniform [0,1)
    float urand() { return noise() * 0.5f + 0.5f; }

    void startRecording() {
        recording = true;
        recPos = 0;
        age = 0;
        xfadeRemain = 0;
    }

    void stopRecording(float sr) {
        recording = false;
        if (recPos < (int)(0.05f * sr)) {   // too short: keep nothing
            loopLen = 0;
            return;
        }
        loopLen = recPos;
        pristine.assign(tape.begin(), tape.begin() + loopLen);
        playPos = 0;
        // start the write-head filter where the seam ends, not from zero,
        // so pass 2 doesn't get a level dip baked into the loop head
        lpState = tape[loopLen - 1];
        dropEnv = 1.f;
        dropTimer = 0;
        // seam declick: over the next ~10 ms, crossfade the loop head with
        // the live input (see process), so tape[0] follows tape[loopLen-1]
        // as smoothly as the input itself did
        xfadeTotal = std::min((int)(0.01f * sr), loopLen);
        xfadeRemain = xfadeTotal;
    }

    bool splice() {
        if ((int)pristine.size() > 0 && loopLen > 0) {
            std::copy(pristine.begin(), pristine.end(), tape.begin());
            age = 0;
            dropTimer = 0;
            return true;
        }
        return false;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (sr != curSampleRate) {
            curSampleRate = sr;
            tape.assign((size_t)(kMaxSeconds * sr), 0.f);
            pristine.clear();
            declickCoef = std::exp(-1.f / (0.002f * sr));   // ~2 ms bridge
            onReset();
        }

        float in = inputs[AUDIO_INPUT].getVoltage() * 0.2f;   // ±5V -> ±1

        // ── record control: button toggles, gate follows its edges ──────────
        if (recButton.process(params[REC_PARAM].getValue() > 0.5f)) {
            if (recording) stopRecording(sr); else startRecording();
        }
        bool gateHigh = inputs[REC_GATE_INPUT].getVoltage() >= 1.f;
        if (gateHigh && !gateWasHigh && !recording) startRecording();
        if (!gateHigh && gateWasHigh && recording) stopRecording(sr);
        gateWasHigh = gateHigh;

        bool spliced = false;
        if (spliceButton.process(params[SPLICE_PARAM].getValue() > 0.5f)
            || spliceTrigger.process(inputs[SPLICE_TRIG_INPUT].getVoltage(), 0.1f, 1.f))
            spliced = splice();

        float decay = params[DECAY_PARAM].getValue();
        if (inputs[DECAY_CV_INPUT].isConnected())
            decay += inputs[DECAY_CV_INPUT].getVoltage() * 0.2f;
        decay = clamp(decay, 0.f, 1.f);
        float wowK = params[WOW_PARAM].getValue();

        float out = 0.f;

        if (recording) {
            // write straight to tape; monitor the input unless muted
            tape[recPos] = in;
            if (++recPos >= (int)tape.size()) stopRecording(sr);
            if (monitorMode != MONITOR_NEVER)
                out = in;
        } else if (loopLen > 0) {
            // ── seam declick: first pass after stop blends the loop head
            //    with the live input, in tape and pristine alike ───────────────
            if (xfadeRemain > 0) {
                float t = 1.f - (float)xfadeRemain / xfadeTotal;
                tape[playPos] = pristine[playPos]
                              = t * tape[playPos] + (1.f - t) * in;
                xfadeRemain--;
            }

            // ── wow/flutter on the play head ─────────────────────────────────
            wowPhase += 0.6f / sr;
            if (wowPhase >= 1.f) wowPhase -= 1.f;
            flutterPhase += 5.3f / sr;
            if (flutterPhase >= 1.f) flutterPhase -= 1.f;
            float wowAmp = wowK * 0.002f * sr * (1.f + 0.01f * std::min(age, 100));
            float offset = wowAmp * (std::sin(2.f * M_PI * wowPhase)
                          + 0.25f * std::sin(2.f * M_PI * flutterPhase));

            float rp = playPos + offset;
            while (rp < 0.f) rp += loopLen;
            while (rp >= loopLen) rp -= loopLen;
            int i0 = (int)rp;
            float f = rp - i0;
            int i1 = i0 + 1; if (i1 >= loopLen) i1 = 0;
            out = tape[i0] + f * (tape[i1] - tape[i0]);

            // ── the write head re-records a slightly worse copy ─────────────
            float w = tape[playPos];
            // per-pass high-frequency loss: one-pole at 22 kHz .. 2.2 kHz
            float fc = 22000.f * std::pow(10.f, -decay);
            float a = 1.f - std::exp(-2.f * (float)M_PI * fc / sr);
            lpState += a * (w - lpState);
            w = lpState;
            // mild saturation and level sag
            float satMix = 0.5f * decay;
            w = (1.f - satMix) * w + satMix * std::tanh(w);
            w *= 1.f - 0.002f * decay;
            // tape hiss
            w += decay * 2e-4f * noise();
            // dropouts: more likely as the tape ages
            if (dropTimer > 0) {
                dropTimer--;
            } else if (urand() < decay * age * 8e-7f) {
                dropTimer = (int)(sr * (0.005f + 0.02f * urand()));
            }
            dropEnv += (((dropTimer > 0) ? 0.15f : 1.f) - dropEnv) * 0.005f;
            w *= dropEnv;
            if (!std::isfinite(w)) w = 0.f;
            tape[playPos] = w;

            if (++playPos >= loopLen) {
                playPos = 0;
                age++;
                eocPulse.trigger(1e-3f);
                eocFlash = 1.f;
            }
        }

        if (monitorMode == MONITOR_ALWAYS && !recording)
            out += in;

        // ── declick: when the output source switches abruptly (rec start,
        //    rec stop with muted monitor, splice), carry the step over as a
        //    decaying offset instead of a click ────────────────────────────
        if (recording != lastRecording || spliced)
            declick = prevOut - out;
        lastRecording = recording;
        out += declick;
        declick *= declickCoef;
        prevOut = out;

        outputs[AUDIO_OUTPUT].setVoltage(5.f * clamp(out, -2.f, 2.f));
        outputs[AGE_OUTPUT].setVoltage(std::min(0.1f * age, 10.f));
        outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
        lights[REC_LIGHT].setBrightness(recording ? 1.f : 0.f);
        // ~100 ms flash per loop wrap; smoothed audio level on the out badge
        eocFlash *= 1.f - 10.f * args.sampleTime;
        if (eocFlash < 0.f) eocFlash = 0.f;
        lights[EOC_LIGHT].setBrightness(eocFlash);
        outEnv += (std::fabs(out) - outEnv) * 0.002f;
        lights[OUT_LIGHT].setBrightness(clamp(outEnv, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "monitorMode", json_integer(monitorMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "monitorMode"))
            monitorMode = clamp((int)json_integer_value(j), 0, 2);
        else if (json_t* j = json_object_get(root, "monitor"))   // pre-2.7.0 patches
            monitorMode = json_boolean_value(j) ? MONITOR_ALWAYS : MONITOR_WHILE_REC;
    }
};

struct TabesWidget : ModuleWidget {
    TabesWidget(Tabes* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/tabes.svg")));

// @layout:begin tabes 40.64 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem DECAY_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem WOW_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DECAY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REC_PARAM TL1105 2.0 param "" 0.0
// @elem REC_LIGHT SmallLight 1.5 light "" 0.0
// @elem EOC_LIGHT SmallLight 1.5 light "" 0.0
// @elem OUT_LIGHT SmallLight 1.5 light "" 0.0
// @elem SPLICE_PARAM TL1105 2.0 param "" 0.0
// @elem REC_GATE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPLICE_TRIG_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem EOC_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem AGE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 20.32 30.00
// @elem LABEL_WOW label 0.0 label "wow" 0.0 11.50 46.50
// @elem LABEL_DECAYCV label 0.0 label "cv" 0.0 29.14 46.50
// @elem LABEL_REC label 0.0 label "rec" 0.0 11.50 61.00
// @elem LABEL_SPLICE label 0.0 label "splice" 0.0 29.14 61.00
// @elem LABEL_GATE label 0.0 label "gate" 0.0 11.50 76.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 29.14 76.50
// @elem LABEL_IN label 0.0 label "in" 0.0 11.50 95.50
// @elem LABEL_EOC label 0.0 label "eoc" 0.0 29.14 95.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 11.50 114.00
// @elem LABEL_AGE label 0.0 label "age" 0.0 29.14 114.00
// @elem BOX_EOC panel_box 7.0 box "" 0.0 29.14 90.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 11.50 108.50
// @elem BOX_AGE panel_box 7.0 box "" 0.0 29.14 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(20.32f, 18.50f)), module, Tabes::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 38.00f)), module, Tabes::WOW_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.14f, 38.00f)), module, Tabes::DECAY_CV_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(11.50f, 54.00f)), module, Tabes::REC_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(14.40f, 51.10f)), module, Tabes::REC_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(34.14f, 85.00f)), module, Tabes::EOC_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(16.50f, 103.50f)), module, Tabes::OUT_LIGHT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(29.14f, 54.00f)), module, Tabes::SPLICE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.50f, 69.00f)), module, Tabes::REC_GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.14f, 69.00f)), module, Tabes::SPLICE_TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.50f, 88.00f)), module, Tabes::AUDIO_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(29.14f, 88.00f)), module, Tabes::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.50f, 106.50f)), module, Tabes::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(29.14f, 106.50f)), module, Tabes::AGE_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Tabes* m = dynamic_cast<Tabes*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Monitor input",
            {"While recording", "Always", "Never"}, &m->monitorMode));
        menu->addChild(createMenuItem("Clear loop", "", [m]() { m->onReset(); }));
    }
};

Model* modelTabes = createModel<Tabes, TabesWidget>("tabes");
