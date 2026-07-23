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
//   Switch  : ABOVE (pass or mute everything outside the slider window)
//   Buttons : one per column, cycling its slider +1 -> 0 -> -1 -> +1
//   In      : AUDIO, 16 x COEFF (poly)
//   Out     : AUDIO, RESIDUAL, COMPONENTS (poly), 16 x COEFF (poly)

#include "forsitan.hpp"
#include "quadrare_dsp.hpp"

using namespace quadrare;

struct Quadrare : Module {
    enum ParamId {
        BAND0_PARAM,
        BAND1_PARAM,
        BAND2_PARAM,
        BAND3_PARAM,
        BAND4_PARAM,
        BAND5_PARAM,
        BAND6_PARAM,
        BAND7_PARAM,
        BAND8_PARAM,
        BAND9_PARAM,
        BAND10_PARAM,
        BAND11_PARAM,
        BAND12_PARAM,
        BAND13_PARAM,
        BAND14_PARAM,
        BAND15_PARAM,
        LEVEL_PARAM,
        SIZE_PARAM,
        KEEP_PARAM,
        QUANT_PARAM,
        DRYWET_PARAM,
        ABOVE_PARAM,
        ZERO0_PARAM,
        ZERO1_PARAM,
        ZERO2_PARAM,
        ZERO3_PARAM,
        ZERO4_PARAM,
        ZERO5_PARAM,
        ZERO6_PARAM,
        ZERO7_PARAM,
        ZERO8_PARAM,
        ZERO9_PARAM,
        ZERO10_PARAM,
        ZERO11_PARAM,
        ZERO12_PARAM,
        ZERO13_PARAM,
        ZERO14_PARAM,
        ZERO15_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        COEFF0_INPUT,
        COEFF1_INPUT,
        COEFF2_INPUT,
        COEFF3_INPUT,
        COEFF4_INPUT,
        COEFF5_INPUT,
        COEFF6_INPUT,
        COEFF7_INPUT,
        COEFF8_INPUT,
        COEFF9_INPUT,
        COEFF10_INPUT,
        COEFF11_INPUT,
        COEFF12_INPUT,
        COEFF13_INPUT,
        COEFF14_INPUT,
        COEFF15_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        RESIDUAL_OUTPUT,
        COMPONENTS_OUTPUT,
        COEFF0_OUTPUT,
        COEFF1_OUTPUT,
        COEFF2_OUTPUT,
        COEFF3_OUTPUT,
        COEFF4_OUTPUT,
        COEFF5_OUTPUT,
        COEFF6_OUTPUT,
        COEFF7_OUTPUT,
        COEFF8_OUTPUT,
        COEFF9_OUTPUT,
        COEFF10_OUTPUT,
        COEFF11_OUTPUT,
        COEFF12_OUTPUT,
        COEFF13_OUTPUT,
        COEFF14_OUTPUT,
        COEFF15_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        // green/red pairs, one per slider handle
        ENUMS(BAND0_LIGHT, 2),
        ENUMS(BAND1_LIGHT, 2),
        ENUMS(BAND2_LIGHT, 2),
        ENUMS(BAND3_LIGHT, 2),
        ENUMS(BAND4_LIGHT, 2),
        ENUMS(BAND5_LIGHT, 2),
        ENUMS(BAND6_LIGHT, 2),
        ENUMS(BAND7_LIGHT, 2),
        ENUMS(BAND8_LIGHT, 2),
        ENUMS(BAND9_LIGHT, 2),
        ENUMS(BAND10_LIGHT, 2),
        ENUMS(BAND11_LIGHT, 2),
        ENUMS(BAND12_LIGHT, 2),
        ENUMS(BAND13_LIGHT, 2),
        ENUMS(BAND14_LIGHT, 2),
        ENUMS(BAND15_LIGHT, 2),
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

    int size = sizeAt(4);       // active transform size, default 256
    int idx = 0;                // position within the current block

    float inBlock[kMaxSize]   = {};
    float outBlock[kMaxSize]  = {};
    float natural[kMaxSize]   = {};
    float synth[kMaxSize]     = {};   // analyzed, gained, and published on COEFF OUT
    float pending[kMaxSize]   = {};   // what was published at the previous boundary
    float recon[kMaxSize]     = {};   // pending, after the COEFF IN substitution
    float scratch[kMaxSize]   = {};
    float components[kSliders][kMaxSize] = {};

    // Dry path, delayed to match the block latency.
    static const int kRing = 4 * kMaxSize;
    float dryRing[kRing] = {};
    int dryWrite = 0;

    bool haveBlock = false;     // false until the first block has been reconstructed
    bool havePending = false;
    dsp::BooleanTrigger zeroTrigger[kSliders];
    int coeffMode = MODE_OVERLAY;
    bool wantComponents = false;

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
        for (int b = 0; b < kSliders; ++b) {
            configParam(BAND0_PARAM + b, -1.f, 1.f, 1.f,
                        string::f("Coefficient %d gain", b), "%", 0.f, 100.f);
            configInput(COEFF0_INPUT + b, string::f("Coefficient %d", b));
            configOutput(COEFF0_OUTPUT + b, string::f("Coefficient %d", b));
            configButton(ZERO0_PARAM + b, string::f("Zero coefficient %d", b));
        }
        configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Output level", "%", 0.f, 100.f);
        configSwitch(SIZE_PARAM, 0.f, (float) (kSizeCount - 1), 4.f, "Size",
                     {"16", "32", "64", "128", "256", "512"});
        configSwitch(ABOVE_PARAM, 0.f, 1.f, 0.f, "Above the window",
                     {"Pass", "Mute"});
        configParam<KeepQuantity>(KEEP_PARAM, 0.f, 1.f, 1.f, "Keep");
        configParam<QuantQuantity>(QUANT_PARAM, 0.f, 1.f, 0.f, "Quantize");
        configParam(DRYWET_PARAM, 0.f, 1.f, 1.f, "Dry/wet", "%", 0.f, 100.f);
        configInput(AUDIO_INPUT, "Audio");
        configOutput(AUDIO_OUTPUT, "Audio");
        configOutput(RESIDUAL_OUTPUT, "Residual (dry minus wet)");
        configOutput(COMPONENTS_OUTPUT, "Band components");
        configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
        perm.build(size);
    }

    void onReset() override {
        size = sizeAt(4);
        perm.build(size);
        resetBuffers();
        coeffMode = MODE_OVERLAY;
    }

    void resetBuffers() {
        idx = 0;
        dryWrite = 0;
        haveBlock = false;
        havePending = false;
        std::fill(inBlock, inBlock + kMaxSize, 0.f);
        std::fill(outBlock, outBlock + kMaxSize, 0.f);
        std::fill(pending, pending + kMaxSize, 0.f);
        std::fill(dryRing, dryRing + kRing, 0.f);
        for (int b = 0; b < kSliders; ++b)
            std::fill(components[b], components[b] + kMaxSize, 0.f);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "coeffMode", json_integer(coeffMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "coeffMode")) coeffMode = json_integer_value(j);
    }

    void process(const ProcessArgs& args) override {
        // Zero buttons cycle their slider through the three landmarks, so the
        // centre-is-mute position is reachable without aiming for it.
        for (int b = 0; b < kSliders; ++b) {
            if (!zeroTrigger[b].process(params[ZERO0_PARAM + b].getValue() > 0.5f))
                continue;
            const float g = params[BAND0_PARAM + b].getValue();
            params[BAND0_PARAM + b].setValue(g == 0.f ? -1.f : g == -1.f ? 1.f : 0.f);
        }

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
            for (int b = 0; b < kSliders; ++b)
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
        const float invN = 1.f / (float) n;

        // Analyze: forward transform, then natural -> sequency for the panel.
        std::copy(inBlock, inBlock + n, natural);
        fwht(natural, n);
        perm.toSequency(natural, synth, n);

        // Slider gains: one coefficient each, the lowest 16 in sequency order.
        for (int b = 0; b < kSliders && b < n; ++b)
            synth[b] *= params[BAND0_PARAM + b].getValue();
        // Everything above the slider window moves together: pass it through
        // untouched, or drop it and leave a steep lowpass at the window edge.
        // At n=16 there is nothing above the window and this does nothing.
        if (params[ABOVE_PARAM].getValue() > 0.5f)
            std::fill(synth + kSliders, synth + n, 0.f);
        keepLargest(synth, n, keepCount(params[KEEP_PARAM].getValue(), n), scratch);
        const float qv = params[QUANT_PARAM].getValue();
        if (qv > 0.001f) quantize(synth, n, quantLevels(qv));

        // COEFF OUT carries what the panel shows and what gets reconstructed,
        // scaled by 1/n so a constant 5 V input reads 5 V on band 0.
        for (int b = 0; b < kSliders; ++b) {
            Output& out = outputs[COEFF0_OUTPUT + b];
            out.setChannels(1);
            out.setVoltage(synth[b] * invN);
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
            bool anyDriven = false;
            for (int b = 0; b < kSliders; ++b)
                if (inputs[COEFF0_INPUT + b].isConnected()) anyDriven = true;
            for (int b = 0; b < kSliders && b < n; ++b) {
                Input& in = inputs[COEFF0_INPUT + b];
                if (in.isConnected()) {
                    const float v = in.getVoltage();
                    recon[b] = std::isfinite(v) ? v * (float) n : 0.f;
                }
                else if (coeffMode == MODE_REPLACE && anyDriven) {
                    recon[b] = 0.f;
                }
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
            const int compChans = wantComponents ? kSliders : 1;
            if (outputs[COMPONENTS_OUTPUT].getChannels() != compChans)
                outputs[COMPONENTS_OUTPUT].setChannels(compChans);
            if (wantComponents) {
                // Each slider owns a single coefficient, so its contribution is
                // one Walsh function scaled: no inverse transform needed.
                for (int b = 0; b < kSliders; ++b) {
                    const float a = (b < n ? recon[b] : 0.f) * invN;
                    const int h = perm.seq2nat[b];
                    for (int t = 0; t < n; ++t)
                        components[b][t] = a * walshSign(h, t);
                }
            }
            haveBlock = true;
        }

        std::copy(synth, synth + n, pending);
        havePending = true;
        updateLights(args, n, invN);
    }

    // One update per block rather than per sample. Each slider light shows the
    // dominant bin of its band, which carries both sign and level; at n=16 a
    // band is one bin, so it is simply the coefficient.
    void updateLights(const ProcessArgs& args, int n, float invN) {
        const float dt = args.sampleTime * n;
        for (int b = 0; b < kSliders; ++b) {
            const float v = (b < n ? synth[b] : 0.f) * invN / 5.f;
            lights[BAND0_LIGHT + 2 * b + 0].setSmoothBrightness(std::max(v, 0.f), dt);
            lights[BAND0_LIGHT + 2 * b + 1].setSmoothBrightness(std::max(-v, 0.f), dt);
        }
    }
};

struct QuadrareWidget : ModuleWidget {
    QuadrareWidget(Quadrare* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/quadrare.svg")));

// @layout:begin quadrare 162.56 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem BAND0_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND0_LIGHT
// @elem BAND1_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND1_LIGHT
// @elem BAND2_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND2_LIGHT
// @elem BAND3_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND3_LIGHT
// @elem BAND4_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND4_LIGHT
// @elem BAND5_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND5_LIGHT
// @elem BAND6_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND6_LIGHT
// @elem BAND7_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND7_LIGHT
// @elem BAND8_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND8_LIGHT
// @elem BAND9_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND9_LIGHT
// @elem BAND10_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND10_LIGHT
// @elem BAND11_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND11_LIGHT
// @elem BAND12_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND12_LIGHT
// @elem BAND13_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND13_LIGHT
// @elem BAND14_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND14_LIGHT
// @elem BAND15_PARAM VCVLightSlider 12.96 param "" 0.0 light=BAND15_LIGHT
// @elem COEFF0_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF1_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF2_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF3_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF4_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF5_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF6_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF7_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF8_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF9_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF10_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF11_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF12_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF13_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF14_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF15_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem COEFF0_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF1_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF2_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF3_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF4_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF5_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF6_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF7_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF8_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF9_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF10_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF11_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF12_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF13_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF14_INPUT PJ301MPort 4.01 input "" 0.0
// @elem COEFF15_INPUT PJ301MPort 4.01 input "" 0.0
// @elem ZERO0_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO1_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO2_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO3_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO4_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO5_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO6_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO7_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO8_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO9_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO10_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO11_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO12_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO13_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO14_PARAM TL1105 2.6 param "" 0.0
// @elem ZERO15_PARAM TL1105 2.6 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SIZE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem KEEP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem QUANT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DRYWET_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ABOVE_PARAM CKSS 2.3 param "" 0.0
// @elem COMPONENTS_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RESIDUAL_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RESIDUAL_LIGHT SmallLight 1.0 light "" 0.0
// @elem AUDIO_LIGHT SmallLight 1.0 light "" 0.0
// @elem BOX_COEFFOUT panel_box 7.0 box "" 0.0 81.28 54.50 box=156x16
// @elem LABEL_B0 label 0.0 label "0" 0.0 8.53 41.50
// @elem LABEL_B1 label 0.0 label "1" 0.0 18.23 41.50
// @elem LABEL_B2 label 0.0 label "2" 0.0 27.93 41.50
// @elem LABEL_B3 label 0.0 label "3" 0.0 37.63 41.50
// @elem LABEL_B4 label 0.0 label "4" 0.0 47.33 41.50
// @elem LABEL_B5 label 0.0 label "5" 0.0 57.03 41.50
// @elem LABEL_B6 label 0.0 label "6" 0.0 66.73 41.50
// @elem LABEL_B7 label 0.0 label "7" 0.0 76.43 41.50
// @elem LABEL_B8 label 0.0 label "8" 0.0 86.13 41.50
// @elem LABEL_B9 label 0.0 label "9" 0.0 95.83 41.50
// @elem LABEL_B10 label 0.0 label "10" 0.0 105.53 41.50
// @elem LABEL_B11 label 0.0 label "11" 0.0 115.23 41.50
// @elem LABEL_B12 label 0.0 label "12" 0.0 124.93 41.50
// @elem LABEL_B13 label 0.0 label "13" 0.0 134.63 41.50
// @elem LABEL_B14 label 0.0 label "14" 0.0 144.33 41.50
// @elem LABEL_B15 label 0.0 label "15" 0.0 154.03 41.50
// @elem LABEL_COEFFOUT label 0.0 label "coeff out" 0.0 81.28 60.50
// @elem LABEL_COEFFIN label 0.0 label "coeff in" 0.0 81.28 77.00
// @elem LABEL_ZERO label 0.0 label "zero" 0.0 81.28 92.00
// @elem LABEL_IN label 0.0 label "in" 0.0 8.38 111.50
// @elem LABEL_SIZE label 0.0 label "size" 0.0 24.58 111.50
// @elem LABEL_KEEP label 0.0 label "keep" 0.0 40.78 111.50
// @elem LABEL_QUANT label 0.0 label "quant" 0.0 56.98 111.50
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 73.18 111.50
// @elem LABEL_DRYWET label 0.0 label "dry/wet" 0.0 89.38 111.50
// @elem LABEL_ABOVE label 0.0 label "above" 0.0 105.58 111.50
// @elem LABEL_COMP label 0.0 label "comp" 0.0 121.78 111.50
// @elem LABEL_RES label 0.0 label "res" 0.0 137.98 111.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 154.18 111.50
// @elem BOX_COMP panel_box 7.0 box "" 0.0 121.78 105.50
// @elem BOX_RES panel_box 7.0 box "" 0.0 137.98 105.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 154.18 105.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 81.28 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(154.94f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(154.94f, 123.42f)))); // SCREW_BR
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(8.53f, 24.00f)), module, Quadrare::BAND0_PARAM, Quadrare::BAND0_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(18.23f, 24.00f)), module, Quadrare::BAND1_PARAM, Quadrare::BAND1_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(27.93f, 24.00f)), module, Quadrare::BAND2_PARAM, Quadrare::BAND2_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(37.63f, 24.00f)), module, Quadrare::BAND3_PARAM, Quadrare::BAND3_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(47.33f, 24.00f)), module, Quadrare::BAND4_PARAM, Quadrare::BAND4_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(57.03f, 24.00f)), module, Quadrare::BAND5_PARAM, Quadrare::BAND5_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(66.73f, 24.00f)), module, Quadrare::BAND6_PARAM, Quadrare::BAND6_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(76.43f, 24.00f)), module, Quadrare::BAND7_PARAM, Quadrare::BAND7_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(86.13f, 24.00f)), module, Quadrare::BAND8_PARAM, Quadrare::BAND8_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(95.83f, 24.00f)), module, Quadrare::BAND9_PARAM, Quadrare::BAND9_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(105.53f, 24.00f)), module, Quadrare::BAND10_PARAM, Quadrare::BAND10_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(115.23f, 24.00f)), module, Quadrare::BAND11_PARAM, Quadrare::BAND11_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(124.93f, 24.00f)), module, Quadrare::BAND12_PARAM, Quadrare::BAND12_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(134.63f, 24.00f)), module, Quadrare::BAND13_PARAM, Quadrare::BAND13_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(144.33f, 24.00f)), module, Quadrare::BAND14_PARAM, Quadrare::BAND14_LIGHT));
        addParam(createLightParamCentered<VCVLightSlider<GreenRedLight>>(mm2px(Vec(154.03f, 24.00f)), module, Quadrare::BAND15_PARAM, Quadrare::BAND15_LIGHT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.38f, 103.00f)), module, Quadrare::AUDIO_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(24.58f, 103.00f)), module, Quadrare::SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.78f, 103.00f)), module, Quadrare::KEEP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(56.98f, 103.00f)), module, Quadrare::QUANT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(73.18f, 103.00f)), module, Quadrare::LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(89.38f, 103.00f)), module, Quadrare::DRYWET_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(105.58f, 103.00f)), module, Quadrare::ABOVE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(121.78f, 103.00f)), module, Quadrare::COMPONENTS_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(137.98f, 103.00f)), module, Quadrare::RESIDUAL_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(154.18f, 103.00f)), module, Quadrare::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.53f, 52.00f)), module, Quadrare::COEFF0_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(18.23f, 52.00f)), module, Quadrare::COEFF1_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.93f, 52.00f)), module, Quadrare::COEFF2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(37.63f, 52.00f)), module, Quadrare::COEFF3_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(47.33f, 52.00f)), module, Quadrare::COEFF4_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(57.03f, 52.00f)), module, Quadrare::COEFF5_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(66.73f, 52.00f)), module, Quadrare::COEFF6_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(76.43f, 52.00f)), module, Quadrare::COEFF7_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(86.13f, 52.00f)), module, Quadrare::COEFF8_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(95.83f, 52.00f)), module, Quadrare::COEFF9_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(105.53f, 52.00f)), module, Quadrare::COEFF10_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(115.23f, 52.00f)), module, Quadrare::COEFF11_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(124.93f, 52.00f)), module, Quadrare::COEFF12_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(134.63f, 52.00f)), module, Quadrare::COEFF13_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(144.33f, 52.00f)), module, Quadrare::COEFF14_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(154.03f, 52.00f)), module, Quadrare::COEFF15_OUTPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.53f, 68.50f)), module, Quadrare::COEFF0_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.23f, 68.50f)), module, Quadrare::COEFF1_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.93f, 68.50f)), module, Quadrare::COEFF2_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(37.63f, 68.50f)), module, Quadrare::COEFF3_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.33f, 68.50f)), module, Quadrare::COEFF4_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(57.03f, 68.50f)), module, Quadrare::COEFF5_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.73f, 68.50f)), module, Quadrare::COEFF6_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(76.43f, 68.50f)), module, Quadrare::COEFF7_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(86.13f, 68.50f)), module, Quadrare::COEFF8_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(95.83f, 68.50f)), module, Quadrare::COEFF9_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(105.53f, 68.50f)), module, Quadrare::COEFF10_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(115.23f, 68.50f)), module, Quadrare::COEFF11_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(124.93f, 68.50f)), module, Quadrare::COEFF12_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(134.63f, 68.50f)), module, Quadrare::COEFF13_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(144.33f, 68.50f)), module, Quadrare::COEFF14_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(154.03f, 68.50f)), module, Quadrare::COEFF15_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(8.53f, 84.00f)), module, Quadrare::ZERO0_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(18.23f, 84.00f)), module, Quadrare::ZERO1_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(27.93f, 84.00f)), module, Quadrare::ZERO2_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(37.63f, 84.00f)), module, Quadrare::ZERO3_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(47.33f, 84.00f)), module, Quadrare::ZERO4_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(57.03f, 84.00f)), module, Quadrare::ZERO5_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(66.73f, 84.00f)), module, Quadrare::ZERO6_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(76.43f, 84.00f)), module, Quadrare::ZERO7_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(86.13f, 84.00f)), module, Quadrare::ZERO8_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(95.83f, 84.00f)), module, Quadrare::ZERO9_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(105.53f, 84.00f)), module, Quadrare::ZERO10_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(115.23f, 84.00f)), module, Quadrare::ZERO11_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(124.93f, 84.00f)), module, Quadrare::ZERO12_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(134.63f, 84.00f)), module, Quadrare::ZERO13_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(144.33f, 84.00f)), module, Quadrare::ZERO14_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(154.03f, 84.00f)), module, Quadrare::ZERO15_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(142.98f, 100.50f)), module, Quadrare::RESIDUAL_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(159.18f, 100.50f)), module, Quadrare::AUDIO_LIGHT));
        // @layout:end
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
