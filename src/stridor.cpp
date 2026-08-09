// stridor.cpp — VCV Rack 2 module
// stridor (Latin: "a creaking, a grating, a shrill sound") is dry friction as
// a voice. A probe is pressed against a resonant object with a normal force
// and dragged across it at a sliding velocity; the contact sticks, loads up,
// lets go, and sticks again. At low velocity those slips are far enough apart
// to hear one at a time and the object creaks. As velocity rises they lock
// into the object's own modes and it squeals. Nothing switches between the
// two: it is one relaxation oscillator crossing a bifurcation.
//
// The engine is a port of the Sound Design Toolkit (GPL-3.0-or-later, see
// src/sdt/): the elasto-plastic bristle friction model of Dupont et al. as
// implemented in SDTInteractors, coupled to the modal resonator of
// SDTResonators, with SDTScraping laying a surface profile under the contact.
// The port is faithful; what is this module's own is the front end:
//
//   - MATERIAL is a continuous morph across four sets of modal ratios and
//     decay factors (rubber, wood, metal, glass) rather than a preset switch.
//   - ROUGH drives the scraping grain and the friction noisiness together,
//     because both are the same physical fact about the surface.
//   - SLIP is a trigger taken from the model's own plastic fraction, so the
//     creak is a rhythm source as well as a sound.
//
// Controls:
//   Knobs : FORCE, VEL, ROUGH, STIFF, DISS, VISC, PITCH, DECAY, MAT
//   In    : FORCE, VEL, V/OCT, ROUGH
//   Out   : OUT, SLIP
//   Lights: output level, slip

#include "forsitan.hpp"
#include "sdt/sdt_control.hpp"
#include "sdt/sdt_filters.hpp"
#include "sdt/sdt_interactors.hpp"
#include "sdt/sdt_material.hpp"

namespace stridor {

static const int kModes = sdt::kMaterialModes;

// Pink-ish noise: three one-poles summed, the usual Voss-free approximation.
// This is the surface the probe is dragged over.
struct PinkNoise {
    double b0 = 0, b1 = 0, b2 = 0;
    sdt::Rng rng{20260813u};
    double process() {
        const double w = rng.white();
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        return (b0 + b1 + b2 + w * 0.1848) * 0.22;
    }
};

// The whole voice: probe, object, contact, surface.
struct Voice {
    sdt::Resonator probe, object;
    sdt::Friction contact;
    sdt::Scraping surface;
    sdt::DCFilter dc;
    PinkNoise pink;
    double sampleRate = 48000.0;
    double lastAlpha = 0.0;
    double energy = 0.0;   // running level, for the LED and the limiter

    void init(double sr) {
        sampleRate = sr;
        probe.setSampleRate(sr);
        object.setSampleRate(sr);
        contact.setSampleRate(sr);
        dc.setFrequency(18.0, sr);
        probe.makeInertial(0.05);
        object.setNModes(kModes);
        object.setNPickups(1);
        object.setActiveModes(kModes);
        // Light modes on purpose: the object has to move enough for its own
        // velocity to swing the contact's relative velocity, which is what
        // makes the stick-slip cycle exist at all. At 0.02 kg the model slides
        // smoothly at every velocity worth playing and never squeals.
        for (int m = 0; m < kModes; m++) object.setWeight(m, 0.005);
        object.setFragmentSize(1.0);
        contact.obj0 = &probe;
        contact.obj1 = &object;
        contact.contact0 = 0;
        contact.contact1 = 0;
        contact.setStribeckVelocity(0.1);
        contact.setStaticCoefficient(0.8);
        contact.setDynamicCoefficient(0.2);
        contact.setBreakAway(0.1);
    }

    void reset() {
        probe.reset();
        object.reset();
        contact.z = 0.0;
        contact.energy = 0.0;
        surface.groundTrace = 0.0;
        dc.reset();
        energy = 0.0;
    }

    // Pickup gain stays O(1) on purpose. It scales the velocity the contact
    // senses as well as the output, so it is the gain of the friction
    // feedback loop: at the SDT patches' 100 the viscosity term damps every
    // mode to Q≈4 and the object stops ringing.
    void setTone(const sdt::Material& mat, double f0, double decay) {
        sdt::applyMaterial(object, mat, f0, decay, 1.0);
    }

    // Returns the object's displacement at the pickup, DC removed.
    double process(double velocity) {
        // The surface profile is a force applied straight to the object.
        const double scrape = surface.process(pink.process());
        double outs[4] = {0, 0, 0, 0};
        contact.process(0.0, velocity, 0.0, scrape, 0.0, 0.0, outs);
        return dc.process(outs[1]);
    }
};

}  // namespace stridor

struct Stridor : Module {
    enum ParamId {
        FORCE_PARAM,
        VEL_PARAM,
        ROUGH_PARAM,
        STIFF_PARAM,
        DISS_PARAM,
        VISC_PARAM,
        PITCH_PARAM,
        DECAY_PARAM,
        MAT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        FORCE_INPUT,
        VEL_INPUT,
        VOCT_INPUT,
        ROUGH_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        SLIP_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        AUDIO_LIGHT,
        SLIP_LIGHT,
        LIGHTS_LEN
    };

    stridor::Voice voice;
    sdt::Material mat;
    dsp::PulseGenerator slipPulse;
    float lastSampleRate = 0.f;
    int ctlCount = 0;
    double lastF0 = -1.0, lastDecay = -1.0, lastMat = -1.0;
    float levelEnv = 0.f, slipEnv = 0.f;
    float outGain = 60000.f;  // the model works in metres; this is the volume
    bool limiter = true;

    Stridor() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(FORCE_PARAM, 0.f, 1.f, 0.5f, "Normal force", " N", 0.f, 12.f);
        // Exponential, and it has to be: everything interesting happens
        // between 1 and 100 mm/s, and above ~0.3 m/s the contact just slides.
        configParam(VEL_PARAM, 0.f, 1.f, 0.7f, "Sliding velocity", " m/s", 533.f, 0.0015f);
        configParam(ROUGH_PARAM, 0.f, 1.f, 0.4f, "Surface roughness", "%", 0.f, 100.f);
        configParam(STIFF_PARAM, 0.f, 1.f, 0.35f, "Contact stiffness", "%", 0.f, 100.f);
        configParam(DISS_PARAM, 0.f, 1.f, 0.3f, "Contact dissipation", "%", 0.f, 100.f);
        configParam(VISC_PARAM, 0.f, 1.f, 0.4f, "Viscosity", "%", 0.f, 100.f);
        configParam(PITCH_PARAM, std::log2(20.f), std::log2(2000.f), std::log2(160.f),
                    "Pitch", " Hz", 2.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.4f, "Decay", "%", 0.f, 100.f);
        configParam(MAT_PARAM, 0.f, 1.f, 0.45f, "Material (rubber to glass)", "%", 0.f, 100.f);
        configInput(FORCE_INPUT, "Normal force CV");
        configInput(VEL_INPUT, "Sliding velocity CV");
        configInput(VOCT_INPUT, "1V/oct pitch");
        configInput(ROUGH_INPUT, "Roughness CV");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(SLIP_OUTPUT, "Slip trigger");
        configLight(AUDIO_LIGHT, "Output level");
        configLight(SLIP_LIGHT, "Slip");
    }

    void onReset() override {
        voice.reset();
        lastSampleRate = 0.f;
        lastF0 = lastDecay = lastMat = -1.0;
    }

    void onSampleRateChange() override { lastSampleRate = 0.f; }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "outGain", json_real(outGain));
        json_object_set_new(root, "limiter", json_boolean(limiter));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "outGain"))
            outGain = clamp((float)json_real_value(j), 1000.f, 500000.f);
        if (json_t* j = json_object_get(root, "limiter")) limiter = json_boolean_value(j);
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != lastSampleRate) {
            voice.init(args.sampleRate);
            lastSampleRate = args.sampleRate;
            lastF0 = lastDecay = lastMat = -1.0;
            ctlCount = 0;
        }

        // ── control rate: everything that costs a transcendental per mode ───
        if (ctlCount-- <= 0) {
            ctlCount = 15;

            const double matKnob =
                sdt::fclip(params[MAT_PARAM].getValue(), 0.0, 1.0);
            const double f0 = clamp(std::exp2(params[PITCH_PARAM].getValue()
                                              + inputs[VOCT_INPUT].getVoltage()),
                                    16.f, 8000.f);
            // 15 ms to 4 s before the material's own scaling
            const double decay = 0.015 * std::pow(266.0, params[DECAY_PARAM].getValue());

            if (matKnob != lastMat) {
                sdt::blendMaterial(matKnob, mat);
                lastMat = matKnob;
                lastF0 = -1.0;
            }
            if (f0 != lastF0 || decay != lastDecay) {
                voice.setTone(mat, f0, decay);
                lastF0 = f0;
                lastDecay = decay;
            }
        }

        // ── audio rate ──────────────────────────────────────────────────────
        const double force = sdt::fclip(params[FORCE_PARAM].getValue()
                                        + inputs[FORCE_INPUT].getVoltage() * 0.1, 0.0, 1.2)
                             * 12.0;
        const double velKnob = sdt::fclip(params[VEL_PARAM].getValue()
                                          + inputs[VEL_INPUT].getVoltage() * 0.1, 0.0, 1.2);
        const double vel = 0.0015 * std::pow(533.0, velKnob);
        const double rough = sdt::fclip(params[ROUGH_PARAM].getValue()
                                        + inputs[ROUGH_INPUT].getVoltage() * 0.1, 0.0, 1.0);

        // Both the surface grain and the contact noisiness are the same fact
        // about the surface, so one knob moves both.
        voice.contact.setNormalForce(force);
        voice.contact.setNoisiness(0.5 + 40.0 * rough * rough);
        // 500 to 10000, the range the SDT tutorial patch sweeps, same curve
        voice.contact.setStiffness(sdt::scale(params[STIFF_PARAM].getValue(), 0.0, 1.0,
                                              500.0, 10000.0, 4.0));
        voice.contact.setDissipation(0.5 + 80.0 * std::pow(params[DISS_PARAM].getValue(), 2.0));
        voice.contact.setViscosity(0.5 + 12.0 * params[VISC_PARAM].getValue());
        voice.surface.setGrain(1e-4 * std::pow(5000.0, rough));
        voice.surface.setForce(force);
        voice.surface.setVelocity(vel);

        // A commanded velocity of exactly zero would let the probe coast, so
        // hold it still instead: the SDT interactor treats 0 as "don't touch".
        const double vDrive = (std::fabs(vel) < 1e-9) ? 1e-9 : vel;
        double y = voice.process(vDrive);
        if (!std::isfinite(y)) {
            voice.reset();
            y = 0.0;
        }

        // The plastic fraction crossing half way is the stick-to-slip
        // transition: the model's own account of when it lets go.
        const double alpha = voice.contact.alpha;
        if (alpha > 0.5 && voice.lastAlpha <= 0.5) slipPulse.trigger(1e-4f);
        voice.lastAlpha = alpha;

        float out = (float)(y * outGain);
        if (limiter) out = 10.f * std::tanh(out * 0.1f);
        out = clamp(out, -10.f, 10.f);
        outputs[AUDIO_OUTPUT].setVoltage(out);

        const bool slip = slipPulse.process(args.sampleTime);
        outputs[SLIP_OUTPUT].setVoltage(slip ? 10.f : 0.f);

        levelEnv += (std::fabs(out) * 0.2f - levelEnv) * 0.002f;
        slipEnv += ((slip ? 1.f : 0.f) - slipEnv) * 0.002f;
        lights[AUDIO_LIGHT].setBrightness(clamp(levelEnv * 1.6f, 0.f, 1.f));
        lights[SLIP_LIGHT].setBrightness(clamp(slipEnv * 8.f, 0.f, 1.f));
    }
};

struct StridorWidget : ModuleWidget {
    StridorWidget(Stridor* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/stridor.svg")));

// @layout:begin stridor 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FORCE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ROUGH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem STIFF_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DISS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VISC_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MAT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FORCE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VEL_INPUT PJ301MPort 4.01 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ROUGH_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SLIP_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem SLIP_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_FORCE label 0.0 label "force" 0.0 12.70 30.50
// @elem LABEL_VEL label 0.0 label "vel" 0.0 25.40 30.50
// @elem LABEL_ROUGH label 0.0 label "rough" 0.0 38.10 30.50
// @elem LABEL_STIFF label 0.0 label "stiff" 0.0 12.70 50.50
// @elem LABEL_DISS label 0.0 label "diss" 0.0 25.40 50.50
// @elem LABEL_VISC label 0.0 label "visc" 0.0 38.10 50.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 12.70 70.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 25.40 70.50
// @elem LABEL_MAT label 0.0 label "mat" 0.0 38.10 70.50
// @elem LABEL_FORCECV label 0.0 label "frc" 0.0 8.15 91.50
// @elem LABEL_VELCV label 0.0 label "vel" 0.0 19.65 91.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 31.15 91.50
// @elem LABEL_ROUGHCV label 0.0 label "rgh" 0.0 42.65 91.50
// @elem BOX_AUDIO panel_box 7.0 box "" 0.0 15.00 106.00
// @elem BOX_SLIP panel_box 7.0 box "" 0.0 35.00 106.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 15.00 111.50
// @elem LABEL_SLIP label 0.0 label "slip" 0.0 35.00 111.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(43.18f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 22.00f)), module, Stridor::FORCE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 22.00f)), module, Stridor::VEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 22.00f)), module, Stridor::ROUGH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 42.00f)), module, Stridor::STIFF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 42.00f)), module, Stridor::DISS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 42.00f)), module, Stridor::VISC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.70f, 62.00f)), module, Stridor::PITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 62.00f)), module, Stridor::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.10f, 62.00f)), module, Stridor::MAT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.15f, 84.00f)), module, Stridor::FORCE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.65f, 84.00f)), module, Stridor::VEL_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.15f, 84.00f)), module, Stridor::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.65f, 84.00f)), module, Stridor::ROUGH_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.00f, 104.00f)), module, Stridor::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(35.00f, 104.00f)), module, Stridor::SLIP_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(20.00f, 101.00f)), module, Stridor::AUDIO_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(40.00f, 101.00f)), module, Stridor::SLIP_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Stridor* m = dynamic_cast<Stridor*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Output gain",
            {"-12 dB", "-6 dB", "0 dB (default)", "+6 dB", "+12 dB"},
            [m]() {
                const float g[5] = {15000.f, 30000.f, 60000.f, 120000.f, 240000.f};
                int best = 2;
                for (int i = 0; i < 5; i++)
                    if (std::fabs(m->outGain - g[i]) < std::fabs(m->outGain - g[best])) best = i;
                return best;
            },
            [m](int i) {
                const float g[5] = {15000.f, 30000.f, 60000.f, 120000.f, 240000.f};
                m->outGain = g[i];
            }));
        menu->addChild(createBoolPtrMenuItem("Output limiter", "", &m->limiter));
    }
};

Model* modelStridor = createModel<Stridor, StridorWidget>("stridor");
