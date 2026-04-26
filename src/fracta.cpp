#include "forsitan.hpp"
#include <cmath>
#include <vector>
#include <algorithm>


// ---------------------------------------------------------------------------
// Fractal interpolation oscillator — Monro (1995), CMJ 19:1 pp.88-98
// ---------------------------------------------------------------------------

struct FractaOsc {
    int N = 2;
    float yorig[4] = {};

    // Shear transform parameters (precomputed, one per segment)
    // c[t] = yorig[t+1] - yorig[t]   (y-tilt, connects endpoints)
    // f[t] = yorig[t]                 (y-offset at segment start)
    // a[t] = 1/N                      (x-scale, same for all)
    // e[t] = t/N                      (x-offset)
    float seg_c[3] = {};
    float seg_f[3] = {};
    float d = 0.5f;  // global displacement (WARP)

    std::vector<float> table;
    float phase = 0.f;
    bool dirty = true;

    int   lastCore = -1;
    float lastD    = -999.f;
    int   lastK    = -1;

    void setCore(int core) {
        if (core == lastCore) return;
        lastCore = core;
        if (core == 0) {
            N = 2;
            yorig[0] = 0.f; yorig[1] = 1.f; yorig[2] = 0.f;
        } else {
            N = 3;
            yorig[0] = 0.f; yorig[1] = 1.f; yorig[2] = 1.f; yorig[3] = 0.f;
        }
        dirty = true;
    }

    void setWarp(float warp) {
        float w = clamp(warp, -0.99f, 0.99f);
        if (w == lastD) return;
        lastD = w;
        d = w;
        dirty = true;
    }

    void rebuild(int k) {
        if (!dirty && k == lastK) return;
        lastK = k;
        dirty = false;

        float invN = 1.f / N;
        for (int t = 0; t < N; t++) {
            // c[t] = yorig[t+1] - yorig[t]
            // This is the correct shear tilt: ensures w_t maps the right endpoint
            // of the previous waveform to yorig[t+1].
            // (NOT multiplied by N — that was a bug that distorted the y-scale.)
            seg_c[t] = yorig[t + 1] - yorig[t];
            seg_f[t] = yorig[t];
        }

        int maxPts = 1;
        for (int i = 0; i < k; i++) maxPts *= N;
        maxPts += 1;

        std::vector<float> xold(maxPts), yold(maxPts);
        std::vector<float> xnew(maxPts), ynew(maxPts);

        // Iteration 1: seed with base waveform
        int currpts = N;
        for (int i = 0; i <= N; i++) {
            xold[i] = i * invN;
            yold[i] = yorig[i];
        }

        // Iterations 2..k: apply all N shear transforms to current waveform
        for (int iter = 2; iter <= k; iter++) {
            int np = 0;
            for (int t = 0; t < N; t++) {
                float ct = seg_c[t];
                float ft = seg_f[t];
                float et = t * invN;
                for (int i = 0; i < currpts; i++) {
                    xnew[np] = invN * xold[i] + et;
                    ynew[np] = ct * xold[i] + d * yold[i] + ft;
                    np++;
                }
            }
            xnew[np] = xold[currpts];
            ynew[np] = yold[currpts];
            std::swap(xold, xnew);
            std::swap(yold, ynew);
            currpts *= N;
        }

        table.resize(currpts + 1);
        for (int i = 0; i <= currpts; i++)
            table[i] = yold[i];
    }

    float next(float phaseInc) {
        phase += phaseInc;
        if (phase >= 1.f) phase -= std::floor(phase);
        int n = (int)table.size() - 1;
        float pos = phase * n;
        int i = (int)pos;
        float frac = pos - i;
        if (i >= n) { i = n - 1; frac = 1.f; }
        return table[i] + frac * (table[i + 1] - table[i]);
    }

    void hardSync() { phase = 0.f; }
};


// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct Fracta : Module {
    enum ParamIds {
        ITER_PARAM, ITER_ATTEN_PARAM,
        WARP_PARAM, WARP_ATTEN_PARAM,
        FREQ_PARAM,
        FREQ_MODE_PARAM,     // 0-3, internal state, no spring-back
        FM_PARAM,
        BOOST_PARAM,
        CORE_BTN_PARAM,      // momentary button
        ALIAS_BTN_PARAM,     // momentary button
        FREQ_MODE_BTN_PARAM, // momentary button cycles FREQ_MODE_PARAM
        NUM_PARAMS
    };
    enum InputIds {
        ITER_INPUT, WARP_INPUT, BOOST_INPUT,
        VOCT_INPUT, FM_INPUT, SYNC_INPUT,
        CORE_INPUT, ALIAS_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { MAIN_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { CORE_LIGHT, ALIAS_LIGHT, FREQ_MODE_LIGHT, NUM_LIGHTS };

    FractaOsc osc;

    // Persistent toggle states (TL1105 is momentary so we track state here)
    bool coreState  = false;
    bool aliasState = false;

    dsp::SchmittTrigger coreBtnTrig, aliasBtnTrig, freqModeBtnTrig;
    dsp::SchmittTrigger coreTrig, aliasTrig, syncTrig;

    Fracta() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ITER_PARAM,          1.f,  8.f,   3.f,  "Iterations");
        configParam(ITER_ATTEN_PARAM,   -1.f,  1.f,   0.f,  "Iterations CV");
        configParam(WARP_PARAM,         -0.99f, 0.99f, 0.5f, "Warp");
        configParam(WARP_ATTEN_PARAM,   -1.f,  1.f,   0.f,  "Warp CV");
        configParam(FREQ_PARAM,         -5.f,  5.f,   0.f,  "Frequency", "V");
        configParam(FREQ_MODE_PARAM,     0.f,  3.f,   0.f,  "Frequency mode");
        configParam(FM_PARAM,            0.f,  1.f,   0.f,  "FM amount");
        configParam(BOOST_PARAM,         0.f,  1.f,   0.f,  "Boost");
        configParam(CORE_BTN_PARAM,      0.f,  1.f,   0.f,  "Core");
        configParam(ALIAS_BTN_PARAM,     0.f,  1.f,   0.f,  "Alias");
        configParam(FREQ_MODE_BTN_PARAM, 0.f,  1.f,   0.f,  "Freq mode button");

        configInput(ITER_INPUT,  "Iterations CV");
        configInput(WARP_INPUT,  "Warp CV");
        configInput(BOOST_INPUT, "Boost CV");
        configInput(VOCT_INPUT,  "1V/Oct");
        configInput(FM_INPUT,    "FM");
        configInput(SYNC_INPUT,  "Sync");
        configInput(CORE_INPUT,  "Core gate");
        configInput(ALIAS_INPUT, "Alias gate");
        configOutput(MAIN_OUTPUT, "Fract");
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "core",  json_boolean(coreState));
        json_object_set_new(root, "alias", json_boolean(aliasState));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "core")))  coreState  = json_boolean_value(j);
        if ((j = json_object_get(root, "alias"))) aliasState = json_boolean_value(j);
    }

    void process(const ProcessArgs& args) override {
        // --- Toggle states: detect rising edge on momentary buttons ---
        if (coreBtnTrig.process(params[CORE_BTN_PARAM].getValue(), 0.1f, 0.9f))
            coreState ^= true;
        if (aliasBtnTrig.process(params[ALIAS_BTN_PARAM].getValue(), 0.1f, 0.9f))
            aliasState ^= true;
        if (freqModeBtnTrig.process(params[FREQ_MODE_BTN_PARAM].getValue(), 0.1f, 0.9f)) {
            int m = (int)std::round(params[FREQ_MODE_PARAM].getValue());
            params[FREQ_MODE_PARAM].setValue((float)((m + 1) % 4));
        }
        // Gate inputs also toggle
        if (coreTrig.process(inputs[CORE_INPUT].getVoltage()))
            coreState ^= true;
        if (aliasTrig.process(inputs[ALIAS_INPUT].getVoltage()))
            aliasState ^= true;

        // --- Core shape ---
        osc.setCore(coreState ? 1 : 0);

        // --- ITER ---
        float iterCV = inputs[ITER_INPUT].getVoltage() * params[ITER_ATTEN_PARAM].getValue();
        int k = (int)clamp(std::round(params[ITER_PARAM].getValue() + iterCV), 1.f, 8.f);

        // --- WARP ---
        float warpCV = inputs[WARP_INPUT].getVoltage() * params[WARP_ATTEN_PARAM].getValue();
        osc.setWarp(params[WARP_PARAM].getValue() + warpCV);

        // --- Frequency ---
        float rawV = params[FREQ_PARAM].getValue();
        int freqMode = (int)clamp(std::round(params[FREQ_MODE_PARAM].getValue()), 0.f, 3.f);
        float freqV;
        switch (freqMode) {
            case 0:  freqV = std::round(rawV);                        break; // octave
            case 1:  freqV = std::round(rawV * 12.f)   / 12.f;       break; // semitone
            case 2:  freqV = std::round(rawV * 1200.f) / 1200.f;     break; // cent
            default: freqV = rawV;                                    break; // free
        }
        freqV += inputs[VOCT_INPUT].getVoltage();
        float freq = 261.626f * std::pow(2.f, freqV);

        // --- FM ---
        if (inputs[FM_INPUT].isConnected())
            freq *= std::pow(2.f, inputs[FM_INPUT].getVoltage() * params[FM_PARAM].getValue());
        freq = clamp(freq, 0.5f, 20000.f);

        // --- Anti-aliasing: cap iterations to keep harmonics below Nyquist ---
        int kEff = k;
        if (!aliasState && freq > 0.f) {
            float kMax = std::floor(std::log(args.sampleRate / freq) / std::log((float)osc.N));
            kEff = (int)clamp((float)k, 1.f, std::max(kMax, 1.f));
        }

        if (kEff != osc.lastK) osc.dirty = true;
        osc.rebuild(kEff);

        // --- Sync ---
        if (syncTrig.process(inputs[SYNC_INPUT].getVoltage()))
            osc.hardSync();

        // --- Oscillator output ---
        float raw = osc.next(freq / args.sampleRate);
        float y   = (raw - 0.5f) * 2.f;  // center around 0, nominal ±1

        // --- Boost: soft saturation + amplitude ---
        float boostCV = inputs[BOOST_INPUT].isConnected()
                      ? inputs[BOOST_INPUT].getVoltage() / 5.f : 0.f;
        float boost = clamp(params[BOOST_PARAM].getValue() + boostCV, 0.f, 1.f);
        if (boost > 0.f) {
            float scale = 1.f + boost;
            y = std::tanh(y * scale) * scale;
        }

        outputs[MAIN_OUTPUT].setVoltage(clamp(y * 5.f, -10.f, 10.f));

        // --- Lights ---
        lights[CORE_LIGHT].setBrightness(coreState ? 1.f : 0.f);
        lights[ALIAS_LIGHT].setBrightness(aliasState ? 1.f : 0.f);
        static const float modeBrightness[4] = {1.f, 0.67f, 0.33f, 0.f};
        lights[FREQ_MODE_LIGHT].setBrightness(modeBrightness[freqMode]);
    }
};


// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------

struct FractaWidget : ModuleWidget {
    FractaWidget(Fracta* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/fracta.svg")));

        addChild(createWidget<ScrewSilver>(Vec(0, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // CORE section
        addParam(createParamCentered<TL1105>(mm2px(Vec(12.0, 21.0)), module, Fracta::CORE_BTN_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(12.0, 34.0)), module, Fracta::CORE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 44.0)), module, Fracta::CORE_INPUT));

        // ITER section
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 21.0)), module, Fracta::ITER_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(30.5, 34.0)), module, Fracta::ITER_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.5, 44.0)), module, Fracta::ITER_INPUT));

        // WARP section
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(49.0, 21.0)), module, Fracta::WARP_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.0, 34.0)), module, Fracta::WARP_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 44.0)), module, Fracta::WARP_INPUT));

        // FREQ section
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 62.0)), module, Fracta::VOCT_INPUT));
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 62.0)), module, Fracta::FREQ_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.0, 62.0)), module, Fracta::FM_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(12.0, 75.0)), module, Fracta::FREQ_MODE_BTN_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(19.5, 75.0)), module, Fracta::FREQ_MODE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 75.0)), module, Fracta::FM_INPUT));

        // SYNC / BOOST section
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 93.0)), module, Fracta::SYNC_INPUT));
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 93.0)), module, Fracta::BOOST_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 93.0)), module, Fracta::BOOST_INPUT));

        // ALIAS section
        addParam(createParamCentered<TL1105>(mm2px(Vec(12.0, 111.0)), module, Fracta::ALIAS_BTN_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(20.5, 111.0)), module, Fracta::ALIAS_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.5, 111.0)), module, Fracta::ALIAS_INPUT));

        // Output
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(49.0, 111.0)), module, Fracta::MAIN_OUTPUT));
    }
};


Model* fracta = createModel<Fracta, FractaWidget>("fracta");
