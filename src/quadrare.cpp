// quadrare.cpp — VCV Rack 2 module
// quadrare (Latin: "to square, to make fit") is a patchable Walsh–Hadamard
// codec. Audio is cut into non-overlapping blocks, transformed into Walsh
// coefficients, manipulated, and transformed back:
//
//   AUDIO IN -> FWHT -> 16 band gains -> KEEP -> QUANT -> IFWHT -> AUDIO OUT
//
// Walsh functions are square waves, so the coefficients describe the sign
// structure of a very short block rather than stable frequency bands. Left
// alone the round trip is transparent to float precision; the character comes
// from what you do in the coefficient domain.
//
// What makes it a modular module rather than an effect: the coefficients are
// on jacks. Each of the 16 columns owns a slider, a COEFF OUT jack and a
// COEFF IN jack, and every jack carries that band's bins as poly channels.
// All 16 jacks carry n/16 channels, always the same width, so they are
// mergeable and interchangeable. A plain OUT->IN cable is bit-exact identity,
// and feeding COEFF IN with no audio present drives the inverse transform as
// a standalone Walsh synthesizer.
//
// The band split is uniform: band k covers bins [k*n/16, (k+1)*n/16), whose
// lower edge sits at k*fs/32 (about k*1500 Hz at 48 kHz) with no n in it. So
// SIZE does not move which frequencies the sliders address; it changes the
// block rate (3 kHz down to 187 Hz), the resolution inside each band, and the
// latency. That block rate is the module's biggest sonic parameter: at n=16
// it is a grit box, at n=256 much more like a spectral filter.
//
// See quadrare-design.md for the decisions behind all of this, and
// doc/quadrare.md for the manual.
//
// Controls:
//   Sliders : 16 bipolar band gains (-1..+1), lit with the band coefficient
//   Knobs   : LEVEL, SIZE, KEEP, QUANT, DRY/WET
//   Button  : FREEZE
//   In      : AUDIO, 16 x COEFF (poly)
//   Out     : AUDIO, RESIDUAL, COMPONENTS (poly), 16 x COEFF (poly)

#include "forsitan.hpp"
#include "quadrare_dsp.hpp"

using namespace quadrare;

struct Quadrare : Module {
    enum ParamId {
        ENUMS(BAND_PARAM, kBands),
        LEVEL_PARAM,
        SIZE_PARAM,
        KEEP_PARAM,
        QUANT_PARAM,
        DRYWET_PARAM,
        FREEZE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        ENUMS(COEFF_INPUT, kBands),
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        RESIDUAL_OUTPUT,
        COMPONENTS_OUTPUT,
        ENUMS(COEFF_OUTPUT, kBands),
        OUTPUTS_LEN
    };
    enum LightId {
        ENUMS(BAND_LIGHT, kBands * 2),   // green/red pairs, in the slider handles
        FREEZE_LIGHT,
        AUDIO_LIGHT,
        RESIDUAL_LIGHT,
        LIGHTS_LEN
    };

    // COEFF IN semantics for a *connected* jack. An unconnected jack always
    // leaves its band internal, in both modes.
    enum CoeffMode {
        MODE_OVERLAY,   // absent channels stay internal
        MODE_REPLACE    // absent channels become zero
    };

    Permutation perm;

    int size = kMinSize;        // active transform size
    int idx = 0;                // position within the current block

    float inBlock[kMaxSize]   = {};
    float outBlock[kMaxSize]  = {};
    float natural[kMaxSize]   = {};
    float held[kMaxSize]      = {};   // sequency-order coefficients, FREEZE holds these
    float synth[kMaxSize]     = {};   // analyzed, gained, and published on COEFF OUT
    float pending[kMaxSize]   = {};   // what was published at the previous boundary
    float recon[kMaxSize]     = {};   // pending, after the COEFF IN substitution
    float scratch[kMaxSize]   = {};
    float components[kBands][kMaxSize] = {};

    // Dry path, delayed to match the block latency.
    static const int kRing = 4 * kMaxSize;
    float dryRing[kRing] = {};
    int dryWrite = 0;

    bool haveBlock = false;     // false until the first block has been reconstructed
    bool havePending = false;
    int coeffMode = MODE_OVERLAY;
    bool wantComponents = false;

    dsp::BooleanTrigger freezeTrigger;
    bool freeze = false;

    // KEEP maps exponentially, so the musically interesting low counts are not
    // squeezed into the first few percent of travel at n=256.
    struct KeepQuantity : ParamQuantity {
        std::string getDisplayValueString() override {
            Quadrare* m = dynamic_cast<Quadrare*>(module);
            const int n = m ? m->size : kMinSize;
            const int k = keepCount(getValue(), n);
            return string::f("%d of %d", k, n);
        }
    };
    static int keepCount(float v, int n) {
        return math::clamp((int) std::round(std::pow((float) n, v)), 1, n);
    }

    struct QuantQuantity : ParamQuantity {
        std::string getDisplayValueString() override {
            const float v = getValue();
            if (v <= 0.001f) return "off";
            return string::f("%.0f levels", quantLevels(v));
        }
    };
    static float quantLevels(float v) {
        return std::pow(2.f, math::rescale(v, 0.f, 1.f, 12.f, 1.f));
    }

    Quadrare() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        for (int b = 0; b < kBands; ++b) {
            configParam(BAND_PARAM + b, -1.f, 1.f, 1.f,
                        string::f("Band %d gain", b), "%", 0.f, 100.f);
            configInput(COEFF_INPUT + b, string::f("Band %d coefficients", b));
            configOutput(COEFF_OUTPUT + b, string::f("Band %d coefficients", b));
        }
        configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Output level", "%", 0.f, 100.f);
        configSwitch(SIZE_PARAM, 0.f, (float) (kSizeCount - 1), 0.f, "Size",
                     {"16", "32", "64", "128", "256"});
        configParam<KeepQuantity>(KEEP_PARAM, 0.f, 1.f, 1.f, "Keep");
        configParam<QuantQuantity>(QUANT_PARAM, 0.f, 1.f, 0.f, "Quantize");
        configParam(DRYWET_PARAM, 0.f, 1.f, 1.f, "Dry/wet", "%", 0.f, 100.f);
        configButton(FREEZE_PARAM, "Freeze");
        configInput(AUDIO_INPUT, "Audio");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(RESIDUAL_OUTPUT, "Residual (dry minus wet)");
        configOutput(COMPONENTS_OUTPUT, "Band components");
        configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
        perm.build(size);
    }

    void onReset() override {
        size = kMinSize;
        perm.build(size);
        resetBuffers();
        freeze = false;
        coeffMode = MODE_OVERLAY;
    }

    void resetBuffers() {
        idx = 0;
        dryWrite = 0;
        haveBlock = false;
        havePending = false;
        std::fill(inBlock, inBlock + kMaxSize, 0.f);
        std::fill(outBlock, outBlock + kMaxSize, 0.f);
        std::fill(held, held + kMaxSize, 0.f);
        std::fill(pending, pending + kMaxSize, 0.f);
        std::fill(dryRing, dryRing + kRing, 0.f);
        for (int b = 0; b < kBands; ++b)
            std::fill(components[b], components[b] + kMaxSize, 0.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "coeffMode", json_integer(coeffMode));
        json_object_set_new(root, "freeze", json_boolean(freeze));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "coeffMode")) coeffMode = json_integer_value(j);
        // "freeze" is written for readability but deliberately not restored:
        // the frozen vector itself is not saved, so loading FREEZE on would
        // leave the module stuck emitting silence with no way to tell why.
        freeze = false;
    }

    void process(const ProcessArgs& args) override {
        if (freezeTrigger.process(params[FREEZE_PARAM].getValue() > 0.5f))
            freeze = !freeze;
        lights[FREEZE_LIGHT].setBrightness(freeze ? 1.f : 0.f);

        // SIZE changes flush: the block in flight was collected at the old
        // size and its dry alignment no longer applies. Costs one block of
        // silence (0.33 ms to 5.3 ms), which is fine for a manual control.
        const int wantSize = sizeAt((int) std::round(params[SIZE_PARAM].getValue()));
        if (wantSize != size) {
            size = wantSize;
            perm.build(size);
            resetBuffers();
        }

        const float x = inputs[AUDIO_INPUT].getVoltage();
        inBlock[idx] = x;

        // Dry delayed by exactly two blocks to match the pipeline, read
        // before the write so the tap lands 2*size samples back.
        const int delay = 2 * size;
        const float dry = dryRing[(dryWrite + kRing - delay) % kRing];
        dryRing[dryWrite] = x;
        dryWrite = (dryWrite + 1) % kRing;

        const float level = params[LEVEL_PARAM].getValue();
        const float wet = haveBlock ? outBlock[idx] * level : 0.f;
        const float mix = params[DRYWET_PARAM].getValue();
        float y = (1.f - mix) * dry + mix * wet;

        // Safety net only: not part of the sound. Pathological COEFF IN
        // voltages are the realistic way to get here.
        if (!std::isfinite(y)) y = 0.f;
        y = math::clamp(y, -100.f, 100.f);
        const float residual = math::clamp(dry - wet, -100.f, 100.f);
        outputs[AUDIO_OUTPUT].setVoltage(y);
        outputs[RESIDUAL_OUTPUT].setVoltage(residual);
        lights[AUDIO_LIGHT].setSmoothBrightness(std::fabs(y) / 5.f, args.sampleTime);
        lights[RESIDUAL_LIGHT].setSmoothBrightness(std::fabs(residual) / 5.f, args.sampleTime);

        // Channel counts are set at the block boundary, not per sample.
        if (wantComponents) {
            for (int b = 0; b < kBands; ++b)
                outputs[COMPONENTS_OUTPUT].setVoltage(components[b][idx] * level, b);
        }
        else {
            outputs[COMPONENTS_OUTPUT].setVoltage(0.f);
        }

        if (++idx >= size) {
            idx = 0;
            computeBlock(args);
        }
    }

    // Called once every `size` samples, at the block boundary.
    void computeBlock(const ProcessArgs& args) {
        const int n = size;
        const int w = bandWidth(n);
        const float invN = 1.f / (float) n;

        // Analyze: forward transform, then natural -> sequency for the panel.
        std::copy(inBlock, inBlock + n, natural);
        fwht(natural, n);
        if (!freeze) perm.toSequency(natural, held, n);

        // Band gains, then the lossy stage. Both act per bin.
        std::copy(held, held + n, synth);
        for (int b = 0; b < kBands; ++b) {
            const float g = params[BAND_PARAM + b].getValue();
            if (g == 1.f) continue;
            const int lo = bandLo(b, n);
            for (int i = lo; i < lo + w; ++i) synth[i] *= g;
        }
        keepLargest(synth, n, keepCount(params[KEEP_PARAM].getValue(), n), scratch);
        const float qv = params[QUANT_PARAM].getValue();
        if (qv > 0.001f) quantize(synth, n, quantLevels(qv));

        // COEFF OUT carries what the panel shows and what gets reconstructed,
        // scaled by 1/n so a constant 5 V input reads 5 V on band 0.
        for (int b = 0; b < kBands; ++b) {
            Output& out = outputs[COEFF_OUTPUT + b];
            out.setChannels(w);
            const int lo = bandLo(b, n);
            for (int c = 0; c < w; ++c) out.setVoltage(synth[lo + c] * invN, c);
        }

        // Reconstruct the vector published at the *previous* boundary, not the
        // one just published. Rack copies cable voltages once per frame and
        // steps modules in arbitrary order, so a COEFF OUT -> COEFF IN patch
        // cannot deliver this block's coefficients back within this block.
        // Waiting a whole block means any cable delay is long past, so the
        // returned values always pair with the vector they came from and the
        // insert stays bit-exact. The price is 2*size samples of latency
        // instead of size, whether or not anything is patched.
        if (havePending) {
            std::copy(pending, pending + n, recon);

            // COEFF IN is read once per block and held for its duration.
            // External values arrive *after* the band gains, so a substituted
            // channel is not multiplied by its slider: a true insert return.
            for (int b = 0; b < kBands; ++b) {
                Input& in = inputs[COEFF_INPUT + b];
                if (!in.isConnected()) continue;
                const int lo = bandLo(b, n);
                const int chans = std::min(in.getChannels(), w);
                for (int c = 0; c < chans; ++c) {
                    const float v = in.getVoltage(c);
                    recon[lo + c] = std::isfinite(v) ? v * (float) n : 0.f;
                }
                if (coeffMode == MODE_REPLACE)
                    for (int c = chans; c < w; ++c) recon[lo + c] = 0.f;
            }

            // Synthesize: sequency -> natural, inverse transform, scale by 1/n.
            perm.toNatural(recon, natural, n);
            fwht(natural, n);
            scale(natural, n, invN);
            std::copy(natural, natural + n, outBlock);

            // Per-band time-domain contributions, only when patched: one
            // partial inverse transform per band, each zeroing every bin
            // outside that band. They sum to outBlock by linearity.
            wantComponents = outputs[COMPONENTS_OUTPUT].isConnected();
            const int compChans = wantComponents ? kBands : 1;
            if (outputs[COMPONENTS_OUTPUT].getChannels() != compChans)
                outputs[COMPONENTS_OUTPUT].setChannels(compChans);
            if (wantComponents) {
                for (int b = 0; b < kBands; ++b) {
                    std::fill(scratch, scratch + n, 0.f);
                    const int lo = bandLo(b, n);
                    for (int i = lo; i < lo + w; ++i) scratch[i] = recon[i];
                    perm.toNatural(scratch, components[b], n);
                    fwht(components[b], n);
                    scale(components[b], n, invN);
                }
            }
            haveBlock = true;
        }

        std::copy(synth, synth + n, pending);
        havePending = true;
        updateLights(args, n, w, invN);
    }

    // One update per block rather than per sample. Each slider light shows the
    // dominant bin of its band, which carries both sign and level; at n=16 a
    // band is one bin, so it is simply the coefficient.
    void updateLights(const ProcessArgs& args, int n, int w, float invN) {
        const float dt = args.sampleTime * n;
        for (int b = 0; b < kBands; ++b) {
            const int lo = bandLo(b, n);
            float peak = 0.f;
            for (int i = lo; i < lo + w; ++i)
                if (std::fabs(synth[i]) > std::fabs(peak)) peak = synth[i];
            const float v = peak * invN / 5.f;
            lights[BAND_LIGHT + 2 * b + 0].setSmoothBrightness(std::max(v, 0.f), dt);
            lights[BAND_LIGHT + 2 * b + 1].setSmoothBrightness(std::max(-v, 0.f), dt);
        }
    }
};

struct QuadrareWidget : ModuleWidget {
    QuadrareWidget(Quadrare* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/quadrare.svg")));

        const float w = 162.56f;
        const float x0 = 7.03f, dx = 9.90f;      // 16 columns
        const float ySlider = 33.f, yOut = 63.f, yIn = 81.f;
        const float yUtil = 103.f;

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(w - 7.62f, 0.f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(w - 7.62f, 123.42f))));

        for (int b = 0; b < kBands; ++b) {
            const float x = x0 + dx * b;
            addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(
                mm2px(Vec(x, ySlider)), module, Quadrare::BAND_PARAM + b,
                Quadrare::BAND_LIGHT + 2 * b));
            addOutput(createOutputCentered<PJ301MPort>(
                mm2px(Vec(x, yOut)), module, Quadrare::COEFF_OUTPUT + b));
            addInput(createInputCentered<PJ301MPort>(
                mm2px(Vec(x, yIn)), module, Quadrare::COEFF_INPUT + b));
        }

        // Ten elements across the bottom row.
        const float u0 = 8.13f, du = 16.26f;
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(u0 + du * 0, yUtil)), module, Quadrare::AUDIO_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(u0 + du * 1, yUtil)), module, Quadrare::SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(u0 + du * 2, yUtil)), module, Quadrare::KEEP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(u0 + du * 3, yUtil)), module, Quadrare::QUANT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(u0 + du * 4, yUtil)), module, Quadrare::LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(u0 + du * 5, yUtil)), module, Quadrare::DRYWET_PARAM));
        addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(
            mm2px(Vec(u0 + du * 6, yUtil)), module, Quadrare::FREEZE_PARAM,
            Quadrare::FREEZE_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(u0 + du * 7, yUtil)), module, Quadrare::COMPONENTS_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(u0 + du * 8, yUtil)), module, Quadrare::RESIDUAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(u0 + du * 9, yUtil)), module, Quadrare::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(u0 + du * 8 + 5.f, yUtil - 5.f)), module, Quadrare::RESIDUAL_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(u0 + du * 9 + 5.f, yUtil - 5.f)), module, Quadrare::AUDIO_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        Quadrare* m = dynamic_cast<Quadrare*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Coeff in mode",
            {"Overlay (absent channels stay internal)",
             "Replace (absent channels become zero)"},
            [m]() { return m->coeffMode; },
            [m](int i) { m->coeffMode = i; }));
    }
};

Model* modelQuadrare = createModel<Quadrare, QuadrareWidget>("quadrare");
