// rubigo.cpp - VCV Rack 2 module
// rubigo (Latin: "rust") is a digital percussion voice played by its own
// generative sequencer, a clone of Body Synths' Metal Fetishist (firmware
// v2.0). The engine lives in src/rubigo/rubigo.hpp, is specified by
// doc/design/rubigo.md, and is measured by test/rubigo_probe.
//
// One oscillator and a noise source (or the audio input) go through a driven
// LP/HP filter, a digital distortion and a clipping volume stage, each with a
// decay-only envelope. Two random generators decide which steps fire and
// what value each step carries, and STEPS locks them into a loop.
//
// Controls:
//   Sequencer : STEPS (OFF / 2..32), SKIPS, STEP MOD + destination switch,
//               TEMPO (a ratio under an external clock), RUN / STOP
//   Voice     : PITCH + saw/square, pitch DECAY / AMOUNT, NOISE, CUTOFF,
//               RES, LP / HP, cutoff DECAY / AMOUNT, VOLUME + DECAY, effect
//               knob + RUST / CORROSION, MIX, TRIGGER
//   In        : TRIG, CLOCK, RESET, SKIPS, MOD, V/OCT, NOISE, CUTOFF, IN
//   Out       : TRIG, CLOCK, MOD, OUT
//   Menu      : Effect, Mod assign, Step lengths, Restart on run, Auto-start,
//               Reasoned random (also what Ctrl-R does here)

#include "forsitan.hpp"
#include "position_switch.hpp"
#include "rubigo/rubigo.hpp"
#include "rubigo/reasoned.hpp"

#include <utility>
#include <vector>

namespace {

const float kLightFlash = 0.05f;          // seconds a TRIGGER or CLOCK flash lasts
const int kLightDivision = 32;            // samples between light updates

// A non-finite voltage reads as zero.
float finite(float v) { return std::isfinite(v) ? v : 0.f; }

const char* kEffectNames[rubigo::FX_LEN] = {
    "Distortion (default)", "2nd oscillator", "Phaser", "Flanger", "Chorus"};
const char* kAssignNames[rubigo::ASSIGN_LEN] = {
    "Cutoff (default)", "Volume decay", "Pitch decay amount",
    "Cutoff decay amount", "Volume", "Effect parameter"};

}  // namespace

struct Rubigo : Module {
    enum ParamId {
        PITCH_PARAM, WAVE_PARAM, PITCH_DECAY_PARAM, PITCH_AMOUNT_PARAM,
        NOISE_PARAM, CUTOFF_PARAM, RES_PARAM, FILTER_PARAM,
        CUTOFF_DECAY_PARAM, CUTOFF_AMOUNT_PARAM,
        VOLUME_PARAM, VOLUME_DECAY_PARAM,
        EFFECT_PARAM, RUST_PARAM, MIX_PARAM,
        STEPS_PARAM, SKIPS_PARAM, STEPMOD_PARAM, DEST_PARAM,
        TEMPO_PARAM, RUN_PARAM, TRIGGER_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        TRIG_INPUT, CLOCK_INPUT, RESET_INPUT, SKIPS_INPUT, STEPMOD_INPUT,
        PITCH_INPUT, NOISE_INPUT, CUTOFF_INPUT, AUDIO_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        TRIG_OUTPUT, CLOCK_OUTPUT, STEPMOD_OUTPUT, AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        TRIGGER_LIGHT, CLOCK_LIGHT,
        LIGHTS_LEN
    };

    rubigo::Engine engine;
    rubigo::Controls ctl;

    dsp::SchmittTrigger trigIn, clockIn, resetIn;
    dsp::BooleanTrigger button;
    dsp::PulseGenerator trigPulse, clockPulse;
    dsp::ClockDivider lightDivider;
    float trigFlash = 0.f, clockFlash = 0.f;

    // Context menu state
    int effect = rubigo::FX_DISTORTION;
    int assign = rubigo::ASSIGN_CUTOFF;
    bool altLengths = false;
    bool restartOnRun = false;
    bool noAutoStart = false;

    bool alternativeEffect() const { return effect >= rubigo::FX_PHASER; }

    // Each control shows a plain number and puts its unit in the label, as
    // Rack's own quantities do: the law lives in getDisplayValue and its
    // inverse in setDisplayValue, so typing "440" into PITCH means 440 Hz.
    // The laws and their inverses are in rubigo.hpp, where rubigo_probe
    // checks that they round-trip.
    struct PitchQuantity : ParamQuantity {
        float getDisplayValue() override { return rubigo::pitchHz(getValue()); }
        void setDisplayValue(float hz) override { setValue(rubigo::pitchKnob(hz)); }
    };
    struct DecayQuantity : ParamQuantity {
        float getDisplayValue() override { return rubigo::decaySeconds(getValue()) * 1000.f; }
        void setDisplayValue(float ms) override { setValue(rubigo::decayKnob(ms * 0.001f)); }
    };
    struct PitchAmountQuantity : ParamQuantity {
        float getDisplayValue() override { return rubigo::amountLaw(getValue()) * rubigo::kPitchEnvMax; }
        void setDisplayValue(float hz) override { setValue(rubigo::amountKnob(hz / rubigo::kPitchEnvMax)); }
    };
    struct CutoffQuantity : ParamQuantity {
        float getDisplayValue() override { return rubigo::cutoffHz(getValue()); }
        void setDisplayValue(float hz) override { setValue(rubigo::cutoffKnob(hz)); }
    };
    // The cutoff envelope adds hertz at its peak.
    struct CutoffAmountQuantity : ParamQuantity {
        float getDisplayValue() override { return getValue() * rubigo::kCutoffEnvHz; }
        void setDisplayValue(float hz) override { setValue(clamp(hz / rubigo::kCutoffEnvHz, 0.f, 1.f)); }
    };
    // Hz on the internal clock. Under an external one TEMPO is a ratio, which
    // is shown as such ("x2", "/4") and can be typed the same way.
    struct TempoQuantity : ParamQuantity {
        bool external() {
            Rubigo* m = dynamic_cast<Rubigo*>(module);
            return m && m->engine.clock.external;
        }
        float getDisplayValue() override { return rubigo::tempoHz(getValue()); }
        void setDisplayValue(float hz) override { setValue(rubigo::tempoKnob(hz)); }
        std::string getUnit() override { return external() ? "" : " Hz"; }
        std::string getDisplayValueString() override {
            if (!external()) return ParamQuantity::getDisplayValueString();
            int r = rubigo::tempoRatio(getValue());
            return r > 0 ? string::f("x%d external", r) : string::f("/%d external", -r);
        }
        void setDisplayValueString(std::string s) override {
            if (!external()) return ParamQuantity::setDisplayValueString(s);
            float z = rubigo::ratioZoneKnob(s);
            if (z >= 0.f) setValue(z);
        }
    };
    // The loop length, 0 for off; a typed length snaps to the nearest detent.
    struct StepsQuantity : ParamQuantity {
        bool alt() {
            Rubigo* m = dynamic_cast<Rubigo*>(module);
            return m && m->altLengths;
        }
        float getDisplayValue() override { return (float)rubigo::stepsLength((int)std::round(getValue()), alt()); }
        void setDisplayValue(float n) override { setValue((float)rubigo::stepsDetent(n, alt())); }
        std::string getUnit() override { return getDisplayValue() > 0.f ? " steps" : " (off)"; }
    };
    // What the knob does depends on the effect chosen in the menu, and so
    // does its unit.
    struct EffectQuantity : ParamQuantity {
        Rubigo* rubigoModule() { return dynamic_cast<Rubigo*>(module); }
        bool osc2() { return rubigoModule() && rubigoModule()->effect == rubigo::FX_OSC2; }
        bool lfo() { return rubigoModule() && rubigoModule()->alternativeEffect(); }
        bool up() { return rubigoModule() && rubigoModule()->params[RUST_PARAM].getValue() > 0.5f; }
        float getDisplayValue() override {
            if (osc2()) return 12.f * (up() ? getValue() : getValue() - 1.f);
            if (lfo()) return rubigo::lfoHz(getValue());
            return getValue() * 100.f;
        }
        void setDisplayValue(float d) override {
            if (osc2()) setValue(clamp(up() ? d / 12.f : d / 12.f + 1.f, 0.f, 1.f));
            else if (lfo()) setValue(rubigo::lfoKnob(d));
            else setValue(clamp(d * 0.01f, 0.f, 1.f));
        }
        std::string getUnit() override { return osc2() ? " semitones" : lfo() ? " Hz LFO" : "%"; }
    };

    Rubigo() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        // One literal call a control, which is what tools/audition/modspec.py
        // reads the ranges, defaults and switch labels from. The defaults are
        // a four-on-the-floor kick at 120 BPM.
        configParam<PitchQuantity>(PITCH_PARAM, 0.f, 1.f, 0.05f, "Pitch", " Hz");
        configSwitch(WAVE_PARAM, 0.f, 1.f, 0.f, "Waveform", {"Square", "Saw"});
        configParam<DecayQuantity>(PITCH_DECAY_PARAM, 0.f, 1.f, 0.25f, "Pitch decay", " ms");
        configParam<PitchAmountQuantity>(PITCH_AMOUNT_PARAM, 0.f, 1.f, 0.7f, "Pitch decay amount", " Hz");
        configParam(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise (or the input)", "%", 0.f, 100.f);
        configParam<CutoffQuantity>(CUTOFF_PARAM, 0.f, 1.f, 0.35f, "Cutoff", " Hz");
        configParam(RES_PARAM, 0.f, 1.f, 0.1f, "Resonance", "%", 0.f, 100.f);
        configSwitch(FILTER_PARAM, 0.f, 1.f, 0.f, "Filter", {"Low-pass", "High-pass"});
        configParam<DecayQuantity>(CUTOFF_DECAY_PARAM, 0.f, 1.f, 0.25f, "Cutoff decay", " ms");
        configParam<CutoffAmountQuantity>(CUTOFF_AMOUNT_PARAM, 0.f, 1.f, 0.f, "Cutoff decay amount", " Hz");
        configParam(VOLUME_PARAM, 0.f, 1.f, 0.75f, "Volume", "%", 0.f, 100.f);
        configParam<DecayQuantity>(VOLUME_DECAY_PARAM, 0.f, 1.f, 0.42f, "Volume decay", " ms");
        configParam<EffectQuantity>(EFFECT_PARAM, 0.f, 1.f, 0.f, "Effect");
        configSwitch(RUST_PARAM, 0.f, 1.f, 0.f, "Effect mode",
                     {"Corrosion (subtle, or 2nd osc down)", "Rust (intense, or 2nd osc up)"});
        configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Effect mix", "%", 0.f, 100.f);
        configParam<StepsQuantity>(STEPS_PARAM, 0.f, 6.f, 0.f, "Steps");
        getParamQuantity(STEPS_PARAM)->snapEnabled = true;
        configParam(SKIPS_PARAM, 0.f, 1.f, 0.f, "Random skips", "%", 0.f, 100.f);
        configParam(STEPMOD_PARAM, 0.f, 1.f, 0.f, "Random step mod", "%", 0.f, 100.f);
        configSwitch(DEST_PARAM, 0.f, 2.f, 2.f, "Step mod destination", {"Cutoff", "Noise", "Pitch"});
        configParam<TempoQuantity>(TEMPO_PARAM, 0.f, 1.f, 0.2f, "Tempo");   // 2 Hz
        configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Sequencer", {"Stop", "Run"});
        configButton(TRIGGER_PARAM, "Trigger");

        configInput(TRIG_INPUT, "Trigger");
        configInput(CLOCK_INPUT, "Clock (takes over from TEMPO)");
        configInput(RESET_INPUT, "Reset (to step one on the next clock)");
        configInput(SKIPS_INPUT, "Random skips CV");
        configInput(STEPMOD_INPUT, "Random step mod CV");
        configInput(PITCH_INPUT, "Pitch V/oct");
        configInput(NOISE_INPUT, "Noise CV");
        configInput(CUTOFF_INPUT, "Cutoff CV (or the Mod assign target)");
        configInput(AUDIO_INPUT, "Audio (replaces the noise)");
        configOutput(TRIG_OUTPUT, "Trigger");
        configOutput(CLOCK_OUTPUT, "Clock");
        configOutput(STEPMOD_OUTPUT, "Step mod");
        configOutput(AUDIO_OUTPUT, "Audio");
        configLight(TRIGGER_LIGHT, "Trigger");
        configLight(CLOCK_LIGHT, "Clock");

        lightDivider.setDivision(kLightDivision);
        engine.seed(random::u32());
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        effect = rubigo::FX_DISTORTION;
        assign = rubigo::ASSIGN_CUTOFF;
        altLengths = false;
        restartOnRun = false;
        noAutoStart = false;
        engine.seq = rubigo::Sequencer();
        engine.lastLength = -1;
    }

    // Ctrl-R: the reasoned random rather than every knob uniformly.
    void onRandomize(const RandomizeEvent& e) override {
        auto u = []() { return random::uniform(); };
        writePatch(rubigo::rollAny(u));
    }

    // The patch as parameter values.
    std::vector<std::pair<int, float>> patchParams(const rubigo::Patch& p) {
        return {
            {PITCH_PARAM, p.pitch}, {WAVE_PARAM, p.wave == rubigo::SAW ? 1.f : 0.f},
            {PITCH_DECAY_PARAM, p.pitchDecay}, {PITCH_AMOUNT_PARAM, p.pitchAmount},
            {NOISE_PARAM, p.noise}, {CUTOFF_PARAM, p.cutoff}, {RES_PARAM, p.resonance},
            {FILTER_PARAM, p.highpass ? 1.f : 0.f},
            {CUTOFF_DECAY_PARAM, p.cutoffDecay}, {CUTOFF_AMOUNT_PARAM, p.cutoffAmount},
            {VOLUME_PARAM, p.volume}, {VOLUME_DECAY_PARAM, p.volumeDecay},
            {EFFECT_PARAM, p.effect}, {RUST_PARAM, p.rust ? 1.f : 0.f}, {MIX_PARAM, p.mix},
            {STEPS_PARAM, (float)p.steps}, {SKIPS_PARAM, p.skips}, {STEPMOD_PARAM, p.stepMod},
            {DEST_PARAM, 2.f - (float)p.dest}, {TEMPO_PARAM, p.tempo}, {RUN_PARAM, 1.f},
        };
    }

    void writePatch(const rubigo::Patch& p) {
        for (const auto& c : patchParams(p)) params[c.first].setValue(c.second);
    }

    // The same from the menu, as one undo step.
    void reasonedRandom(int archetype) {
        auto u = []() { return random::uniform(); };
        rubigo::Patch p = archetype < 0 ? rubigo::rollAny(u) : rubigo::roll(archetype, u);
        history::ComplexAction* h = new history::ComplexAction;
        h->name = archetype < 0 ? "rubigo: reasoned random"
                                : std::string("rubigo: reasoned random ") + rubigo::archetypeName(archetype);
        for (const auto& c : patchParams(p)) {
            float old = params[c.first].getValue();
            params[c.first].setValue(c.second);
            if (old == c.second) continue;
            history::ParamChange* pc = new history::ParamChange;
            pc->moduleId = id;
            pc->paramId = c.first;
            pc->oldValue = old;
            pc->newValue = c.second;
            h->push(pc);
        }
        if (APP && APP->history && !h->isEmpty()) APP->history->push(h);
        else delete h;
    }

    // The menu's state, and the locked sequence: the hardware cannot keep a
    // sequence, but a patch that loses its loop on reload would be worse.
    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "effect", json_integer(effect));
        json_object_set_new(root, "assign", json_integer(assign));
        json_object_set_new(root, "altLengths", json_boolean(altLengths));
        json_object_set_new(root, "restartOnRun", json_boolean(restartOnRun));
        json_object_set_new(root, "noAutoStart", json_boolean(noAutoStart));
        const rubigo::Sequencer& s = engine.seq;
        json_t* seq = json_object();
        json_object_set_new(seq, "length", json_integer(s.length));
        json_object_set_new(seq, "pos", json_integer(s.pos));
        json_object_set_new(seq, "held", json_real(s.held));
        json_t* slots = json_array();
        for (int i = 0; i < rubigo::kSlots; i++) {
            json_t* t = json_array();
            json_array_append_new(t, json_real(s.slot[i].skip));
            json_array_append_new(t, json_real(s.slot[i].mod));
            json_array_append_new(t, json_boolean(s.slot[i].valid));
            json_array_append_new(slots, t);
        }
        json_object_set_new(seq, "slots", slots);
        json_object_set_new(root, "sequence", seq);
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "effect"))) effect = clamp((int)json_integer_value(j), 0, rubigo::FX_LEN - 1);
        if ((j = json_object_get(root, "assign"))) assign = clamp((int)json_integer_value(j), 0, rubigo::ASSIGN_LEN - 1);
        if ((j = json_object_get(root, "altLengths"))) altLengths = json_boolean_value(j);
        if ((j = json_object_get(root, "restartOnRun"))) restartOnRun = json_boolean_value(j);
        if ((j = json_object_get(root, "noAutoStart"))) noAutoStart = json_boolean_value(j);
        json_t* seq = json_object_get(root, "sequence");
        if (!seq) return;
        rubigo::Sequencer s;
        if ((j = json_object_get(seq, "length"))) s.length = clamp((int)json_integer_value(j), 0, rubigo::kSlots);
        if ((j = json_object_get(seq, "pos"))) s.pos = clamp((int)json_integer_value(j), 0, rubigo::kSlots - 1);
        if ((j = json_object_get(seq, "held"))) s.held = clamp((float)json_number_value(j), 0.f, 1.f);
        json_t* slots = json_object_get(seq, "slots");
        for (int i = 0; slots && i < rubigo::kSlots && i < (int)json_array_size(slots); i++) {
            json_t* t = json_array_get(slots, i);
            if (!t || json_array_size(t) < 3) continue;
            s.slot[i].skip = clamp((float)json_number_value(json_array_get(t, 0)), 0.f, 1.f);
            s.slot[i].mod = clamp((float)json_number_value(json_array_get(t, 1)), 0.f, 1.f);
            s.slot[i].valid = json_boolean_value(json_array_get(t, 2));
        }
        if (s.length > 0 && s.pos >= s.length) s.pos = 0;
        engine.seq = s;
        // The loop is already the length STEPS asks for: do not lock it again.
        engine.lastLength = s.length;
    }

    void readControls() {
        ctl.pitch = params[PITCH_PARAM].getValue();
        ctl.wave = params[WAVE_PARAM].getValue() > 0.5f ? rubigo::SAW : rubigo::SQUARE;
        ctl.pitchDecay = params[PITCH_DECAY_PARAM].getValue();
        ctl.pitchAmount = params[PITCH_AMOUNT_PARAM].getValue();
        ctl.noise = params[NOISE_PARAM].getValue();
        ctl.cutoff = params[CUTOFF_PARAM].getValue();
        ctl.resonance = params[RES_PARAM].getValue();
        ctl.highpass = params[FILTER_PARAM].getValue() > 0.5f;
        ctl.cutoffDecay = params[CUTOFF_DECAY_PARAM].getValue();
        ctl.cutoffAmount = params[CUTOFF_AMOUNT_PARAM].getValue();
        ctl.volume = params[VOLUME_PARAM].getValue();
        ctl.volumeDecay = params[VOLUME_DECAY_PARAM].getValue();
        ctl.effect = params[EFFECT_PARAM].getValue();
        ctl.rust = params[RUST_PARAM].getValue() > 0.5f;
        ctl.mix = params[MIX_PARAM].getValue();
        ctl.play = params[RUN_PARAM].getValue() > 0.5f;
        ctl.tempo = params[TEMPO_PARAM].getValue();
        ctl.skips = params[SKIPS_PARAM].getValue();
        ctl.stepMod = params[STEPMOD_PARAM].getValue();
        ctl.dest = 2 - clamp((int)std::round(params[DEST_PARAM].getValue()), 0, 2);
        ctl.steps = clamp((int)std::round(params[STEPS_PARAM].getValue()), 0, 6);

        ctl.pitchCv = finite(inputs[PITCH_INPUT].getVoltage());
        ctl.noiseCv = finite(inputs[NOISE_INPUT].getVoltage());
        ctl.cutoffCv = finite(inputs[CUTOFF_INPUT].getVoltage());
        ctl.skipsCv = finite(inputs[SKIPS_INPUT].getVoltage());
        ctl.stepModCv = finite(inputs[STEPMOD_INPUT].getVoltage());
        ctl.extConnected = inputs[AUDIO_INPUT].isConnected();
        ctl.ext = finite(inputs[AUDIO_INPUT].getVoltage());
        ctl.clockConnected = inputs[CLOCK_INPUT].isConnected();

        ctl.fx = effect;
        ctl.assign = assign;
        ctl.altLengths = altLengths;
        ctl.restartOnPlay = restartOnRun;
        ctl.noAutoStart = noAutoStart;
    }

    void process(const ProcessArgs& args) override {
        readControls();

        rubigo::Events ev;
        bool trig = trigIn.process(finite(inputs[TRIG_INPUT].getVoltage()), 0.1f, 1.f);
        bool pressed = button.process(params[TRIGGER_PARAM].getValue() > 0.5f);
        ev.trigger = trig || pressed;
        ev.clock = clockIn.process(finite(inputs[CLOCK_INPUT].getVoltage()), 0.1f, 1.f);
        ev.reset = resetIn.process(finite(inputs[RESET_INPUT].getVoltage()), 0.1f, 1.f);

        rubigo::Output o = engine.process(ctl, ev, args.sampleRate);

        if (o.trigger) {
            trigPulse.trigger(rubigo::kPulse);
            trigFlash = kLightFlash;
        }
        if (o.clock) {
            clockPulse.trigger(rubigo::kPulse);
            clockFlash = kLightFlash;
        }
        outputs[TRIG_OUTPUT].setVoltage(trigPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[CLOCK_OUTPUT].setVoltage(clockPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[STEPMOD_OUTPUT].setVoltage(o.stepMod);
        outputs[AUDIO_OUTPUT].setVoltage(clamp(o.audio, -10.f, 10.f));

        if (lightDivider.process()) {
            float dt = args.sampleTime * kLightDivision;
            lights[TRIGGER_LIGHT].setBrightness(trigFlash > 0.f ? 1.f : 0.f);
            lights[CLOCK_LIGHT].setBrightness(clockFlash > 0.f ? 1.f : 0.f);
            trigFlash -= dt;
            clockFlash -= dt;
        }
    }
};

struct RubigoWidget : ModuleWidget {
    RubigoWidget(Rubigo* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/rubigo.svg")));

        // @layout:begin rubigo 101.6 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem DEST_PARAM CKSSThreePos 2.3 param "" 0.0
// @elem STEPMOD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STEPS_PARAM RoundHugeBlackKnob 9.12 param "" 0.0
// @elem SKIPS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TEMPO_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CLOCK_LIGHT SmallLight 1.0 light "" 0.0
// @elem RUST_PARAM CKSS 2.3 param "" 0.0
// @elem EFFECT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM Trimpot 3.03 param "" 0.0
// @elem VOLUME_DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VOLUME_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RUN_PARAM CKSS 2.3 param "" 0.0
// @elem WAVE_PARAM CKSS 2.3 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem NOISE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FILTER_PARAM CKSS 2.3 param "" 0.0
// @elem CUTOFF_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_AMOUNT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TRIGGER_PARAM VCVLightBezel 3.6 param "" 0.0 light=TRIGGER_LIGHT
// @elem CUTOFF_DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CUTOFF_AMOUNT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TRIG_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RESET_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SKIPS_INPUT PJ301MPort 4.01 input "" 0.0
// @elem STEPMOD_INPUT PJ301MPort 4.01 input "" 0.0
// @elem PITCH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem NOISE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CUTOFF_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TRIG_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CLOCK_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem STEPMOD_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem LABEL_PIT label 0.0 label "pit" 0.0 8.50 13.20
// @elem LABEL_NSE label 0.0 label "nse" 0.0 15.00 21.00
// @elem LABEL_CUT label 0.0 label "cut" 0.0 8.50 28.50
// @elem LABEL_STEPMOD label 0.0 label "step mod" 0.0 25.40 28.50
// @elem LABEL_STEPS label 0.0 label "steps" 0.0 50.80 34.50
// @elem LABEL_SKIPS label 0.0 label "skips" 0.0 76.20 28.50
// @elem LABEL_TEMPO label 0.0 label "tempo" 0.0 93.10 28.50
// @elem LABEL_RUST label 0.0 label "rust" 0.0 8.50 35.20
// @elem LABEL_CORR label 0.0 label "corr" 0.0 8.50 50.50
// @elem LABEL_EFFECT label 0.0 label "effect" 0.0 25.40 50.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 42.30 49.50
// @elem LABEL_VDECAY label 0.0 label "decay" 0.0 59.30 50.50
// @elem LABEL_VOLUME label 0.0 label "volume" 0.0 76.20 50.50
// @elem LABEL_RUN label 0.0 label "run" 0.0 93.10 35.20
// @elem LABEL_STOP label 0.0 label "stop" 0.0 93.10 50.50
// @elem LABEL_SAW label 0.0 label "saw" 0.0 8.50 56.20
// @elem LABEL_SQR label 0.0 label "sqr" 0.0 8.50 71.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 25.40 71.50
// @elem LABEL_NOISE label 0.0 label "noise" 0.0 42.30 71.50
// @elem LABEL_HP label 0.0 label "hp" 0.0 59.30 56.20
// @elem LABEL_LP label 0.0 label "lp" 0.0 59.30 71.50
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 76.20 71.50
// @elem LABEL_RES label 0.0 label "res" 0.0 93.10 71.50
// @elem LABEL_PDECAY label 0.0 label "decay" 0.0 8.50 91.00
// @elem LABEL_PAMOUNT label 0.0 label "amount" 0.0 25.40 91.00
// @elem LABEL_TRIGGER label 0.0 label "trigger" 0.0 50.80 89.50
// @elem LABEL_CDECAY label 0.0 label "decay" 0.0 76.20 91.00
// @elem LABEL_CAMOUNT label 0.0 label "amount" 0.0 93.10 91.00
// @elem LABEL_TRIG_IN label 0.0 label "trig" 0.0 8.80 104.25
// @elem LABEL_CLOCK_IN label 0.0 label "clk" 0.0 19.30 104.25
// @elem LABEL_RESET_IN label 0.0 label "rst" 0.0 29.80 104.25
// @elem LABEL_SKIPS_IN label 0.0 label "skp" 0.0 40.30 104.25
// @elem LABEL_STEPMOD_IN label 0.0 label "mod" 0.0 50.80 104.25
// @elem LABEL_PITCH_IN label 0.0 label "v/o" 0.0 61.30 104.25
// @elem LABEL_NOISE_IN label 0.0 label "nse" 0.0 71.80 104.25
// @elem LABEL_CUTOFF_IN label 0.0 label "cut" 0.0 82.30 104.25
// @elem LABEL_AUDIO_IN label 0.0 label "in" 0.0 92.80 104.25
// @elem BOX_TRIG panel_box 7.0 box "" 0.0 14.00 113.00
// @elem LABEL_TRIG_OUT label 0.0 label "trig" 0.0 14.00 118.50
// @elem BOX_CLOCK panel_box 7.0 box "" 0.0 30.00 113.00
// @elem LABEL_CLOCK_OUT label 0.0 label "clk" 0.0 30.00 118.50
// @elem BOX_STEPMOD panel_box 7.0 box "" 0.0 71.60 113.00
// @elem LABEL_STEPMOD_OUT label 0.0 label "mod" 0.0 71.60 118.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 87.60 113.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 87.60 118.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 50.80 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<CKSSThreePos>(mm2px(Vec(8.50f, 20.00f)), module, Rubigo::DEST_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 20.00f)), module, Rubigo::STEPMOD_PARAM));
        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(50.80f, 21.00f)), module, Rubigo::STEPS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.20f, 20.00f)), module, Rubigo::SKIPS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(93.10f, 20.00f)), module, Rubigo::TEMPO_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(97.60f, 15.50f)), module, Rubigo::CLOCK_LIGHT));
        addParam(createParamCentered<CKSS>(mm2px(Vec(8.50f, 42.00f)), module, Rubigo::RUST_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 42.00f)), module, Rubigo::EFFECT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(42.30f, 42.00f)), module, Rubigo::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(59.30f, 42.00f)), module, Rubigo::VOLUME_DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.20f, 42.00f)), module, Rubigo::VOLUME_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(93.10f, 42.00f)), module, Rubigo::RUN_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(8.50f, 63.00f)), module, Rubigo::WAVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 63.00f)), module, Rubigo::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(42.30f, 63.00f)), module, Rubigo::NOISE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(59.30f, 63.00f)), module, Rubigo::FILTER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.20f, 63.00f)), module, Rubigo::CUTOFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(93.10f, 63.00f)), module, Rubigo::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(8.50f, 82.50f)), module, Rubigo::PITCH_DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 82.50f)), module, Rubigo::PITCH_AMOUNT_PARAM));
        addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(50.80f, 82.50f)), module, Rubigo::TRIGGER_PARAM, Rubigo::TRIGGER_LIGHT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.20f, 82.50f)), module, Rubigo::CUTOFF_DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(93.10f, 82.50f)), module, Rubigo::CUTOFF_AMOUNT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.80f, 96.75f)), module, Rubigo::TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.30f, 96.75f)), module, Rubigo::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(29.80f, 96.75f)), module, Rubigo::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.30f, 96.75f)), module, Rubigo::SKIPS_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(50.80f, 96.75f)), module, Rubigo::STEPMOD_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.30f, 96.75f)), module, Rubigo::PITCH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(71.80f, 96.75f)), module, Rubigo::NOISE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(82.30f, 96.75f)), module, Rubigo::CUTOFF_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(92.80f, 96.75f)), module, Rubigo::AUDIO_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.00f, 111.00f)), module, Rubigo::TRIG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.00f, 111.00f)), module, Rubigo::CLOCK_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(71.60f, 111.00f)), module, Rubigo::STEPMOD_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(87.60f, 111.00f)), module, Rubigo::AUDIO_OUTPUT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Rubigo* m = dynamic_cast<Rubigo*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Effect",
            {kEffectNames[0], kEffectNames[1], kEffectNames[2], kEffectNames[3], kEffectNames[4]},
            [m]() { return m->effect; },
            [m](int i) { m->effect = i; }));
        menu->addChild(createIndexSubmenuItem("Mod assign (cutoff step mod and CV)",
            {kAssignNames[0], kAssignNames[1], kAssignNames[2], kAssignNames[3], kAssignNames[4], kAssignNames[5]},
            [m]() { return m->assign; },
            [m](int i) { m->assign = i; }));
        menu->addChild(createIndexSubmenuItem("Step lengths",
            {"2 4 8 10 16 32 (default)", "3 5 7 12 18 24"},
            [m]() { return m->altLengths ? 1 : 0; },
            [m](int i) { m->altLengths = i == 1; }));
        menu->addChild(createBoolPtrMenuItem("Restart the sequence on run", "", &m->restartOnRun));
        menu->addChild(createBoolMenuItem("Internal clock returns when the external stops", "",
            [m]() { return !m->noAutoStart; },
            [m](bool b) { m->noAutoStart = !b; }));
        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Reasoned random", "", [=](Menu* sub) {
            sub->addChild(createMenuLabel("Knobs and switches; the loop is kept"));
            sub->addChild(createMenuItem("Any archetype", "Ctrl+R", [=]() { m->reasonedRandom(-1); }));
            sub->addChild(new MenuSeparator);
            for (int a = 0; a < rubigo::ARCH_LEN; a++)
                sub->addChild(createMenuItem(rubigo::archetypeName(a), "", [=]() { m->reasonedRandom(a); }));
        }));
    }
};

Model* modelRubigo = createModel<Rubigo, RubigoWidget>("rubigo");
