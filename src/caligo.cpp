// caligo.cpp — VCV Rack 2 module
// caligo (Latin: "mist, fog, gloom, murk"; as a verb, "to be dark, to steam")
// is a port of Julian Parker's Greyhole: a long modulated echo wrapped inside
// a nested allpass diffusion network, so every repeat comes back smeared
// further than the last. It is not a reverb — the loop delay is long enough to
// hear as repeats, and the diffuser sits in the forward path of that loop.
//
// Sources:
//   - Julian Parker, Greyhole (2013), part of the DEIND project, with bug
//     fixes and interface changes by Till Bovermann. SuperCollider UGen in
//     sc3-plugins (source/DEINDUGens), and the same algorithm as `jp_gh_rev`
//     in Faust's faustlibraries/reverbs.lib.
//   - Named after the Eventide effect of a similar name.
//   The engine here is written from the published algorithm, not translated
//   from either upstream. See src/caligo_dsp.hpp for the topology and for the
//   fidelity measurements against a Faust build of the reference.
//
//   in ─> diffuser (3 x 4-deep nested allpass) ─> damping ─┬─> mix ─> out
//            ^                                             v
//            └── x feedback <─ rtn/snd <─ long delay <─ modulated delay
//
// Controls:
//   Knobs : TIME, SIZE, DIFF, FEEDBACK, DAMP, MOD, RATE, MIX, SPIN, DRIFT,
//           each with its own attenuverter and CV input
//   Buttons: FRZ (latching freeze), SCT (reseed the scattering network),
//           both with a gate/trigger input
//   In    : L, R (R normalled to L), CLK (time, optionally rate, snap to
//           clock ratios), RTN L/R (feedback loop return, normalled through)
//   Out   : SND L/R (feedback loop send, post long delay), L, R
//
// SPIN is the algorithm's own hardcoded pi/2 rotator angle brought out to a
// knob: fully CW is the original's hard channel interleave, fully CCW is two
// independent mono echoes. DRIFT walks SIZE with a slow bounded random walk,
// so the whole scattering pattern wanders. FEEDBACK runs to 120% and the loop
// saturator is what holds it there.

#include "forsitan.hpp"
#include "caligo_dsp.hpp"

// clock sync ratios, 1/1 in the middle — the same table antrum uses, so the
// two modules snap to the same grid
static const float kSyncRatios[15] = {
    1.f / 12.f, 1.f / 8.f, 1.f / 6.f, 1.f / 4.f, 1.f / 3.f, 1.f / 2.f,
    2.f / 3.f, 1.f, 3.f / 2.f, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f
};
static const char* kSyncNames[15] = {
    "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "2/3", "1/1",
    "3/2", "2", "3", "4", "6", "8", "12"
};

struct Caligo;

// The time knob means three different things depending on the patch: a free
// delay time, a clock ratio, or (at an extreme sample rate) a time the buffer
// cannot hold. Report whichever one is actually in force.
struct TimeQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
};

struct Caligo : Module {
    enum ParamId {
        TIME_PARAM,
        SIZE_PARAM,
        DIFF_PARAM,
        FEEDBACK_PARAM,
        DAMP_PARAM,
        MOD_PARAM,
        RATE_PARAM,
        MIX_PARAM,
        SPIN_PARAM,
        DRIFT_PARAM,
        TIME_ATT_PARAM,
        SIZE_ATT_PARAM,
        DIFF_ATT_PARAM,
        FEEDBACK_ATT_PARAM,
        DAMP_ATT_PARAM,
        MOD_ATT_PARAM,
        RATE_ATT_PARAM,
        MIX_ATT_PARAM,
        SPIN_ATT_PARAM,
        DRIFT_ATT_PARAM,
        FRZ_PARAM,
        SCT_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT,
        IN_R_INPUT,
        CLK_INPUT,
        FRZ_INPUT,
        SCT_INPUT,
        RTN_L_INPUT,
        RTN_R_INPUT,
        TIME_CV_INPUT,
        SIZE_CV_INPUT,
        DIFF_CV_INPUT,
        FEEDBACK_CV_INPUT,
        DAMP_CV_INPUT,
        MOD_CV_INPUT,
        RATE_CV_INPUT,
        MIX_CV_INPUT,
        SPIN_CV_INPUT,
        DRIFT_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SND_L_OUTPUT,
        SND_R_OUTPUT,
        OUT_L_OUTPUT,
        OUT_R_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        FRZ_LIGHT,
        SCT_LIGHT,
        RATE_LIGHT,
        LEVEL_L_LIGHT,
        LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    // one-pole parameter smoother
    struct Smooth {
        float current = 0.f, target = 0.f, b1 = 0.f;
        void setup(float initial, float freqHz, float rate) {
            current = target = initial;
            b1 = std::exp(-2.f * (float)M_PI * freqHz / rate);
        }
        inline float tick() {
            current = target + (current - target) * b1;
            return current;
        }
    };

    // diff's knob taper: mildly expanded at the bottom so the 0.3..0.7 band,
    // where the reversed-sounding build-up lives, gets more travel
    static constexpr float kDiffTaper = 0.8f;

    caligo_dsp::Engine engine;
    caligo_dsp::Params p;
    // time and size have to survive being swept; the loop parameters are
    // deliberately sluggish, and diff and spin feed block-rate trigonometry
    // so they must not step fast enough to alias
    Smooth smTime, smSize, smDiff, smFeedback, smDamp, smMod, smRate, smMix,
        smSpin, smDrift, smFreeze;
    dsp::SchmittTrigger clockTrigger, frzButton, sctButton, sctTrigger;
    bool freezeLatched = false;
    bool clockSyncRate = false;
    uint32_t scatterSeed = 0;
    float clockPeriod = 0.f;      // seconds between clock edges, 0 = unknown
    float clockTimer = 0.f;
    int pendingRatio = 7;         // ratio the knob asks for
    int appliedRatio = 7;         // ratio in force, latched at a clock edge
    float syncTimeSec = 0.f;      // clocked delay time, latched at an edge
    bool timeSynced = false;
    float effTimeSec = 0.4f;
    float sr = 0.f;
    int controlPhase = 0;
    float prevSndL = 0.f, prevSndR = 0.f;
    float sctFlash = 0.f;
    float levelEnvL = 0.f, levelEnvR = 0.f;

    Caligo() {
        using namespace caligo_dsp;
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        // exponential displays: value = multiplier * base^knob
        configParam<TimeQuantity>(TIME_PARAM, 0.f, 1.f, 0.5f, "Time", " ms",
                                  kMaxTimeSec / kMinTimeSec, kMinTimeSec * 1000.f);
        configParam(SIZE_PARAM, 0.f, 1.f, 1.f / 3.f, "Size", "x",
                    kMaxSize / kMinSize, kMinSize);
        configParam(DIFF_PARAM, 0.f, 1.f, 0.6906f,
                    "Diffusion (plain delay / build-up / smooth decay)",
                    "%", 0.f, 100.f);
        configParam(FEEDBACK_PARAM, 0.f, kMaxFeedback, 0.9f, "Feedback", "%",
                    0.f, 100.f);
        configParam(DAMP_PARAM, 0.f, 1.f, 0.f, "Damping", "%", 0.f, 100.f);
        configParam(MOD_PARAM, 0.f, 1.f, 0.1f, "Modulation depth", "%", 0.f, 100.f);
        configParam(RATE_PARAM, 0.f, 1.f, 0.7411f, "Modulation rate", " Hz",
                    500.f, 0.02f);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
        configParam(SPIN_PARAM, 0.f, 1.f, 1.f,
                    "Spin (two mono echoes / stereo interleave)", "%", 0.f, 100.f);
        configParam(DRIFT_PARAM, 0.f, 1.f, 0.f, "Drift (random walk on size)",
                    "%", 0.f, 100.f);
        configParam(TIME_ATT_PARAM, -1.f, 1.f, 0.f, "Time CV", "%", 0.f, 100.f);
        configParam(SIZE_ATT_PARAM, -1.f, 1.f, 0.f, "Size CV", "%", 0.f, 100.f);
        configParam(DIFF_ATT_PARAM, -1.f, 1.f, 0.f, "Diffusion CV", "%", 0.f, 100.f);
        configParam(FEEDBACK_ATT_PARAM, -1.f, 1.f, 0.f, "Feedback CV", "%", 0.f, 100.f);
        configParam(DAMP_ATT_PARAM, -1.f, 1.f, 0.f, "Damping CV", "%", 0.f, 100.f);
        configParam(MOD_ATT_PARAM, -1.f, 1.f, 0.f, "Modulation depth CV", "%", 0.f, 100.f);
        configParam(RATE_ATT_PARAM, -1.f, 1.f, 0.f, "Modulation rate CV", "%", 0.f, 100.f);
        configParam(MIX_ATT_PARAM, -1.f, 1.f, 0.f, "Mix CV", "%", 0.f, 100.f);
        configParam(SPIN_ATT_PARAM, -1.f, 1.f, 0.f, "Spin CV", "%", 0.f, 100.f);
        configParam(DRIFT_ATT_PARAM, -1.f, 1.f, 0.f, "Drift CV", "%", 0.f, 100.f);
        configButton(FRZ_PARAM, "Freeze the loop");
        configButton(SCT_PARAM, "Reseed the scattering network");
        configInput(IN_L_INPUT, "Left audio");
        configInput(IN_R_INPUT, "Right audio (normalled to left)");
        configInput(CLK_INPUT, "Clock (snaps time to ratios)");
        configInput(FRZ_INPUT, "Freeze gate");
        configInput(SCT_INPUT, "Scatter trigger");
        configInput(RTN_L_INPUT, "Feedback loop return left (normalled through)");
        configInput(RTN_R_INPUT, "Feedback loop return right (normalled to left)");
        configInput(TIME_CV_INPUT, "Time CV");
        configInput(SIZE_CV_INPUT, "Size CV");
        configInput(DIFF_CV_INPUT, "Diffusion CV");
        configInput(FEEDBACK_CV_INPUT, "Feedback CV");
        configInput(DAMP_CV_INPUT, "Damping CV");
        configInput(MOD_CV_INPUT, "Modulation depth CV");
        configInput(RATE_CV_INPUT, "Modulation rate CV");
        configInput(MIX_CV_INPUT, "Mix CV");
        configInput(SPIN_CV_INPUT, "Spin CV");
        configInput(DRIFT_CV_INPUT, "Drift CV");
        configOutput(SND_L_OUTPUT, "Feedback loop send left");
        configOutput(SND_R_OUTPUT, "Feedback loop send right");
        configOutput(OUT_L_OUTPUT, "Left");
        configOutput(OUT_R_OUTPUT, "Right");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
    }

    void initEngine(float sampleRate) {
        using namespace caligo_dsp;
        sr = sampleRate;
        engine.seed = scatterSeed;
        engine.init(sr);
        smTime.setup(0.4f * sr, 50.f, sr);
        smSize.setup(1.f, 100.f, sr);
        smDiff.setup(0.707f, 20.f, sr);
        smFeedback.setup(0.9f, 20.f, sr);
        smDamp.setup(0.f, 20.f, sr);
        smMod.setup(0.1f, 20.f, sr);
        smRate.setup(2.f, 20.f, sr);
        smMix.setup(0.5f, 20.f, sr);
        smSpin.setup(kMaxSpin, 20.f, sr);
        smDrift.setup(0.f, 20.f, sr);
        smFreeze.setup(0.f, 16.f, sr);
        controlPhase = 0;
        clockPeriod = 0.f;
        clockTimer = 0.f;
        syncTimeSec = 0.f;
        prevSndL = prevSndR = 0.f;
        // The setup() calls above only exist for their coefficients: start
        // every smoother at what the knobs actually say. Otherwise a patch
        // loads with eleven parameters ramping in from the factory defaults,
        // and — the reason this matters rather than being tidiness — the engine
        // primes its 24 delay lengths from the *first* size it is handed, so a
        // smoother still sitting on 1.0 would have it prime the wrong room and
        // then glide for three seconds.
        updateControls();
        smTime.current = smTime.target;
        smSize.current = smSize.target;
        smDiff.current = smDiff.target;
        smFeedback.current = smFeedback.target;
        smDamp.current = smDamp.target;
        smMod.current = smMod.target;
        smRate.current = smRate.target;
        smMix.current = smMix.target;
        smSpin.current = smSpin.target;
        smDrift.current = smDrift.target;
        smFreeze.current = smFreeze.target;
    }

    void onReset() override {
        sr = 0.f;                 // force a re-init on the next process()
        freezeLatched = false;
        scatterSeed = 0;
        clockSyncRate = false;
        p.tape = false;
        p.freezeBypassDamp = true;
        p.freezeOpenInput = false;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "freeze", json_boolean(freezeLatched));
        json_object_set_new(root, "tape", json_boolean(p.tape));
        json_object_set_new(root, "clockSyncRate", json_boolean(clockSyncRate));
        json_object_set_new(root, "freezeBypassDamp",
                            json_boolean(p.freezeBypassDamp));
        json_object_set_new(root, "freezeOpenInput",
                            json_boolean(p.freezeOpenInput));
        json_object_set_new(root, "scatterSeed",
                            json_integer((json_int_t)scatterSeed));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = json_object_get(root, "freeze");
        if (j) freezeLatched = json_boolean_value(j);
        j = json_object_get(root, "tape");
        if (j) p.tape = json_boolean_value(j);
        j = json_object_get(root, "clockSyncRate");
        if (j) clockSyncRate = json_boolean_value(j);
        j = json_object_get(root, "freezeBypassDamp");
        if (j) p.freezeBypassDamp = json_boolean_value(j);
        j = json_object_get(root, "freezeOpenInput");
        if (j) p.freezeOpenInput = json_boolean_value(j);
        j = json_object_get(root, "scatterSeed");
        if (j) {
            scatterSeed = (uint32_t)json_integer_value(j);
            engine.seed = scatterSeed;
            if (sr > 0.f) engine.reseed(scatterSeed);
        }
    }

    // knob + attenuverted CV, 5 V covering `span` of parameter range
    float knobCv(int param, int att, int input, float span) {
        float v = params[param].getValue();
        if (inputs[input].isConnected())
            v += inputs[input].getVoltage() * 0.2f * params[att].getValue() * span;
        return v;
    }

    static int ratioIndex(float t) {
        return clamp((int)std::lround(clamp(t, 0.f, 1.f) * 14.f), 0, 14);
    }

    // The seed walks deterministically, so a patch that was scattered N times
    // from a given seed lands in the same room every time it is reopened.
    void nextScatterSeed() {
        uint32_t v = scatterSeed + 0x9e3779b9u;
        v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
        scatterSeed = v ? v : 1u;
        engine.reseed(scatterSeed);
        sctFlash = 1.f;
    }

    void updateControls() {
        using namespace caligo_dsp;
        float maxTime = engine.maxTimeSec();
        timeSynced = inputs[CLK_INPUT].isConnected() && clockPeriod > 0.f;
        bool frozen = freezeLatched || inputs[FRZ_INPUT].getVoltage() >= 1.5f;

        // ---- time: 10 ms..16 s exponential, or a ratio of the clock. A
        //      frozen loop holds its length, and in dissolve mode a clocked
        //      ratio change waits for the next clock edge so it lands on the
        //      beat rather than mid-bar.
        float t = clamp(knobCv(TIME_PARAM, TIME_ATT_PARAM, TIME_CV_INPUT, 1.f),
                        0.f, 1.f);
        pendingRatio = ratioIndex(t);
        if (!frozen) {
            if (timeSynced) {
                if (syncTimeSec <= 0.f) {
                    appliedRatio = pendingRatio;
                    syncTimeSec = clockPeriod * kSyncRatios[appliedRatio];
                }
                effTimeSec = clamp(syncTimeSec, kMinTimeSec, maxTime);
            }
            else {
                effTimeSec = kMinTimeSec
                    * std::pow(kMaxTimeSec / kMinTimeSec, t);
                effTimeSec = std::min(effTimeSec, maxTime);
            }
            smTime.target = effTimeSec * sr;
        }

        // ---- size: 0.5..4, exponential
        t = clamp(knobCv(SIZE_PARAM, SIZE_ATT_PARAM, SIZE_CV_INPUT, 1.f), 0.f, 1.f);
        smSize.target = kMinSize * std::pow(kMaxSize / kMinSize, t);

        // ---- diffusion: the allpass rotation angle in radians, tapered
        t = clamp(knobCv(DIFF_PARAM, DIFF_ATT_PARAM, DIFF_CV_INPUT, 1.f), 0.f, 1.f);
        smDiff.target = kMaxDiff * std::pow(t, kDiffTaper);

        // ---- feedback: up to 120% loop gain, the saturator catching the rest
        smFeedback.target = clamp(
            knobCv(FEEDBACK_PARAM, FEEDBACK_ATT_PARAM, FEEDBACK_CV_INPUT, kMaxFeedback),
            0.f, kMaxFeedback);

        // ---- damping
        t = clamp(knobCv(DAMP_PARAM, DAMP_ATT_PARAM, DAMP_CV_INPUT, 1.f), 0.f, 1.f);
        smDamp.target = t * 0.99f;

        // ---- modulation depth and rate
        smMod.target = clamp(knobCv(MOD_PARAM, MOD_ATT_PARAM, MOD_CV_INPUT, 1.f),
                             0.f, 1.f);
        t = clamp(knobCv(RATE_PARAM, RATE_ATT_PARAM, RATE_CV_INPUT, 1.f), 0.f, 1.f);
        if (timeSynced && clockSyncRate)
            smRate.target = clamp(kSyncRatios[ratioIndex(t)] / clockPeriod, 0.02f, 20.f);
        else
            smRate.target = 0.02f * std::pow(500.f, t);

        // ---- mix, spin, drift
        smMix.target = clamp(knobCv(MIX_PARAM, MIX_ATT_PARAM, MIX_CV_INPUT, 1.f),
                             0.f, 1.f);
        t = clamp(knobCv(SPIN_PARAM, SPIN_ATT_PARAM, SPIN_CV_INPUT, 1.f), 0.f, 1.f);
        smSpin.target = t * kMaxSpin;
        smDrift.target = clamp(knobCv(DRIFT_PARAM, DRIFT_ATT_PARAM, DRIFT_CV_INPUT, 1.f),
                               0.f, 1.f);

        smFreeze.target = frozen ? 1.f : 0.f;
    }

    void process(const ProcessArgs& args) override {
        if (sr != args.sampleRate)
            initEngine(args.sampleRate);

        if (frzButton.process(params[FRZ_PARAM].getValue(), 0.1f, 0.5f))
            freezeLatched = !freezeLatched;
        bool reseed = sctButton.process(params[SCT_PARAM].getValue(), 0.1f, 0.5f);
        reseed |= sctTrigger.process(inputs[SCT_INPUT].getVoltage(), 0.2f, 1.5f);
        if (reseed)
            nextScatterSeed();

        // ---- clock tracking. The ratio in force is latched at the edge, so a
        //      knob move lands on the next beat.
        clockTimer += args.sampleTime;
        if (clockTrigger.process(inputs[CLK_INPUT].getVoltage(), 0.2f, 1.5f)) {
            if (clockTimer > 0.001f && clockTimer < 20.f)
                clockPeriod = clockTimer;
            clockTimer = 0.f;
            appliedRatio = pendingRatio;
            if (clockPeriod > 0.f)
                syncTimeSec = clockPeriod * kSyncRatios[appliedRatio];
        }
        if (clockTimer > 20.f) {
            clockPeriod = 0.f;
            syncTimeSec = 0.f;
        }

        if (controlPhase == 0)
            updateControls();

        p.timeSamples = smTime.tick();
        p.size = smSize.tick();
        p.diff = smDiff.tick();
        p.feedback = smFeedback.tick();
        p.damp = smDamp.tick();
        p.modDepth = smMod.tick();
        p.modFreq = smRate.tick();
        p.mix = smMix.tick();
        p.spin = smSpin.tick();
        p.drift = smDrift.tick();
        p.freeze = smFreeze.tick();

        if (controlPhase == 0)
            engine.updateControl(p);
        if (++controlPhase >= caligo_dsp::kCoefUpdate)
            controlPhase = 0;

        float inL = inputs[IN_L_INPUT].getVoltage() * 0.2f;
        float inR = inputs[IN_R_INPUT].isConnected()
                  ? inputs[IN_R_INPUT].getVoltage() * 0.2f : inL;

        // ---- the feedback loop break. Unpatched, each channel's return is
        //      its own send from the previous sample, which is the one sample
        //      of loop latency the algorithm has anyway. rtn R falls back to
        //      rtn L when only the left is patched.
        bool pl = inputs[RTN_L_INPUT].isConnected();
        bool pr = inputs[RTN_R_INPUT].isConnected();
        float rtnL = pl ? inputs[RTN_L_INPUT].getVoltage() * 0.2f : prevSndL;
        float rtnR = pr ? inputs[RTN_R_INPUT].getVoltage() * 0.2f
                        : (pl ? rtnL : prevSndR);

        float outL, outR, sndL, sndR;
        engine.process(inL, inR, rtnL, rtnR, p, outL, outR, sndL, sndR);
        prevSndL = sndL;
        prevSndR = sndR;

        outputs[SND_L_OUTPUT].setVoltage(clamp(5.f * sndL, -10.f, 10.f));
        outputs[SND_R_OUTPUT].setVoltage(clamp(5.f * sndR, -10.f, 10.f));
        outputs[OUT_L_OUTPUT].setVoltage(clamp(5.f * outL, -10.f, 10.f));
        outputs[OUT_R_OUTPUT].setVoltage(clamp(5.f * outR, -10.f, 10.f));

        levelEnvL += (std::fabs(outL) - levelEnvL) * 0.002f;
        levelEnvR += (std::fabs(outR) - levelEnvR) * 0.002f;
        lights[LEVEL_L_LIGHT].setBrightness(clamp(levelEnvL * 2.f, 0.f, 1.f));
        lights[LEVEL_R_LIGHT].setBrightness(clamp(levelEnvR * 2.f, 0.f, 1.f));
        lights[FRZ_LIGHT].setBrightness(smFreeze.target > 0.5f ? 1.f : 0.f);
        sctFlash -= sctFlash * 4.f * args.sampleTime;
        lights[SCT_LIGHT].setBrightness(clamp(sctFlash, 0.f, 1.f));
        lights[RATE_LIGHT].setBrightness(engine.lfoPhase < 0.5f ? 1.f : 0.f);
    }
};

std::string TimeQuantity::getDisplayValueString() {
    Caligo* m = dynamic_cast<Caligo*>(module);
    if (!m || m->sr <= 0.f)
        return ParamQuantity::getDisplayValueString();
    if (m->timeSynced)
        return string::f("%s of clock (%.0f ms)", kSyncNames[m->appliedRatio],
                         m->effTimeSec * 1000.f);
    float ceiling = m->engine.maxTimeSec();
    if (m->effTimeSec >= ceiling - 1e-4f && ceiling < caligo_dsp::kMaxTimeSec - 0.01f)
        return string::f("%.2f s (buffer limit at %.0f kHz)", m->effTimeSec,
                         m->sr * 0.001f);
    if (m->effTimeSec >= 1.f)
        return string::f("%.2f s", m->effTimeSec);
    return string::f("%.0f ms", m->effTimeSec * 1000.f);
}

struct CaligoWidget : ModuleWidget {
    CaligoWidget(Caligo* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/caligo.svg")));

// @layout:begin caligo 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem TIME_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem SIZE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DIFF_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem FEEDBACK_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DAMP_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TIME_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem TIME_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SIZE_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem SIZE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DIFF_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem DIFF_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FEEDBACK_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem FEEDBACK_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DAMP_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem DAMP_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MOD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RATE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPIN_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DRIFT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MOD_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem MOD_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RATE_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem RATE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MIX_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SPIN_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem SPIN_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DRIFT_ATT_PARAM Trimpot 3.03 param "" 0.0
// @elem DRIFT_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SND_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem SND_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem RTN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RTN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FRZ_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FRZ_PARAM TL1105 2.6 param "" 0.0
// @elem SCT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SCT_PARAM TL1105 2.6 param "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLK_INPUT PJ301MPort 4.01 input "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem FRZ_LIGHT SmallLight 1.0 light "" 0.0
// @elem SCT_LIGHT SmallLight 1.0 light "" 0.0
// @elem RATE_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_TIME label 0.0 label "time" 0.0 13.00 32.50
// @elem LABEL_SIZE label 0.0 label "size" 0.0 37.00 32.50
// @elem LABEL_DIFF label 0.0 label "diff" 0.0 61.00 32.50
// @elem LABEL_FEEDBACK label 0.0 label "feedback" 0.0 85.00 32.50
// @elem LABEL_DAMP label 0.0 label "damp" 0.0 109.00 32.50
// @elem LABEL_MOD label 0.0 label "mod" 0.0 13.00 63.50
// @elem LABEL_RATE label 0.0 label "rate" 0.0 37.00 63.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 61.00 63.50
// @elem LABEL_SPIN label 0.0 label "spin" 0.0 85.00 63.50
// @elem LABEL_DRIFT label 0.0 label "drift" 0.0 109.00 63.50
// @elem LABEL_SND_L label 0.0 label "snd L" 0.0 16.00 95.50
// @elem LABEL_SND_R label 0.0 label "snd R" 0.0 32.00 95.50
// @elem LABEL_RTN_L label 0.0 label "rtn L" 0.0 50.00 95.50
// @elem LABEL_RTN_R label 0.0 label "rtn R" 0.0 62.00 95.50
// @elem LABEL_FRZ label 0.0 label "frz" 0.0 82.25 95.50
// @elem LABEL_SCT label 0.0 label "sct" 0.0 104.25 95.50
// @elem LABEL_IN_L label 0.0 label "in L" 0.0 16.00 114.50
// @elem LABEL_IN_R label 0.0 label "in R" 0.0 32.00 114.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 61.00 114.50
// @elem LABEL_L label 0.0 label "L" 0.0 90.00 114.50
// @elem LABEL_R label 0.0 label "R" 0.0 106.00 114.50
// @elem BOX_SND_L panel_box 7.0 box "" 0.0 16.00 90.00
// @elem BOX_SND_R panel_box 7.0 box "" 0.0 32.00 90.00
// @elem BOX_OUT_L panel_box 7.0 box "" 0.0 90.00 109.00
// @elem BOX_OUT_R panel_box 7.0 box "" 0.0 106.00 109.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(13.00f, 21.00f)), module, Caligo::TIME_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(37.00f, 21.00f)), module, Caligo::SIZE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(61.00f, 21.00f)), module, Caligo::DIFF_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(85.00f, 21.00f)), module, Caligo::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(109.00f, 21.00f)), module, Caligo::DAMP_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(8.00f, 39.00f)), module, Caligo::TIME_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.50f, 39.00f)), module, Caligo::TIME_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(32.00f, 39.00f)), module, Caligo::SIZE_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.50f, 39.00f)), module, Caligo::SIZE_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(56.00f, 39.00f)), module, Caligo::DIFF_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.50f, 39.00f)), module, Caligo::DIFF_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(80.00f, 39.00f)), module, Caligo::FEEDBACK_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(89.50f, 39.00f)), module, Caligo::FEEDBACK_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(104.00f, 39.00f)), module, Caligo::DAMP_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(113.50f, 39.00f)), module, Caligo::DAMP_CV_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.00f, 55.00f)), module, Caligo::MOD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(37.00f, 55.00f)), module, Caligo::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(61.00f, 55.00f)), module, Caligo::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(85.00f, 55.00f)), module, Caligo::SPIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(109.00f, 55.00f)), module, Caligo::DRIFT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(8.00f, 70.00f)), module, Caligo::MOD_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(17.50f, 70.00f)), module, Caligo::MOD_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(32.00f, 70.00f)), module, Caligo::RATE_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.50f, 70.00f)), module, Caligo::RATE_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(56.00f, 70.00f)), module, Caligo::MIX_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.50f, 70.00f)), module, Caligo::MIX_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(80.00f, 70.00f)), module, Caligo::SPIN_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(89.50f, 70.00f)), module, Caligo::SPIN_CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(104.00f, 70.00f)), module, Caligo::DRIFT_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(113.50f, 70.00f)), module, Caligo::DRIFT_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(16.00f, 88.00f)), module, Caligo::SND_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.00f, 88.00f)), module, Caligo::SND_R_OUTPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(50.00f, 88.00f)), module, Caligo::RTN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(62.00f, 88.00f)), module, Caligo::RTN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(78.00f, 88.00f)), module, Caligo::FRZ_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(86.50f, 88.00f)), module, Caligo::FRZ_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(100.00f, 88.00f)), module, Caligo::SCT_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(108.50f, 88.00f)), module, Caligo::SCT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.00f, 107.00f)), module, Caligo::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.00f, 107.00f)), module, Caligo::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(61.00f, 107.00f)), module, Caligo::CLK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(90.00f, 107.00f)), module, Caligo::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(106.00f, 107.00f)), module, Caligo::OUT_R_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(90.00f, 84.50f)), module, Caligo::FRZ_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(112.00f, 84.50f)), module, Caligo::SCT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(42.50f, 49.50f)), module, Caligo::RATE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(95.00f, 104.00f)), module, Caligo::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(111.00f, 104.00f)), module, Caligo::LEVEL_R_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Caligo* module = getModule<Caligo>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Time change",
            {"Dissolve (crossfade, as the original)", "Tape (pitch-bending)"},
            [=]() { return module->p.tape ? 1 : 0; },
            [=](int idx) { module->p.tape = (idx == 1); }));
        menu->addChild(createIndexSubmenuItem("Clock sync target",
            {"Time", "Time + rate"},
            [=]() { return module->clockSyncRate ? 1 : 0; },
            [=](int idx) { module->clockSyncRate = (idx == 1); }));
        menu->addChild(createBoolPtrMenuItem("Freeze bypasses damping", "",
                                             &module->p.freezeBypassDamp));
        menu->addChild(createIndexSubmenuItem("Input to loop when frozen",
            {"Muted (true freeze)", "Open (feed the frozen cloud)"},
            [=]() { return module->p.freezeOpenInput ? 1 : 0; },
            [=](int idx) { module->p.freezeOpenInput = (idx == 1); }));
        menu->addChild(createMenuItem("Reset scattering to the original", "",
            [=]() {
                module->scatterSeed = 0;
                module->engine.reseed(0);
            }));
    }
};

Model* modelCaligo = createModel<Caligo, CaligoWidget>("caligo");
