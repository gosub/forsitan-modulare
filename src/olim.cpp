// olim.cpp - VCV Rack 2 module
// olim (Latin: "once upon a time", and also "someday") is an eight-head
// stereo delay over one long buffer, a clone of Olivia Artz Modular's Time
// Machine with its VCA expander built in. The engine lives in
// src/olim/olim.hpp, is specified by doc/design/olim.md, and is measured by
// test/olim_probe.
//
// TIME places the farthest head, SPREAD crowds the other seven toward now or
// toward TIME, nine sliders mix the dry signal and the heads, and FEEDBACK
// runs from sound on sound (the arc, exactly 1) into a howl. With a clock at
// CLOCK, TIME becomes power-of-two multiples of it.
//
// Controls:
//   Knobs   : TIME, SPREAD, FEEDBACK
//   Sliders : DRY, HEAD 1..8, each lit by its signal's level
//   In      : IN L, IN R (normalled from L), TIME / SPREAD / FEEDBACK CV,
//             CLOCK, and a linear VCA per slider (0..5 V, unpatched = unity)
//   Out     : OUT L, OUT R (limited to +-5 V)
//   Menu    : Memory 20 / 60 / 150 s

#include "forsitan.hpp"
#include "imber/imber_worker.hpp"
#include "olim/olim.hpp"

#include <atomic>
#include <cstdlib>
#include <memory>

namespace {

const float kMemoryChoices[] = {20.f, 60.f, 150.f};
const int kNumMemoryChoices = 3;
const float kDefaultMemory = 150.f;
// The output fades out before the buffer is replaced and back in after.
const float kSwapFade = 0.005f;   // seconds

}  // namespace

// In the engine's namespace, not an anonymous one: Olim holds them, and a
// module type with fields of internal linkage is its own warning.
namespace olim {

// Two zeroed channels of `n` samples, freed when it goes. calloc, not a
// vector, so pages are committed only as the write head reaches them.
struct Buffers {
    float* l = nullptr;
    float* r = nullptr;
    size_t n = 0;
    Buffers() {}
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;
    ~Buffers() { release(); }
    bool allocate(size_t samples) {
        release();
        l = (float*)std::calloc(samples, sizeof(float));
        r = (float*)std::calloc(samples, sizeof(float));
        if (!l || !r) { release(); return false; }
        n = samples;
        return true;
    }
    void release() {
        std::free(l);
        std::free(r);
        l = r = nullptr;
        n = 0;
    }
    void swap(Buffers& o) {
        std::swap(l, o.l);
        std::swap(r, o.r);
        std::swap(n, o.n);
    }
};

// A buffer being replaced off the audio thread. The worker frees the old
// buffers before allocating the new ones, so a 150 s buffer at 192 kHz is
// never held twice, and nothing large is ever freed on the audio thread.
struct SwapJob {
    Buffers old, fresh;
    size_t samples = 0;
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
};

}  // namespace olim

struct Olim;

namespace {

// TIME reads in seconds when free, as a multiple of the clock when clocked.
struct OlimTimeQuantity : ParamQuantity {
    float getDisplayValue() override { return olim::freeTime(getValue(), 0.f); }
    void setDisplayValue(float v) override {
        setValue(std::sqrt(clamp(v, 0.f, olim::kTimeMax) / olim::kTimeMax));
    }
    std::string getDisplayValueString() override;
};

// FEEDBACK reads as the loop gain it sets: 1 across the arc.
struct OlimFeedbackQuantity : ParamQuantity {
    float getDisplayValue() override { return olim::feedbackGain(getValue(), 0.f); }
    void setDisplayValue(float v) override {
        float g = clamp(v, 0.f, 2.f);
        // invert the twice-flattened knob: below the arc, on it, above it
        float x;
        if (g < 1.f) x = g * 0.5f * olim::kArcLow;
        else if (g == 1.f) x = 0.5f;
        else x = olim::kArcHigh + (g - 1.f) * (1.f - olim::kArcHigh);
        setValue(x);
    }
};

// SPREAD reads -100% (toward now) .. +100% (toward TIME), 0 across its flat.
struct OlimSpreadQuantity : ParamQuantity {
    float getDisplayValue() override {
        return 200.f * olim::noonFlat2(getValue()) - 100.f;
    }
    void setDisplayValue(float v) override {
        float s = clamp(v, -100.f, 100.f);
        if (s < 0.f) setValue((s + 100.f) / 100.f * olim::kArcLow);
        else if (s == 0.f) setValue(0.5f);
        else setValue(olim::kArcHigh + s / 100.f * (1.f - olim::kArcHigh));
    }
};

}  // namespace

struct Olim : Module {
    enum ParamId {
        TIME_PARAM,
        SPREAD_PARAM,
        FEEDBACK_PARAM,
        DRY_PARAM,
        HEAD1_PARAM,
        HEAD2_PARAM,
        HEAD3_PARAM,
        HEAD4_PARAM,
        HEAD5_PARAM,
        HEAD6_PARAM,
        HEAD7_PARAM,
        HEAD8_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT,
        IN_R_INPUT,
        TIME_CV_INPUT,
        SPREAD_CV_INPUT,
        FEEDBACK_CV_INPUT,
        CLOCK_INPUT,
        DRY_VCA_INPUT,
        HEAD1_VCA_INPUT,
        HEAD2_VCA_INPUT,
        HEAD3_VCA_INPUT,
        HEAD4_VCA_INPUT,
        HEAD5_VCA_INPUT,
        HEAD6_VCA_INPUT,
        HEAD7_VCA_INPUT,
        HEAD8_VCA_INPUT,
        INPUTS_LEN
    };
    enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, OUTPUTS_LEN };
    enum LightId {
        DRY_LIGHT,
        HEAD1_LIGHT,
        HEAD2_LIGHT,
        HEAD3_LIGHT,
        HEAD4_LIGHT,
        HEAD5_LIGHT,
        HEAD6_LIGHT,
        HEAD7_LIGHT,
        HEAD8_LIGHT,
        CLOCK_LIGHT,
        LEVEL_L_LIGHT,
        LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    olim::Engine engine;
    olim::Controls ctl;
    olim::Buffers live;
    std::shared_ptr<olim::SwapJob> job;
    float memory = kDefaultMemory;      // seconds, the menu's choice
    float swapGain = 1.f;               // the fade around a buffer swap
    bool swapFailed = false;
    dsp::SchmittTrigger clockTrigger;
    dsp::PulseGenerator clockPulse;
    dsp::ClockDivider lightDivider;

    Olim() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam<OlimTimeQuantity>(TIME_PARAM, 0.f, 1.f, 0.5f, "Time", " s");
        configParam<OlimSpreadQuantity>(SPREAD_PARAM, 0.f, 1.f, 0.5f, "Spread", "%");
        configParam<OlimFeedbackQuantity>(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback", "x");
        // one line each, not a loop: tools/audition/modspec reads the
        // defaults from these literals
        configParam(DRY_PARAM, 0.f, 1.f, 1.f, "Dry", "%", 0.f, 100.f);
        configParam(HEAD1_PARAM, 0.f, 1.f, 0.5f, "Head 1", "%", 0.f, 100.f);
        configParam(HEAD2_PARAM, 0.f, 1.f, 0.5f, "Head 2", "%", 0.f, 100.f);
        configParam(HEAD3_PARAM, 0.f, 1.f, 0.5f, "Head 3", "%", 0.f, 100.f);
        configParam(HEAD4_PARAM, 0.f, 1.f, 0.5f, "Head 4", "%", 0.f, 100.f);
        configParam(HEAD5_PARAM, 0.f, 1.f, 0.5f, "Head 5", "%", 0.f, 100.f);
        configParam(HEAD6_PARAM, 0.f, 1.f, 0.5f, "Head 6", "%", 0.f, 100.f);
        configParam(HEAD7_PARAM, 0.f, 1.f, 0.5f, "Head 7", "%", 0.f, 100.f);
        configParam(HEAD8_PARAM, 0.f, 1.f, 0.5f, "Head 8", "%", 0.f, 100.f);
        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right (normalled from left)");
        configInput(TIME_CV_INPUT, "Time CV (+1 V halves it)");
        configInput(SPREAD_CV_INPUT, "Spread CV");
        configInput(FEEDBACK_CV_INPUT, "Feedback CV (+5 V adds 1)");
        configInput(CLOCK_INPUT, "Clock");
        configInput(DRY_VCA_INPUT, "Dry VCA (0..5 V)");
        for (int i = 0; i < olim::kHeads; i++)
            configInput(HEAD1_VCA_INPUT + i, string::f("Head %d VCA (0..5 V)", i + 1));
        configOutput(OUT_L_OUTPUT, "Left");
        configOutput(OUT_R_OUTPUT, "Right");
        configLight(DRY_LIGHT, "Input level");
        for (int i = 0; i < olim::kHeads; i++)
            configLight(HEAD1_LIGHT + i, string::f("Head %d level", i + 1));
        configLight(CLOCK_LIGHT, "Clock");
        configLight(LEVEL_L_LIGHT, "Left level");
        configLight(LEVEL_R_LIGHT, "Right level");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
        lightDivider.setDivision(32);

        // the first buffer is allocated here, on the UI thread; later ones by
        // maintainBuffer
        engine.setSampleRate(sampleRate());
        if (live.allocate(wantedSamples())) engine.attach(live.l, live.r, live.n);
        else swapFailed = true;
    }

    float sampleRate() const { return APP ? APP->engine->getSampleRate() : 48000.f; }

    size_t wantedSamples() const {
        return olim::Engine::bufferSamples(memory, engine.sr);
    }

    void onSampleRateChange() override {
        // the engine clamps every head to the buffer it has, so running on
        // the old one until the new one arrives is safe
        engine.setSampleRate(sampleRate());
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "memory", json_real(memory));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "memory")) {
            float m = (float)json_number_value(j);
            for (int i = 0; i < kNumMemoryChoices; i++)
                if (std::fabs(m - kMemoryChoices[i]) < 0.5f) memory = kMemoryChoices[i];
        }
    }

    void onReset() override {
        memory = kDefaultMemory;
    }

    // Keeps the buffer the size the Memory menu and the sample rate ask for:
    // fade out, hand the old buffer to a worker that frees it and allocates
    // the new one, attach that, fade back in.
    void maintainBuffer(float sampleTime) {
        const float step = sampleTime / kSwapFade;
        if (job) {
            if (!job->done.load(std::memory_order_acquire)) return;
            if (job->failed.load()) {
                swapFailed = true;
                WARN("olim: cannot allocate %zu samples of memory", job->samples);
            } else {
                live.swap(job->fresh);
                engine.attach(live.l, live.r, live.n);
                swapFailed = false;
            }
            job.reset();
        }
        bool wanted = !swapFailed && live.n != wantedSamples();
        if (!wanted) {
            swapGain = std::min(swapGain + step, 1.f);
            return;
        }
        if (swapGain > 0.f) {
            swapGain = std::max(swapGain - step, 0.f);
            return;
        }
        std::shared_ptr<olim::SwapJob> j = std::make_shared<olim::SwapJob>();
        j->samples = wantedSamples();
        engine.attach(nullptr, nullptr, 0);
        j->old.swap(live);
        bool started = imber_worker::startDetached([j]() {
            j->old.release();
            if (!j->fresh.allocate(j->samples)) j->failed.store(true);
            j->done.store(true, std::memory_order_release);
        });
        if (!started) {
            // run it here rather than not at all: calloc is lazy, and the
            // old buffer's pages go back in one call
            j->old.release();
            if (!j->fresh.allocate(j->samples)) j->failed.store(true);
            j->done.store(true);
        }
        job = j;
    }

    // Memory menu: a new size clears a failed allocation's latch.
    void setMemory(float seconds) {
        memory = seconds;
        swapFailed = false;
    }

    void process(const ProcessArgs& args) override {
        maintainBuffer(args.sampleTime);

        ctl.time = params[TIME_PARAM].getValue();
        ctl.spread = params[SPREAD_PARAM].getValue();
        ctl.feedback = params[FEEDBACK_PARAM].getValue();
        ctl.timeCv = inputs[TIME_CV_INPUT].getVoltage();
        ctl.spreadCv = inputs[SPREAD_CV_INPUT].getVoltage();
        ctl.feedbackCv = inputs[FEEDBACK_CV_INPUT].getVoltage();
        ctl.dry = params[DRY_PARAM].getValue()
                * olim::vcaGain(inputs[DRY_VCA_INPUT].getNormalVoltage(olim::kFullScale));
        for (int i = 0; i < olim::kHeads; i++)
            ctl.gain[i] = params[HEAD1_PARAM + i].getValue()
                        * olim::vcaGain(inputs[HEAD1_VCA_INPUT + i].getNormalVoltage(olim::kFullScale));

        // no clock cable, no clock: a clock that was just unpatched would
        // otherwise hold TIME for another two seconds
        bool clockHigh = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
                clockPulse.trigger(0.05f);
            clockHigh = clockTrigger.isHigh();
        } else {
            engine.clock = olim::ClockMeter();
            engine.clock.setSampleRate(engine.sr);
        }

        float inL = inputs[IN_L_INPUT].getVoltage();
        float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inL;
        // a NaN in would live in the buffer for as long as the memory is
        if (!std::isfinite(inL)) inL = 0.f;
        if (!std::isfinite(inR)) inR = 0.f;

        float outL, outR;
        engine.process(ctl, clockHigh, inL, inR, outL, outR);
        outputs[OUT_L_OUTPUT].setVoltage(outL * swapGain);
        outputs[OUT_R_OUTPUT].setVoltage(outR * swapGain);

        bool pulse = clockPulse.process(args.sampleTime);
        if (lightDivider.process()) {
            float dt = args.sampleTime * lightDivider.getDivision();
            lights[DRY_LIGHT].setBrightness(engine.inputLevel());
            for (int i = 0; i < olim::kHeads; i++)
                lights[HEAD1_LIGHT + i].setBrightness(engine.headLevel(i));
            lights[CLOCK_LIGHT].setBrightnessSmooth(
                engine.clocked() ? (pulse ? 1.f : 0.25f) : 0.f, dt);
            lights[LEVEL_L_LIGHT].setBrightnessSmooth(std::fabs(outL) / olim::kFullScale, dt);
            lights[LEVEL_R_LIGHT].setBrightnessSmooth(std::fabs(outR) / olim::kFullScale, dt);
        }
    }
};

std::string OlimTimeQuantity::getDisplayValueString() {
    Olim* m = dynamic_cast<Olim*>(module);
    if (m && m->engine.clocked()) {
        float ratio = m->engine.timeSec / m->engine.clock.period();
        return ratio >= 1.f ? string::f("%g clocks", std::round(ratio))
                            : string::f("1/%g clock", std::round(1.f / ratio));
    }
    return ParamQuantity::getDisplayValueString();
}

namespace {

// The FEEDBACK knob's arc: the stretch of its travel where the loop gain is
// exactly 1, drawn over the knob's own rotation.
struct OlimArc : widget::Widget {
    float radius = 0.f;
    void draw(const DrawArgs& args) override {
        // RoundBigBlackKnob turns through +-0.83 pi from straight up
        const float a0 = -0.83f * (float)M_PI, a1 = 0.83f * (float)M_PI;
        auto angle = [&](float v) { return a0 + (a1 - a0) * v - 0.5f * (float)M_PI; };
        Vec c = box.size.div(2.f);
        nvgBeginPath(args.vg);
        nvgArc(args.vg, c.x, c.y, radius, angle(olim::kArcLow), angle(olim::kArcHigh), NVG_CW);
        nvgStrokeColor(args.vg, nvgRGB(0xff, 0xd5, 0x00));
        nvgStrokeWidth(args.vg, mm2px(0.6f));
        nvgLineCap(args.vg, NVG_ROUND);
        nvgStroke(args.vg);
    }
};

}  // namespace

struct OlimWidget : ModuleWidget {
    OlimWidget(Olim* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/olim.svg")));

// @layout:begin olim 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem TIME_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem SPREAD_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem FEEDBACK_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TIME_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SPREAD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FEEDBACK_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DRY_PARAM VCVLightSlider 12.96 param "" 0.0 light=DRY_LIGHT
// @elem HEAD1_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD1_LIGHT
// @elem HEAD2_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD2_LIGHT
// @elem HEAD3_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD3_LIGHT
// @elem HEAD4_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD4_LIGHT
// @elem HEAD5_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD5_LIGHT
// @elem HEAD6_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD6_LIGHT
// @elem HEAD7_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD7_LIGHT
// @elem HEAD8_PARAM VCVLightSlider 12.96 param "" 0.0 light=HEAD8_LIGHT
// @elem DRY_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD1_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD2_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD3_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD4_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD5_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD6_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD7_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HEAD8_VCA_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLOCK_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_TIME label 0.0 label "time" 0.0 25.00 32.50
// @elem LABEL_SPREAD label 0.0 label "spread" 0.0 61.00 32.50
// @elem LABEL_FEEDBACK label 0.0 label "feedback" 0.0 97.00 32.50
// @elem LABEL_TIME_CV label 0.0 label "cv" 0.0 25.00 47.50
// @elem LABEL_SPREAD_CV label 0.0 label "cv" 0.0 61.00 47.50
// @elem LABEL_FEEDBACK_CV label 0.0 label "cv" 0.0 97.00 47.50
// @elem LABEL_DRY label 0.0 label "dry" 0.0 15.00 91.50
// @elem LABEL_H1 label 0.0 label "1" 0.0 26.50 91.50
// @elem LABEL_H2 label 0.0 label "2" 0.0 38.00 91.50
// @elem LABEL_H3 label 0.0 label "3" 0.0 49.50 91.50
// @elem LABEL_H4 label 0.0 label "4" 0.0 61.00 91.50
// @elem LABEL_H5 label 0.0 label "5" 0.0 72.50 91.50
// @elem LABEL_H6 label 0.0 label "6" 0.0 84.00 91.50
// @elem LABEL_H7 label 0.0 label "7" 0.0 95.50 91.50
// @elem LABEL_H8 label 0.0 label "8" 0.0 107.00 91.50
// @elem LABEL_IN_L label 0.0 label "in L" 0.0 16.00 114.50
// @elem LABEL_IN_R label 0.0 label "in R" 0.0 32.00 114.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 61.00 114.50
// @elem LABEL_L label 0.0 label "L" 0.0 90.00 114.50
// @elem LABEL_R label 0.0 label "R" 0.0 106.00 114.50
// @elem BOX_OUT_L panel_box 7.0 box "" 0.0 90.00 109.00
// @elem BOX_OUT_R panel_box 7.0 box "" 0.0 106.00 109.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.00f, 21.00f)), module, Olim::TIME_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(61.00f, 21.00f)), module, Olim::SPREAD_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(97.00f, 21.00f)), module, Olim::FEEDBACK_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.00f, 40.00f)), module, Olim::TIME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 40.00f)), module, Olim::SPREAD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(97.00f, 40.00f)), module, Olim::FEEDBACK_CV_INPUT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(15.00f, 63.50f)), module, Olim::DRY_PARAM, Olim::DRY_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(26.50f, 63.50f)), module, Olim::HEAD1_PARAM, Olim::HEAD1_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(38.00f, 63.50f)), module, Olim::HEAD2_PARAM, Olim::HEAD2_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(49.50f, 63.50f)), module, Olim::HEAD3_PARAM, Olim::HEAD3_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(61.00f, 63.50f)), module, Olim::HEAD4_PARAM, Olim::HEAD4_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(72.50f, 63.50f)), module, Olim::HEAD5_PARAM, Olim::HEAD5_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(84.00f, 63.50f)), module, Olim::HEAD6_PARAM, Olim::HEAD6_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(95.50f, 63.50f)), module, Olim::HEAD7_PARAM, Olim::HEAD7_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<YellowLight>>(mm2px(Vec(107.00f, 63.50f)), module, Olim::HEAD8_PARAM, Olim::HEAD8_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.00f, 84.00f)), module, Olim::DRY_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(26.50f, 84.00f)), module, Olim::HEAD1_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.00f, 84.00f)), module, Olim::HEAD2_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.50f, 84.00f)), module, Olim::HEAD3_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 84.00f)), module, Olim::HEAD4_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.50f, 84.00f)), module, Olim::HEAD5_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(84.00f, 84.00f)), module, Olim::HEAD6_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(95.50f, 84.00f)), module, Olim::HEAD7_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(107.00f, 84.00f)), module, Olim::HEAD8_VCA_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.00f, 107.00f)), module, Olim::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.00f, 107.00f)), module, Olim::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 107.00f)), module, Olim::CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(90.00f, 107.00f)), module, Olim::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(106.00f, 107.00f)), module, Olim::OUT_R_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(66.00f, 104.00f)), module, Olim::CLOCK_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(95.00f, 104.00f)), module, Olim::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(111.00f, 104.00f)), module, Olim::LEVEL_R_LIGHT));
        // @layout:end

        OlimArc* arc = new OlimArc;
        arc->radius = mm2px(8.9f);
        arc->box.size = mm2px(Vec(20.f, 20.f));
        arc->box.pos = mm2px(Vec(97.f - 10.f, 21.f - 10.f));
        addChild(arc);
    }

    void appendContextMenu(Menu* menu) override {
        Olim* m = getModule<Olim>();
        if (!m) return;
        menu->addChild(new MenuSeparator);
        std::vector<std::string> labels;
        for (int i = 0; i < kNumMemoryChoices; i++)
            labels.push_back(string::f("%g s", kMemoryChoices[i]));
        menu->addChild(createIndexSubmenuItem("Memory", labels,
            [=]() {
                for (int i = 0; i < kNumMemoryChoices; i++)
                    if (m->memory == kMemoryChoices[i]) return i;
                return kNumMemoryChoices - 1;
            },
            [=](int i) { m->setMemory(kMemoryChoices[i]); }));
        if (m->swapFailed)
            menu->addChild(createMenuLabel("Not enough memory for that: passing dry only"));
    }
};

Model* modelOlim = createModel<Olim, OlimWidget>("olim");
