// turba.cpp — VCV Rack 2 module
// turba (Latin: "uproar, tumult"; a disorderly crowd) takes after Skrewell,
// John Nowak's chaotic sound generator in the REAKTOR factory library, whose
// user interface is eight vertical bars and four knobs and whose output is
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
// of Skrewell's own ensemble file; see the header of turba_dsp.hpp for what
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
#include "turba_dsp.hpp"

using turba_dsp::NCH;

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
    if (topology != turba_dsp::TOPO_BARE) return true;
    return func != F_CUTOFF && func != F_TYPE;
}

// How many samples between control-rate updates of the mapped targets.
static const int CONTROL_PERIOD = 32;

// Volts per internal unit at 0 dB on the fader. REAKTOR's audio is +-1 into
// a soundcard; a Rack cable is +-5, and eight voices sum into each side.
static const float OUT_VOLTS = 0.9f;

// Lissajous history, written by the audio thread and read by the widget.
static const int SCOPE_POINTS = 512;
static const int SCOPE_DECIM = 12;
static const float scopeScales[4] = {1.f, 2.f, 4.f, 8.f};

struct Turba : Module {
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
        PARAMS_LEN
    };
    enum InputId {
        PITCH_CV_INPUT,
        CUTOFF_CV_INPUT,
        DELAY_CV_INPUT,
        FLOW_CV_INPUT,
        AUDIO_INPUT,
        RAND_INPUT,
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

    turba_dsp::Engine eng;
    turba_dsp::Targets tgt;
    int controlPhase = 0;
    dsp::SchmittTrigger randTrig;
    dsp::BooleanTrigger randBtn;
    float envL = 0.f, envR = 0.f;

    // scope ring buffer
    float scopeX[SCOPE_POINTS] = {};
    float scopeY[SCOPE_POINTS] = {};
    int scopeHead = 0, scopeCount = 0;

    // context menu options
    // Skrewell's "Display Control", which scales the Lissajous. Index into
    // scopeScales below; 1x means +/-5 V fills the box.
    int scopeScale = 0;

    Turba() {
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
        // The ensemble's output stage is a dB fader with exactly this travel.
        configParam(LEVEL_PARAM, turba_dsp::K_OUT_MIN_DB, turba_dsp::K_OUT_MAX_DB,
                    0.f, "Output level", " dB");
        configButton(RAND_PARAM, "Randomize channels");

        configInput(PITCH_CV_INPUT, "Pitch mapping CV");
        configInput(CUTOFF_CV_INPUT, "Cutoff mapping CV");
        configInput(DELAY_CV_INPUT, "Delay mapping CV");
        configInput(FLOW_CV_INPUT, "Flow CV");
        configInput(AUDIO_INPUT, "Audio (into all eight loops)");
        configInput(RAND_INPUT, "Randomize trigger");
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
        scopeScale = 0;
        scopeCount = 0;
    }

    float macro(int knob, int att, int cv) {
        float v = params[knob].getValue();
        if (inputs[cv].isConnected())
            v += params[att].getValue() * inputs[cv].getVoltage() * 0.1f;
        return clamp(v, 0.f, 1.f);
    }

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    void randomizeChannels() {
        // All 64, as the original's does: "sends random values to all 64
        // columns", in carloskleiber's words.
        for (int f = 0; f < NFUNC; f++)
            for (int c = 0; c < NCH; c++)
                params[CH_PARAM + f * NCH + c].setValue(random::uniform());
    }

    void updateTargets(float sr) {
        using namespace turba_dsp;

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
            eng.topology = (int)std::round(params[MODE_PARAM].getValue());
            updateTargets(args.sampleRate);
            eng.glide(tgt, CONTROL_PERIOD);
        }
        if (++controlPhase >= CONTROL_PERIOD) controlPhase = 0;

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
        outputs[CV_OUTPUT].setVoltage(clamp(cv * 2.f, -5.f, 5.f));

        envL += (std::fabs(l) - envL) * 0.002f;
        envR += (std::fabs(r) - envR) * 0.002f;
        lights[LEFT_LIGHT].setBrightness(clamp(envL * 0.4f, 0.f, 1.f));
        lights[RIGHT_LIGHT].setBrightness(clamp(envR * 0.4f, 0.f, 1.f));

        if ((args.frame % SCOPE_DECIM) == 0) {
            const float sc = 0.2f * scopeScales[scopeScale];
            scopeX[scopeHead] = clamp(l * sc, -1.f, 1.f);
            scopeY[scopeHead] = clamp(r * sc, -1.f, 1.f);
            scopeHead = (scopeHead + 1) % SCOPE_POINTS;
            if (scopeCount < SCOPE_POINTS) scopeCount++;
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "scopeScale", json_integer(scopeScale));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = NULL;
        j = json_object_get(root, "scopeScale");
        if (j) scopeScale = clamp((int)json_integer_value(j), 0, 3);
    }
};

// --------------------------------------------------------------- undo ---

// One drag over the edit area is one undo step, covering all eight bars of
// the function that was on screen.
struct TurbaEditAction : history::ModuleAction {
    int func = 0;
    float before[NCH] = {};
    float after[NCH] = {};

    TurbaEditAction() { name = "turba edit"; }

    void apply(const float* v) {
        engine::Module* m = APP->engine->getModule(moduleId);
        if (!m) return;
        for (int c = 0; c < NCH; c++)
            APP->engine->setParamValue(m, Turba::CH_PARAM + func * NCH + c, v[c]);
    }
    void undo() override { apply(before); }
    void redo() override { apply(after); }
};

// ---------------------------------------------------------- edit area ---

struct TurbaEditArea : OpaqueWidget {
    Turba* module = NULL;
    Vec dragPos;
    bool dragging = false;
    float dragBefore[NCH] = {};

    int func() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Turba::FUNC_PARAM].getValue()),
                     0, NFUNC - 1);
    }
    int topology() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Turba::MODE_PARAM].getValue()),
                     0, 2);
    }
    int editMode() const {
        if (!module) return 0;
        return clamp((int)std::round(module->params[Turba::EDIT_PARAM].getValue()),
                     0, 2);
    }
    float bar(int c) const {
        if (!module) return chDefault[0][c];
        return module->params[Turba::CH_PARAM + func() * NCH + c].getValue();
    }
    void setBar(int c, float v) {
        if (!module) return;
        module->params[Turba::CH_PARAM + func() * NCH + c].setValue(clamp(v, 0.f, 1.f));
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
        for (int c = 0; c < NCH; c++) dragBefore[c] = bar(c);
    }

    void endEdit() {
        if (!module || !dragging) return;
        dragging = false;
        TurbaEditAction* a = new TurbaEditAction;
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
            default: {  // rand: jog every bar, by how far the mouse moved
                const float amt = (std::fabs(delta.x) + std::fabs(delta.y)) / h;
                for (int i = 0; i < NCH; i++)
                    setBar(i, bar(i) + (random::uniform() - 0.5f) * amt * 2.f);
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
            const char* nm = (topology() == turba_dsp::TOPO_LOOP)
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

struct TurbaScope : TransparentWidget {
    Turba* module = NULL;

    void draw(const DrawArgs& args) override {
        const float w = box.size.x, h = box.size.y;

        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, w, h, 3.f);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        // crosshair
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, w / 2, 3);
        nvgLineTo(args.vg, w / 2, h - 3);
        nvgMoveTo(args.vg, 3, h / 2);
        nvgLineTo(args.vg, w - 3, h / 2);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x14));
        nvgStrokeWidth(args.vg, 0.7f);
        nvgStroke(args.vg);

        if (!module || module->scopeCount < 2) return;

        nvgScissor(args.vg, 1, 1, w - 2, h - 2);
        const int n = module->scopeCount;
        const int head = module->scopeHead;
        nvgBeginPath(args.vg);
        for (int i = 0; i < n; i++) {
            const int idx = (head - n + i + SCOPE_POINTS * 2) % SCOPE_POINTS;
            const float x = (module->scopeX[idx] * 0.5f + 0.5f) * (w - 6.f) + 3.f;
            const float y = (0.5f - module->scopeY[idx] * 0.5f) * (h - 6.f) + 3.f;
            if (i == 0) nvgMoveTo(args.vg, x, y);
            else nvgLineTo(args.vg, x, y);
        }
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0xb0));
        nvgStrokeWidth(args.vg, 1.f);
        nvgLineCap(args.vg, NVG_ROUND);
        nvgLineJoin(args.vg, NVG_ROUND);
        nvgStroke(args.vg);
        nvgResetScissor(args.vg);
    }
};

// ------------------------------------------------------------ widget ---

struct TurbaWidget : ModuleWidget {
    TurbaWidget(Turba* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/turba.svg")));

        TurbaEditArea* edit = new TurbaEditArea();
        edit->module = module;
        edit->box.pos = mm2px(Vec(4.f, 10.f));
        edit->box.size = mm2px(Vec(78.f, 36.f));
        addChild(edit);

        TurbaScope* scope = new TurbaScope();
        scope->module = module;
        scope->box.pos = mm2px(Vec(86.f, 10.f));
        scope->box.size = mm2px(Vec(32.f, 32.f));
        addChild(scope);

// @layout:begin turba 121.92 128.5
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
// @elem AUDIO_INPUT PJ301MPort 4.01 input "" 0.0 12.00 111.00
// @elem RAND_INPUT PJ301MPort 4.01 input "" 0.0 30.00 111.00
// @elem CV_OUTPUT PJ301MPort 4.01 output "" 0.0 76.00 111.00
// @elem LEFT_OUTPUT PJ301MPort 4.01 output "" 0.0 94.00 111.00
// @elem RIGHT_OUTPUT PJ301MPort 4.01 output "" 0.0 110.00 111.00
// @elem LEFT_LIGHT SmallLight 1.0 light "" 0.0 99.00 108.00
// @elem RIGHT_LIGHT SmallLight 1.0 light "" 0.0 115.00 108.00
// @elem BOX_CV panel_box 7.0 box "" 0.0 76.00 113.00
// @elem BOX_LEFT panel_box 7.0 box "" 0.0 94.00 113.00
// @elem BOX_RIGHT panel_box 7.0 box "" 0.0 110.00 113.00
// @elem LABEL_FUNC label 0.0 label "function" 0.0 16.00 61.50
// @elem LABEL_MODE label 0.0 label "mode" 0.0 42.00 61.50
// @elem LABEL_EDIT label 0.0 label "edit" 0.0 64.00 61.50
// @elem LABEL_RAND_PARAM label 0.0 label "rand" 0.0 82.00 60.00
// @elem LABEL_LEVEL label 0.0 label "level" 0.0 100.00 61.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 16.00 84.50
// @elem LABEL_CUTOFF label 0.0 label "cutoff" 0.0 44.00 84.50
// @elem LABEL_DELAY label 0.0 label "delay" 0.0 72.00 84.50
// @elem LABEL_FLOW label 0.0 label "flow" 0.0 100.00 84.50
// @elem LABEL_AUDIO label 0.0 label "in" 0.0 12.00 118.50
// @elem LABEL_RAND_INPUT label 0.0 label "rand" 0.0 30.00 118.50
// @elem LABEL_CV label 0.0 label "cv" 0.0 76.00 118.50
// @elem LABEL_LEFT label 0.0 label "L" 0.0 94.00 118.50
// @elem LABEL_RIGHT label 0.0 label "R" 0.0 110.00 118.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 60.96 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(114.30f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(16.00f, 53.00f)), module, Turba::FUNC_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(42.00f, 53.00f)), module, Turba::MODE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(64.00f, 53.00f)), module, Turba::EDIT_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(82.00f, 53.00f)), module, Turba::RAND_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(100.00f, 53.00f)), module, Turba::LEVEL_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(16.00f, 73.00f)), module, Turba::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(44.00f, 73.00f)), module, Turba::CUTOFF_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(72.00f, 73.00f)), module, Turba::DELAY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(100.00f, 73.00f)), module, Turba::FLOW_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(16.00f, 89.00f)), module, Turba::PITCH_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(44.00f, 89.00f)), module, Turba::CUTOFF_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(72.00f, 89.00f)), module, Turba::DELAY_ATT_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(100.00f, 89.00f)), module, Turba::FLOW_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.00f, 98.00f)), module, Turba::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.00f, 98.00f)), module, Turba::CUTOFF_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.00f, 98.00f)), module, Turba::DELAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(100.00f, 98.00f)), module, Turba::FLOW_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.00f, 111.00f)), module, Turba::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.00f, 111.00f)), module, Turba::RAND_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(76.00f, 111.00f)), module, Turba::CV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(94.00f, 111.00f)), module, Turba::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(110.00f, 111.00f)), module, Turba::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(99.00f, 108.00f)), module, Turba::LEFT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(115.00f, 108.00f)), module, Turba::RIGHT_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Turba* module = getModule<Turba>();

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Display scale",
            {"1x (+/-5 V)", "2x", "4x", "8x"},
            [=]() { return module->scopeScale; },
            [=](int idx) { module->scopeScale = idx; }));
    }
};

Model* modelTurba = createModel<Turba, TurbaWidget>("turba");
