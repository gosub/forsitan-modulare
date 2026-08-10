// scrupea.cpp — VCV Rack 2 module
// scrupea (Latin: jagged, made of sharp stones -- Virgil's scrupea saxa; and
// about as close as a real Latin word gets to the sound of Skrewell) takes
// after Skrewell, the chaotic sound generator in the REAKTOR factory library,
// whose interface is eight vertical bars and four knobs and whose output is
// anything from a meditative drone to crackling harshness. This is not a
// port: Skrewell's chaos lives inside REAKTOR's closed built-in filters and
// people who tried to reproduce it elsewhere found that the smallest
// difference in the filter changes everything ("butterflies, hurricanes, and
// stuff"). What is taken is the architecture, described in the factory
// library manual, and the interface.
//
// Eight voices, each holding two levers, a lever being an oscillator into a
// feedback comb tuned as a pitch with a normalizer in it. Two of the three
// topologies put a resonant filter in that loop or in front of it. Every
// lever is frequency- and amplitude-modulated by a lever in whichever voice
// its fm and am bars point at, so the bank is coupled all-to-all. There is
// no gate and no pitch input; like the original it simply runs.
//
// Every voice has its own value for each of eight parameters, edited as
// eight bars in the edit area, one bar per voice. The four macro knobs do
// not offset those values, they *map* them: three of them are the position
// of a curve -- the fourth power, the identity, the fourth root -- so hard
// left crushes the whole bank low and only the tallest bars survive, and
// hard right lifts them all. The fourth, flow, is a crossfade of five
// global settings between a calm end and a chaotic one.
//
// The mapping laws, the loop order and every numeric range here are read out
// of Skrewell's own ensemble file; see the header of scrupea_dsp.hpp for what
// that read did and did not recover.
//
// Controls:
//   Edit  : eight bars, one per voice, for the selected function. Draw
//           sets bars directly, wrap shifts all of them and mirrors at the
//           ends, rand jogs all of them at once.
//   Knobs : function, level (a dB fader), and the four macros pitch, cutoff,
//           delay, flow (each with an attenuverter and a CV input)
//   Switch: mode (loop / pre / bare), edit (draw / wrap / rand)
//   In    : audio (injected into all sixteen loops), rand trigger
//   Out   : L, R, cv (the bank's own slow wander, +/-5 V)
//   Light : L and R output level

#include "forsitan.hpp"
#include "scrupea_dsp.hpp"

using scrupea_dsp::NCH;

static const int NFUNC = 8;

// The eight bars, in the ensemble's own order and standing for the same
// things: `F fm A am cut lbh DEL FB`. Two of them are read differently
// depending on which tone generator is running, exactly as in the original,
// where the three generators share one set of eight bars: the multimode one
// reads bars five and six as a cutoff and a filter type, the bandpass one as
// the two corners of its band, and the third has no filter and ignores both.
static const char* funcName[NFUNC] = {
    "pitch", "fm", "amp", "am", "cutoff", "type", "time", "fbk"
};
static const char* funcNameLoop[NFUNC] = {
    "pitch", "fm", "amp", "am", "hp", "lp", "time", "fbk"
};

// A starting bank. The three rows that matter for whether this thing sits
// still are fbk, time and pitch, and they are set where they are on purpose:
// long delays (30-310 ms) with every loop near unity, and the eight pitches
// inside a fifth of each other rather than spread over three octaves. That is
// the regime where the bank wanders on its own -- the loops take a tenth of a
// second per pass, so state survives long enough to evolve, and voices close
// in pitch beat slowly against each other through the cross-modulation. An
// earlier default with half the feedback and 2-40 ms delays measured 0.04
// octaves of spectral wander over a minute. See "Making it wander" in the
// manual. The snapshots in the ensemble would have given the original's own
// starting values, but they are packed and did not decode.
static const float chDefault[NFUNC][NCH] = {
    {0.48f, 0.38f, 0.56f, 0.42f, 0.52f, 0.34f, 0.60f, 0.44f},   // pitch
    {0.30f, 0.22f, 0.36f, 0.18f, 0.28f, 0.34f, 0.24f, 0.32f},   // fm
    {0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f},   // amp
    {0.26f, 0.34f, 0.20f, 0.38f, 0.30f, 0.24f, 0.36f, 0.28f},   // am
    {0.66f, 0.52f, 0.74f, 0.58f, 0.70f, 0.48f, 0.78f, 0.62f},   // cutoff
    {0.10f, 0.45f, 0.00f, 0.60f, 0.20f, 0.85f, 0.05f, 0.35f},   // type
    {0.74f, 0.87f, 0.70f, 0.94f, 0.81f, 1.00f, 0.77f, 0.90f},   // time
    {0.95f, 0.91f, 0.99f, 0.88f, 0.97f, 0.92f, 1.00f, 0.89f},   // fbk
};

enum FuncId { F_PITCH, F_FM, F_AMP, F_AM, F_CUTOFF, F_TYPE, F_TIME, F_FBK };

// The bare topology has no filter, so two of the eight functions have nothing
// to point at there. carloskleiber, dissecting the original's polycontrol:
// "In Skrewell it controls 8 (or 6) parameters of 8 oscillators" -- the six
// being exactly this case. The bars stay editable, because they are real
// parameters and a mode switch must not silently drop them, but the edit area
// says so rather than letting you draw into a function that does nothing.
static bool funcActive(int func, int topology) {
    if (topology != scrupea_dsp::TOPO_BARE) return true;
    return func != F_CUTOFF && func != F_TYPE;
}

// How many samples between control-rate updates of the mapped targets.
static const int CONTROL_PERIOD = 32;

// Volts per internal unit at 0 dB on the fader. REAKTOR's audio is +-1 into
// a soundcard; a Rack cable is +-5, and eight voices sum into each side.
static const float OUT_VOLTS = 0.9f;

// Lissajous history, written by the audio thread and read by the widget.
//
// **Sampled at Reaktor's event rate, not at audio rate**, and that is the
// whole difference between a figure and a fog. The display in the ensemble is
// an XY panel element, and a panel element is fed events: the audio rate over
// the control-rate ratio, 64 by default, so about 750 points a second at
// 48 kHz rather than eight thousand. Undersampling a Lissajous is not a loss
// of detail, it is the mechanism -- the sampling beats against the signal and
// the figure is traced out slowly and precesses, which is where the curves
// and the little curls come from. Sample it densely instead and every one of
// those figures fills in and becomes a cloud.
//
// 2048 points at that rate is about 2.7 seconds of persistence.
// 384 points per voice at that rate is about half a second of persistence.
static const int SCOPE_POINTS = 384;
static const int SCOPE_DECIM = 64;

struct Scrupea : Module {
    enum ParamId {
        CH_PARAM,                        // NFUNC * NCH of them, func-major
        FUNC_PARAM = CH_PARAM + NFUNC * NCH,
        MODE_PARAM,
        EDIT_PARAM,
        PITCH_PARAM,
        CUTOFF_PARAM,
        DELAY_PARAM,
        FLOW_PARAM,
        PITCH_ATT_PARAM,
        CUTOFF_ATT_PARAM,
        DELAY_ATT_PARAM,
        FLOW_ATT_PARAM,
        LEVEL_PARAM,
        RAND_PARAM,
        MORPH_PARAM,
        MORPH_TIME_PARAM,
        SCOPE_X_PARAM,     // no widget: dragged on the display itself
        SCOPE_Y_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        PITCH_CV_INPUT,
        CUTOFF_CV_INPUT,
        DELAY_CV_INPUT,
        FLOW_CV_INPUT,
        AUDIO_INPUT,
        RAND_INPUT,
        MORPH_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        CV_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEFT_LIGHT,
        RIGHT_LIGHT,
        LIGHTS_LEN
    };

    scrupea_dsp::Engine eng;
    scrupea_dsp::Targets tgt;
    int controlPhase = 0;
    dsp::SchmittTrigger randTrig;
    dsp::BooleanTrigger randBtn;
    float envL = 0.f, envR = 0.f;

    // morph: every bar walking to a destination of its own, the way a rand
    // drag moves them, but on a gate and at a time you set rather than at the
    // speed of your hand
    float morphTarget[NFUNC][NCH] = {};
    float morphInc[NFUNC][NCH] = {};
    int morphLeft[NFUNC][NCH] = {};
    bool morphing = false;

    // scope ring buffer, one point per voice per slot
    float scopeX[SCOPE_POINTS][NCH] = {};
    float scopeY[SCOPE_POINTS][NCH] = {};
    int scopeHead = 0, scopeCount = 0;

    Scrupea() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        for (int f = 0; f < NFUNC; f++)
            for (int c = 0; c < NCH; c++)
                configParam(CH_PARAM + f * NCH + c, 0.f, 1.f, chDefault[f][c],
                            string::f("Channel %d %s", c + 1, funcName[f]),
                            "%", 0.f, 100.f);

        configSwitch(FUNC_PARAM, 0.f, (float)(NFUNC - 1), 0.f, "Function",
                     {"Pitch", "FM source", "Amplitude", "AM source",
                      "Cutoff / HP corner", "Filter type / LP corner",
                      "Delay time", "Feedback"});
        configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Topology",
                     {"Filter in the loop", "Filter before the delay",
                      "No filter, parabolic oscillator"});
        configSwitch(EDIT_PARAM, 0.f, 2.f, 0.f, "Edit mode",
                     {"Draw", "Wrap", "Rand"});
        getParamQuantity(FUNC_PARAM)->randomizeEnabled = false;
        getParamQuantity(EDIT_PARAM)->randomizeEnabled = false;

        // The four master knobs are 0..1 in the ensemble, and so are they
        // here. Three of them are the position of a `shaper`, which is a
        // curve and not an offset; the fourth is the Pos of five Selectors.
        configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch mapping");
        configParam(CUTOFF_PARAM, 0.f, 1.f, 0.5f, "Cutoff mapping");
        configParam(DELAY_PARAM, 0.f, 1.f, 0.5f, "Delay time mapping");
        configParam(FLOW_PARAM, 0.f, 1.f, 0.5f, "Flow");
        configParam(PITCH_ATT_PARAM, -1.f, 1.f, 0.f, "Pitch CV amount");
        configParam(CUTOFF_ATT_PARAM, -1.f, 1.f, 0.f, "Cutoff CV amount");
        configParam(DELAY_ATT_PARAM, -1.f, 1.f, 0.f, "Delay CV amount");
        configParam(FLOW_ATT_PARAM, -1.f, 1.f, 0.f, "Flow CV amount");
        // The ensemble's output stage is a dB fader with exactly this travel,
        // and it starts where the ensemble's starts, at unity.
        configParam(LEVEL_PARAM, scrupea_dsp::K_OUT_MIN_DB, scrupea_dsp::K_OUT_MAX_DB,
                    0.f, "Output level", " dB");
        configButton(RAND_PARAM, "Randomize channels");
        configButton(MORPH_PARAM, "Morph");
        // time for a bar to reach wherever it is going, 0.1 s to 30 s
        configParam(MORPH_TIME_PARAM, 0.f, 1.f, 0.5f, "Morph time", " s",
                    300.f, 0.1f);

        // Skrewell's XY pad. It is a Reaktor XY element, which is a display
        // and a mouse control at once: dragging it emits MX and MY, two
        // one-poles at about 0.8 Hz smooth them, and they become `scX` and
        // `scY` -- the positions of two Selectors that choose which lever of
        // the running tone generator drives each axis of the Lissajous. It
        // changes what you are looking at, not what you are hearing. Here you
        // drag the Lissajous itself; at 0 and 1 the axes are L and R.
        configParam(SCOPE_X_PARAM, 0.f, 4.f, 1.f, "Scope X scale");
        configParam(SCOPE_Y_PARAM, 0.f, 4.f, 1.f, "Scope Y scale");
        getParamQuantity(SCOPE_X_PARAM)->randomizeEnabled = false;
        getParamQuantity(SCOPE_Y_PARAM)->randomizeEnabled = false;

        configInput(PITCH_CV_INPUT, "Pitch mapping CV");
        configInput(CUTOFF_CV_INPUT, "Cutoff mapping CV");
        configInput(DELAY_CV_INPUT, "Delay mapping CV");
        configInput(FLOW_CV_INPUT, "Flow CV");
        configInput(AUDIO_INPUT, "Audio (into all eight loops)");
        configInput(RAND_INPUT, "Randomize trigger");
        configInput(MORPH_INPUT, "Morph gate");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configOutput(CV_OUTPUT, "Chaos CV");
        configBypass(AUDIO_INPUT, LEFT_OUTPUT);
        configBypass(AUDIO_INPUT, RIGHT_OUTPUT);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        eng.setSampleRate(e.sampleRate);
    }

    void onReset() override {
        eng.reset();
        scopeCount = 0;
    }

    float macro(int knob, int att, int cv) {
        float v = params[knob].getValue();
        if (inputs[cv].isConnected())
            v += params[att].getValue() * inputs[cv].getVoltage() * 0.1f;
        return clamp(v, 0.f, 1.f);
    }

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    // Morph: every bar walks to a destination of its own, the way a rand
    // drag moves them, but driven by a gate and at a time you set rather than
    // by the speed of your hand. Each leg takes the same time whatever its
    // distance, so the whole bank arrives together and immediately sets off
    // somewhere new -- a fresh bank every `time`, not a jitter.
    void startMorph(float secs, float sr) {
        const int n = std::max(1, (int)(secs * sr / CONTROL_PERIOD));
        for (int f = 0; f < NFUNC; f++)
            for (int c = 0; c < NCH; c++)
                aimMorph(f, c, n);
    }

    void aimMorph(int f, int c, int ticks) {
        const float here = params[CH_PARAM + f * NCH + c].getValue();
        morphTarget[f][c] = random::uniform();
        morphInc[f][c] = (morphTarget[f][c] - here) / (float)ticks;
        morphLeft[f][c] = ticks;
    }

    void morphStep(float secs, float sr) {
        const int n = std::max(1, (int)(secs * sr / CONTROL_PERIOD));
        for (int f = 0; f < NFUNC; f++) {
            for (int c = 0; c < NCH; c++) {
                Param& p = params[CH_PARAM + f * NCH + c];
                if (--morphLeft[f][c] <= 0) {
                    p.setValue(clamp(morphTarget[f][c], 0.f, 1.f));
                    aimMorph(f, c, n);   // the trimpot bites on the next leg
                } else {
                    p.setValue(clamp(p.getValue() + morphInc[f][c], 0.f, 1.f));
                }
            }
        }
    }

    void randomizeChannels() {
        // All 64, as the original's does: "sends random values to all 64
        // columns", in carloskleiber's words.
        for (int f = 0; f < NFUNC; f++)
            for (int c = 0; c < NCH; c++)
                params[CH_PARAM + f * NCH + c].setValue(random::uniform());
    }

    void updateTargets(float sr) {
        using namespace scrupea_dsp;

        const float mOsc = macro(PITCH_PARAM, PITCH_ATT_PARAM, PITCH_CV_INPUT);
        const float mFil = macro(CUTOFF_PARAM, CUTOFF_ATT_PARAM, CUTOFF_CV_INPUT);
        const float mDel = macro(DELAY_PARAM, DELAY_ATT_PARAM, DELAY_CV_INPUT);
        const float flow = macro(FLOW_PARAM, FLOW_ATT_PARAM, FLOW_CV_INPUT);

        // Flow is not a bar mapping like the other three. In the ensemble it
        // is the Pos of five Selectors, each blending between a pair of
        // knobs, and these are those five: the FM ratio, the AM depth, the
        // resonance of every filter, the smoothing multiplier and the
        // normalizer's floor. At flow zero fm is 1, which is no modulation at
        // all, and am is 0: eight plain oscillators through resonant filters
        // into tuned combs. At flow one it is four octaves of FM either way
        // and ring modulation on top.
        tgt.fmDepth = lerp(SET_FM_LO,  SET_FM_HI,  flow);
        tgt.amDepth = lerp(SET_AM_LO,  SET_AM_HI,  flow);
        tgt.res     = lerp(SET_RES_LO, SET_RES_HI, flow);
        tgt.smt     = lerp(SET_SMT_LO, SET_SMT_HI, flow);
        tgt.nrm     = lerp(SET_NRM_LO, SET_NRM_HI, flow);

        const int topo = eng.topology;

        for (int c = 0; c < NCH; c++) {
            const float bF   = params[CH_PARAM + F_PITCH  * NCH + c].getValue();
            const float bFm  = params[CH_PARAM + F_FM     * NCH + c].getValue();
            const float bA   = params[CH_PARAM + F_AMP    * NCH + c].getValue();
            const float bAm  = params[CH_PARAM + F_AM     * NCH + c].getValue();
            const float b5   = params[CH_PARAM + F_CUTOFF * NCH + c].getValue();
            const float b6   = params[CH_PARAM + F_TYPE   * NCH + c].getValue();
            const float bDel = params[CH_PARAM + F_TIME   * NCH + c].getValue();
            const float bFb  = params[CH_PARAM + F_FBK    * NCH + c].getValue();

            // `osc F`: the shaped bar is the Pos of a Selector between two
            // pitch knobs, and an Exp turns the result into hertz.
            tgt.hz[c] = pitchToHz(
                lerp(SET_PITCH_LO, SET_PITCH_HI, shape(bF, mOsc)));

            // `osc A`: a Selector between constants 0 and 1, so the bar is
            // the oscillator's amplitude, straight through and unshaped.
            tgt.amp[c] = bA;

            if (topo == TOPO_LOOP) {
                // The bandpass mapping, which is not two independent knobs:
                // the LP corner is cc1/2 * (1 - cc2) + cc2 and the HP corner
                // is that multiplied by cc2 again, so the HP can never climb
                // above the LP and the band never closes.
                const float u = shape(b5, mFil) * 0.5f * (1.f - b6) + b6;
                const float h = u * b6;
                tgt.lpHz[c]  = pitchToHz(lerp(SET_CUT_LO, SET_CUT_HI, u));
                tgt.cutHz[c] = pitchToHz(lerp(SET_CUT_LO, SET_CUT_HI, h));
                tgt.typ[c] = 0.f;
            }
            else {
                tgt.cutHz[c] = pitchToHz(
                    lerp(SET_CUT_LO, SET_CUT_HI, shape(b5, mFil)));
                tgt.lpHz[c] = tgt.cutHz[c];
                // `lbh` times a constant 2, the Pos of a Selector over the
                // filter's LP, BP and HP outputs in that order.
                tgt.typ[c] = b6 * 2.f;
            }

            // `delay`: the shaped bar picks a pitch between the `short` and
            // `long` knobs, and `- P - Ms -` is an Exp into 1000 divided by
            // it. The delay time is one period of that pitch, which is why
            // every loop is a comb tuned to a note.
            tgt.delMs[c] = 1000.f / pitchToHz(
                lerp(SET_DEL_SHORT, SET_DEL_LONG, shape(bDel, mDel)));

            // `fbck`: a Selector between constants 0 and 1.
            tgt.fbk[c] = bFb;

            // The fm and am bars are Selector positions, not depths: they
            // choose which of the eight voices modulates this one. The
            // scaling is the ensemble's -- inside `crossvoice` a constant 8
            // multiplies the bar before it reaches the selector, so the top
            // eighth of a bar's travel all lands on the last voice.
            tgt.fmPos[c] = bFm * (float)NCH;
            tgt.amPos[c] = bAm * (float)NCH;
        }
    }

    void process(const ProcessArgs& args) override {
        if (randTrig.process(inputs[RAND_INPUT].getVoltage(), 0.1f, 2.f)
            || randBtn.process(params[RAND_PARAM].getValue() > 0.5f))
            randomizeChannels();

        if (controlPhase == 0) {
            // morph runs at the control rate, like everything else that moves
            // sixty-four parameters at once
            const bool want = inputs[MORPH_INPUT].getVoltage() > 1.f
                              || params[MORPH_PARAM].getValue() > 0.5f;
            const float msecs = std::pow(300.f,
                params[MORPH_TIME_PARAM].getValue()) * 0.1f;
            if (want && !morphing) startMorph(msecs, args.sampleRate);
            if (want) morphStep(msecs, args.sampleRate);
            morphing = want;

            eng.topology = (int)std::round(params[MODE_PARAM].getValue());
            updateTargets(args.sampleRate);
            eng.glide(tgt, CONTROL_PERIOD);
        }
        if (++controlPhase >= CONTROL_PERIOD) controlPhase = 0;

        eng.scX = params[SCOPE_X_PARAM].getValue();
        eng.scY = params[SCOPE_Y_PARAM].getValue();

        const float in = inputs[AUDIO_INPUT].getVoltage() * 0.1f;
        float l = 0.f, r = 0.f, cv = 0.f;
        eng.process(in, &l, &r, &cv);

        // The dB fader, and the one number that has to come from outside the
        // ensemble: how much of a Rack volt an internal unit is worth. Set so
        // the default bank at 0 dB sits near a 5 V peak.
        const float gain = std::pow(10.f, params[LEVEL_PARAM].getValue() * 0.05f)
                           * OUT_VOLTS;
        l *= gain;
        r *= gain;

        // Every normalizer bounds its own lever to +-1, so the mix is bounded
        // too, but the fader reaches +18 dB. Rack still needs a rail.
        outputs[LEFT_OUTPUT].setVoltage(clamp(l, -10.f, 10.f));
        outputs[RIGHT_OUTPUT].setVoltage(clamp(r, -10.f, 10.f));
        // The bank runs a good deal slower than it used to now the delay and
        // pitch ranges are the factory ones, so there is more below 25 Hz for
        // this to pick up; it was pinning against the rail at 2x.
        outputs[CV_OUTPUT].setVoltage(clamp(cv * 0.6f, -5.f, 5.f));

        envL += (std::fabs(l) - envL) * 0.002f;
        envR += (std::fabs(r) - envR) * 0.002f;
        lights[LEFT_LIGHT].setBrightness(clamp(envL * 0.4f, 0.f, 1.f));
        lights[RIGHT_LIGHT].setBrightness(clamp(envR * 0.4f, 0.f, 1.f));

        if ((args.frame % SCOPE_DECIM) == 0) {
            // The display's own taps: one point per voice, ahead of the fader
            // as the ensemble takes them, against the +-1.2 frame its own
            // record declares. Turning the output down must not empty it.
            const float sc = 1.f / scrupea_dsp::Engine::SCOPE_RANGE;
            for (int c = 0; c < NCH; c++) {
                scopeX[scopeHead][c] = clamp(eng.scopeVX[c] * sc, -1.f, 1.f);
                scopeY[scopeHead][c] = clamp(eng.scopeVY[c] * sc, -1.f, 1.f);
            }
            scopeHead = (scopeHead + 1) % SCOPE_POINTS;
            if (scopeCount < SCOPE_POINTS) scopeCount++;
        }
    }

    // Nothing outside the params needs saving: the XY pad is a pair of them.
};

// --------------------------------------------------------------- undo ---

// One drag over the edit area is one undo step, covering all eight bars of
// the function that was on screen.
struct ScrupeaEditAction : history::ModuleAction {
    int func = 0;
    float before[NCH] = {};
    float after[NCH] = {};

    ScrupeaEditAction() { name = "scrupea edit"; }

    void apply(const float* v) {
        engine::Module* m = APP->engine->getModule(moduleId);
        if (!m) return;
        for (int c = 0; c < NCH; c++)
            APP->engine->setParamValue(m, Scrupea::CH_PARAM + func * NCH + c, v[c]);
    }
    void undo() override { apply(before); }
    void redo() override { apply(after); }
};

// ---------------------------------------------------------- edit area ---

struct ScrupeaEditArea : OpaqueWidget {
    Scrupea* module = NULL;
    Vec dragPos;
    bool dragging = false;
    float dragBefore[NCH] = {};
    // rand mode: where each bar is currently heading
    float randTarget[NCH] = {};

    int func() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Scrupea::FUNC_PARAM].getValue()),
                     0, NFUNC - 1);
    }
    int topology() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Scrupea::MODE_PARAM].getValue()),
                     0, 2);
    }
    int editMode() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Scrupea::EDIT_PARAM].getValue()),
                     0, 2);
    }
    float bar(int c) const {
        if (!module) return chDefault[0][c];
        return module->params[Scrupea::CH_PARAM + func() * NCH + c].getValue();
    }
    void setBar(int c, float v) {
        if (!module) return;
        module->params[Scrupea::CH_PARAM + func() * NCH + c].setValue(clamp(v, 0.f, 1.f));
    }

    // Fold a value back into 0..1 by reflection, so a wrap drag that runs
    // past an end comes back down rather than piling up against it.
    static float mirror(float v) {
        for (int guard = 0; guard < 8; guard++) {
            if (v < 0.f) v = -v;
            else if (v > 1.f) v = 2.f - v;
            else break;
        }
        return clamp(v, 0.f, 1.f);
    }

    void beginEdit() {
        if (!module || dragging) return;
        dragging = true;
        for (int c = 0; c < NCH; c++) {
            dragBefore[c] = bar(c);
            randTarget[c] = random::uniform();
        }
    }

    void endEdit() {
        if (!module || !dragging) return;
        dragging = false;
        ScrupeaEditAction* a = new ScrupeaEditAction;
        a->moduleId = module->id;
        a->func = func();
        bool changed = false;
        for (int c = 0; c < NCH; c++) {
            a->before[c] = dragBefore[c];
            a->after[c] = bar(c);
            if (a->before[c] != a->after[c]) changed = true;
        }
        if (changed) APP->history->push(a);
        else delete a;
    }

    void editAt(Vec pos, Vec delta) {
        if (!module) return;
        const float w = box.size.x, h = box.size.y;
        switch (editMode()) {
            case 0: {   // draw
                int c = (int)std::floor(pos.x / w * NCH);
                c = clamp(c, 0, NCH - 1);
                setBar(c, 1.f - pos.y / h);
                break;
            }
            case 1: {   // wrap: shift the whole shape, mirrored at the ends
                const float d = -delta.y / h;
                for (int i = 0; i < NCH; i++) setBar(i, mirror(bar(i) + d));
                break;
            }
            default: {  // rand: every bar walks to a destination of its own
                // Not a jitter. Each bar picks somewhere to go and travels
                // there while you move the mouse, at a rate set by how far you
                // move it; when it arrives it picks somewhere else. Eight bars
                // set off at once and, being different distances away, arrive
                // at different times, so the shape keeps reorganising rather
                // than shivering in place.
                const float step = (std::fabs(delta.x) + std::fabs(delta.y))
                                   / h * 0.8f;
                for (int i = 0; i < NCH; i++) {
                    float v = bar(i);
                    float left = step;
                    // a fast drag can cross several destinations in one move
                    for (int guard = 0; guard < 4 && left > 0.f; guard++) {
                        const float d = randTarget[i] - v;
                        const float dist = std::fabs(d);
                        if (dist <= left) {
                            v = randTarget[i];
                            left -= dist;
                            randTarget[i] = random::uniform();
                        } else {
                            v += d > 0.f ? left : -left;
                            left = 0.f;
                        }
                    }
                    setBar(i, v);
                }
                break;
            }
        }
    }

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            e.consume(this);
            dragPos = e.pos;
            beginEdit();
            if (editMode() == 0) editAt(e.pos, Vec(0, 0));
            return;
        }
        // Everything else falls through, so a right-click still opens the
        // module's own context menu.
        Widget::onButton(e);
    }

    void onDragMove(const DragMoveEvent& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) return;
        const float zoom = getAbsoluteZoom();
        const Vec d = e.mouseDelta.div(zoom == 0.f ? 1.f : zoom);
        dragPos = dragPos.plus(d);
        editAt(dragPos, d);
    }

    void onDragEnd(const DragEndEvent& e) override { endEdit(); }

    void onDoubleClick(const DoubleClickEvent& e) override {
        if (!module) return;
        beginEdit();
        for (int c = 0; c < NCH; c++) setBar(c, chDefault[func()][c]);
        endEdit();
        e.consume(this);
    }

    void draw(const DrawArgs& args) override {
        const float w = box.size.x, h = box.size.y;

        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, w, h, 3.f);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgScissor(args.vg, 1, 1, w - 2, h - 2);

        // horizontal quarter rules
        for (int i = 1; i < 4; i++) {
            const float y = h * i / 4.f;
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, 2, y);
            nvgLineTo(args.vg, w - 2, y);
            nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x18));
            nvgStrokeWidth(args.vg, 0.7f);
            nvgStroke(args.vg);
        }

        const bool active = funcActive(func(), topology());
        const float pitch = w / NCH;
        for (int c = 0; c < NCH; c++) {
            const float v = bar(c);
            const float x = c * pitch + pitch * 0.16f;
            const float bw = pitch * 0.68f;
            const float bh = v * (h - 6.f);

            nvgBeginPath(args.vg);
            nvgRect(args.vg, x, 3.f, bw, h - 6.f);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x12));
            nvgFill(args.vg);

            nvgBeginPath(args.vg);
            nvgRect(args.vg, x, h - 3.f - bh, bw, bh);
            nvgFillColor(args.vg, active ? nvgRGBA(0xff, 0xd5, 0x00, 0xcc)
                                         : nvgRGBA(0xff, 0xd5, 0x00, 0x33));
            nvgFill(args.vg);

            // cap line, so a bar at zero is still visible
            nvgBeginPath(args.vg);
            nvgRect(args.vg, x, h - 4.f - bh, bw, 1.5f);
            nvgFillColor(args.vg, active ? nvgRGB(0xff, 0xf0, 0x80)
                                         : nvgRGB(0x80, 0x78, 0x40));
            nvgFill(args.vg);
        }

        nvgResetScissor(args.vg);

        // function name and edit mode, bottom left and right
        std::shared_ptr<window::Font> font =
            APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (font) {
            static const char* editName[3] = {"draw", "wrap", "rand"};
            nvgFontFaceId(args.vg, font->handle);
            nvgFontSize(args.vg, 10.f);
            nvgFillColor(args.vg, active ? nvgRGBA(0xff, 0xd5, 0x00, 0x99)
                                         : nvgRGBA(0xff, 0xd5, 0x00, 0x44));
            nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            const char* nm = (topology() == scrupea_dsp::TOPO_LOOP)
                             ? funcNameLoop[func()] : funcName[func()];
            if (active)
                nvgText(args.vg, 4.f, 3.f, nm, NULL);
            else
                nvgText(args.vg, 4.f, 3.f,
                        string::f("%s (no filter in bare)", nm).c_str(), NULL);
            nvgFillColor(args.vg, nvgRGBA(0xe5, 0xe5, 0xe5, 0x66));
            nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgText(args.vg, w - 4.f, 3.f, editName[editMode()], NULL);
        }

        OpaqueWidget::draw(args);
    }
};

// ------------------------------------------------------------- scope ---

// The Lissajous, after the one on Skrewell's panel, which plots **points and
// not lines**. That is not a detail: joining consecutive samples draws a
// closed outline, while scattering them draws where the signal *spends its
// time*, and those are different pictures. It is what gives the original its
// squares, its curves and its little curls -- a pair of square-ish waves
// piles the dots into four dense corners rather than drawing a box, and a
// slow frequency drift smears a curl instead of a smooth ribbon.
//
// Phosphor on top: the dots are drawn oldest to newest in bands, each band
// twice -- a wide dim pass for the bloom and a small bright one for the grain
// -- with alpha and colour ramping from a dim amber at the tail to near-white
// at the head.
//
// Eight figures, not one: the ensemble hands its display a poly signal and it
// plots a point per voice. A calm voice traces a curve inside the frame; a
// chaotic one fills it and squares off against the edges, because each
// lever's normalizer bounds it to 1 and the frame is +-1.2.
//
// It is also the panel's XY control: drag it to set `scX` and `scY`, the gain
// on each axis -- the pad's own tooltip in the ensemble says it "scales the
// Lissajous display". See SCOPE_X_PARAM.
struct ScrupeaScope : OpaqueWidget {
    Scrupea* module = NULL;
    Vec dragPos;
    bool dragging = false;

    static const int NSEG = 12;

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            e.consume(this);
            dragPos = e.pos;
            dragging = true;
            return;
        }
        // right-click falls through to the module's own menu
        Widget::onButton(e);
    }

    void onDragMove(const DragMoveEvent& e) override {
        if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT) return;
        const float zoom = getAbsoluteZoom();
        const Vec d = e.mouseDelta.div(zoom == 0.f ? 1.f : zoom);
        dragPos = dragPos.plus(d);
        module->params[Scrupea::SCOPE_X_PARAM].setValue(
            clamp(dragPos.x / box.size.x * 4.f, 0.f, 4.f));
        module->params[Scrupea::SCOPE_Y_PARAM].setValue(
            clamp((1.f - dragPos.y / box.size.y) * 4.f, 0.f, 4.f));
    }

    void onDragEnd(const DragEndEvent& e) override { dragging = false; }

    void onDoubleClick(const DoubleClickEvent& e) override {
        if (!module) return;
        module->params[Scrupea::SCOPE_X_PARAM].setValue(1.f);
        module->params[Scrupea::SCOPE_Y_PARAM].setValue(1.f);
        e.consume(this);
    }

    void draw(const DrawArgs& args) override {
        const float w = box.size.x, h = box.size.y;

        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, w, h, 3.f);
        nvgFillColor(args.vg, nvgRGB(0x0b, 0x0b, 0x0b));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgScissor(args.vg, 1, 1, w - 2, h - 2);

        // graticule: crosshair, a half-scale box and a unit circle, all faint
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, w / 2, 3);
        nvgLineTo(args.vg, w / 2, h - 3);
        nvgMoveTo(args.vg, 3, h / 2);
        nvgLineTo(args.vg, w - 3, h / 2);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x16));
        nvgStrokeWidth(args.vg, 0.7f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, w * 0.25f, h * 0.25f, w * 0.5f, h * 0.5f);
        nvgCircle(args.vg, w / 2, h / 2, std::min(w, h) * 0.5f - 3.f);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x0c));
        nvgStrokeWidth(args.vg, 0.6f);
        nvgStroke(args.vg);

        if (!module || module->scopeCount < 4) {
            nvgResetScissor(args.vg);
            OpaqueWidget::draw(args);
            return;
        }

        const int n = module->scopeCount;
        const int head = module->scopeHead;
        const float px = w - 6.f, py = h - 6.f;

        // One polyline per voice per age band, stroked twice -- a wide dim
        // pass for the bloom and a narrow bright one for the filament. Lines
        // and not a scatter: a voice's samples are consecutive in time, so
        // joining them is what draws the curve, and eight of them keep their
        // own shapes instead of pooling into one cloud. Bands run oldest
        // first so the bright head is laid over the dim tail, and they
        // overlap by a point so there are no gaps at the joins.
        nvgLineCap(args.vg, NVG_ROUND);
        nvgLineJoin(args.vg, NVG_ROUND);
        for (int seg = 0; seg < NSEG; seg++) {
            const int i0 = (int)((int64_t)n * seg / NSEG);
            const int i1 = (int)((int64_t)n * (seg + 1) / NSEG);
            if (i1 - i0 < 1) continue;

            // age: 0 at the tail, 1 at the head, cubed so the current figure
            // carries and the rest is a ghost of where it has been
            const float age = (float)(seg + 1) / (float)NSEG;
            const float a3 = age * age * age;
            const float wht = a3 * a3;

            for (int c = 0; c < NCH; c++) {
                nvgBeginPath(args.vg);
                for (int i = i0; i <= i1 && i < n; i++) {
                    const int idx = (head - n + i + SCOPE_POINTS * 2) % SCOPE_POINTS;
                    const float x = (module->scopeX[idx][c] * 0.5f + 0.5f)
                                    * px + 3.f;
                    const float y = (0.5f - module->scopeY[idx][c] * 0.5f)
                                    * py + 3.f;
                    if (i == i0) nvgMoveTo(args.vg, x, y);
                    else nvgLineTo(args.vg, x, y);
                }
                nvgStrokeColor(args.vg, nvgRGBAf(1.f, 0.86f, 0.15f, 0.07f * a3));
                nvgStrokeWidth(args.vg, 3.0f);
                nvgStroke(args.vg);
                nvgStrokeColor(args.vg, nvgRGBAf(1.f,
                                                 0.84f + 0.16f * wht,
                                                 0.10f + 0.80f * wht,
                                                 0.08f + 0.80f * a3));
                nvgStrokeWidth(args.vg, 0.9f);
                nvgStroke(args.vg);
            }
        }

        // the axis scales, when they are not both at unity
        const float sx = module->params[Scrupea::SCOPE_X_PARAM].getValue();
        const float sy = module->params[Scrupea::SCOPE_Y_PARAM].getValue();
        if (dragging || std::fabs(sx - 1.f) > 0.01f ||
            std::fabs(sy - 1.f) > 0.01f) {
            std::shared_ptr<window::Font> font = APP->window->loadFont(
                asset::system("res/fonts/ShareTechMono-Regular.ttf"));
            if (font) {
                nvgFontFaceId(args.vg, font->handle);
                nvgFontSize(args.vg, 9.f);
                nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x99));
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
                nvgText(args.vg, 4.f, h - 3.f,
                        string::f("x %.2f  y %.2f", sx, sy).c_str(), NULL);
            }
        }
        nvgResetScissor(args.vg);
        OpaqueWidget::draw(args);
    }
};

// ------------------------------------------------------------ widget ---

struct ScrupeaWidget : ModuleWidget {
    ScrupeaWidget(Scrupea* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/scrupea.svg")));

        ScrupeaEditArea* edit = new ScrupeaEditArea();
        edit->module = module;
        edit->box.pos = mm2px(Vec(4.f, 10.f));
        edit->box.size = mm2px(Vec(78.f, 36.f));
        addChild(edit);

        ScrupeaScope* scope = new ScrupeaScope();
        scope->module = module;
        scope->box.pos = mm2px(Vec(86.f, 10.f));
        scope->box.size = mm2px(Vec(32.f, 32.f));
        addChild(scope);

// @layout:begin scrupea 121.92 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem FUNC_PARAM RoundBlackKnob 4.8 param "" 0.0 16.00 53.00
// @elem MODE_PARAM CKSSThree 2.3 param "" 0.0 42.00 53.00
// @elem EDIT_PARAM CKSSThree 2.3 param "" 0.0 64.00 53.00
// @elem RAND_PARAM TL1105 2.6 param "" 0.0 82.00 53.00
// @elem LEVEL_PARAM RoundBlackKnob 4.8 param "" 0.0 100.00 53.00
// @elem PITCH_PARAM RoundBigBlackKnob 7.62 param "" 0.0 16.00 73.00
// @elem CUTOFF_PARAM RoundBigBlackKnob 7.62 param "" 0.0 44.00 73.00
// @elem DELAY_PARAM RoundBigBlackKnob 7.62 param "" 0.0 72.00 73.00
// @elem FLOW_PARAM RoundBigBlackKnob 7.62 param "" 0.0 100.00 73.00
// @elem PITCH_ATT_PARAM Trimpot 2.5 param "" 0.0 16.00 89.00
// @elem CUTOFF_ATT_PARAM Trimpot 2.5 param "" 0.0 44.00 89.00
// @elem DELAY_ATT_PARAM Trimpot 2.5 param "" 0.0 72.00 89.00
// @elem FLOW_ATT_PARAM Trimpot 2.5 param "" 0.0 100.00 89.00
// @elem PITCH_CV_INPUT PJ301MPort 4.01 input "" 0.0 16.00 98.00
// @elem CUTOFF_CV_INPUT PJ301MPort 4.01 input "" 0.0 44.00 98.00
// @elem DELAY_CV_INPUT PJ301MPort 4.01 input "" 0.0 72.00 98.00
// @elem FLOW_CV_INPUT PJ301MPort 4.01 input "" 0.0 100.00 98.00
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0 10.00 111.00
// @elem RAND_INPUT PJ301MPort 4.01 input "" 0.0 20.00 111.00
// @elem MORPH_INPUT PJ301MPort 4.01 input "" 0.0 54.00 111.00
// @elem MORPH_PARAM TL1105 2.6 param "" 0.0 62.50 111.00
// @elem MORPH_TIME_PARAM Trimpot 2.5 param "" 0.0 69.50 111.00
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0 80.50 111.00
// @elem LEFT_OUTPUT PJ301MPort 4.01 output "" 0.0 96.00 111.00
// @elem RIGHT_OUTPUT PJ301MPort 4.01 output "" 0.0 112.00 111.00
// @elem LEFT_LIGHT SmallLight 1.0 light "" 0.0 101.00 108.00
// @elem RIGHT_LIGHT SmallLight 1.0 light "" 0.0 117.00 108.00
// @elem BOX_CV panel_box 7.0 box "" 0.0 80.50 113.00
// @elem BOX_LEFT panel_box 7.0 box "" 0.0 96.00 113.00
// @elem BOX_RIGHT panel_box 7.0 box "" 0.0 112.00 113.00
// @elem LABEL_FUNC label 0.0 label "function" 0.0 16.00 61.50
// @elem LABEL_MODE label 0.0 label "mode" 0.0 42.00 61.50
// @elem LABEL_EDIT label 0.0 label "edit" 0.0 64.00 61.50
// @elem LABEL_RAND_PARAM label 0.0 label "rand" 0.0 82.00 60.00
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 100.00 61.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 16.00 84.50
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 44.00 84.50
// @elem LABEL_DELAY label 0.0 label "delay" 0.0 72.00 84.50
// @elem LABEL_FLOW label 0.0 label "flow" 0.0 100.00 84.50
// @elem LABEL_AUDIO label 0.0 label "in" 0.0 10.00 118.50
// @elem LABEL_RAND_INPUT label 0.0 label "rand" 0.0 20.00 118.50
// @elem LABEL_MORPH_PARAM label 0.0 label "morph" 0.0 60.96 105.20
// @elem LABEL_CV label 0.0 label "cv" 0.0 80.50 118.50
// @elem LABEL_LEFT label 0.0 label "L" 0.0 96.00 118.50
// @elem LABEL_RIGHT label 0.0 label "R" 0.0 112.00 118.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(16.00f, 53.00f)), module, Scrupea::FUNC_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(42.00f, 53.00f)), module, Scrupea::MODE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(64.00f, 53.00f)), module, Scrupea::EDIT_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(82.00f, 53.00f)), module, Scrupea::RAND_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(100.00f, 53.00f)), module, Scrupea::LEVEL_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(16.00f, 73.00f)), module, Scrupea::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(44.00f, 73.00f)), module, Scrupea::CUTOFF_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(72.00f, 73.00f)), module, Scrupea::DELAY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(100.00f, 73.00f)), module, Scrupea::FLOW_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(16.00f, 89.00f)), module, Scrupea::PITCH_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(44.00f, 89.00f)), module, Scrupea::CUTOFF_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(72.00f, 89.00f)), module, Scrupea::DELAY_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(100.00f, 89.00f)), module, Scrupea::FLOW_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.00f, 98.00f)), module, Scrupea::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.00f, 98.00f)), module, Scrupea::CUTOFF_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.00f, 98.00f)), module, Scrupea::DELAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(100.00f, 98.00f)), module, Scrupea::FLOW_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 111.00f)), module, Scrupea::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.00f, 111.00f)), module, Scrupea::RAND_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.00f, 111.00f)), module, Scrupea::MORPH_INPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(62.50f, 111.00f)), module, Scrupea::MORPH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(69.50f, 111.00f)), module, Scrupea::MORPH_TIME_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(80.50f, 111.00f)), module, Scrupea::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(96.00f, 111.00f)), module, Scrupea::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(112.00f, 111.00f)), module, Scrupea::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(101.00f, 108.00f)), module, Scrupea::LEFT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(117.00f, 108.00f)), module, Scrupea::RIGHT_LIGHT));
        // @layout:end
    }

};

Model* modelScrupea = createModel<Scrupea, ScrupeaWidget>("scrupea");
