// sylla.cpp — VCV Rack 2 module
// sylla (diminutive of Latin syllaba, "syllable") is the small voice of
// the imber pair: a random sample generator and player. Pick an engine,
// press GEN, and a worker thread renders a brand new sample from the
// shared procedural generator library — nothing is ever loaded from
// disk, every sound is spoken fresh. The previous sample keeps playing
// until the new one is ready.
//
// ENGINE is a rotary switch over the generators themselves, one per
// position, grouped and named by family (drone, pad, air, bell, pluck,
// phrase, dust, broken, micro) and ordered bed to point, with a weighted
// random as the last stop. It used to select a family and roll one of its
// members at every render, which made the knob dishonest: its position did
// not determine the sound, so wanting another take on what you just heard
// was impossible. Now GEN only changes the seed.
//
// Controls:
//   Knobs : ENGINE (snap), SPEED (0.1–2x, CV adds 1 V/oct), LEN (play
//           window from the start of the buffer), LEVEL
//   Switch: LOOP (one-shot / loop), GATE (trig / gate mode)
//   Button: GEN (with busy LED), PLAY (a trigger and a gate source of
//           its own, so the module plays with nothing patched)
//   In    : GEN trigger, SPEED CV, TRIG / gate. The transport is the
//           LOOP x GATE square:
//              one-shot + trig : an edge plays the window once
//              one-shot + gate : plays while the gate is high
//              loop     + trig : an edge toggles the loop on / off
//              loop     + gate : loops while the gate is high
//           Generating a sample never starts playback by itself; the
//           default state is one-shot + trigger, so a fresh sylla waits
//           for a press. Retriggers and new samples landing under the
//           playhead hand over through a short crossfade.
//   Out   : OUT (mono, level LED), EOC trigger (fires at each window
//           end / loop wrap)
//
// True to Haiku's "built from nothing" spirit the sample itself is not
// saved with the patch — only its seed is, so a reload regenerates the
// exact same sound.

#include "forsitan.hpp"
#include "imber/imber_gen.hpp"
#include "imber/imber_worker.hpp"
#include <thread>
#include <atomic>
#include <memory>

// crossfade time for playhead jumps (retrigger, or a new sample landing)
static const float SYLLA_XFADE_SEC = 0.004f;

// v1, the 2.9 taxonomy: ten positions, each a family the render rolled a
// member of. Kept so patches saved under it still reproduce.
static const std::vector<std::string> SYLLA_FAMILIES_V1 = {
    "Drone", "Pad", "Fragment", "Bell", "Ambient", "Glitch",
    "Karplus", "Skip", "Micro", "Random"
};

// The knob is a rotary switch over the engines themselves, so its position
// decides the sound and GEN only changes the seed. Families survive as the
// grouping that orders and names the positions.
static std::vector<std::string> syllaEngineLabels() {
    std::vector<std::string> v;
    int n;
    const imber_gen::EngineEntry* t = imber_gen::engineTable(&n);
    for (int i = 0; i < n; i++)
        v.push_back(t[i].name);
    v.push_back("Random");
    return v;
}

struct Sylla : Module {
    enum ParamId {
        FAMILY_PARAM,
        GEN_PARAM,
        TRIG_PARAM,
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

    // 0 = v1 (legacy, 2.9), 1 = v2. New modules get v2; a patch with no
    // familySet key predates the choice and must stay on v1. The buffer is
    // always seed x settings, so changing this re-renders the same seed
    // rather than waiting for the next GEN: what you hear is what a reload
    // would bring back.
    int familySet = 1;

    // which engines the last knob position may roll, one bit each, all of
    // them by default. v2 only: v1's random derives its family from the
    // seed and is left exactly as it was.
    uint32_t randomPool = imber_gen::kAllEngines;

    // what the generators pitch to. The defaults are the tuning the whole
    // library was written against, so a patch with neither key reproduces
    // its sound exactly.
    int rootNote = imber_dsp::kDefaultRoot;
    int scaleIndex = imber_dsp::kDefaultScale;

    float pos = 0.f;
    bool playing = false;
    float env = 0.f;          // declick on gate stop
    float levelEnv = 0.f;
    dsp::SchmittTrigger genTrig, playTrig;
    dsp::BooleanTrigger genButton, trigButton;
    dsp::PulseGenerator eocPulse;
    bool pendingRender = false;
    bool running = true;      // loop + trigger mode: the run latch
    bool oneShotDone = false; // one-shot + gate mode: window already spoken

    // Any jump in the playhead — a retrigger, or a fresh sample landing
    // under it — is a step, and a step is a click. Both go through a
    // short crossfade instead: the outgoing audio keeps playing from a
    // second read head while the new one comes up under it. tailSrc is
    // the main buffer for a retrigger, or tailBuf (holding the sample
    // just replaced) when a render lands.
    std::vector<float> tailBuf;
    const std::vector<float>* tailSrc = nullptr;
    float tailPos = 0.f;
    float tailEnv = 0.f;
    float xfade = 1.f;        // 0 = all tail, 1 = all main

    Sylla() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // The range is the wider of the two selections, always. Rack restores
        // params before dataFromJson runs, so a v2 patch would lose its
        // position if the knob were still narrow at load; starting wide and
        // narrowing afterwards is safe in both directions, since a legacy
        // value can only be 0..9.
        configSwitch(FAMILY_PARAM, 0.f, (float)imber_gen::engineCount(), 0.f,
                     "Engine", syllaEngineLabels());
        configButton(GEN_PARAM, "Generate a new sample");
        configButton(TRIG_PARAM, "Play / retrigger");
        configParam(SPEED_PARAM, 0.1f, 2.f, 1.f, "Speed", "x");
        configParam(LEN_PARAM, 0.02f, 1.f, 1.f, "Play window", "%", 0.f, 100.f);
        configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level", "%", 0.f, 100.f);
        configSwitch(LOOP_PARAM, 0.f, 1.f, 0.f, "Play mode", {"One-shot", "Loop"});
        configSwitch(GATE_PARAM, 0.f, 1.f, 0.f, "Trigger mode", {"Trigger", "Gate"});
        configInput(GEN_INPUT, "Generate trigger");
        configInput(SPEED_INPUT, "Speed (1 V/oct around the knob)");
        configInput(TRIG_INPUT, "Trigger / gate");
        configOutput(OUT_OUTPUT, "Audio");
        configOutput(EOC_OUTPUT, "End of cycle");
    }

    // The knob is 28 engines in v2 and 10 families in v1 legacy, so both its
    // detent count and its tooltip names follow the selection in force.
    void applyFamilyLabels() {
        SwitchQuantity* q =
            dynamic_cast<SwitchQuantity*>(paramQuantities[FAMILY_PARAM]);
        if (!q)
            return;
        if (familySet) {
            q->labels = syllaEngineLabels();
            q->maxValue = (float)imber_gen::engineCount();
        }
        else {
            q->labels = SYLLA_FAMILIES_V1;
            q->maxValue = (float)(imber_gen::FAM_COUNT);
            // coming from the wider knob, park anywhere past the legacy
            // positions on its last one rather than leaving it out of range
            if (params[FAMILY_PARAM].getValue() > q->maxValue)
                params[FAMILY_PARAM].setValue(q->maxValue);
        }
    }

    // a setting that feeds the generators changed: rebuild this very sample
    // from its own seed so the sound and the patch stay in agreement
    void reRenderCurrent() {
        applyFamilyLabels();
        pendingRender = true;
    }

    // true when the knob sits on the roll rather than on a named engine,
    // which is the only case where the pool feeds the current sample
    bool onRandom() {
        return familySet
               && (int)std::round(params[FAMILY_PARAM].getValue())
                      >= imber_gen::engineCount();
    }

    // hand the currently sounding audio to the tail voice, which fades it
    // out while whatever comes next fades in
    void beginXfade(const std::vector<float>* src) {
        tailSrc = src;
        tailPos = pos;
        tailEnv = env;
        xfade = 0.f;
    }

    void startRender(float sr, uint64_t seed) {
        if (job)   // a render is already in flight; latest request wins later
            return;
        sampleSeed = seed;
        int pos = (int)std::round(params[FAMILY_PARAM].getValue());
        int set = familySet;
        uint32_t pool = randomPool;
        imber_dsp::Tuning tune = imber_dsp::makeTuning(scaleIndex, rootNote);
        std::shared_ptr<Job> j(new Job());
        job = j;
        // startDetached, not std::thread: this runs on the audio thread and
        // the worker must not inherit its realtime policy, or RLIMIT_RTTIME
        // kills Rack partway through a slow render. See imber_worker.hpp.
        imber_worker::startDetached([j, pos, set, pool, tune, sr, seed]() {
            imber_dsp::Rng rng;
            rng.seed(seed);
            rng.tune = tune;
            if (set == 0) {
                // v1 "random" picks a family from the seed itself, so a
                // reload from the saved seed regenerates the same sound
                int f = pos;
                if (f >= imber_gen::FAM_COUNT)
                    f = (int)(seed % (uint64_t)imber_gen::FAM_COUNT);
                imber_gen::renderFamily(f, rng, sr, j->buf);
            }
            else if (pos >= imber_gen::engineCount())
                imber_gen::renderLoop2(rng, sr, j->buf, pool);   // last position
            else
                imber_gen::renderEngine(pos, rng, sr, j->buf);
            j->done.store(true);
        });
    }

    void onReset() override {
        buffer.clear();
        playing = false;
        running = true;
        oneShotDone = false;
        tailSrc = nullptr;
        xfade = 1.f;
        pos = 0.f;
        sampleSeed = 0;
        familySet = 1;
        randomPool = imber_gen::kAllEngines;
        rootNote = imber_dsp::kDefaultRoot;
        scaleIndex = imber_dsp::kDefaultScale;
        applyFamilyLabels();
        pendingRender = true;
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "sampleSeed", json_integer((json_int_t)sampleSeed));
        json_object_set_new(rootJ, "running", json_boolean(running));
        json_object_set_new(rootJ, "familySet", json_integer(familySet));
        json_object_set_new(rootJ, "randomPool", json_integer((json_int_t)randomPool));
        json_object_set_new(rootJ, "root", json_integer(rootNote));
        json_object_set_new(rootJ, "scale", json_integer(scaleIndex));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j = json_object_get(rootJ, "sampleSeed");
        if (j) {
            sampleSeed = (uint64_t)json_integer_value(j);
            pendingRender = true;
        }
        json_t* r = json_object_get(rootJ, "running");
        if (r)
            running = json_boolean_value(r);
        // no key means a patch from before the v2 set existed: its seed only
        // reproduces under the taxonomy it was rendered with
        json_t* f = json_object_get(rootJ, "familySet");
        familySet = f ? clamp((int)json_integer_value(f), 0, 1) : 0;
        json_t* rp = json_object_get(rootJ, "randomPool");
        if (rp)
            randomPool = (uint32_t)json_integer_value(rp);
        json_t* rt = json_object_get(rootJ, "root");
        if (rt)
            rootNote = clamp((int)json_integer_value(rt), 0, 11);
        json_t* sc = json_object_get(rootJ, "scale");
        if (sc)
            scaleIndex = clamp((int)json_integer_value(sc), 0,
                               imber_dsp::kScaleCount - 1);
        applyFamilyLabels();
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

        // collect a finished render (vector swaps, no allocation here).
        // The outgoing sample is parked in tailBuf so the crossfade can
        // keep reading it after the new one takes its place.
        if (job && job->done.load()) {
            tailBuf.swap(buffer);
            buffer.swap(job->buf);
            job.reset();
            beginXfade(&tailBuf);
            if (pos >= (float)buffer.size())
                pos = 0.f;
        }
        lights[BUSY_LIGHT].setBrightness(job ? 1.f : 0.f);

        bool loop = params[LOOP_PARAM].getValue() > 0.5f;
        bool gateMode = params[GATE_PARAM].getValue() > 0.5f;
        // the button is a trigger and a gate source in its own right, so
        // everything below works with no cable patched
        bool btnHeld = params[TRIG_PARAM].getValue() > 0.5f;
        bool gateHigh = inputs[TRIG_INPUT].getVoltage() >= 1.f || btnHeld;
        bool trigEdge = playTrig.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
        trigEdge |= trigButton.process(btnHeld);

        // a retrigger crossfades from the window it interrupts, reading
        // the same buffer from where the playhead was
        auto restart = [&]() {
            beginXfade(&buffer);
            pos = 0.f;
        };

        //          | trigger mode                 | gate mode
        //  one-shot| edge plays the window once   | plays while the gate is high
        //  loop    | edge toggles the loop on/off | loops while the gate is high
        if (gateMode) {
            if (trigEdge) {
                restart();      // a rising gate always restarts the window
                oneShotDone = false;
            }
            playing = gateHigh && !(!loop && oneShotDone);
        }
        else if (loop) {
            if (trigEdge) {
                running = !running;
                if (running)
                    restart();
            }
            playing = running;
        }
        else {
            if (trigEdge) {
                playing = true;
                restart();
            }
        }
        bool wantPlay = playing;

        // one rate for both read heads, so the tail keeps its pitch
        float rate = imber_dsp::clampf(params[SPEED_PARAM].getValue()
            * std::pow(2.f, inputs[SPEED_INPUT].getVoltage()), 0.05f, 8.f);

        float out = 0.f;
        if (!buffer.empty() && (wantPlay || env > 0.001f)) {
            int len = (int)buffer.size();
            float win = std::max(0.03f * args.sampleRate,
                                 params[LEN_PARAM].getValue() * len);
            win = std::min(win, (float)len);
            // Looping wraps through an equal-power seam instead of jumping.
            // Two things stand in the way of a clean wrap: the jump itself,
            // and the 25 ms fade every generated buffer carries at both
            // ends, which put a hole in the sound once per lap. So a loop
            // starts its laps past that fade-in (head) and crossfades the
            // head back in under the tail (xf), the way imber's read heads
            // do. One-shot keeps both buffer fades: there they are the
            // sample's own attack and release, which is what you want.
            float head = loop ? std::min(0.025f * args.sampleRate, 0.1f * win)
                              : 0.f;
            float span = win - head;
            float xf = loop ? std::min(0.025f * args.sampleRate, span * 0.25f)
                            : 0.f;
            if (pos >= win) {
                eocPulse.trigger(1e-3f);
                if (loop) {
                    pos -= (span - xf);
                    if (pos >= win)     // LEN just shrank under the playhead
                        pos = head;
                }
                else {
                    // the window has been spoken; in gate mode it stays
                    // quiet until the gate falls and rises again
                    playing = false;
                    oneShotDone = true;
                    pos = 0.f;
                }
            }
            wantPlay = playing;
            env +=((wantPlay ? 1.f : 0.f) - env) * (1.f / (0.003f * args.sampleRate));
            if (wantPlay || env > 0.001f) {
                auto readAt = [&](float p) {
                    p = imber_dsp::clampf(p, 0.f, (float)(len - 1));
                    int i0 = (int)p;
                    int i1 = std::min(i0 + 1, len - 1);
                    return buffer[i0] + (buffer[i1] - buffer[i0]) * (p - i0);
                };
                float smp = readAt(pos);
                float ov = pos - (win - xf);
                if (xf >= 1.f && ov > 0.f) {
                    float u = imber_dsp::clampf(ov / xf, 0.f, 1.f);
                    smp = smp * std::cos(u * M_PI * 0.5f)
                          + readAt(head + ov) * std::sin(u * M_PI * 0.5f);
                }
                // the first lap still opens on the buffer's own fade-in, so
                // this only softens a mid-buffer start; the window tail fade
                // is one-shot only, the seam covers the looping case
                float fadeIn = std::min(1.f, pos / (0.003f * args.sampleRate));
                float fadeOut = loop ? 1.f
                    : std::min(1.f, (win - pos) / (0.01f * args.sampleRate));
                out = smp * env * fadeIn * imber_dsp::clampf(fadeOut, 0.f, 1.f);
                pos += rate;
            }
        }

        // crossfade: the interrupted audio still plays from the tail head
        // while the new window comes up under it
        if (xfade < 1.f) {
            float tail = 0.f;
            if (tailSrc && tailEnv > 0.001f) {
                int tlen = (int)tailSrc->size();
                if (tailPos >= 0.f && tailPos < (float)(tlen - 1)) {
                    int i0 = (int)tailPos;
                    float fr = tailPos - i0;
                    const std::vector<float>& tb = *tailSrc;
                    tail = (tb[i0] + (tb[i0 + 1] - tb[i0]) * fr) * tailEnv;
                }
            }
            out = out * xfade + tail * (1.f - xfade);
            tailPos += rate;
            xfade += args.sampleTime / SYLLA_XFADE_SEC;
            if (xfade >= 1.f) {
                xfade = 1.f;
                tailSrc = nullptr;
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
// @elem TRIG_PARAM TL1105 2.6 param "" 0.0
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
// @elem LABEL_FAMILY label 0.0 label "engine" 0.0 12.60 29.50
// @elem LABEL_GEN label 0.0 label "gen" 0.0 30.00 26.00
// @elem LABEL_SPEED label 0.0 label "speed" 0.0 12.60 49.50
// @elem LABEL_LEN label 0.0 label "len" 0.0 28.00 49.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 12.60 68.50
// @elem LABEL_LOOP label 0.0 label "loop" 0.0 24.00 68.50
// @elem LABEL_GATE label 0.0 label "gate" 0.0 35.00 68.50
// @elem LABEL_GENIN label 0.0 label "gen" 0.0 8.00 87.50
// @elem LABEL_SPD label 0.0 label "spd" 0.0 20.32 87.50
// @elem LABEL_TRIG label 0.0 label "trig" 0.0 32.60 87.50
// @elem LABEL_PLAY label 0.0 label "play" 0.0 32.60 99.00
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
        addParam(createParamCentered<TL1105>(mm2px(Vec(32.60f, 92.00f)), module, Sylla::TRIG_PARAM));
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

    void appendContextMenu(Menu* menu) override {
        Sylla* m = dynamic_cast<Sylla*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        // the knob has 28 detents, so offer the same list as a menu for when
        // you know exactly which engine you want
        if (m->familySet) {
            menu->addChild(createIndexSubmenuItem("Engine", syllaEngineLabels(),
                [m]() {
                    return (int)std::round(
                        m->params[Sylla::FAMILY_PARAM].getValue());
                },
                [m](int i) {
                    m->params[Sylla::FAMILY_PARAM].setValue((float)i);
                }));
        }

        // which engines the last knob position may land on. Only the loop
        // pool is eligible, so the two self-finishing one-shots are not
        // listed: they are reachable by name and never by a roll.
        if (m->familySet) {
            menu->addChild(createSubmenuItem("Random pool", "", [m](Menu* sub) {
                sub->addChild(createMenuItem("Enable all", "", [m]() {
                    m->randomPool = imber_gen::kAllEngines;
                    if (m->onRandom()) m->reRenderCurrent();
                }));
                sub->addChild(new MenuSeparator);
                int n;
                const imber_gen::EngineEntry* t = imber_gen::engineTable(&n);
                for (int i = 0; i < n; i++) {
                    if (t[i].weight <= 0.f)
                        continue;
                    sub->addChild(createCheckMenuItem(t[i].name, "",
                        [m, i]() { return (m->randomPool & (1u << i)) != 0; },
                        [m, i]() {
                            m->randomPool ^= (1u << i);
                            if (m->onRandom()) m->reRenderCurrent();
                        }));
                }
            }));
        }

        menu->addChild(createIndexSubmenuItem("Generator selection",
            {"engines (one per knob position)",
             "v1 legacy families (2.9, knob rolls a member)"},
            [m]() { return m->familySet ? 0 : 1; },
            [m](int i) {
                m->familySet = i ? 0 : 1;
                m->reRenderCurrent();
            }));

        std::vector<std::string> notes;
        for (int i = 0; i < 12; i++)
            notes.push_back(imber_dsp::noteName(i));
        menu->addChild(createIndexSubmenuItem("Root", notes,
            [m]() { return m->rootNote; },
            [m](int i) { m->rootNote = i; m->reRenderCurrent(); }));

        std::vector<std::string> scales;
        for (int i = 0; i < imber_dsp::kScaleCount; i++)
            scales.push_back(imber_dsp::kScales[i].name);
        menu->addChild(createIndexSubmenuItem("Scale", scales,
            [m]() { return m->scaleIndex; },
            [m](int i) { m->scaleIndex = i; m->reRenderCurrent(); }));
    }
};

Model* modelSylla = createModel<Sylla, SyllaWidget>("sylla");
