// sylla.cpp — VCV Rack 2 module
// sylla (diminutive of Latin syllaba, "syllable") is the small voice of
// the imber pair: a random sample generator and player. Pick a family
// (drone, pad, fragment, bell, ambient, glitch, karplus, skip, micro),
// press GEN, and a worker thread renders a brand new sample from the
// shared procedural generator library — nothing is ever loaded from
// disk, every sound is spoken fresh. The previous sample keeps playing
// until the new one is ready.
//
// Controls:
//   Knobs : FAMILY (snap), SPEED (0.1–2x, CV adds 1 V/oct), LEN (play
//           window from the start of the buffer), LEVEL
//   Switch: LOOP (one-shot / loop), GATE (trig / gate mode)
//   Button: GEN (with busy LED)
//   In    : GEN trigger, SPEED CV, TRIG (fire / retrigger; gate in
//           gate mode; unpatched + LOOP = free-running)
//   Out   : OUT (mono, level LED), EOC trigger (fires at each window
//           end / loop wrap)
//
// True to Haiku's "built from nothing" spirit the sample itself is not
// saved with the patch — only its seed is, so a reload regenerates the
// exact same sound.

#include "forsitan.hpp"
#include "imber/imber_gen.hpp"
#include <thread>
#include <atomic>
#include <memory>

struct Sylla : Module {
    enum ParamId {
        FAMILY_PARAM,
        GEN_PARAM,
        SPEED_PARAM,
        LEN_PARAM,
        LEVEL_PARAM,
        LOOP_PARAM,   // 1 = loop
        GATE_PARAM,   // 1 = gate mode
        PARAMS_LEN
    };
    enum InputId {
        GEN_INPUT,
        SPEED_INPUT,
        TRIG_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_OUTPUT,
        EOC_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        BUSY_LIGHT,
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    // a render in flight: the detached worker only touches this object,
    // which both sides hold via shared_ptr, so teardown is always safe
    struct Job {
        std::atomic<bool> done;
        std::vector<float> buf;
        Job() : done(false) {}
    };

    std::vector<float> buffer;
    std::shared_ptr<Job> job;
    uint64_t sampleSeed = 0;

    float pos = 0.f;
    bool playing = false;
    float env = 0.f;          // declick on gate stop
    float levelEnv = 0.f;
    dsp::SchmittTrigger genTrig, playTrig;
    dsp::BooleanTrigger genButton;
    dsp::PulseGenerator eocPulse;
    bool pendingRender = false;

    Sylla() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configSwitch(FAMILY_PARAM, 0.f, 8.f, 0.f, "Family",
            {"Drone", "Pad", "Fragment", "Bell", "Ambient", "Glitch",
             "Karplus", "Skip", "Micro"});
        configButton(GEN_PARAM, "Generate a new sample");
        configParam(SPEED_PARAM, 0.1f, 2.f, 1.f, "Speed", "x");
        configParam(LEN_PARAM, 0.02f, 1.f, 1.f, "Play window", "%", 0.f, 100.f);
        configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level", "%", 0.f, 100.f);
        configSwitch(LOOP_PARAM, 0.f, 1.f, 1.f, "Play mode", {"One-shot", "Loop"});
        configSwitch(GATE_PARAM, 0.f, 1.f, 0.f, "Trigger mode", {"Trigger", "Gate"});
        configInput(GEN_INPUT, "Generate trigger");
        configInput(SPEED_INPUT, "Speed (1 V/oct around the knob)");
        configInput(TRIG_INPUT, "Trigger / gate");
        configOutput(OUT_OUTPUT, "Audio");
        configOutput(EOC_OUTPUT, "End of cycle");
    }

    void startRender(float sr, uint64_t seed) {
        if (job)   // a render is already in flight; latest request wins later
            return;
        sampleSeed = seed;
        int family = (int)std::round(params[FAMILY_PARAM].getValue());
        std::shared_ptr<Job> j(new Job());
        job = j;
        std::thread([j, family, sr, seed]() {
            imber_dsp::Rng rng;
            rng.seed(seed);
            imber_gen::renderFamily(family, rng, sr, j->buf);
            j->done.store(true);
        }).detach();
    }

    void onReset() override {
        buffer.clear();
        playing = false;
        pos = 0.f;
        sampleSeed = 0;
        pendingRender = true;
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "sampleSeed", json_integer((json_int_t)sampleSeed));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j = json_object_get(rootJ, "sampleSeed");
        if (j) {
            sampleSeed = (uint64_t)json_integer_value(j);
            pendingRender = true;
        }
    }

    void process(const ProcessArgs& args) override {
        // first sample ever, or a reload: regenerate from the saved seed
        if (buffer.empty() && !job && !pendingRender)
            pendingRender = true;
        if (pendingRender && !job) {
            pendingRender = false;
            startRender(args.sampleRate,
                        sampleSeed ? sampleSeed : (uint64_t)random::u64());
        }

        bool genHit = genButton.process(params[GEN_PARAM].getValue() > 0.5f);
        genHit |= genTrig.process(inputs[GEN_INPUT].getVoltage(), 0.1f, 1.f);
        if (genHit && !job)
            startRender(args.sampleRate, (uint64_t)random::u64());

        // collect a finished render (vector swap, no allocation here)
        if (job && job->done.load()) {
            buffer.swap(job->buf);
            job.reset();
            if (pos >= (float)buffer.size())
                pos = 0.f;
            // a fresh sample on a silent one-shot plays itself once
            if (!playing && !inputs[TRIG_INPUT].isConnected()) {
                playing = true;
                pos = 0.f;
            }
        }
        lights[BUSY_LIGHT].setBrightness(job ? 1.f : 0.f);

        bool loop = params[LOOP_PARAM].getValue() > 0.5f;
        bool gateMode = params[GATE_PARAM].getValue() > 0.5f;
        bool connected = inputs[TRIG_INPUT].isConnected();
        bool gateHigh = inputs[TRIG_INPUT].getVoltage() >= 1.f;
        if (playTrig.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) {
            playing = true;
            pos = 0.f;
        }
        bool wantPlay = playing;
        if (gateMode && connected)
            wantPlay = playing && gateHigh;
        if (!connected && loop)
            wantPlay = true;   // free-running loop when nothing is patched

        float out = 0.f;
        if (!buffer.empty() && (wantPlay || env > 0.001f)) {
            int len = (int)buffer.size();
            float win = std::max(0.03f * args.sampleRate,
                                 params[LEN_PARAM].getValue() * len);
            win = std::min(win, (float)len);
            if (pos >= win) {
                eocPulse.trigger(1e-3f);
                if (loop)
                    pos -= win;
                else {
                    playing = false;
                    pos = 0.f;
                }
            }
            wantPlay = playing;
            if (gateMode && connected)
                wantPlay = playing && gateHigh;
            if (!connected && loop)
                wantPlay = true;
            env += ((wantPlay ? 1.f : 0.f) - env) * (1.f / (0.003f * args.sampleRate));
            if (wantPlay || env > 0.001f) {
                int i0 = (int)pos;
                int i1 = std::min(i0 + 1, len - 1);
                float fr = pos - i0;
                float smp = buffer[i0] + (buffer[i1] - buffer[i0]) * fr;
                // window edge fades on top of the buffer's own
                float fadeIn = std::min(1.f, pos / (0.003f * args.sampleRate));
                float fadeOut = std::min(1.f, (win - pos)
                                              / (0.01f * args.sampleRate));
                out = smp * env * fadeIn * imber_dsp::clampf(fadeOut, 0.f, 1.f);
                float rate = params[SPEED_PARAM].getValue()
                    * std::pow(2.f, inputs[SPEED_INPUT].getVoltage());
                pos += imber_dsp::clampf(rate, 0.05f, 8.f);
            }
        }

        float v = out * params[LEVEL_PARAM].getValue() * 5.f;
        outputs[OUT_OUTPUT].setVoltage(v);
        outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
        levelEnv += (std::fabs(v * 0.2f) - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }
};

struct SyllaWidget : ModuleWidget {
    SyllaWidget(Sylla* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/sylla.svg")));

// @layout:begin sylla 40.64 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FAMILY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem GEN_PARAM TL1105 2.6 param "" 0.0
// @elem SPEED_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEN_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LOOP_PARAM CKSS 2.0 param "" 0.0
// @elem GATE_PARAM CKSS 2.0 param "" 0.0
// @elem GEN_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPEED_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.18 input "" 0.0
// @elem OUT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem EOC_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem BUSY_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_FAMILY label 0.0 label "family" 0.0 12.60 29.50
// @elem LABEL_GEN label 0.0 label "gen" 0.0 30.00 26.00
// @elem LABEL_SPEED label 0.0 label "speed" 0.0 12.60 49.50
// @elem LABEL_LEN label 0.0 label "len" 0.0 28.00 49.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 12.60 68.50
// @elem LABEL_LOOP label 0.0 label "loop" 0.0 24.00 68.50
// @elem LABEL_GATE label 0.0 label "gate" 0.0 35.00 68.50
// @elem LABEL_GENIN label 0.0 label "gen" 0.0 8.00 87.50
// @elem LABEL_SPD label 0.0 label "spd" 0.0 20.32 87.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 32.60 87.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 12.40 114.00
// @elem LABEL_EOC label 0.0 label "eoc" 0.0 28.20 114.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 12.40 108.50
// @elem BOX_EOC panel_box 7.0 box "" 0.0 28.20 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(30.48f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(30.48f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.60f, 21.00f)), module, Sylla::FAMILY_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(30.00f, 19.00f)), module, Sylla::GEN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.60f, 41.00f)), module, Sylla::SPEED_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(28.00f, 41.00f)), module, Sylla::LEN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.60f, 60.00f)), module, Sylla::LEVEL_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(24.00f, 60.00f)), module, Sylla::LOOP_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(35.00f, 60.00f)), module, Sylla::GATE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.00f, 80.00f)), module, Sylla::GEN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 80.00f)), module, Sylla::SPEED_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.60f, 80.00f)), module, Sylla::TRIG_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.40f, 106.50f)), module, Sylla::OUT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(28.20f, 106.50f)), module, Sylla::EOC_OUTPUT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(33.20f, 15.80f)), module, Sylla::BUSY_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(17.40f, 103.50f)), module, Sylla::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelSylla = createModel<Sylla, SyllaWidget>("sylla");
