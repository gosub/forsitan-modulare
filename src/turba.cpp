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
// Eight parallel channels, each an oscillator into a feedback delay with a
// normalizer in the loop, and a resonant filter whose position is what the
// three topologies differ in. The channels are cross-coupled in a ring: each
// oscillator is frequency-modulated by its neighbour's loop signal on one
// side and amplitude-modulated by the other side's. There is no gate and no
// pitch input; like the original it simply runs.
//
// Every channel has its own value for each of eight parameters, edited as
// eight bars in the edit area, one bar per channel. The four macro knobs do
// not offset those values, they *map* them: each applies a power curve to
// all eight at once, so turning one to the left crushes the whole bank low
// and only the highest bars survive, and to the right lifts them all.
//
// Controls:
//   Edit  : eight bars, one per channel, for the selected function. Draw
//           sets bars directly, wrap shifts all of them and mirrors at the
//           ends, rand jogs all of them at once.
//   Knobs : function, level, and the four macros pitch, cutoff, delay, flow
//           (each with an attenuverter and a CV input)
//   Switch: mode (loop / pre / bare), edit (draw / wrap / rand)
//   In    : audio (injected into all eight loops), rand trigger
//   Out   : L, R, cv (the bank's own slow wander, +/-5 V)
//   Light : L and R output level

#include "forsitan.hpp"
#include "turba_dsp.hpp"

using turba_dsp::NCH;

static const int NFUNC = 8;

static const char* funcName[NFUNC] = {
    "pitch", "cutoff", "type", "time", "fbk", "fm", "am", "level"
};

// A starting bank. The three rows that matter for whether this thing sits
// still are fbk, time and pitch, and they are set where they are on purpose:
// long delays (30-307 ms) with every loop just under or just over unity, and
// the eight pitches inside a fifth of each other rather than spread over
// three octaves. That is the regime where the bank wanders on its own -- the
// loops take a tenth of a second per pass, so state survives long enough to
// evolve, and channels close in pitch beat slowly against each other through
// the cross-modulation. An earlier default with half the feedback and 2-40 ms
// delays measured 0.04 octaves of spectral wander over a minute; this one
// measures 0.19-0.20 in every topology. See "Making it wander" in the manual.
static const float chDefault[NFUNC][NCH] = {
    {0.48f, 0.38f, 0.56f, 0.42f, 0.52f, 0.34f, 0.60f, 0.44f},   // pitch
    {0.66f, 0.52f, 0.74f, 0.58f, 0.70f, 0.48f, 0.78f, 0.62f},   // cutoff
    {0.10f, 0.45f, 0.00f, 0.60f, 0.20f, 0.85f, 0.05f, 0.35f},   // type
    {0.74f, 0.87f, 0.70f, 0.94f, 0.81f, 1.00f, 0.77f, 0.90f},   // time
    {0.95f, 0.91f, 0.99f, 0.88f, 0.97f, 0.92f, 1.00f, 0.89f},   // fbk
    {0.30f, 0.22f, 0.36f, 0.18f, 0.28f, 0.34f, 0.24f, 0.32f},   // fm
    {0.26f, 0.34f, 0.20f, 0.38f, 0.30f, 0.24f, 0.36f, 0.28f},   // am
    {0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f, 0.72f},   // level
};

enum FuncId { F_PITCH, F_CUTOFF, F_TYPE, F_TIME, F_FBK, F_FM, F_AM, F_LEVEL };

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

// Lissajous history, written by the audio thread and read by the widget.
static const int SCOPE_POINTS = 512;
static const int SCOPE_DECIM = 12;
static const float scopeScales[4] = {1.f, 2.f, 4.f, 8.f};
static const float bifDepths[4] = {0.f, 1.25f, 2.5f, 3.5f};
static const int crushLevels[4] = {0, 12, 10, 8};

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
    bool ringCoupling = true;
    // Depth of the two-state switch, in octaves of filter cutoff. Index into
    // bifDepths; see turba_dsp.hpp for what it does and why it is the cutoff
    // that gets switched.
    int bifIndex = 2;
    // Both levers of each channel. On by default because it is the actual
    // structure of the ensemble -- every tone generator in Skrewell holds two
    // LEVER macros with a crossvoice between them -- and off it halves the
    // CPU and gives a thinner, more separated version of the same bank.
    bool oscPairs = true;
    // colB's other observation about why Skrewell sounds like it does: it is
    // digital, "with aliasing and quantization". raw drops the band-limiting
    // from the pulses; crush quantizes each loop signal.
    bool oscRaw = false;
    int crushIndex = 0;
    bool randomizeAllFuncs = true;
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
                     {"Pitch", "Cutoff", "Resonance", "Delay time",
                      "Feedback", "FM amount", "AM amount", "Level"});
        configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Topology",
                     {"Filter in the loop", "Filter before the delay",
                      "No filter, parabolic oscillator"});
        configSwitch(EDIT_PARAM, 0.f, 2.f, 0.f, "Edit mode",
                     {"Draw", "Wrap", "Rand"});
        getParamQuantity(FUNC_PARAM)->randomizeEnabled = false;
        getParamQuantity(EDIT_PARAM)->randomizeEnabled = false;

        configParam(PITCH_PARAM, -1.f, 1.f, 0.f, "Pitch mapping");
        configParam(CUTOFF_PARAM, -1.f, 1.f, 0.f, "Cutoff mapping");
        configParam(DELAY_PARAM, -1.f, 1.f, 0.f, "Delay time mapping");
        configParam(FLOW_PARAM, -1.f, 1.f, 0.f, "Flow");
        configParam(PITCH_ATT_PARAM, -1.f, 1.f, 0.f, "Pitch CV amount");
        configParam(CUTOFF_ATT_PARAM, -1.f, 1.f, 0.f, "Cutoff CV amount");
        configParam(DELAY_ATT_PARAM, -1.f, 1.f, 0.f, "Delay CV amount");
        configParam(FLOW_ATT_PARAM, -1.f, 1.f, 0.f, "Flow CV amount");
        configParam(LEVEL_PARAM, 0.f, 1.f, 0.5f, "Output level", "%", 0.f, 100.f);
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
        ringCoupling = true;
        randomizeAllFuncs = true;
        scopeScale = 0;
        bifIndex = 2;
        oscPairs = true;
        oscRaw = false;
        crushIndex = 0;
        scopeCount = 0;
    }

    // The whole point of the macro knobs: a power curve over the eight bars
    // rather than an offset. gamma > 1 pushes everything towards zero and
    // only the tallest bars stay up; gamma < 1 lifts the whole bank.
    static float mapValue(float v, float gamma) {
        if (v <= 0.f) return 0.f;
        if (v >= 1.f) return 1.f;
        return std::pow(v, gamma);
    }

    float macro(int knob, int att, int cv) {
        float v = params[knob].getValue();
        if (inputs[cv].isConnected())
            v += params[att].getValue() * inputs[cv].getVoltage() * 0.2f;
        return clamp(v, -1.f, 1.f);
    }

    void randomizeChannels() {
        const int lo = randomizeAllFuncs ? 0 : (int)params[FUNC_PARAM].getValue();
        const int hi = randomizeAllFuncs ? NFUNC : lo + 1;
        for (int f = lo; f < hi; f++)
            for (int c = 0; c < NCH; c++)
                params[CH_PARAM + f * NCH + c].setValue(random::uniform());
    }

    void updateTargets(float sr) {
        const float gP = std::pow(5.f, -macro(PITCH_PARAM, PITCH_ATT_PARAM, PITCH_CV_INPUT));
        const float gC = std::pow(5.f, -macro(CUTOFF_PARAM, CUTOFF_ATT_PARAM, CUTOFF_CV_INPUT));
        const float gD = std::pow(5.f, -macro(DELAY_PARAM, DELAY_ATT_PARAM, DELAY_CV_INPUT));
        const float flow = macro(FLOW_PARAM, FLOW_ATT_PARAM, FLOW_CV_INPUT);
        const float gF = std::pow(5.f, -flow);

        // Flow also sets how quickly the engine chases its own controls:
        // "less modulation and more inertia" at one end, twitchy at the other.
        eng.setInertia(std::pow(10.f, -1.3f - 1.1f * flow));   // 1 s .. 2.5 ms

        const float nyq = 0.45f * sr;
        for (int c = 0; c < NCH; c++) {
            const float p = params[CH_PARAM + F_PITCH  * NCH + c].getValue();
            const float k = params[CH_PARAM + F_CUTOFF * NCH + c].getValue();
            const float y = params[CH_PARAM + F_TYPE   * NCH + c].getValue();
            const float t = params[CH_PARAM + F_TIME   * NCH + c].getValue();
            const float b = params[CH_PARAM + F_FBK    * NCH + c].getValue();
            const float m = params[CH_PARAM + F_FM     * NCH + c].getValue();
            const float a = params[CH_PARAM + F_AM     * NCH + c].getValue();
            const float l = params[CH_PARAM + F_LEVEL  * NCH + c].getValue();

            tgt.oct[c] = mapValue(p, gP) * 11.f;                  // 8 Hz .. 16 kHz

            float fc = 20.f * std::exp2(mapValue(k, gC) * 10.f);
            if (fc > nyq) fc = nyq;
            tgt.g[c] = std::tan((float)M_PI * fc / sr);

            // Resonance is not a bar. In the ensemble `res` is an input the
            // tone generator feeds its levers, and no bar carries it -- the
            // eight bars are F, A, cut, lbh, DEL, FB, fm, am. Here it comes
            // from flow, and mapped the *other* way: flow to the right takes
            // the resonance down. A high-Q loop filter rings on one narrow
            // band and stays orderly; open it out and the loop gets broadband
            // gain, the saturator starts folding it, and the bank tips over
            // into chaos. That inversion is what stumped the people porting
            // it, and it is measurable here: 0/s below flow -0.5, 670/s above.
            const float qf = mapValue(0.55f, 1.f / gF);
            const float Q = 0.6f + qf * qf * 18.f;
            tgt.k[c] = 1.f / Q;
            tgt.typ[c] = y;

            const float ms = 0.15f * std::exp2(mapValue(t, gD) * 11.f);
            tgt.dly[c] = ms * 0.001f * sr;

            tgt.fbk[c] = b * 1.02f;
            tgt.fm[c]  = mapValue(m, gF) * 4.f;
            tgt.am[c]  = mapValue(a, gF);
            tgt.lvl[c] = l * l;
        }
    }

    void process(const ProcessArgs& args) override {
        if (randTrig.process(inputs[RAND_INPUT].getVoltage(), 0.1f, 2.f)
            || randBtn.process(params[RAND_PARAM].getValue() > 0.5f))
            randomizeChannels();

        if (controlPhase == 0) {
            eng.topology = (int)std::round(params[MODE_PARAM].getValue());
            eng.ringCoupling = ringCoupling;
            eng.bifurcate = bifDepths[bifIndex];
            eng.pairs = oscPairs;
            eng.bandLimit = !oscRaw;
            eng.crushBits = crushLevels[crushIndex];
            updateTargets(args.sampleRate);
            eng.glide(tgt, CONTROL_PERIOD);
        }
        if (++controlPhase >= CONTROL_PERIOD) controlPhase = 0;

        const float in = inputs[AUDIO_INPUT].getVoltage() * 0.1f;
        float l = 0.f, r = 0.f, cv = 0.f;
        eng.process(in, &l, &r, &cv);

        // 14 rather than 10 since the loop was rewired: the output is now
        // the normalizer's take on the *delayed* signal, which sits about
        // 5 dB below the pre-delay sum the old order put on the jack.
        const float gain = params[LEVEL_PARAM].getValue() * 14.f;
        l *= gain;
        r *= gain;

        // The engine's own saturator bounds the mix to +-1, but the level
        // knob scales past that, so the jack needs a rail of its own.
        outputs[LEFT_OUTPUT].setVoltage(clamp(l, -10.f, 10.f));
        outputs[RIGHT_OUTPUT].setVoltage(clamp(r, -10.f, 10.f));
        outputs[CV_OUTPUT].setVoltage(clamp(cv * 10.f, -5.f, 5.f));

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
        json_object_set_new(root, "ringCoupling", json_boolean(ringCoupling));
        json_object_set_new(root, "randomizeAllFuncs", json_boolean(randomizeAllFuncs));
        json_object_set_new(root, "scopeScale", json_integer(scopeScale));
        json_object_set_new(root, "bifIndex", json_integer(bifIndex));
        json_object_set_new(root, "oscPairs", json_boolean(oscPairs));
        json_object_set_new(root, "oscRaw", json_boolean(oscRaw));
        json_object_set_new(root, "crushIndex", json_integer(crushIndex));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j = json_object_get(root, "ringCoupling");
        if (j) ringCoupling = json_boolean_value(j);
        j = json_object_get(root, "randomizeAllFuncs");
        if (j) randomizeAllFuncs = json_boolean_value(j);
        j = json_object_get(root, "scopeScale");
        if (j) scopeScale = clamp((int)json_integer_value(j), 0, 3);
        j = json_object_get(root, "bifIndex");
        if (j) bifIndex = clamp((int)json_integer_value(j), 0, 3);
        j = json_object_get(root, "oscPairs");
        if (j) oscPairs = json_boolean_value(j);
        j = json_object_get(root, "oscRaw");
        if (j) oscRaw = json_boolean_value(j);
        j = json_object_get(root, "crushIndex");
        if (j) crushIndex = clamp((int)json_integer_value(j), 0, 3);
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
            if (active)
                nvgText(args.vg, 4.f, 3.f, funcName[func()], NULL);
            else
                nvgText(args.vg, 4.f, 3.f,
                        string::f("%s (no filter in bare)",
                                  funcName[func()]).c_str(), NULL);
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
        menu->addChild(createBoolPtrMenuItem("Crossvoice (all channels)", "", &module->ringCoupling));
        menu->addChild(createBoolPtrMenuItem("Randomize all functions", "",
                                             &module->randomizeAllFuncs));
        menu->addChild(createBoolPtrMenuItem("Lever pairs (both loops per channel)", "",
                                             &module->oscPairs));
        menu->addChild(createBoolPtrMenuItem("Raw oscillators (aliasing)", "",
                                             &module->oscRaw));
        menu->addChild(createIndexSubmenuItem("Bit crush",
            {"off", "12 bit", "10 bit", "8 bit"},
            [=]() { return module->crushIndex; },
            [=](int idx) { module->crushIndex = idx; }));
        menu->addChild(createIndexSubmenuItem("Two-state switching",
            {"off", "light", "normal", "wild"},
            [=]() { return module->bifIndex; },
            [=](int idx) { module->bifIndex = idx; }));
        menu->addChild(createIndexSubmenuItem("Display scale",
            {"1x (+/-5 V)", "2x", "4x", "8x"},
            [=]() { return module->scopeScale; },
            [=](int idx) { module->scopeScale = idx; }));
        menu->addChild(createMenuItem("Randomize channels", "", [=]() {
            module->randomizeChannels();
        }));
        menu->addChild(createMenuItem("Reset channels to default", "", [=]() {
            for (int f = 0; f < NFUNC; f++)
                for (int c = 0; c < NCH; c++)
                    module->params[Turba::CH_PARAM + f * NCH + c]
                        .setValue(chDefault[f][c]);
        }));
    }
};

Model* modelTurba = createModel<Turba, TurbaWidget>("turba");
