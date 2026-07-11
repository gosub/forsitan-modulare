// bulla.cpp — VCV Rack 2 module
// bulla (Latin: "bubble, blip"; also the Roman amulet) is an implementation
// of Rob Hordijk's Blippoo Box, the Benjolin's chaotic sibling, described in
// his paper "The Blippoo Box: A Chaotic Electronic Music Instrument, Bent by
// Design" (Leonardo Music Journal, 2009). Structure follows olaf's
// SuperCollider realization (https://sccode.org/1-5bB), reimplemented from
// scratch.
//
// Two triangle oscillators cross-modulate each other and clock two
// "runglers" (8-step shift registers, each clocked by one oscillator and fed
// data bits by the other, with a 3-bit DAC on the newest bits). The rungler
// CVs modulate the oscillators back, plus the two cutoffs of a twin-peak
// filter (two resonant lowpasses in opposite phase, summed), which processes
// the comparator square of the two triangles. A sample & hold (osc A
// sampled by osc B) adds a third modulation stream. The whole thing hovers
// deliciously between pattern and chaos.
//
// Controls:
//   Knobs : FREQ A, FREQ B, RES, B>A, A>B, SH>OSC, R>A, R>B, SH>FLT,
//           PEAK 1, PEAK 2, R>FLT
//   In    : FREQ A CV, FREQ B CV, PEAK 1 CV, PEAK 2 CV (all 1V/oct-ish)
//   Out   : OUT (twin-peak filter), RUNG (rungler CV, 0..10V)
//   Light : LEVEL (output amplitude)

#include "forsitan.hpp"

struct Bulla : Module {
    enum ParamId {
        FREQ_A_PARAM,
        FREQ_B_PARAM,
        RES_PARAM,
        FM_BA_PARAM,
        FM_AB_PARAM,
        SAH_OSC_PARAM,
        RUNG_A_PARAM,
        RUNG_B_PARAM,
        SAH_FILT_PARAM,
        PEAK1_PARAM,
        PEAK2_PARAM,
        RUNG_FILT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        FREQ_A_CV_INPUT,
        FREQ_B_CV_INPUT,
        PEAK1_CV_INPUT,
        PEAK2_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        RUNGLER_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    // oscillators
    float phaseA = 0.f, phaseB = 0.25f;
    bool prevPulsA = false, prevPulsB = false;
    // runglers: 3-bit shift registers with DAC on the newest bits
    uint32_t regA = 0x5u, regB = 0x2u;
    float rungler1 = 0.f, rungler2 = 0.f;   // smoothed DAC outputs
    float sah = 0.f, sahLag = 0.f;
    float compLag = 0.f;
    // twin-peak Chamberlin SVFs (+ cutoff smoothing)
    float lp1Low = 0.f, lp1Band = 0.f, lp2Low = 0.f, lp2Band = 0.f;
    float pf1Lag = 200.f, pf2Lag = 800.f;
    float levelEnv = 0.f;

    Bulla() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // oscillators sweep from LFO into audio range: ~0.03 Hz .. 11 kHz
        configParam(FREQ_A_PARAM, 0.f, 1.f, 0.65f, "Osc A frequency");
        configParam(FREQ_B_PARAM, 0.f, 1.f, 0.35f, "Osc B frequency");
        configParam(RES_PARAM, 0.f, 1.f, 0.7f, "Filter resonance");
        configParam(FM_BA_PARAM, 0.f, 1.f, 0.1f, "Osc B > osc A FM");
        configParam(FM_AB_PARAM, 0.f, 1.f, 0.05f, "Osc A > osc B FM");
        configParam(SAH_OSC_PARAM, 0.f, 1.f, 0.1f, "S&H > oscillators");
        configParam(RUNG_A_PARAM, 0.f, 1.f, 0.3f, "Rungler > osc A");
        configParam(RUNG_B_PARAM, 0.f, 1.f, 0.15f, "Rungler > osc B");
        configParam(SAH_FILT_PARAM, 0.f, 1.f, 0.25f, "S&H > filter");
        configParam(PEAK1_PARAM, 0.f, 1.f, 0.35f, "Peak 1 frequency");
        configParam(PEAK2_PARAM, 0.f, 1.f, 0.55f, "Peak 2 frequency");
        configParam(RUNG_FILT_PARAM, 0.f, 1.f, 0.4f, "Rungler > filter");
        configInput(FREQ_A_CV_INPUT, "Osc A frequency CV (1V/oct)");
        configInput(FREQ_B_CV_INPUT, "Osc B frequency CV (1V/oct)");
        configInput(PEAK1_CV_INPUT, "Peak 1 frequency CV (1V/oct)");
        configInput(PEAK2_CV_INPUT, "Peak 2 frequency CV (1V/oct)");
        configOutput(AUDIO_OUTPUT, "Twin-peak filter");
        configOutput(RUNGLER_OUTPUT, "Rungler CV");
        configLight(LEVEL_LIGHT, "Output level");
    }

    void onReset() override {
        phaseA = 0.f; phaseB = 0.25f;
        regA = 0x5u; regB = 0x2u;
        rungler1 = rungler2 = 0.f;
        sah = sahLag = compLag = 0.f;
        lp1Low = lp1Band = lp2Low = lp2Band = 0.f;
        levelEnv = 0.f;
    }

    // fold into [0, 20000] Hz
    static float fold(float f) {
        f = std::fabs(f);
        if (f > 20000.f) f = 40000.f - f;
        return clamp(f, 0.f, 20000.f);
    }

    // rungler DAC: newest bit heaviest, per the SC realization
    static float dac(uint32_t reg) {
        return ((reg & 1u) * 4u + ((reg >> 1) & 1u) * 2u + ((reg >> 2) & 1u)) / 7.f;
    }

    static float softLimit(float x) {
        if (x >  1.f) return  1.f + std::tanh(x - 1.f);
        if (x < -1.f) return -1.f + std::tanh(x + 1.f);
        return x;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;

        // ── knob mappings ───────────────────────────────────────────────────
        // base freqs ~0.03 Hz .. 11 kHz, CVs are 1V/oct
        float pitchA = -5.f + 18.5f * params[FREQ_A_PARAM].getValue()
                     + inputs[FREQ_A_CV_INPUT].getVoltage();
        float pitchB = -5.f + 18.5f * params[FREQ_B_PARAM].getValue()
                     + inputs[FREQ_B_CV_INPUT].getVoltage();
        float freqA = std::pow(2.f, clamp(pitchA, -6.f, 14.f));
        float freqB = std::pow(2.f, clamp(pitchB, -6.f, 14.f));

        // modulation depths in Hz, quadratic taper up to 5 kHz
        auto depth = [&](int p) {
            float k = params[p].getValue();
            return 5000.f * k * k;
        };
        float fmBA = depth(FM_BA_PARAM);
        float fmAB = depth(FM_AB_PARAM);
        float fmRA = depth(RUNG_A_PARAM);
        float fmRB = depth(RUNG_B_PARAM);
        float fmSahOsc  = depth(SAH_OSC_PARAM);
        float fmSahFilt = depth(SAH_FILT_PARAM);
        float fmRFilt   = depth(RUNG_FILT_PARAM);

        // peak freqs 20 Hz .. 20 kHz, CVs 1V/oct
        float p1 = 20.f * std::pow(2.f, 10.f * params[PEAK1_PARAM].getValue()
                                        + inputs[PEAK1_CV_INPUT].getVoltage());
        float p2 = 20.f * std::pow(2.f, 10.f * params[PEAK2_PARAM].getValue()
                                        + inputs[PEAK2_CV_INPUT].getVoltage());

        // ── previous-sample triangle values (one-sample feedback, as SC) ────
        float triA = 1.f - 4.f * std::fabs(phaseA - 0.5f);
        float triB = 1.f - 4.f * std::fabs(phaseB - 0.5f);

        // ── advance oscillators with all modulations ────────────────────────
        float fA = fold(freqA + triB * fmBA + rungler1 * fmRA + sahLag * fmSahOsc);
        float fB = fold(freqB + triA * fmAB + rungler2 * fmRB + sahLag * fmSahOsc);
        phaseA += fA / sr; if (phaseA >= 1.f) phaseA -= 1.f;
        phaseB += fB / sr; if (phaseB >= 1.f) phaseB -= 1.f;
        triA = 1.f - 4.f * std::fabs(phaseA - 0.5f);
        triB = 1.f - 4.f * std::fabs(phaseB - 0.5f);

        // pulse = "triangle is rising" (first half of the phase)
        bool pulsA = phaseA < 0.5f;
        bool pulsB = phaseB < 0.5f;

        // ── runglers, clocked on pulse rising edges ─────────────────────────
        if (pulsA && !prevPulsA)
            regA = (regA << 1) | (triB > 0.f ? 1u : 0u);   // clock A, data B
        if (pulsB && !prevPulsB) {
            regB = (regB << 1) | (triA > 0.f ? 1u : 0u);   // clock B, data A
            sah = triA;                                     // S&H: A latched by B
        }
        prevPulsA = pulsA;
        prevPulsB = pulsB;

        // 0.1 ms smoothing on the stepped signals (the SC .lag calls)
        float lagA = std::min(1.f, 10000.f / sr);
        rungler1 += (dac(regA) - rungler1) * lagA;
        rungler2 += (dac(regB) - rungler2) * lagA;
        sahLag += (sah - sahLag) * lagA;
        float rung = rungler1 + rungler2;

        // comparator of the two triangles is the filter's audio source
        float comp = (triA < triB ? 0.5f : -0.5f);
        compLag += (comp - compLag) * lagA;

        // ── twin-peak filter ────────────────────────────────────────────────
        float pf1 = std::fabs(p1 - sahLag * fmSahFilt + rung * fmRFilt);
        float pf2 = std::fabs(p2 + sahLag * fmSahFilt + rung * fmRFilt);
        // ~5 ms cutoff smoothing keeps the filter stable (the SC .lag(0.005))
        float lagF = std::min(1.f, 200.f / sr);
        pf1Lag += (pf1 - pf1Lag) * lagF;
        pf2Lag += (pf2 - pf2Lag) * lagF;

        float q1 = 1.f / (1.f + 19.f * params[RES_PARAM].getValue());
        auto rlpf = [&](float in, float fc, float& low, float& band) {
            fc = clamp(fc, 20.f, std::min(0.2f * sr, 16000.f));
            float f1 = 2.f * std::sin((float)M_PI * fc / sr);
            low += f1 * band;
            float high = in - low - q1 * band;
            band += f1 * high;
            if (!std::isfinite(low) || !std::isfinite(band)) low = band = 0.f;
            return low;
        };
        float sig = rlpf(compLag, pf1Lag, lp1Low, lp1Band)
                  + rlpf(-compLag, pf2Lag, lp2Low, lp2Band);

        float out = softLimit(sig * 0.4f);
        outputs[AUDIO_OUTPUT].setVoltage(5.f * out);
        outputs[RUNGLER_OUTPUT].setVoltage(5.f * rung);   // 0..10V

        levelEnv += (std::fabs(out) - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }
};

struct BullaWidget : ModuleWidget {
    BullaWidget(Bulla* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/bulla.svg")));

// @layout:begin bulla 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FREQ_A_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FREQ_B_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RES_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FM_BA_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FM_AB_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SAH_OSC_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RUNG_A_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RUNG_B_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SAH_FILT_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem PEAK1_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem PEAK2_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RUNG_FILT_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FREQ_A_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FREQ_B_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PEAK1_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PEAK2_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RUNGLER_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_FREQA label 0.0 label "freq a" 0.0 10.40 28.50
// @elem LABEL_FREQB label 0.0 label "freq b" 0.0 25.40 28.50
// @elem LABEL_RES label 0.0 label "res" 0.0 40.40 28.50
// @elem LABEL_BA label 0.0 label "b>a" 0.0 10.40 45.50
// @elem LABEL_AB label 0.0 label "a>b" 0.0 25.40 45.50
// @elem LABEL_SHOSC label 0.0 label "sh>osc" 0.0 40.40 45.50
// @elem LABEL_RA label 0.0 label "r>a" 0.0 10.40 62.50
// @elem LABEL_RB label 0.0 label "r>b" 0.0 25.40 62.50
// @elem LABEL_SHFLT label 0.0 label "sh>flt" 0.0 40.40 62.50
// @elem LABEL_PEAK1 label 0.0 label "peak 1" 0.0 10.40 79.50
// @elem LABEL_PEAK2 label 0.0 label "peak 2" 0.0 25.40 79.50
// @elem LABEL_RFLT label 0.0 label "r>flt" 0.0 40.40 79.50
// @elem LABEL_CVA label 0.0 label "a" 0.0 7.40 94.50
// @elem LABEL_CVB label 0.0 label "b" 0.0 19.40 94.50
// @elem LABEL_CVP1 label 0.0 label "p1" 0.0 31.40 94.50
// @elem LABEL_CVP2 label 0.0 label "p2" 0.0 43.40 94.50
// @elem LABEL_RUNG label 0.0 label "rung" 0.0 25.10 114.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 40.90 114.00
// @elem BOX_RUNG panel_box 7.0 box "" 0.0 25.10 108.50
// @elem BOX_OUT panel_box 7.0 box "" 0.0 40.90 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 20.00f)), module, Bulla::FREQ_A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 20.00f)), module, Bulla::FREQ_B_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 20.00f)), module, Bulla::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 37.00f)), module, Bulla::FM_BA_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 37.00f)), module, Bulla::FM_AB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 37.00f)), module, Bulla::SAH_OSC_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 54.00f)), module, Bulla::RUNG_A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 54.00f)), module, Bulla::RUNG_B_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 54.00f)), module, Bulla::SAH_FILT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 71.00f)), module, Bulla::PEAK1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 71.00f)), module, Bulla::PEAK2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 71.00f)), module, Bulla::RUNG_FILT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.40f, 87.00f)), module, Bulla::FREQ_A_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.40f, 87.00f)), module, Bulla::FREQ_B_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.40f, 87.00f)), module, Bulla::PEAK1_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.40f, 87.00f)), module, Bulla::PEAK2_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.10f, 106.50f)), module, Bulla::RUNGLER_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.90f, 106.50f)), module, Bulla::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(44.70f, 102.70f)), module, Bulla::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelBulla = createModel<Bulla, BullaWidget>("bulla");
