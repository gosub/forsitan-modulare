#include "forsitan.hpp"
#include <cmath>
#include <vector>
#include <algorithm>


// ---------------------------------------------------------------------------
// Fractal interpolation oscillator — Monro (1995), CMJ 19:1 pp.88-98
// ---------------------------------------------------------------------------

struct FractaOsc {
    // Base waveform
    int N = 2;                    // number of segments (2=triangle, 3=trapezoid)
    float yorig[4] = {};          // N+1 y-values; xorig[i] = i/N (equal spacing)

    // Shear transform parameters (per segment, precomputed)
    float seg_c[3] = {};          // y-tilt per segment t (1-indexed, stored 0-indexed)
    float seg_f[3] = {};          // y-offset per segment t
    float d = 0.5f;               // global displacement (WARP)

    // Wavetable
    std::vector<float> table;     // N^k + 1 points, x uniform in [0,1]

    // Oscillator state
    float phase = 0.f;
    bool dirty = true;

    // Cached params for change detection
    int lastCore = -1;
    float lastD = -999.f;
    int lastK = -1;

    void setCore(int core) {
        if (core == lastCore) return;
        lastCore = core;
        if (core == 0) {
            // Triangle: (0,0), (0.5,1), (1,0)
            N = 2;
            yorig[0] = 0.f; yorig[1] = 1.f; yorig[2] = 0.f;
        } else {
            // Trapezoid: (0,0), (1/3,1), (2/3,1), (1,0)
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

        // Precompute per-segment shear params
        // a[t] = 1/N (same for all), e[t] = (t-1)/N
        // c[t] = N*(yorig[t] - yorig[t-1])  (since yorig[0]=yorig[N]=0, cross-term vanishes)
        // f[t] = yorig[t-1]
        float invN = 1.f / N;
        for (int t = 0; t < N; t++) {
            seg_c[t] = N * (yorig[t + 1] - yorig[t]);
            seg_f[t] = yorig[t];
        }

        // Iterative construction using two alternating buffers
        // First "iteration" is just copying the base points
        int maxPts = 1;
        for (int i = 0; i < k; i++) maxPts *= N;
        maxPts += 1;

        std::vector<float> xold(maxPts), yold(maxPts);
        std::vector<float> xnew(maxPts), ynew(maxPts);

        // Seed: base points (counts as iteration 1)
        int currpts = N;   // number of intervals
        for (int i = 0; i <= N; i++) {
            xold[i] = i * invN;
            yold[i] = yorig[i];
        }

        // Subsequent iterations
        for (int iter = 2; iter <= k; iter++) {
            int np = 0;
            for (int t = 0; t < N; t++) {
                float at = invN;
                float et = t * invN;
                float ct = seg_c[t];
                float ft = seg_f[t];
                for (int i = 0; i < currpts; i++) {
                    xnew[np] = at * xold[i] + et;
                    ynew[np] = ct * xold[i] + d * yold[i] + ft;
                    np++;
                }
            }
            // Last point
            xnew[np] = xold[currpts];
            ynew[np] = yold[currpts];

            std::swap(xold, xnew);
            std::swap(yold, ynew);
            currpts *= N;
        }

        // Store y-values into table (x is uniform so we only need y)
        int total = currpts + 1;
        table.resize(total);
        for (int i = 0; i < total; i++) {
            table[i] = yold[i];
        }
    }

    // Returns sample in [~0, ~1] (may slightly exceed due to high warp)
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

    void hardSync() {
        phase = 0.f;
    }
};


// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct Fracta : Module {
    enum ParamIds {
        ITER_PARAM, ITER_ATTEN_PARAM,
        WARP_PARAM, WARP_ATTEN_PARAM,
        FREQ_PARAM, FREQ_MODE_PARAM,
        FM_PARAM,
        BOOST_PARAM,
        CORE_PARAM,
        ALIAS_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        ITER_INPUT, WARP_INPUT, BOOST_INPUT,
        VOCT_INPUT, FM_INPUT, SYNC_INPUT,
        CORE_INPUT, ALIAS_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { MAIN_OUTPUT, NUM_OUTPUTS };
    enum LightIds { CORE_LIGHT, ALIAS_LIGHT, FREQ_MODE_LIGHT, NUM_LIGHTS };

    FractaOsc osc;
    dsp::SchmittTrigger syncTrig, coreTrig, aliasTrig;

    Fracta() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ITER_PARAM,       1.f,  8.f, 3.f,  "Iterations");
        configParam(ITER_ATTEN_PARAM,-1.f,  1.f, 0.f,  "Iterations CV amount");
        configParam(WARP_PARAM,      -0.99f, 0.99f, 0.5f, "Warp");
        configParam(WARP_ATTEN_PARAM,-1.f,  1.f, 0.f,  "Warp CV amount");
        configParam(FREQ_PARAM,      -4.f,  4.f, 0.f,  "Frequency offset", "V");
        configParam(FREQ_MODE_PARAM,  0.f,  3.f, 0.f,  "Frequency mode");
        configParam(FM_PARAM,         0.f,  1.f, 0.f,  "FM amount");
        configParam(BOOST_PARAM,      0.f,  1.f, 0.f,  "Boost");
        configParam(CORE_PARAM,       0.f,  1.f, 0.f,  "Core");
        configParam(ALIAS_PARAM,      0.f,  1.f, 0.f,  "Alias");

        configInput(ITER_INPUT,  "Iterations CV");
        configInput(WARP_INPUT,  "Warp CV");
        configInput(BOOST_INPUT, "Boost CV");
        configInput(VOCT_INPUT,  "1V/Oct");
        configInput(FM_INPUT,    "FM");
        configInput(SYNC_INPUT,  "Sync");
        configInput(CORE_INPUT,  "Core gate");
        configInput(ALIAS_INPUT, "Alias gate");

        configOutput(MAIN_OUTPUT, "Fracta");
    }

    void process(const ProcessArgs& args) override {
        // --- Toggle buttons via gate inputs ---
        if (coreTrig.process(inputs[CORE_INPUT].getVoltage())) {
            float v = params[CORE_PARAM].getValue();
            params[CORE_PARAM].setValue(v >= 0.5f ? 0.f : 1.f);
        }
        if (aliasTrig.process(inputs[ALIAS_INPUT].getVoltage())) {
            float v = params[ALIAS_PARAM].getValue();
            params[ALIAS_PARAM].setValue(v >= 0.5f ? 0.f : 1.f);
        }
        // --- Core shape ---
        int core = (int)std::round(params[CORE_PARAM].getValue());
        osc.setCore(core);

        // --- ITER ---
        float iterCV = inputs[ITER_INPUT].getVoltage() * params[ITER_ATTEN_PARAM].getValue();
        int k = (int)clamp(std::round(params[ITER_PARAM].getValue() + iterCV), 1.f, 8.f);

        // --- WARP ---
        float warpCV = inputs[WARP_INPUT].getVoltage() * params[WARP_ATTEN_PARAM].getValue();
        float warpVal = params[WARP_PARAM].getValue() + warpCV;
        osc.setWarp(warpVal);

        // --- Frequency ---
        float rawFreqV = params[FREQ_PARAM].getValue();
        int freqMode = (int)clamp(std::round(params[FREQ_MODE_PARAM].getValue()), 0.f, 3.f);
        float freqV;
        switch (freqMode) {
            case 0:  freqV = std::round(rawFreqV); break;
            case 1:  freqV = std::round(rawFreqV * 12.f) / 12.f; break;
            case 2:  freqV = std::round(rawFreqV * 1200.f) / 1200.f; break;
            default: freqV = rawFreqV; break;
        }
        freqV += inputs[VOCT_INPUT].getVoltage();
        float freq = 261.626f * std::pow(2.f, freqV);

        // --- FM ---
        if (inputs[FM_INPUT].isConnected()) {
            float fmDepth = params[FM_PARAM].getValue();
            freq *= std::pow(2.f, inputs[FM_INPUT].getVoltage() * fmDepth);
        }
        freq = clamp(freq, 0.5f, 20000.f);

        // --- Anti-aliasing: compute effective k ---
        bool aliasOn = params[ALIAS_PARAM].getValue() >= 0.5f;
        int kEff = k;
        if (!aliasOn && freq > 0.f) {
            float kMax = std::floor(std::log(args.sampleRate / freq) / std::log((float)osc.N));
            kEff = (int)clamp((float)k, 1.f, kMax);
        }
        kEff = std::max(kEff, 1);

        // Mark dirty if k changed (warp/core already handled in setters)
        if (kEff != osc.lastK) osc.dirty = true;
        osc.rebuild(kEff);

        // --- Sync ---
        if (syncTrig.process(inputs[SYNC_INPUT].getVoltage())) {
            osc.hardSync();
        }

        // --- Advance oscillator ---
        float phaseInc = freq / args.sampleRate;
        float raw = osc.next(phaseInc);

        // raw is in [0,1] nominally; center and scale to ±1
        float y = (raw - 0.5f) * 2.f;

        // --- Boost / soft saturation ---
        float boostCV = inputs[BOOST_INPUT].isConnected()
            ? inputs[BOOST_INPUT].getVoltage() / 5.f : 0.f;
        float boost = clamp(params[BOOST_PARAM].getValue() + boostCV, 0.f, 1.f);
        float scale = 1.f + boost;
        y = std::tanh(y * scale) * scale;

        outputs[MAIN_OUTPUT].setVoltage(clamp(y * 5.f, -10.f, 10.f));

        // --- Lights ---
        lights[CORE_LIGHT].setBrightness(core);
        lights[ALIAS_LIGHT].setBrightness(aliasOn ? 1.f : 0.f);
        static const float freqModeBrightness[4] = {1.f, 0.67f, 0.33f, 0.f};
        lights[FREQ_MODE_LIGHT].setBrightness(freqModeBrightness[freqMode]);
    }
};


// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------

struct FreqModeButton : TL1105 {
    Fracta* module = nullptr;

    void onButton(const ButtonEvent& e) override {
        TL1105::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
            int mode = (int)clamp(std::round(module->params[Fracta::FREQ_MODE_PARAM].getValue()), 0.f, 3.f);
            module->params[Fracta::FREQ_MODE_PARAM].setValue((float)((mode + 1) % 4));
            e.consume(this);
        }
    }
};


struct FractaWidget : ModuleWidget {
    FractaWidget(Fracta* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/fracta.svg")));

        addChild(createWidget<ScrewSilver>(Vec(0, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // CORE section
        addParam(createParamCentered<TL1105>(mm2px(Vec(12.0, 22.0)), module, Fracta::CORE_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(12.0, 32.0)), module, Fracta::CORE_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 44.0)), module, Fracta::CORE_INPUT));

        // ITER section
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 22.0)), module, Fracta::ITER_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(30.5, 33.0)), module, Fracta::ITER_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.5, 44.0)), module, Fracta::ITER_INPUT));

        // WARP section
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(49.0, 22.0)), module, Fracta::WARP_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.0, 33.0)), module, Fracta::WARP_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 44.0)), module, Fracta::WARP_INPUT));

        // FREQ section
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 60.0)), module, Fracta::VOCT_INPUT));
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 60.0)), module, Fracta::FREQ_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(49.0, 60.0)), module, Fracta::FM_PARAM));

        // FREQ mode button + LED
        FreqModeButton* fmb = createParamCentered<FreqModeButton>(mm2px(Vec(19.0, 68.0)), module, Fracta::FREQ_MODE_PARAM);
        fmb->module = module;
        addParam(fmb);
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(25.0, 68.0)), module, Fracta::FREQ_MODE_LIGHT));

        // FM input
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 73.0)), module, Fracta::FM_INPUT));

        // SYNC / BOOST section
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.0, 84.0)), module, Fracta::SYNC_INPUT));
        addParam(createParamCentered<Rogan1PWhite>(mm2px(Vec(30.5, 84.0)), module, Fracta::BOOST_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 84.0)), module, Fracta::BOOST_INPUT));

        // ALIAS section
        addParam(createParamCentered<TL1105>(mm2px(Vec(12.0, 98.0)), module, Fracta::ALIAS_PARAM));
        addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(22.0, 98.0)), module, Fracta::ALIAS_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(49.0, 98.0)), module, Fracta::ALIAS_INPUT));

        // Output
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.5, 113.0)), module, Fracta::MAIN_OUTPUT));
    }
};


Model* fracta = createModel<Fracta, FractaWidget>("fracta");
