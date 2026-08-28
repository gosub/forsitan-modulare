// ululo.cpp - VCV Rack 2 module
// ululo (Latin: "I howl") simulates holding an electric guitar up to a
// screaming amplifier. The topology follows Nathaniel Virgo's SuperCollider
// "Guitar feedback emulation" (https://sccode.org/1-U), reimplemented from
// scratch: the amp output travels through the air (a short delay set by
// DIST), excites six comb-filter "strings", and the string sum goes through
// tone filters and a saturating amp stage whose output closes the loop.
//
// The strings default to standard guitar tuning (E2 A2 D3 G3 B3 E4). A
// polyphonic V/OCT input retunes them: with fewer than six channels the
// remaining strings repeat the chord in higher octaves, so interea can play
// chords on feedback. WHAMMY bends all strings down, up to an octave.
//
// Controls:
//   Knobs : GAIN (feedback), DIST (amp distance), DECAY (string sustain),
//           TONE (amp lowpass), DRIVE (saturation), WHAMMY (bend down),
//           IN LVL (external input level)
//   In    : GAIN CV, V/OCT (poly, retunes strings), WHAMMY CV,
//           DIST CV, DECAY CV, TONE CV, IN (audio)
//   Out   : OUT
//   Light : LEVEL (output amplitude)

#include "forsitan.hpp"

static constexpr int kStrings = 6;
// standard tuning E2 A2 D3 G3 B3 E4
static const float kOpenTuning[kStrings] =
    {82.407f, 110.f, 146.832f, 195.998f, 246.942f, 329.628f};

struct Ululo : Module {
    enum ParamId {
        GAIN_PARAM,
        DIST_PARAM,
        DECAY_PARAM,
        TONE_PARAM,
        DRIVE_PARAM,
        WHAMMY_PARAM,
        IN_LEVEL_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        GAIN_CV_INPUT,
        VOCT_INPUT,
        WHAMMY_CV_INPUT,
        AUDIO_INPUT,
        // appended in 2.7.5 (after AUDIO_INPUT so saved patches keep their ports)
        DIST_CV_INPUT,
        DECAY_CV_INPUT,
        TONE_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    // one comb-filter string: y = buf[t - d]; buf[t] = in + decay * y
    struct CombString {
        std::vector<float> buf;
        int pos = 0;
        void init(int size) {
            buf.assign(size, 0.f);
            pos = 0;
        }
        float process(float in, float delaySamples, float decay) {
            int n = (int)buf.size();
            if (n == 0) return 0.f;
            delaySamples = clamp(delaySamples, 2.f, (float)(n - 2));
            float rp = pos - delaySamples;
            if (rp < 0.f) rp += n;
            int i0 = (int)rp;
            float f = rp - i0;
            int i1 = i0 + 1; if (i1 >= n) i1 -= n;
            float y = buf[i0] + f * (buf[i1] - buf[i0]);
            // soft-bound the string's energy so a long howl doesn't charge the
            // comb to huge amplitudes that take ages to ring down
            float w = in + decay * y;
            if (w >  2.f) w =  2.f + std::tanh(w - 2.f);
            if (w < -2.f) w = -2.f + std::tanh(w + 2.f);
            buf[pos] = w;
            if (++pos >= n) pos = 0;
            return y;
        }
    };

    CombString strings[kStrings];
    std::vector<float> fbBuf;    // air between amp and guitar
    int fbPos = 0;

    float lpState = 0.f;         // amp tone lowpass
    float hpX = 0.f, hpY = 0.f;  // 80 Hz highpass (DC / rumble blocker)
    float curSampleRate = 0.f;
    float levelEnv = 0.f;
    uint32_t noiseState = 0x2545f491u;

    Ululo() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Feedback gain", "%", 0.f, 100.f);
        configParam(DIST_PARAM, 0.f, 1.f, 0.4f, "Amp distance");
        configParam(DECAY_PARAM, 0.f, 1.f, 0.7f, "String decay");
        configParam(TONE_PARAM, 0.f, 1.f, 0.7f, "Tone");
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.4f, "Drive");
        configParam(WHAMMY_PARAM, 0.f, 1.f, 0.f, "Whammy (bend down)");
        configParam(IN_LEVEL_PARAM, 0.f, 1.f, 0.f, "Input level", "%", 0.f, 100.f);
        configInput(GAIN_CV_INPUT, "Feedback gain CV");
        configInput(VOCT_INPUT, "String tuning (polyphonic 1V/oct)");
        configInput(WHAMMY_CV_INPUT, "Whammy CV");
        configInput(AUDIO_INPUT, "Audio");
        configInput(DIST_CV_INPUT, "Amp distance CV");
        configInput(DECAY_CV_INPUT, "String decay CV");
        configInput(TONE_CV_INPUT, "Tone CV");
        configOutput(AUDIO_OUTPUT, "Audio");
        configLight(LEVEL_LIGHT, "Output level");
    }

    void onReset() override {
        for (int i = 0; i < kStrings; i++)
            std::fill(strings[i].buf.begin(), strings[i].buf.end(), 0.f);
        std::fill(fbBuf.begin(), fbBuf.end(), 0.f);
        fbPos = 0;
        lpState = hpX = hpY = 0.f;
        levelEnv = 0.f;
    }

    float noise() {
        uint32_t& s = noiseState;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (sr != curSampleRate) {
            curSampleRate = sr;
            // strings reach down to ~25 Hz with V/OCT + whammy
            int combSize = (int)(0.1f * sr) + 4;
            for (int i = 0; i < kStrings; i++)
                strings[i].init(combSize);
            fbBuf.assign((int)(0.11f * sr) + 4, 0.f);
            fbPos = 0;
            lpState = hpX = hpY = 0.f;
        }

        // ── controls ────────────────────────────────────────────────────────
        float gain = params[GAIN_PARAM].getValue();
        if (inputs[GAIN_CV_INPUT].isConnected())
            gain += inputs[GAIN_CV_INPUT].getVoltage() * 0.2f;
        gain = clamp(gain, 0.f, 2.f);

        // amp distance: 5 .. 100 ms, exponential
        float distK = params[DIST_PARAM].getValue();
        if (inputs[DIST_CV_INPUT].isConnected())
            distK += inputs[DIST_CV_INPUT].getVoltage() * 0.1f;
        distK = clamp(distK, 0.f, 1.f);
        float distT = 0.005f * std::pow(20.f, distK);
        float distSamples = clamp(distT * sr, 1.f, (float)fbBuf.size() - 2.f);

        // string sustain: comb feedback 0.80 .. 0.998
        float decayK = params[DECAY_PARAM].getValue();
        if (inputs[DECAY_CV_INPUT].isConnected())
            decayK += inputs[DECAY_CV_INPUT].getVoltage() * 0.1f;
        decayK = clamp(decayK, 0.f, 1.f);
        float decay = 1.f - std::pow(10.f, -0.7f - 2.f * decayK);

        // amp tone: one-pole lowpass 400 Hz .. 10 kHz
        float toneK = params[TONE_PARAM].getValue();
        if (inputs[TONE_CV_INPUT].isConnected())
            toneK += inputs[TONE_CV_INPUT].getVoltage() * 0.1f;
        toneK = clamp(toneK, 0.f, 1.f);
        float toneHz = 400.f * std::pow(25.f, toneK);
        float lpA = 1.f - std::exp(-2.f * M_PI * toneHz / sr);
        // fixed 80 Hz highpass pole
        float hpR = 1.f - 2.f * M_PI * 80.f / sr;

        float drive = 0.5f + 7.5f * params[DRIVE_PARAM].getValue();

        // whammy: bend all strings down, up to 12 semitones
        float wham = params[WHAMMY_PARAM].getValue();
        if (inputs[WHAMMY_CV_INPUT].isConnected())
            wham += inputs[WHAMMY_CV_INPUT].getVoltage() * 0.1f;
        wham = clamp(wham, 0.f, 1.f);
        float bendRatio = std::pow(2.f, wham);   // delay multiplier (down 1 octave)

        // ── string tuning ───────────────────────────────────────────────────
        float freq[kStrings];
        int channels = inputs[VOCT_INPUT].getChannels();
        if (channels > 0) {
            int used = std::min(channels, kStrings);
            for (int i = 0; i < kStrings; i++) {
                float v = inputs[VOCT_INPUT].getVoltage(i % used);
                // repeat the chord an octave up for the remaining strings
                freq[i] = dsp::FREQ_C4 * std::pow(2.f, v + (float)(i / used));
            }
        } else {
            for (int i = 0; i < kStrings; i++)
                freq[i] = kOpenTuning[i];
        }

        // ── the loop ────────────────────────────────────────────────────────
        // read the amp signal after its trip through the air
        int n = (int)fbBuf.size();
        float rp = fbPos - distSamples;
        if (rp < 0.f) rp += n;
        int i0 = (int)rp;
        float f = rp - i0;
        int i1 = i0 + 1; if (i1 >= n) i1 -= n;
        float air = fbBuf[i0] + f * (fbBuf[i1] - fbBuf[i0]);

        float ext = inputs[AUDIO_INPUT].getVoltage() * 0.1f
                  * params[IN_LEVEL_PARAM].getValue();
        // faint noise floor lets feedback start from silence
        float excite = air * gain + ext + 1e-4f * gain * noise();

        // six strings ring in parallel
        float sum = 0.f;
        for (int i = 0; i < kStrings; i++) {
            float fq = clamp(freq[i], 25.f, 4000.f);
            sum += strings[i].process(excite, sr / fq * bendRatio, decay);
        }
        sum *= 0.35f;

        // amp stage: tone lowpass, rumble highpass, saturation
        lpState += lpA * (sum - lpState);
        float hp = lpState - hpX + hpR * hpY;
        hpX = lpState;
        hpY = hp;
        if (!std::isfinite(hp)) { hp = 0.f; lpState = hpX = hpY = 0.f; onReset(); }
        float out = std::tanh(hp * drive);

        // close the loop and output
        fbBuf[fbPos] = out;
        if (++fbPos >= n) fbPos = 0;

        outputs[AUDIO_OUTPUT].setVoltage(5.f * out);

        levelEnv += (std::fabs(out) - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }
};

struct UluloWidget : ModuleWidget {
    UluloWidget(Ululo* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/ululo.svg")));

// @layout:begin ululo 40.64 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem GAIN_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem DIST_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DECAY_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem TONE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem DRIVE_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem WHAMMY_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem IN_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem GAIN_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem WHAMMY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DIST_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem DECAY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TONE_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_GAIN label 0.0 label "gain" 0.0 20.32 30.00
// @elem LABEL_DIST label 0.0 label "dist" 0.0 11.50 44.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 29.14 44.50
// @elem LABEL_TONE label 0.0 label "tone" 0.0 11.50 60.00
// @elem LABEL_DRIVE label 0.0 label "drive" 0.0 29.14 60.00
// @elem LABEL_WHAMMY label 0.0 label "whammy" 0.0 11.50 75.50
// @elem LABEL_INLVL label 0.0 label "in lvl" 0.0 29.14 75.50
// @elem LABEL_GAINCV label 0.0 label "gain" 0.0 8.50 89.00
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 20.32 89.00
// @elem LABEL_WHAMCV label 0.0 label "wham" 0.0 32.14 89.00
// @elem LABEL_DISTCV label 0.0 label "dist" 0.0 8.50 102.50
// @elem LABEL_DECAYCV label 0.0 label "decay" 0.0 20.32 102.50
// @elem LABEL_TONECV label 0.0 label "tone" 0.0 32.14 102.50
// @elem LABEL_IN label 0.0 label "in" 0.0 11.50 117.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 29.14 117.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 29.14 111.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 20.32 123.00

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(33.02f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(20.32f, 18.50f)), module, Ululo::GAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 36.00f)), module, Ululo::DIST_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.14f, 36.00f)), module, Ululo::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 51.50f)), module, Ululo::TONE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.14f, 51.50f)), module, Ululo::DRIVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.50f, 67.00f)), module, Ululo::WHAMMY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(29.14f, 67.00f)), module, Ululo::IN_LEVEL_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.50f, 81.50f)), module, Ululo::GAIN_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 81.50f)), module, Ululo::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.14f, 81.50f)), module, Ululo::WHAMMY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.50f, 95.00f)), module, Ululo::DIST_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 95.00f)), module, Ululo::DECAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.14f, 95.00f)), module, Ululo::TONE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.50f, 109.50f)), module, Ululo::AUDIO_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(29.14f, 109.50f)), module, Ululo::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(34.14f, 106.50f)), module, Ululo::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelUlulo = createModel<Ululo, UluloWidget>("ululo");
