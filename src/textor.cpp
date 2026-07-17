// textor.cpp — VCV Rack 2 module
// textor (Latin: "weaver") is a clone of the Fieldtone Weaver Modular, the
// one-knob "happy accidents" sampler: it captures two seconds of audio and
// reweaves it into a hypnotic loop. Every movement of the weave knob
// generates a completely new loop — nothing saves, nothing recalls, there
// is no way back. The original's algorithm is unpublished; this engine is
// designed from its documented behavior and from signal analysis of demo
// recordings (2026-07: onset/tempo autocorrelation, envelope, stereo and
// pitch statistics of the official walkthrough and a no-talking demo).
//
// What the analysis established, and this engine implements:
//   - every weave rolls its own tempo (dominant inter-onset periods from
//     ~90 ms to ~1.3 s between rolls) and its own loop span (~1–4.5 s)
//   - the three elements behave like randomly-divided periodic trains
//     ("a random clock divider"), loosely on the grid, not a strict
//     quantized pattern
//   - loops evolve: successive repeats stay similar but drift (timing
//     jitter, occasional fragment re-picks, probabilistic fires)
//   - fragment envelopes are mostly soft (median attack ~150 ms)
//   - pitch shifts are semitone-quantized even in random mode
//   - the stereo image is nearly mono (L/R correlation ~0.95) with slow,
//     gentle spatial movement, not per-grain panning
//   - one element per roll carries decaying delay repeats at ~0.1–0.45 s
//     ("a delayed spacey element")
//   - on the hardware, every reroll restarts the fresh weave immediately,
//     so a knob sweep sputters a rapid cascade of pattern beginnings;
//     here that lives behind the "Restart loop on every weave" context
//     menu option (off by default, weaves swap seamlessly instead)
//
// The three woven elements, each with a level knob and a gate output (the
// gates fire even with an empty buffer, so it doubles as a random rhythm
// generator):
//   warp  — long, sparse foundation strands
//   weft  — medium strands crossing it
//   fleck — shorter, brighter, flightier accents
//
// The weave knob has three zones, like the hardware:
//   full ccw       RESET — erases the sample and stops the loom
//   low zone       REC   — entering it starts a 2 s capture
//   the rest       WEAVE — any movement reweaves a brand new loop
// When a capture completes with the loom stopped, playback starts by
// itself with a fresh weave, like the hardware. The WEAVE input rerolls
// on any voltage *change* (>0.5 V), so stepped random CV rerolls on every
// new step, and a plain trigger works too.
//
// True to the original's impermanence, the sample is not saved with the
// patch — only the weave (its seed) survives a reload.
//
// Controls:
//   Knobs : WEAVE (big), WARP / WEFT / FLECK levels
//   Switch: MODE (texture/rhythm), PITCH (root/random)
//   Button: REC (with LED)
//   In    : audio IN, REC trigger, WEAVE trigger/CV, CLOCK
//   Out   : L, R, three element GATEs
//   Light : REC (red, while capturing), gate activity per element,
//           output level per channel
//   Menu  : Restart loop on every weave (hardware knob-sweep behavior)

#include "forsitan.hpp"

namespace textor_dsp {

// deterministic per-loop RNG (xorshift32)
struct Rng {
    uint32_t s = 1;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uniform() { return (next() >> 8) * (1.f / 16777216.f); }
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }
    int irange(int lo, int hi) { return lo + (int)(uniform() * (hi - lo + 1)) % (hi - lo + 1); }
};

inline float softLimit(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

} // namespace textor_dsp

struct Textor : Module {
    enum ParamId {
        WEAVE_PARAM,
        WARP_LEVEL_PARAM,
        WEFT_LEVEL_PARAM,
        FLECK_LEVEL_PARAM,
        MODE_PARAM,   // 1 = texture, 0 = rhythm
        PITCH_PARAM,  // 1 = root, 0 = random
        REC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        REC_INPUT,
        WEAVE_INPUT,
        CLOCK_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        WARP_GATE_OUTPUT,
        WEFT_GATE_OUTPUT,
        FLECK_GATE_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        REC_LIGHT,
        WARP_LIGHT,
        WEFT_LIGHT,
        FLECK_LIGHT,
        LEVEL_L_LIGHT,
        LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    static constexpr int kElements = 3;
    static constexpr int kSteps = 16;
    static constexpr int kMaxStrands = 4;   // per element
    static constexpr int kMaxVoices = 16;
    static constexpr float kBufferSeconds = 2.f;
    static constexpr float kMaxDelaySeconds = 0.5f;
    static constexpr int kControlDiv = 32;
    // weave knob zones
    static constexpr float kResetZone = 0.04f;
    static constexpr float kRecZone = 0.12f;

    // a periodic voice within the loop: fires when (step % div) == phase,
    // always replaying the same fragment — that is what makes the loop a
    // loop — but with per-fire jitter and occasional mutation
    struct Strand {
        int div = 4, phase = 0;
        float prob = 1.f;       // fire probability per occurrence
        float startFrac = 0.f;  // buffer start, 0..1 (mutates slowly)
        float lenS = 0.5f;      // fragment length in seconds
        float semi = 0.f;       // pitch shift, whole semitones
        bool reverse = false;
        float pan = 0.f;        // narrow: -0.25..0.25
        float amp = 1.f;
        float attackFrac = 0.4f;   // fraction of length spent rising
        float jitterFrac = 0.08f;  // timing jitter, fraction of a step
    };

    struct Voice {
        bool active = false;
        int elem = 0;
        int startDelay = 0;     // per-fire timing jitter, samples
        float pos = 0.f;        // buffer read position in samples
        float rate = 1.f;       // signed sample increment
        float age = 0.f;        // samples since onset
        float lenSamp = 1.f;
        float attackFrac = 0.4f;
        float panL = 0.7f, panR = 0.7f;
        float amp = 1.f;
        float kill = 0.f;       // >0: fast fadeout in progress
        int32_t order = 0;      // for oldest-voice stealing
    };

    // sample memory: active cloth + shadow capture buffer
    std::vector<float> cloth, shadow;
    int bufLen = 96000;
    bool capturing = false;
    int capturePos = 0;

    // the woven loop
    Strand strands[kElements][kMaxStrands];
    int strandCount[kElements] = {};
    uint32_t seed = 0;
    uint32_t moveCounter = 0;
    bool loomRunning = false;
    bool lastTexture = true, lastRoot = true;
    textor_dsp::Rng rt;   // free-running RNG for per-fire evolution

    // per-roll character
    float rollStepS = 0.125f;      // this roll's step period, seconds
    int delayElem = 1;             // which element carries the delay (-1: none)
    int delaySamp = 4800;
    float delayFb = 0.45f;
    float panRate[kElements] = {}; // slow spatial drift per element
    float panPhase[kElements] = {};

    // delay bus (one stereo delay, used by delayElem)
    std::vector<float> dlyL, dlyR;
    int dlyPos = 0;

    // step sequencing
    int step = 0;
    float stepPhase = 0.f;      // samples into current step
    float stepSamples = 6000.f;
    float clockInterval = 0.f;  // measured clock period, samples
    float sinceClock = 1e9f;

    // playback
    Voice voices[kMaxVoices];
    int32_t voiceOrder = 0;

    // options
    bool sweepRestart = false;  // hardware behavior: reroll restarts loop
    int evolution = 1;          // 0 frozen, 1 slow, 2 fast (demo-matched)

    // control state
    float lastWovenKnob = -1.f;
    float lastWeaveCv = 0.f;
    bool weaveCvPrimed = false;
    bool wasInReset = false, wasInRec = false;
    bool zonesPrimed = false;   // first evaluation records zones without acting
    int controlPhase = 0;
    dsp::SchmittTrigger recTrig, clockTrig;
    dsp::BooleanTrigger recButton;
    dsp::PulseGenerator gatePulse[kElements];
    float lightEnv[kElements] = {};
    float levelEnvL = 0.f, levelEnvR = 0.f;
    float sr = 0.f;   // buffers (re)allocate lazily when this diverges

    Textor() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(WEAVE_PARAM, 0.f, 1.f, 0.f, "Weave");
        configParam(WARP_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Warp level");
        configParam(WEFT_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Weft level");
        configParam(FLECK_LEVEL_PARAM, 0.f, 1.f, 0.8f, "Fleck level");
        configSwitch(MODE_PARAM, 0.f, 1.f, 1.f, "Mode", {"Rhythm", "Texture"});
        configSwitch(PITCH_PARAM, 0.f, 1.f, 1.f, "Pitch", {"Random", "Root"});
        configButton(REC_PARAM, "Record");
        configInput(AUDIO_INPUT, "Audio");
        configInput(REC_INPUT, "Record trigger");
        configInput(WEAVE_INPUT, "Weave (rerolls on any voltage change)");
        configInput(CLOCK_INPUT, "Clock (paces the loom's steps)");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configOutput(WARP_GATE_OUTPUT, "Warp gate");
        configOutput(WEFT_GATE_OUTPUT, "Weft gate");
        configOutput(FLECK_GATE_OUTPUT, "Fleck gate");
    }

    void initBuffers(float sampleRate) {
        sr = sampleRate;
        bufLen = (int)(kBufferSeconds * sr);
        cloth.assign(bufLen, 0.f);
        shadow.assign(bufLen, 0.f);
        dlyL.assign((int)(kMaxDelaySeconds * sr) + 4, 0.f);
        dlyR.assign((int)(kMaxDelaySeconds * sr) + 4, 0.f);
        dlyPos = 0;
        capturing = false;
        capturePos = 0;
        stepSamples = rollStepS * sr;
        clockInterval = 0.f;
        sinceClock = 1e9f;
        for (auto& v : voices)
            v.active = false;
        if (loomRunning)
            weaveStrands();
    }

    void onReset() override {
        loomRunning = false;
        seed = 0;
        moveCounter = 0;
        lastWovenKnob = -1.f;
        weaveCvPrimed = false;
        sweepRestart = false;
        sr = 0.f;   // force buffer re-init on the next process()
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "seed", json_integer((json_int_t)seed));
        json_object_set_new(rootJ, "moveCounter", json_integer((json_int_t)moveCounter));
        json_object_set_new(rootJ, "loomRunning", json_boolean(loomRunning));
        json_object_set_new(rootJ, "sweepRestart", json_boolean(sweepRestart));
        json_object_set_new(rootJ, "evolution", json_integer(evolution));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j;
        if ((j = json_object_get(rootJ, "seed")))
            seed = (uint32_t)json_integer_value(j);
        if ((j = json_object_get(rootJ, "moveCounter")))
            moveCounter = (uint32_t)json_integer_value(j);
        if ((j = json_object_get(rootJ, "loomRunning")))
            loomRunning = json_boolean_value(j);
        if ((j = json_object_get(rootJ, "sweepRestart")))
            sweepRestart = json_boolean_value(j);
        if ((j = json_object_get(rootJ, "evolution")))
            evolution = clamp((int)json_integer_value(j), 0, 2);
        if (loomRunning)
            weaveStrands();
    }

    // generate the loop from the current seed and switches: a per-roll
    // tempo, per-roll delay character, and each element's strands
    void weaveStrands() {
        using namespace textor_dsp;
        bool texture = params[MODE_PARAM].getValue() > 0.5f;
        bool root = params[PITCH_PARAM].getValue() > 0.5f;
        lastTexture = texture;
        lastRoot = root;

        textor_dsp::Rng w;
        w.seed(seed);
        rt.seed(seed ^ 0xABCD1234u);

        // this roll's tempo: log-uniform 45..300 ms per step, so the
        // 16-step loop spans ~0.7..4.8 s (matches measured loop lags and
        // the wide per-roll tempo spread of the demos)
        rollStepS = 0.045f * std::pow(300.f / 45.f, w.uniform());
        if (sr > 0.f)
            stepSamples = rollStepS * sr;

        // most rolls hand one element decaying delay repeats; about a
        // third come out completely dry
        {
            float u = w.uniform();
            delayElem = (u < 0.35f) ? -1
                      : (u < 0.48f) ? 0    // warp
                      : (u < 0.80f) ? 1    // weft
                                    : 2;   // fleck
            float t = clamp((1.f + (float)w.irange(0, 2)) * rollStepS, 0.10f, 0.48f);
            delaySamp = std::max(1, (int)(t * (sr > 0.f ? sr : 48000.f)));
            if (sr > 0.f)
                delaySamp = std::min(delaySamp, (int)dlyL.size() - 2);
            delayFb = w.range(0.35f, 0.6f);
        }

        // slow spatial drift, one lazy LFO per element
        for (int e = 0; e < kElements; e++) {
            panRate[e] = w.range(0.03f, 0.15f);
            panPhase[e] = w.range(0.f, 2.f * (float)M_PI);
        }

        // sympathetic intervals, weighted toward the root
        static const float kRootSemis[10] =
            {0.f, 0.f, 0.f, 12.f, -12.f, 7.f, -7.f, 5.f, -5.f, 24.f};
        // element character tables: divisions of the 16-step loop
        static const int kWarpDivs[4] = {4, 8, 8, 16};
        static const int kWeftDivs[4] = {1, 2, 3, 4};
        static const int kFleckDivs[5] = {1, 2, 2, 3, 4};

        for (int e = 0; e < kElements; e++) {
            int n;
            if (e == 0)
                n = 1 + (w.uniform() < 0.5f ? 1 : 0);
            else if (e == 1)
                n = 1 + (w.uniform() < 0.5f ? 1 : 0);
            else
                n = 1 + w.irange(0, 2);
            strandCount[e] = std::min(n, kMaxStrands);
            for (int i = 0; i < strandCount[e]; i++) {
                Strand& s = strands[e][i];
                if (e == 0)
                    s.div = kWarpDivs[w.irange(0, 3)];
                else if (e == 1)
                    s.div = kWeftDivs[w.irange(0, 3)];
                else
                    s.div = kFleckDivs[w.irange(0, 4)];
                s.phase = w.irange(0, s.div - 1);
                s.prob = (e == 0) ? w.range(0.9f, 1.f)
                       : (e == 1) ? w.range(0.75f, 1.f)
                                  : w.range(0.6f, 0.95f);
                s.startFrac = w.uniform();
                float slot = s.div * rollStepS;   // time until it fires again
                if (texture) {
                    // long smears that overlap their own repeats
                    s.lenS = clamp(slot * w.range(0.8f, 1.6f), 0.15f, 1.8f);
                    s.attackFrac = w.range(0.3f, 0.6f);
                }
                else {
                    s.lenS = clamp(slot * w.range(0.3f, 0.8f), 0.05f, 0.5f);
                    s.attackFrac = w.range(0.12f, 0.32f);
                }
                // semitone-quantized even in random mode (measured)
                if (root)
                    s.semi = kRootSemis[w.irange(0, 9)];
                else
                    s.semi = (float)w.irange(-12, 12);
                if (e == 2)
                    s.semi += 12.f;   // flecks sit an octave up
                s.reverse = w.uniform() < (texture ? 0.3f : 0.15f);
                s.pan = w.range(-0.25f, 0.25f);   // near-mono field
                s.amp = w.range(0.7f, 1.f);
                s.jitterFrac = w.range(0.04f, 0.14f);
            }
        }
    }

    void reweave() {
        moveCounter++;
        float knob = params[WEAVE_PARAM].getValue();
        seed = (uint32_t)(knob * 65535.f) * 2654435761u + moveCounter * 0x9e3779b9u;
        weaveStrands();
        if (sweepRestart && loomRunning) {
            // hardware behavior: the fresh weave starts NOW — fade the old
            // voices fast and fire step 0 on the next sample (next clock
            // edge when externally clocked), so a knob sweep sputters a
            // cascade of pattern beginnings
            for (auto& v : voices)
                if (v.active && v.kill <= 0.f)
                    v.kill = 1.f;
            step = kSteps - 1;
            stepPhase = stepSamples;
        }
        loomRunning = true;
    }

    void startCapture() {
        if (capturing)
            return;
        capturing = true;
        capturePos = 0;
    }

    void clearSample() {
        std::fill(cloth.begin(), cloth.end(), 0.f);
        std::fill(shadow.begin(), shadow.end(), 0.f);
        std::fill(dlyL.begin(), dlyL.end(), 0.f);
        std::fill(dlyR.begin(), dlyR.end(), 0.f);
        capturing = false;
        for (auto& v : voices)
            v.active = false;
        loomRunning = false;
    }

    void fireStep(float interval) {
        using namespace textor_dsp;
        // how much the loop is allowed to drift per fire
        static const float kMutateP[3] = {0.f, 0.01f, 0.10f};
        static const float kSkipScale[3] = {0.f, 0.35f, 1.f};
        static const float kJitScale[3] = {0.f, 0.5f, 1.f};
        static const float kAmpJit[3] = {0.f, 0.05f, 0.15f};
        int evo = clamp(evolution, 0, 2);
        for (int e = 0; e < kElements; e++) {
            for (int i = 0; i < strandCount[e]; i++) {
                Strand& s = strands[e][i];
                if (step % s.div != s.phase)
                    continue;
                // probabilistic fire: the loop breathes
                if (rt.uniform() < (1.f - s.prob) * kSkipScale[evo])
                    continue;
                // occasional mutation: the strand re-picks its fragment,
                // and the loop drifts somewhere new
                if (rt.uniform() < kMutateP[evo])
                    s.startFrac = rt.uniform();

                gatePulse[e].trigger(0.002f);
                lightEnv[e] = 1.f;

                // steal the oldest voice
                Voice* v = nullptr;
                for (auto& c : voices)
                    if (!c.active) { v = &c; break; }
                if (!v) {
                    v = &voices[0];
                    for (auto& c : voices)
                        if (c.order < v->order) v = &c;
                }
                v->active = true;
                v->elem = e;
                v->order = voiceOrder++;
                v->age = 0.f;
                v->kill = 0.f;
                v->startDelay = (int)(s.jitterFrac * kJitScale[evo] * interval * rt.uniform());
                v->lenSamp = std::max(s.lenS * sr, 32.f);
                v->attackFrac = s.attackFrac;
                float rate = std::pow(2.f, s.semi / 12.f);
                v->rate = s.reverse ? -rate : rate;
                v->pos = s.startFrac * (float)(bufLen - 1);
                v->amp = s.amp * rt.range(1.f - kAmpJit[evo], 1.f + kAmpJit[evo]);
                // narrow pan + this element's slow drift, frozen at fire
                float pan = clamp(s.pan + 0.12f * std::sin(panPhase[e]), -1.f, 1.f);
                float p = (pan + 1.f) * 0.25f * (float)M_PI;
                v->panL = std::cos(p);
                v->panR = std::sin(p);
            }
        }
    }

    void updateControls() {
        // record button / trigger
        bool recHit = recButton.process(params[REC_PARAM].getValue() > 0.5f);
        recHit |= recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f);
        if (recHit)
            startCapture();

        // weave input: reroll on any voltage change, so stepped random CV
        // rerolls per step and a plain trigger works too
        if (inputs[WEAVE_INPUT].isConnected()) {
            float wcv = inputs[WEAVE_INPUT].getVoltage();
            if (!weaveCvPrimed) {
                weaveCvPrimed = true;
                lastWeaveCv = wcv;
            }
            else if (std::fabs(wcv - lastWeaveCv) > 0.5f) {
                lastWeaveCv = wcv;
                reweave();
            }
        }
        else
            weaveCvPrimed = false;

        // knob zones (act on transitions only; the first evaluation just
        // records where the knob already is, so a patch loading with the
        // knob parked in a zone doesn't erase or record spuriously)
        float knob = params[WEAVE_PARAM].getValue();
        bool inReset = knob < kResetZone;
        bool inRec = !inReset && knob < kRecZone;
        if (!zonesPrimed) {
            zonesPrimed = true;
        }
        else {
            if (inReset && !wasInReset)
                clearSample();
            if (inRec && !wasInRec)
                startCapture();
        }
        wasInReset = inReset;
        wasInRec = inRec;
        if (!inReset && !inRec) {
            if (lastWovenKnob < 0.f)
                lastWovenKnob = knob;
            else if (std::fabs(knob - lastWovenKnob) > 0.008f) {
                lastWovenKnob = knob;
                reweave();
            }
        }

        // flipping a switch re-renders the same weave in the new mode
        bool texture = params[MODE_PARAM].getValue() > 0.5f;
        bool root = params[PITCH_PARAM].getValue() > 0.5f;
        if (loomRunning && (texture != lastTexture || root != lastRoot))
            weaveStrands();

        // slow spatial drift
        for (int e = 0; e < kElements; e++) {
            panPhase[e] += panRate[e] * 2.f * (float)M_PI * kControlDiv / sr;
            if (panPhase[e] > 2.f * (float)M_PI)
                panPhase[e] -= 2.f * (float)M_PI;
        }
    }

    void process(const ProcessArgs& args) override {
        using namespace textor_dsp;

        if (sr != args.sampleRate)
            initBuffers(args.sampleRate);
        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        // --- capture (records the shadow, swaps in when full) ---
        if (capturing) {
            shadow[capturePos] = inputs[AUDIO_INPUT].getVoltage() * 0.2f;
            if (++capturePos >= bufLen) {
                capturing = false;
                std::copy(shadow.begin(), shadow.end(), cloth.begin());
                // the hardware starts playing a fresh weave by itself
                // the moment a capture lands on a silent loom
                if (!loomRunning)
                    reweave();
            }
        }
        lights[REC_LIGHT].setBrightness(capturing ? 1.f : 0.f);

        // --- step clock ---
        sinceClock += 1.f;
        bool clocked = inputs[CLOCK_INPUT].isConnected();
        bool clockEdge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
        if (clockEdge) {
            if (sinceClock < 2.f * sr)
                clockInterval = sinceClock;
            sinceClock = 0.f;
        }
        float interval = (clocked && clockInterval > 0.f) ? clockInterval : stepSamples;
        if (loomRunning) {
            if (clocked) {
                if (clockEdge) {
                    step = (step + 1) % kSteps;
                    stepPhase = 0.f;
                    fireStep(interval);
                }
            }
            else {
                stepPhase += 1.f;
                if (stepPhase >= stepSamples) {
                    stepPhase -= stepSamples;
                    step = (step + 1) % kSteps;
                    fireStep(interval);
                }
            }
        }

        // --- voices, summed per element ---
        float elemL[kElements] = {}, elemR[kElements] = {};
        for (auto& v : voices) {
            if (!v.active)
                continue;
            if (v.startDelay > 0) {
                v.startDelay--;
                continue;
            }
            // wrap the read head around the cloth
            if (v.pos >= (float)bufLen) v.pos -= (float)bufLen;
            if (v.pos < 0.f) v.pos += (float)bufLen;
            int i0 = (int)v.pos;
            int i1 = i0 + 1;
            if (i1 >= bufLen) i1 = 0;
            float frac = v.pos - (float)i0;
            float smp = cloth[i0] + (cloth[i1] - cloth[i0]) * frac;

            // asymmetric raised-cosine window: soft rise, soft fall
            float t = v.age / v.lenSamp;
            float a = v.attackFrac;
            float env;
            if (t < a)
                env = 0.5f * (1.f - std::cos((float)M_PI * t / a));
            else
                env = 0.5f * (1.f + std::cos((float)M_PI * (t - a) / (1.f - a)));

            if (v.kill > 0.f) {
                // reroll restart: old weave fades out in ~8 ms
                v.kill -= 1.f / (0.008f * sr);
                if (v.kill <= 0.f) {
                    v.active = false;
                    continue;
                }
                env *= v.kill;
            }

            smp *= env * v.amp;
            elemL[v.elem] += smp * v.panL;
            elemR[v.elem] += smp * v.panR;

            v.pos += v.rate;
            v.age += 1.f;
            if (v.age >= v.lenSamp)
                v.active = false;
        }

        // --- the delayed spacey element: decaying repeats on its bus ---
        // on a dry roll (delayElem < 0) the bus keeps circulating with
        // feedback only, so a leftover tail fades out instead of waiting,
        // frozen, for the next delayed roll
        {
            int n = (int)dlyL.size();
            int readIdx = dlyPos - delaySamp;
            if (readIdx < 0) readIdx += n;
            float wetL = dlyL[readIdx];
            float wetR = dlyR[readIdx];
            float inL = 0.f, inR = 0.f;
            if (delayElem >= 0) {
                inL = elemL[delayElem];
                inR = elemR[delayElem];
            }
            dlyL[dlyPos] = inL + wetL * delayFb;
            dlyR[dlyPos] = inR + wetR * delayFb;
            if (++dlyPos >= n) dlyPos = 0;
            if (delayElem >= 0) {
                elemL[delayElem] += wetL * 0.6f;
                elemR[delayElem] += wetR * 0.6f;
            }
        }

        float levels[kElements] = {
            params[WARP_LEVEL_PARAM].getValue(),
            params[WEFT_LEVEL_PARAM].getValue(),
            params[FLECK_LEVEL_PARAM].getValue()};
        float outL = 0.f, outR = 0.f;
        for (int e = 0; e < kElements; e++) {
            outL += elemL[e] * levels[e];
            outR += elemR[e] * levels[e];
        }

        float vL = 5.f * softLimit(outL * 1.4f);
        float vR = 5.f * softLimit(outR * 1.4f);
        outputs[LEFT_OUTPUT].setVoltage(vL);
        outputs[RIGHT_OUTPUT].setVoltage(vR);
        levelEnvL += (std::fabs(vL * 0.2f) - levelEnvL) * 0.002f;
        levelEnvR += (std::fabs(vR * 0.2f) - levelEnvR) * 0.002f;
        lights[LEVEL_L_LIGHT].setBrightness(clamp(levelEnvL, 0.f, 1.f));
        lights[LEVEL_R_LIGHT].setBrightness(clamp(levelEnvR, 0.f, 1.f));

        for (int e = 0; e < kElements; e++) {
            outputs[WARP_GATE_OUTPUT + e].setVoltage(
                gatePulse[e].process(args.sampleTime) ? 10.f : 0.f);
            lightEnv[e] += (0.f - lightEnv[e]) * 8.f * args.sampleTime;
            lights[WARP_LIGHT + e].setBrightness(lightEnv[e]);
        }
    }
};

struct TextorWidget : ModuleWidget {
    TextorWidget(Textor* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/textor.svg")));

// @layout:begin textor 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem WEAVE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem WARP_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem WEFT_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FLECK_LEVEL_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem MODE_PARAM CKSS 2.0 param "" 0.0
// @elem PITCH_PARAM CKSS 2.0 param "" 0.0
// @elem REC_PARAM TL1105 2.6 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REC_INPUT PJ301MPort 4.18 input "" 0.0
// @elem WEAVE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem WARP_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem WEFT_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem FLECK_GATE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem REC_LIGHT SmallLight 1.5 light "" 0.0
// @elem WARP_LIGHT SmallLight 1.5 light "" 0.0
// @elem WEFT_LIGHT SmallLight 1.5 light "" 0.0
// @elem FLECK_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_WEAVE label 0.0 label "weave" 0.0 25.40 35.50
// @elem LABEL_REC label 0.0 label "rec" 0.0 10.40 29.00
// @elem LABEL_MODE label 0.0 label "mode" 0.0 40.40 25.20
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 40.40 41.20
// @elem LABEL_WARP label 0.0 label "warp" 0.0 9.40 58.50
// @elem LABEL_WEFT label 0.0 label "weft" 0.0 25.40 58.50
// @elem LABEL_FLECK label 0.0 label "fleck" 0.0 41.40 58.50
// @elem LABEL_G1 label 0.0 label "g1" 0.0 9.40 75.50
// @elem LABEL_G2 label 0.0 label "g2" 0.0 25.40 75.50
// @elem LABEL_G3 label 0.0 label "g3" 0.0 41.40 75.50
// @elem BOX_G1 panel_box 7.0 box "" 0.0 9.40 70.00
// @elem BOX_G2 panel_box 7.0 box "" 0.0 25.40 70.00
// @elem BOX_G3 panel_box 7.0 box "" 0.0 41.40 70.00
// @elem LABEL_IN label 0.0 label "in" 0.0 5.50 96.50
// @elem LABEL_RECIN label 0.0 label "rec" 0.0 18.77 96.50
// @elem LABEL_WEAVEIN label 0.0 label "weave" 0.0 32.03 96.50
// @elem LABEL_CLK label 0.0 label "clk" 0.0 45.30 96.50
// @elem LABEL_L label 0.0 label "L" 0.0 25.10 114.00
// @elem LABEL_R label 0.0 label "R" 0.0 40.90 114.00
// @elem BOX_L panel_box 7.0 box "" 0.0 25.10 108.50
// @elem BOX_R panel_box 7.0 box "" 0.0 40.90 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 25.40 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.40f, 24.00f)), module, Textor::WEAVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9.40f, 50.00f)), module, Textor::WARP_LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 50.00f)), module, Textor::WEFT_LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(41.40f, 50.00f)), module, Textor::FLECK_LEVEL_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(40.40f, 17.00f)), module, Textor::MODE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(40.40f, 33.00f)), module, Textor::PITCH_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(10.40f, 22.00f)), module, Textor::REC_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.50f, 89.00f)), module, Textor::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.77f, 89.00f)), module, Textor::REC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.03f, 89.00f)), module, Textor::WEAVE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(45.30f, 89.00f)), module, Textor::CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.10f, 106.50f)), module, Textor::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.90f, 106.50f)), module, Textor::RIGHT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(9.40f, 68.00f)), module, Textor::WARP_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.40f, 68.00f)), module, Textor::WEFT_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(41.40f, 68.00f)), module, Textor::FLECK_GATE_OUTPUT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(13.60f, 18.80f)), module, Textor::REC_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(14.40f, 65.00f)), module, Textor::WARP_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(30.40f, 65.00f)), module, Textor::WEFT_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(46.40f, 65.00f)), module, Textor::FLECK_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(30.10f, 103.50f)), module, Textor::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(45.90f, 103.50f)), module, Textor::LEVEL_R_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Textor* module = getModule<Textor>();
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Restart loop on every weave",
            "", &module->sweepRestart));
        menu->addChild(createIndexPtrSubmenuItem("Loop evolution",
            {"Frozen", "Slow", "Fast"}, &module->evolution));
    }
};

Model* modelTextor = createModel<Textor, TextorWidget>("textor");
