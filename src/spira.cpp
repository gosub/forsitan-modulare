// spira.cpp - VCV Rack 2 module
// spira (Latin: "coil, spiral") is a looper of grains, or a granulator of
// loops: the input is a line, and circles grow off it. A circle is a short
// window of the line that loops on itself while the line goes on, and
// between its turns it may shrink or grow, speed up or slow down, fade,
// darken or thin, which turns the circle into a spiral. The engine lives in
// src/spira/spira.hpp, is specified by doc/design/spira.md, and is measured
// by test/spira_probe.
//
// Controls:
//   The spiral : SIZE (the first lap), SPIRAL (the next lap over this one),
//                TAPE (by speed, or by cutting), FADE, TONE, ANCHOR
//   The lap    : PITCH, SHAPE, SOFT, direction (forward, ping-pong, reverse),
//                JITTER
//   The line   : RATE, REACH, LINE, BIRTH, HOLD, SPREAD, MIX
//   In         : IN L, IN R (normalled from L), V/OCT, SIZE (1 V/oct), SPIRAL,
//                SHAPE, REACH, RATE (1 V/oct) CV, BIRTH, HOLD
//   Out        : TURN (a lap of the newest circle), V/OCT (its speed),
//                OUT L, OUT R
//   Menu       : circles keep the settings they were born with

#include "forsitan.hpp"
#include "imber/imber_worker.hpp"
#include "position_switch.hpp"
#include "spira/spira.hpp"

#include <atomic>
#include <memory>

namespace {

const float kSwapFade = 0.005f;      // the output fades around a buffer swap, seconds
const float kPulse = 0.001f;         // TURN, seconds
const float kLightFlash = 0.05f;     // seconds a TURN flash lasts
const int kLightDivision = 32;

// A non-finite voltage reads as zero.
float finite(float v) { return std::isfinite(v) ? v : 0.f; }

}  // namespace

namespace spira {

// A buffer being replaced off the audio thread: the worker frees the old one
// before allocating the new, and nothing large is freed on the audio thread.
struct SwapJob {
    Buffers old, fresh;
    size_t samples = 0;
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
};

}  // namespace spira

struct Spira : Module {
    enum ParamId {
        SIZE_PARAM, SPIRAL_PARAM, TAPE_PARAM, FADE_PARAM, RATE_PARAM,
        PITCH_PARAM, TONE_PARAM, ANCHOR_PARAM, JITTER_PARAM, REACH_PARAM, LINE_PARAM,
        SHAPE_PARAM, SOFT_PARAM, DIRECTION_PARAM, SPREAD_PARAM, MIX_PARAM,
        BIRTH_PARAM, HOLD_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT, IN_R_INPUT, VOCT_INPUT, SIZE_INPUT, SPIRAL_INPUT, SHAPE_INPUT,
        REACH_INPUT, RATE_INPUT, BIRTH_INPUT, HOLD_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        TURN_OUTPUT, VOCT_OUTPUT, OUT_L_OUTPUT, OUT_R_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        BIRTH_LIGHT, HOLD_LIGHT,
        // the ring around SPIRAL, one red-green-blue light per circle
        RING1_LIGHT, RING1_LIGHT_G, RING1_LIGHT_B,
        RING2_LIGHT, RING2_LIGHT_G, RING2_LIGHT_B,
        RING3_LIGHT, RING3_LIGHT_G, RING3_LIGHT_B,
        RING4_LIGHT, RING4_LIGHT_G, RING4_LIGHT_B,
        RING5_LIGHT, RING5_LIGHT_G, RING5_LIGHT_B,
        RING6_LIGHT, RING6_LIGHT_G, RING6_LIGHT_B,
        RING7_LIGHT, RING7_LIGHT_G, RING7_LIGHT_B,
        RING8_LIGHT, RING8_LIGHT_G, RING8_LIGHT_B,
        LIGHTS_LEN
    };

    spira::Engine engine;
    spira::Controls ctl;
    spira::Buffers live;
    std::shared_ptr<spira::SwapJob> job;
    float swapGain = 1.f;
    bool swapFailed = false;

    dsp::SchmittTrigger birthIn;
    dsp::BooleanTrigger birthButton, holdButton;
    dsp::PulseGenerator turnPulse;
    dsp::ClockDivider lightDivider;
    float birthFlash = 0.f;
    float ringPeak[spira::kCircles] = {};      // loudest since the last light update
    float ringSpiral[spira::kCircles] = {};
    bool holdLatched = false;

    // menu
    bool keepBirth = false;

    // The laws that Rack's own display arguments cannot say. Each shows a
    // plain number with its unit in the label, and reads a typed one back
    // through the law's inverse (spira.hpp; spira_probe checks both).
    struct SpiralQuantity : ParamQuantity {
        float getDisplayValue() override { return spira::spiralRatio(getValue()); }
        void setDisplayValue(float r) override { setValue(spira::spiralKnob(r)); }
    };
    struct RateQuantity : ParamQuantity {
        float getDisplayValue() override { return spira::rateHz(getValue()); }
        void setDisplayValue(float hz) override { setValue(spira::rateKnob(hz)); }
        std::string getDisplayValueString() override {
            return spira::rateHz(getValue()) > 0.f ? ParamQuantity::getDisplayValueString()
                                                   : "Off (BIRTH only)";
        }
        std::string getUnit() override { return spira::rateHz(getValue()) > 0.f ? " Hz" : ""; }
    };

    Spira() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // one literal line per control: tools/audition/modspec reads the
        // defaults from them
        configParam(SIZE_PARAM, 0.f, 1.f, 0.5372f, "Size (the first lap)", " ms", 400.f, 10.f);   // 250 ms
        configParam<SpiralQuantity>(SPIRAL_PARAM, -1.f, 1.f, 0.f, "Spiral (next lap / this lap)", "x");
        configParam(TAPE_PARAM, 0.f, 1.f, 1.f, "Tape (0% cuts the window, 100% changes the speed)", "%", 0.f, 100.f);
        configParam(FADE_PARAM, -24.f, 3.f, -3.f, "Fade per turn (per first-lap length)", " dB");
        configParam<RateQuantity>(RATE_PARAM, 0.f, 1.f, 0.3966f, "Rate (circles per second)");   // 0.5 Hz
        configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " semitones");
        configParam(TONE_PARAM, -2.f, 2.f, 0.f, "Tone per turn (below 0 darker, above thinner)", " oct");
        configParam(ANCHOR_PARAM, 0.f, 1.f, 0.f, "Anchor (which end stays when TAPE cuts: start .. end)", "%", 0.f, 100.f);
        configParam(JITTER_PARAM, 0.f, 1.f, 0.f, "Jitter", "%", 0.f, 100.f);
        configParam(REACH_PARAM, 0.f, 1.f, 0.f, "Reach (how far back on the line)", "%", 0.f, 100.f);
        configParam(LINE_PARAM, 0.f, 1.f, 0.6114f, "Line (REACH's range, HOLD's loop)", " s", 30.f, 1.f);   // 8 s
        configParam(SHAPE_PARAM, -1.f, 1.f, 0.f, "Lap shape (below 0 decays, above swells)", "%", 0.f, 100.f);
        configParam(SOFT_PARAM, 0.f, 1.f, 0.25f, "Soft (the crossfade between laps)", "%", 0.f, 100.f);
        configSwitch(DIRECTION_PARAM, 0.f, 2.f, 2.f, "Direction", {"Reverse", "Ping-pong", "Forward"});
        configParam(SPREAD_PARAM, 0.f, 1.f, 0.5f, "Spread", "%", 0.f, 100.f);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix (line .. circles, both at unity in the middle)", "%", 0.f, 100.f);
        configButton(BIRTH_PARAM, "Birth (a circle now)");
        configButton(HOLD_PARAM, "Hold (loop the line)");

        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right (normalled from left)");
        configInput(VOCT_INPUT, "Pitch V/oct");
        configInput(SIZE_INPUT, "Size CV (1 V/oct: +1 V doubles the lap)");
        configInput(SPIRAL_INPUT, "Spiral CV (+-5 V covers the knob)");
        configInput(SHAPE_INPUT, "Shape CV (+-5 V covers the knob)");
        configInput(REACH_INPUT, "Reach CV (0..10 V)");
        configInput(RATE_INPUT, "Rate CV (1 V/oct)");
        configInput(BIRTH_INPUT, "Birth trigger");
        configInput(HOLD_INPUT, "Hold gate");
        configOutput(TURN_OUTPUT, "Turn (each lap of the newest circle)");
        configOutput(VOCT_OUTPUT, "Speed of the newest circle, V/oct");
        configOutput(OUT_L_OUTPUT, "Left");
        configOutput(OUT_R_OUTPUT, "Right");
        configLight(BIRTH_LIGHT, "Birth");
        for (int i = 0; i < spira::kCircles; i++)
            configLight(RING1_LIGHT + 3 * i, string::f("Circle %d (brightness its level, colour its spiral)", i + 1));
        configLight(HOLD_LIGHT, "Hold");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);

        lightDivider.setDivision(kLightDivision);
        engine.rng.seed(random::u32());
        // the first buffer is allocated here, on the UI thread; later ones,
        // after a sample rate change, by maintainBuffer
        engine.setSampleRate(sampleRate());
        if (live.allocate(spira::Engine::bufferSamples(engine.sr))) engine.attach(live.l, live.r, live.n);
        else swapFailed = true;
    }

    float sampleRate() const { return APP ? APP->engine->getSampleRate() : 48000.f; }

    void onSampleRateChange() override {
        // the engine checks every read against the buffer it has, so running
        // on the old one until the new one arrives is safe
        engine.setSampleRate(sampleRate());
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        holdLatched = false;
        keepBirth = false;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "hold", json_boolean(holdLatched));
        json_object_set_new(root, "keepBirth", json_boolean(keepBirth));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "hold"))) holdLatched = json_boolean_value(j);
        if ((j = json_object_get(root, "keepBirth"))) keepBirth = json_boolean_value(j);
    }

    // Keeps the buffer the size the sample rate asks for: fade out, hand the
    // old buffer to a worker that frees it and allocates the new one, attach
    // that, fade back in.
    void maintainBuffer(float sampleTime) {
        const float step = sampleTime / kSwapFade;
        if (job) {
            if (!job->done.load(std::memory_order_acquire)) return;
            if (job->failed.load()) {
                swapFailed = true;
                WARN("spira: cannot allocate %zu samples of line", job->samples);
            } else {
                live.swap(job->fresh);
                engine.attach(live.l, live.r, live.n);
                swapFailed = false;
            }
            job.reset();
        }
        size_t wanted = spira::Engine::bufferSamples(engine.sr);
        if (swapFailed || live.n == wanted) {
            swapGain = std::min(swapGain + step, 1.f);
            return;
        }
        if (swapGain > 0.f) {
            swapGain = std::max(swapGain - step, 0.f);
            return;
        }
        std::shared_ptr<spira::SwapJob> j = std::make_shared<spira::SwapJob>();
        j->samples = wanted;
        engine.attach(nullptr, nullptr, 0);
        j->old.swap(live);
        bool started = imber_worker::startDetached([j]() {
            j->old.release();
            if (!j->fresh.allocate(j->samples)) j->failed.store(true);
            j->done.store(true, std::memory_order_release);
        });
        if (!started) {
            j->old.release();
            if (!j->fresh.allocate(j->samples)) j->failed.store(true);
            j->done.store(true);
        }
        job = j;
    }

    // A circle's light: brightness its level in dB, -48..0 dB over the
    // light's travel, since a circle 20 dB down is still plainly heard; colour
    // its spiral, the house yellow for a circle, heating to orange-red as it
    // winds in and cooling to blue as it unwinds. The colour follows the
    // SPIRAL knob's travel rather than the ratio, so a slight spiral already
    // shows.
    static void ringColour(float level, float spiral, float rgb[3]) {
        static const float yellow[3] = {1.f, 0.835f, 0.f};
        static const float hot[3] = {1.f, 0.2f, 0.f};
        static const float cold[3] = {0.1f, 0.3f, 1.f};
        float b = level > 1e-6f ? clamp((20.f * std::log10(level) + 48.f) / 48.f, 0.f, 1.f) : 0.f;
        float u = clamp(std::sqrt(std::fabs(spiral)), 0.f, 1.f);
        const float* to = spiral < 0.f ? hot : cold;
        for (int c = 0; c < 3; c++) rgb[c] = b * (yellow[c] + u * (to[c] - yellow[c]));
    }

    void readControls() {
        float sizeCv = finite(inputs[SIZE_INPUT].getVoltage());
        ctl.size = clamp(spira::sizeSeconds(params[SIZE_PARAM].getValue()) * std::exp2(sizeCv),
                         spira::kSizeMin, spira::kSizeMax);
        float spiral = params[SPIRAL_PARAM].getValue() + finite(inputs[SPIRAL_INPUT].getVoltage()) / 5.f;
        ctl.spiral = spira::spiralRatio(clamp(spiral, -1.f, 1.f));
        ctl.tape = params[TAPE_PARAM].getValue();
        ctl.fadeDb = params[FADE_PARAM].getValue();
        float rate = spira::rateHz(params[RATE_PARAM].getValue());
        if (rate > 0.f) rate = clamp(rate * std::exp2(finite(inputs[RATE_INPUT].getVoltage())),
                                     spira::kRateMin, spira::kRateMax);
        ctl.rate = rate;
        ctl.pitch = params[PITCH_PARAM].getValue() + 12.f * finite(inputs[VOCT_INPUT].getVoltage());
        ctl.tone = params[TONE_PARAM].getValue();
        ctl.anchor = params[ANCHOR_PARAM].getValue();
        ctl.jitter = params[JITTER_PARAM].getValue();
        ctl.reach = clamp(params[REACH_PARAM].getValue() + finite(inputs[REACH_INPUT].getVoltage()) / 10.f, 0.f, 1.f);
        ctl.line = spira::lineSeconds(params[LINE_PARAM].getValue());
        ctl.shape = clamp(params[SHAPE_PARAM].getValue() + finite(inputs[SHAPE_INPUT].getVoltage()) / 5.f, -1.f, 1.f);
        ctl.soft = params[SOFT_PARAM].getValue();
        ctl.direction = 2 - clamp((int)std::round(params[DIRECTION_PARAM].getValue()), 0, 2);
        ctl.spread = params[SPREAD_PARAM].getValue();
        ctl.mix = params[MIX_PARAM].getValue();
        ctl.keepBirth = keepBirth;
    }

    void process(const ProcessArgs& args) override {
        maintainBuffer(args.sampleTime);
        readControls();

        if (holdButton.process(params[HOLD_PARAM].getValue() > 0.5f)) holdLatched = !holdLatched;
        ctl.hold = holdLatched || finite(inputs[HOLD_INPUT].getVoltage()) >= 1.f;

        spira::Events ev;
        bool trig = birthIn.process(finite(inputs[BIRTH_INPUT].getVoltage()), 0.1f, 1.f);
        bool pressed = birthButton.process(params[BIRTH_PARAM].getValue() > 0.5f);
        ev.birth = trig || pressed;

        float inL = finite(inputs[IN_L_INPUT].getVoltage());
        float inR = inputs[IN_R_INPUT].isConnected() ? finite(inputs[IN_R_INPUT].getVoltage()) : inL;
        spira::Output o = engine.process(ctl, ev, inL, inR);

        if (o.turn) turnPulse.trigger(kPulse);
        if (o.born) birthFlash = kLightFlash;
        for (int i = 0; i < spira::kCircles; i++) {
            if (o.ring[i] >= ringPeak[i]) ringSpiral[i] = o.ringSpiral[i];
            ringPeak[i] = std::max(ringPeak[i], o.ring[i]);
        }
        outputs[TURN_OUTPUT].setVoltage(turnPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[VOCT_OUTPUT].setVoltage(o.speedOct);
        outputs[OUT_L_OUTPUT].setVoltage(clamp(o.l * swapGain, -12.f, 12.f));
        outputs[OUT_R_OUTPUT].setVoltage(clamp(o.r * swapGain, -12.f, 12.f));

        if (lightDivider.process()) {
            float dt = args.sampleTime * kLightDivision;
            lights[BIRTH_LIGHT].setBrightness(birthFlash > 0.f ? 1.f : 0.f);
            lights[HOLD_LIGHT].setBrightness(ctl.hold ? 1.f : 0.f);
            birthFlash -= dt;
            for (int i = 0; i < spira::kCircles; i++) {
                float rgb[3];
                ringColour(ringPeak[i], ringSpiral[i], rgb);
                for (int c = 0; c < 3; c++) lights[RING1_LIGHT + 3 * i + c].setBrightnessSmooth(rgb[c], dt);
                ringPeak[i] = 0.f;
            }
        }
    }
};

struct SpiraWidget : ModuleWidget {
    SpiraWidget(Spira* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/spira.svg")));

        // @layout:begin spira 132.08 128.5 svg=tools/panels/gen_spira_panel.py
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem SIZE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TAPE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPIRAL_PARAM RoundHugeBlackKnob 9.12 param "" 0.0
// @elem RING1_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING2_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING3_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING4_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING5_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING6_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING7_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem RING8_LIGHT SmallLight<RedGreenBlueLight> 1.0 light "" 0.0
// @elem FADE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RATE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TONE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ANCHOR_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem JITTER_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem REACH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LINE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SHAPE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SOFT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DIRECTION_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem SPREAD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BIRTH_PARAM VCVLightBezel 3.6 param "" 0.0 light=BIRTH_LIGHT
// @elem HOLD_PARAM VCVLightBezel 3.6 param "" 0.0 light=HOLD_LIGHT
// @elem IN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SIZE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SPIRAL_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SHAPE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem REACH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RATE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem BIRTH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HOLD_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TURN_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem VOCT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_SIZE label 0.0 label "size" 0.0 16.04 38.50
// @elem LABEL_TAPE label 0.0 label "tape" 0.0 36.04 31.50
// @elem LABEL_SPIRAL label 0.0 label "spiral" 0.0 66.04 40.50
// @elem LABEL_FADE label 0.0 label "fade" 0.0 96.04 31.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 116.04 38.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 16.04 61.50
// @elem LABEL_TONE label 0.0 label "tone" 0.0 36.04 61.50
// @elem LABEL_ANCHOR label 0.0 label "anchor" 0.0 56.04 61.50
// @elem LABEL_JITTER label 0.0 label "jitter" 0.0 76.04 61.50
// @elem LABEL_REACH label 0.0 label "reach" 0.0 96.04 61.50
// @elem LABEL_LINE label 0.0 label "line" 0.0 116.04 61.50
// @elem LABEL_SHAPE label 0.0 label "shape" 0.0 16.04 82.50
// @elem LABEL_SOFT label 0.0 label "soft" 0.0 36.04 82.50
// @elem LABEL_FWD label 0.0 label "fwd" 0.0 56.04 67.20
// @elem LABEL_PP label 0.0 label "p-p" 0.0 62.60 75.00
// @elem LABEL_REV label 0.0 label "rev" 0.0 56.04 82.50
// @elem LABEL_SPREAD label 0.0 label "spread" 0.0 76.04 82.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 96.04 82.50
// @elem LABEL_BIRTH label 0.0 label "birth" 0.0 109.00 81.00
// @elem LABEL_HOLD label 0.0 label "hold" 0.0 123.00 81.00
// @elem LABEL_IN_L label 0.0 label "in L" 0.0 10.92 101.50
// @elem LABEL_IN_R label 0.0 label "in R" 0.0 23.17 101.50
// @elem LABEL_VOCT_IN label 0.0 label "v/oct" 0.0 35.42 101.50
// @elem LABEL_SIZE_IN label 0.0 label "size" 0.0 47.67 101.50
// @elem LABEL_SPIRAL_IN label 0.0 label "spir" 0.0 59.92 101.50
// @elem LABEL_SHAPE_IN label 0.0 label "shape" 0.0 72.17 101.50
// @elem LABEL_REACH_IN label 0.0 label "reach" 0.0 84.42 101.50
// @elem LABEL_RATE_IN label 0.0 label "rate" 0.0 96.67 101.50
// @elem LABEL_BIRTH_IN label 0.0 label "birth" 0.0 108.92 101.50
// @elem LABEL_HOLD_IN label 0.0 label "hold" 0.0 121.17 101.50
// @elem BOX_TURN panel_box 7.0 box "" 0.0 28.00 113.00
// @elem LABEL_TURN_OUT label 0.0 label "turn" 0.0 28.00 118.50
// @elem BOX_VOCT panel_box 7.0 box "" 0.0 44.00 113.00
// @elem LABEL_VOCT_OUT label 0.0 label "v/oct" 0.0 44.00 118.50
// @elem BOX_L panel_box 7.0 box "" 0.0 88.08 113.00
// @elem LABEL_L_OUT label 0.0 label "L" 0.0 88.08 118.50
// @elem BOX_R panel_box 7.0 box "" 0.0 104.08 113.00
// @elem LABEL_R_OUT label 0.0 label "R" 0.0 104.08 118.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 66.04 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(124.46f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(16.04f, 27.00f)), module, Spira::SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.04f, 23.00f)), module, Spira::TAPE_PARAM));
        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(66.04f, 27.00f)), module, Spira::SPIRAL_PARAM));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(55.21f, 20.75f)), module, Spira::RING1_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(57.54f, 17.84f)), module, Spira::RING2_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(60.62f, 15.74f)), module, Spira::RING3_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(64.18f, 14.64f)), module, Spira::RING4_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(67.90f, 14.64f)), module, Spira::RING5_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(71.46f, 15.74f)), module, Spira::RING6_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(74.54f, 17.84f)), module, Spira::RING7_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(76.87f, 20.75f)), module, Spira::RING8_LIGHT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(96.04f, 23.00f)), module, Spira::FADE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(116.04f, 27.00f)), module, Spira::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(16.04f, 53.00f)), module, Spira::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.04f, 53.00f)), module, Spira::TONE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(56.04f, 53.00f)), module, Spira::ANCHOR_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.04f, 53.00f)), module, Spira::JITTER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(96.04f, 53.00f)), module, Spira::REACH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(116.04f, 53.00f)), module, Spira::LINE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(16.04f, 74.00f)), module, Spira::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.04f, 74.00f)), module, Spira::SOFT_PARAM));
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(56.04f, 74.00f)), module, Spira::DIRECTION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.04f, 74.00f)), module, Spira::SPREAD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(96.04f, 74.00f)), module, Spira::MIX_PARAM));
        addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(109.00f, 74.00f)), module, Spira::BIRTH_PARAM, Spira::BIRTH_LIGHT));
        addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(123.00f, 74.00f)), module, Spira::HOLD_PARAM, Spira::HOLD_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.92f, 94.00f)), module, Spira::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(23.17f, 94.00f)), module, Spira::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.42f, 94.00f)), module, Spira::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.67f, 94.00f)), module, Spira::SIZE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(59.92f, 94.00f)), module, Spira::SPIRAL_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.17f, 94.00f)), module, Spira::SHAPE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(84.42f, 94.00f)), module, Spira::REACH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(96.67f, 94.00f)), module, Spira::RATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(108.92f, 94.00f)), module, Spira::BIRTH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(121.17f, 94.00f)), module, Spira::HOLD_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(28.00f, 111.00f)), module, Spira::TURN_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.00f, 111.00f)), module, Spira::VOCT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(88.08f, 111.00f)), module, Spira::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(104.08f, 111.00f)), module, Spira::OUT_R_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Spira* m = dynamic_cast<Spira*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Circles keep the settings they were born with", "", &m->keepBirth));
        if (m->swapFailed)
            menu->addChild(createMenuLabel("Out of memory for the line: passing the input only"));
    }
};

Model* modelSpira = createModel<Spira, SpiraWidget>("spira");
