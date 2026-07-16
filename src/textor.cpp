// textor.cpp — VCV Rack 2 module
// textor (Latin: "weaver") is a clone of the Fieldtone Weaver Modular, the
// one-knob "happy accidents" sampler: it captures two seconds of audio and
// reweaves it into a hypnotic loop. Every movement of the weave knob
// generates a completely new loop — nothing saves, nothing recalls, there
// is no way back. The original's algorithm is unpublished; this engine is
// designed from its documented behavior.
//
// The captured sample is scattered over a 16-step loop as three woven
// elements, each with its own level and gate output (the gates fire even
// with an empty buffer, so it doubles as a random rhythm generator):
//   warp  — long, dense foundation strands
//   weft  — medium strands crossing it
//   fleck — sparse, short, bright accents
// Each strand is a fragment of the buffer with its own start, length,
// pitch, direction and pan, fixed for the life of the loop.
//
// The weave knob has three zones, like the hardware:
//   full ccw       RESET — erases the sample and stops the loom
//   low zone       REC   — entering it starts a 2 s capture
//   the rest       WEAVE — any movement reweaves a brand new loop
//
// TEXTURE mode weaves long overlapping windowed strands (a morphing pad);
// RHYTHM mode weaves short percussive fragments. The PITCH switch selects
// ROOT (fragments shifted by musical intervals, sympathetic to the
// sample's own key) or RANDOM (continuous random shifts). Re-recording
// while the loop plays replaces the cloth but keeps the weave; the CLOCK
// input paces the 16 steps externally.
//
// True to the original's impermanence, the sample is not saved with the
// patch — only the weave (its seed) survives a reload.
//
// Controls:
//   Knobs : WEAVE (big), WARP / WEFT / FLECK levels
//   Switch: MODE (texture/rhythm), PITCH (root/random)
//   Button: REC (with LED)
//   In    : audio IN, REC trigger, WEAVE trigger, CLOCK
//   Out   : L, R, three element GATEs
//   Light : REC (red, while capturing), gate activity per element

#include "forsitan.hpp"

namespace textor_dsp {

// deterministic per-loop RNG (xorshift32)
struct Rng {
    uint32_t s = 1;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uniform() { return (next() >> 8) * (1.f / 16777216.f); }
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }
    int irange(int lo, int hi) { return lo + (int)(uniform() * (hi - lo + 1)) % (hi - lo + 1); }
};

inline float softLimit(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

} // namespace textor_dsp

struct Textor : Module {
    enum ParamId {
        WEAVE_PARAM,
        WARP_LEVEL_PARAM,
        WEFT_LEVEL_PARAM,
        FLECK_LEVEL_PARAM,
        MODE_PARAM,   // 1 = texture, 0 = rhythm
        PITCH_PARAM,  // 1 = root, 0 = random
        REC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        REC_INPUT,
        WEAVE_INPUT,
        CLOCK_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        WARP_GATE_OUTPUT,
        WEFT_GATE_OUTPUT,
        FLECK_GATE_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        REC_LIGHT,
        WARP_LIGHT,
        WEFT_LIGHT,
        FLECK_LIGHT,
        LIGHTS_LEN
    };

    static constexpr int kElements = 3;
    static constexpr int kSteps = 16;
    static constexpr int kMaxEventsPerElement = 12;
    static constexpr int kMaxVoices = 16;
    static constexpr float kBufferSeconds = 2.f;
    static constexpr int kControlDiv = 32;
    // weave knob zones
    static constexpr float kResetZone = 0.04f;
    static constexpr float kRecZone = 0.12f;

    struct Event {
        int step = 0;
        float startFrac = 0.f;  // buffer start, 0..1
        float lenS = 0.5f;      // strand length in seconds
        float semi = 0.f;       // pitch shift in semitones
        bool reverse = false;
        float pan = 0.f;        // -1..1
        float amp = 1.f;
    };

    struct Voice {
        bool active = false;
        int elem = 0;
        float pos = 0.f;        // buffer read position in samples
        float rate = 1.f;       // signed sample increment
        float age = 0.f;        // samples since onset
        float lenSamp = 1.f;
        float panL = 0.7f, panR = 0.7f;
        float amp = 1.f;
        bool texture = true;
        int32_t order = 0;      // for oldest-voice stealing
    };

    // sample memory: active cloth + shadow capture buffer
    std::vector<float> cloth, shadow;
    int bufLen = 96000;
    bool capturing = false;
    int capturePos = 0;

    // the woven loop
    Event events[kElements][kMaxEventsPerElement];
    int eventCount[kElements] = {};
    uint32_t seed = 0;
    uint32_t moveCounter = 0;
    bool loomRunning = false;
    bool lastTexture = true, lastRoot = true;

    // step sequencing
    int step = 0;
    float stepPhase = 0.f;      // samples into current step
    float stepSamples = 6000.f; // internal: 125 ms
    float clockInterval = 0.f;  // measured, in samples
    float sinceClock = 1e9f;

    // playback
    Voice voices[kMaxVoices];
    int32_t voiceOrder = 0;

    // control state
    float lastWovenKnob = -1.f;
    bool wasInReset = false, wasInRec = false;
    int controlPhase = 0;
    dsp::SchmittTrigger recTrig, weaveTrig, clockTrig;
    dsp::BooleanTrigger recButton;
    dsp::PulseGenerator gatePulse[kElements];
    float lightEnv[kElements] = {};
    float sr = 48000.f;

    Textor() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(WEAVE_PARAM, 0.f, 1.f, 0.f, "Weave");
        configParam(WARP_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Warp level");
        configParam(WEFT_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Weft level");
        configParam(FLECK_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Fleck level");
        configSwitch(MODE_PARAM, 0.f, 1.f, 1.f, "Mode", {"Rhythm", "Texture"});
        configSwitch(PITCH_PARAM, 0.f, 1.f, 1.f, "Pitch", {"Random", "Root"});
        configButton(REC_PARAM, "Record");
        configInput(AUDIO_INPUT, "Audio");
        configInput(REC_INPUT, "Record trigger");
        configInput(WEAVE_INPUT, "Weave trigger (new loop)");
        configInput(CLOCK_INPUT, "Clock (paces the 16 steps)");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configOutput(WARP_GATE_OUTPUT, "Warp gate");
        configOutput(WEFT_GATE_OUTPUT, "Weft gate");
        configOutput(FLECK_GATE_OUTPUT, "Fleck gate");
        onSampleRateChange();
    }

    void onSampleRateChange() override {
        sr = APP->engine->getSampleRate();
        bufLen = (int)(kBufferSeconds * sr);
        cloth.assign(bufLen, 0.f);
        shadow.assign(bufLen, 0.f);
        capturing = false;
        capturePos = 0;
        stepSamples = 0.125f * sr;
        clockInterval = 0.f;
        sinceClock = 1e9f;
        for (auto& v : voices)
            v.active = false;
        if (loomRunning)
            weaveEvents();
    }

    void onReset() override {
        loomRunning = false;
        seed = 0;
        moveCounter = 0;
        lastWovenKnob = -1.f;
        onSampleRateChange();
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "seed", json_integer((json_int_t)seed));
        json_object_set_new(rootJ, "moveCounter", json_integer((json_int_t)moveCounter));
        json_object_set_new(rootJ, "loomRunning", json_boolean(loomRunning));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j;
        if ((j = json_object_get(rootJ, "seed")))
            seed = (uint32_t)json_integer_value(j);
        if ((j = json_object_get(rootJ, "moveCounter")))
            moveCounter = (uint32_t)json_integer_value(j);
        if ((j = json_object_get(rootJ, "loomRunning")))
            loomRunning = json_boolean_value(j);
        if (loomRunning)
            weaveEvents();
    }

    // generate the loop's event list from the current seed and switches
    void weaveEvents() {
        using namespace textor_dsp;
        bool texture = params[MODE_PARAM].getValue() > 0.5f;
        bool root = params[PITCH_PARAM].getValue() > 0.5f;
        lastTexture = texture;
        lastRoot = root;

        // sympathetic intervals, weighted toward the root
        static const float kRootSemis[10] =
            {0.f, 0.f, 0.f, 12.f, -12.f, 7.f, -7.f, 5.f, -5.f, 24.f};

        Rng rng;
        rng.seed(seed);
        for (int e = 0; e < kElements; e++) {
            int lo, hi;
            if (texture) {
                // warp dense, weft medium, fleck sparse
                lo = (e == 0) ? 4 : (e == 1) ? 3 : 2;
                hi = (e == 0) ? 8 : (e == 1) ? 6 : 4;
            }
            else {
                lo = (e == 0) ? 2 : (e == 1) ? 3 : 4;
                hi = (e == 0) ? 5 : (e == 1) ? 8 : 10;
            }
            int n = std::min(rng.irange(lo, hi), kMaxEventsPerElement);
            eventCount[e] = n;
            for (int i = 0; i < n; i++) {
                Event& ev = events[e][i];
                ev.step = rng.irange(0, kSteps - 1);
                ev.startFrac = rng.uniform();
                if (texture) {
                    // long overlapping strands; flecks stay shorter
                    float maxLen = (e == 2) ? 0.8f : 1.6f;
                    ev.lenS = rng.range(0.4f, maxLen);
                }
                else {
                    ev.lenS = rng.range(0.04f, (e == 0) ? 0.3f : 0.18f);
                }
                if (root)
                    ev.semi = kRootSemis[rng.irange(0, 9)] + ((e == 2) ? 12.f : 0.f);
                else
                    ev.semi = rng.range(-12.f, 12.f) + ((e == 2) ? 7.f : 0.f);
                ev.reverse = rng.uniform() < (texture ? 0.3f : 0.15f);
                ev.pan = rng.range(-0.9f, 0.9f);
                ev.amp = rng.range(0.7f, 1.f);
            }
        }
    }

    void reweave() {
        moveCounter++;
        float knob = params[WEAVE_PARAM].getValue();
        seed = (uint32_t)(knob * 65535.f) * 2654435761u + moveCounter * 0x9e3779b9u;
        weaveEvents();
        loomRunning = true;
    }

    void startCapture() {
        if (capturing)
            return;
        capturing = true;
        capturePos = 0;
    }

    void clearSample() {
        std::fill(cloth.begin(), cloth.end(), 0.f);
        std::fill(shadow.begin(), shadow.end(), 0.f);
        capturing = false;
        for (auto& v : voices)
            v.active = false;
        loomRunning = false;
    }

    void fireStep() {
        using namespace textor_dsp;
        bool texture = lastTexture;
        for (int e = 0; e < kElements; e++) {
            for (int i = 0; i < eventCount[e]; i++) {
                const Event& ev = events[e][i];
                if (ev.step != step)
                    continue;
                gatePulse[e].trigger(0.002f);
                lightEnv[e] = 1.f;

                // steal the oldest voice
                Voice* v = nullptr;
                for (auto& c : voices)
                    if (!c.active) { v = &c; break; }
                if (!v) {
                    v = &voices[0];
                    for (auto& c : voices)
                        if (c.order < v->order) v = &c;
                }
                v->active = true;
                v->elem = e;
                v->order = voiceOrder++;
                v->age = 0.f;
                v->lenSamp = std::max(ev.lenS * sr, 32.f);
                float rate = std::pow(2.f, ev.semi / 12.f);
                v->rate = ev.reverse ? -rate : rate;
                v->pos = ev.startFrac * (float)(bufLen - 1);
                v->amp = ev.amp;
                v->texture = texture;
                // equal-power pan
                float p = (ev.pan + 1.f) * 0.25f * M_PI;
                v->panL = std::cos(p);
                v->panR = std::sin(p);
            }
        }
    }

    void updateControls() {
        // record button / trigger
        bool recHit = recButton.process(params[REC_PARAM].getValue() > 0.5f);
        recHit |= recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f);
        if (recHit)
            startCapture();

        // weave trigger = a nudge of the knob
        if (weaveTrig.process(inputs[WEAVE_INPUT].getVoltage(), 0.1f, 1.f))
            reweave();

        // knob zones
        float knob = params[WEAVE_PARAM].getValue();
        bool inReset = knob < kResetZone;
        bool inRec = !inReset && knob < kRecZone;
        if (inReset && !wasInReset)
            clearSample();
        if (inRec && !wasInRec)
            startCapture();
        if (!inReset && !inRec) {
            if (lastWovenKnob < 0.f)
                lastWovenKnob = knob;
            else if (std::fabs(knob - lastWovenKnob) > 0.008f) {
                lastWovenKnob = knob;
                reweave();
            }
        }
        wasInReset = inReset;
        wasInRec = inRec;

        // flipping a switch re-renders the same weave in the new mode
        bool texture = params[MODE_PARAM].getValue() > 0.5f;
        bool root = params[PITCH_PARAM].getValue() > 0.5f;
        if (loomRunning && (texture != lastTexture || root != lastRoot))
            weaveEvents();
    }

    void process(const ProcessArgs& args) override {
        using namespace textor_dsp;

        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        // --- capture (records the shadow, swaps in when full) ---
        if (capturing) {
            shadow[capturePos] = inputs[AUDIO_INPUT].getVoltage() * 0.2f;
            if (++capturePos >= bufLen) {
                capturing = false;
                std::copy(shadow.begin(), shadow.end(), cloth.begin());
            }
        }
        lights[REC_LIGHT].setBrightness(capturing ? 1.f : 0.f);

        // --- step clock ---
        sinceClock += 1.f;
        bool clocked = inputs[CLOCK_INPUT].isConnected();
        bool clockEdge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
        if (clockEdge) {
            if (sinceClock < 2.f * sr)
                clockInterval = sinceClock;
            sinceClock = 0.f;
        }
        if (loomRunning) {
            if (clocked) {
                if (clockEdge) {
                    step = (step + 1) % kSteps;
                    stepPhase = 0.f;
                    fireStep();
                }
            }
            else {
                stepPhase += 1.f;
                if (stepPhase >= stepSamples) {
                    stepPhase -= stepSamples;
                    step = (step + 1) % kSteps;
                    fireStep();
                }
            }
        }

        // --- voices ---
        float outL = 0.f, outR = 0.f;
        float levels[kElements] = {
            params[WARP_LEVEL_PARAM].getValue(),
            params[WEFT_LEVEL_PARAM].getValue(),
            params[FLECK_LEVEL_PARAM].getValue()};
        for (auto& v : voices) {
            if (!v.active)
                continue;
            // wrap the read head around the cloth
            if (v.pos >= (float)bufLen) v.pos -= (float)bufLen;
            if (v.pos < 0.f) v.pos += (float)bufLen;
            int i0 = (int)v.pos;
            int i1 = i0 + 1;
            if (i1 >= bufLen) i1 = 0;
            float frac = v.pos - (float)i0;
            float s = cloth[i0] + (cloth[i1] - cloth[i0]) * frac;

            float t = v.age / v.lenSamp;
            float env;
            if (v.texture) {
                // raised-cosine window
                float w = std::sin((float)M_PI * t);
                env = w * w;
            }
            else {
                // fast attack, exponential-ish decay
                float atk = std::min(v.age / (0.005f * sr), 1.f);
                env = atk * std::exp(-5.f * t);
            }
            s *= env * v.amp * levels[v.elem];
            outL += s * v.panL;
            outR += s * v.panR;

            v.pos += v.rate;
            v.age += 1.f;
            if (v.age >= v.lenSamp)
                v.active = false;
        }

        outputs[LEFT_OUTPUT].setVoltage(5.f * softLimit(outL * 1.4f));
        outputs[RIGHT_OUTPUT].setVoltage(5.f * softLimit(outR * 1.4f));

        for (int e = 0; e < kElements; e++) {
            outputs[WARP_GATE_OUTPUT + e].setVoltage(
                gatePulse[e].process(args.sampleTime) ? 10.f : 0.f);
            lightEnv[e] += (0.f - lightEnv[e]) * 8.f * args.sampleTime;
            lights[WARP_LIGHT + e].setBrightness(lightEnv[e]);
        }
    }
};

struct TextorWidget : ModuleWidget {
    TextorWidget(Textor* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/textor.svg")));

// @layout:begin textor 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem WEAVE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem WARP_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem WEFT_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FLECK_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem MODE_PARAM CKSS 2.0 param "" 0.0
// @elem PITCH_PARAM CKSS 2.0 param "" 0.0
// @elem REC_PARAM TL1105 2.6 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REC_INPUT PJ301MPort 4.18 input "" 0.0
// @elem WEAVE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem WARP_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem WEFT_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem FLECK_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem REC_LIGHT SmallLight 1.5 light "" 0.0
// @elem WARP_LIGHT SmallLight 1.5 light "" 0.0
// @elem WEFT_LIGHT SmallLight 1.5 light "" 0.0
// @elem FLECK_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_WEAVE label 0.0 label "weave" 0.0 25.40 35.50
// @elem LABEL_REC label 0.0 label "rec" 0.0 10.40 29.00
// @elem LABEL_MODE label 0.0 label "rhyt-text" 0.0 40.40 29.00
// @elem LABEL_WARP label 0.0 label "warp" 0.0 10.40 54.50
// @elem LABEL_WEFT label 0.0 label "weft" 0.0 25.40 54.50
// @elem LABEL_FLECK label 0.0 label "fleck" 0.0 40.40 54.50
// @elem LABEL_PITCH label 0.0 label "rnd-root" 0.0 25.40 45.00
// @elem LABEL_G1 label 0.0 label "g1" 0.0 10.40 73.50
// @elem LABEL_G2 label 0.0 label "g2" 0.0 25.40 73.50
// @elem LABEL_G3 label 0.0 label "g3" 0.0 40.40 73.50
// @elem BOX_G1 panel_box 6.0 box "" 0.0 10.40 68.00
// @elem BOX_G2 panel_box 6.0 box "" 0.0 25.40 68.00
// @elem BOX_G3 panel_box 6.0 box "" 0.0 40.40 68.00
// @elem LABEL_IN label 0.0 label "in" 0.0 6.90 94.50
// @elem LABEL_RECIN label 0.0 label "rec" 0.0 19.40 94.50
// @elem LABEL_WEAVEIN label 0.0 label "weave" 0.0 31.90 94.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 44.40 94.50
// @elem LABEL_L label 0.0 label "l" 0.0 25.10 114.00
// @elem LABEL_R label 0.0 label "r" 0.0 40.90 114.00
// @elem BOX_L panel_box 7.0 box "" 0.0 25.10 108.50
// @elem BOX_R panel_box 7.0 box "" 0.0 40.90 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 10.40 116.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.40f, 24.00f)), module, Textor::WEAVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 46.00f)), module, Textor::WARP_LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 46.00f)), module, Textor::WEFT_LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 46.00f)), module, Textor::FLECK_LEVEL_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(40.40f, 22.00f)), module, Textor::MODE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(25.40f, 40.00f)), module, Textor::PITCH_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(10.40f, 22.00f)), module, Textor::REC_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.90f, 87.00f)), module, Textor::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.40f, 87.00f)), module, Textor::REC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.90f, 87.00f)), module, Textor::WEAVE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.40f, 87.00f)), module, Textor::CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.10f, 106.50f)), module, Textor::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.90f, 106.50f)), module, Textor::RIGHT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.40f, 66.00f)), module, Textor::WARP_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.40f, 66.00f)), module, Textor::WEFT_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.40f, 66.00f)), module, Textor::FLECK_GATE_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(13.60f, 18.80f)), module, Textor::REC_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(15.40f, 63.00f)), module, Textor::WARP_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(30.40f, 63.00f)), module, Textor::WEFT_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(45.40f, 63.00f)), module, Textor::FLECK_LIGHT));
        // @layout:end
    }
};

Model* modelTextor = createModel<Textor, TextorWidget>("textor");
