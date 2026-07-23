// vestigia.cpp — VCV Rack 2 module
// vestigia (Latin: "traces, footprints") is a stereo memory effect. It
// continuously writes the incoming signal into an endless tape loop, keeps
// a parallel block-based activity map of where meaningful sound actually
// lives, and recalls those regions — during pauses, on transients, or by
// probability — degrading each memory a little more every time it returns.
//
// It is not a delay. The write head never stops (except FREEZE); what the
// present does to the past is set by the MEMORY MODE:
//   oblivion   — the present replaces the past
//   remanence  — the present rewrites the past but leaves traces (30% kept)
//   sediment   — the present accumulates over the past through saturation
//
// The recollection engine consults the activity map, never arbitrary buffer
// positions, so silence is never recalled. Three recollection modes shape
// WHEN memories return:
//   listen   — event-centered, triggered by transients, prefers recent
//   breathe  — recall follows the input envelope and its density
//   dream    — recall rises as the input falls quiet; older, reversed, longer
//
// Macro controls (each a coordinated bundle, per the design doc):
//   MEMORY    temporal horizon available to recall (50 ms .. 16 s)
//   RECALL    rate/probability of automatic recollection events
//   AGE       how hard recalled memories deteriorate (band, rate, bits, jitter)
//   SMEAR     distinct fragments -> diffuse all-pass cloud
//   FORGET    how fast memory and feedback lose persistence
//   TEMPER    behavioral instability: timing/speed jitter, spatial drift
//   DIRECTION forward .. reverse probability, chosen per recollection
//   MIX       equal-power dry/wet
//   OUTPUT    final gain (unity at 12 o'clock)
//
// FREEZE stops writing while recall keeps running. EVENT forces a recall
// from a valid region (and never fires EVENT OUT if there is none). CLEAR
// wipes buffer, activity map, heads and feedback.
//
// The audio buffer is not saved with the patch; only the random seed is, so
// probabilistic behavior is reproducible across a reload.
//
// This is a first release (design doc v0.2, MVP per its sections 37/45): two
// playback heads, eight-region descriptors, 16 s horizon, no external clock,
// no per-voice polyphony. The panel exposes CV for MEMORY/RECALL/AGE/SMEAR;
// the remaining macro CV inputs from the doc are deferred.

#include "forsitan.hpp"
#include <vector>

namespace vestigia_dsp {

// deterministic RNG (xorshift32) — musical randomness seeded from the patch
struct Rng {
    uint32_t s = 0x1234567u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float uniform() { return (next() >> 8) * (1.f / 16777216.f); }
    float bipolar() { return uniform() * 2.f - 1.f; }
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }
};

// bounded soft saturator (odd, ~unity slope near 0, asymptotes to ±1)
inline float softSat(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

// one-pole DC blocker
struct DCBlock {
    float x1 = 0.f, y1 = 0.f;
    float process(float x) {
        float y = x - x1 + 0.995f * y1;
        x1 = x; y1 = y;
        return y;
    }
    void reset() { x1 = y1 = 0.f; }
};

// Schroeder all-pass for the smear/diffusion network
struct AllPass {
    std::vector<float> buf;
    int pos = 0;
    float g = 0.5f;
    void init(int len) { buf.assign(std::max(1, len), 0.f); pos = 0; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); pos = 0; }
    float process(float x) {
        float d = buf[pos];
        float v = x + g * d;
        float y = d - g * v;
        buf[pos] = v;
        if (++pos >= (int)buf.size()) pos = 0;
        return y;
    }
};

} // namespace vestigia_dsp

struct Vestigia : Module {
    enum ParamId {
        MEMORY_PARAM, RECALL_PARAM, AGE_PARAM, SMEAR_PARAM, FORGET_PARAM,
        TEMPER_PARAM, DIRECTION_PARAM, MIX_PARAM, OUTPUT_PARAM,
        MODE_PARAM,     // 0 listen, 1 breathe, 2 dream
        MEMMODE_PARAM,  // 0 oblivion, 1 remanence, 2 sediment
        FREEZE_PARAM, EVENT_PARAM, CLEAR_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT, IN_R_INPUT,
        FREEZE_INPUT, EVENT_INPUT, CLEAR_INPUT,
        MEMORY_CV_INPUT, RECALL_CV_INPUT, AGE_CV_INPUT, SMEAR_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_L_OUTPUT, OUT_R_OUTPUT,
        EVENT_OUTPUT, ENV_OUTPUT, CHAOS_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        FREEZE_LIGHT, EVENT_LIGHT, LEVEL_L_LIGHT, LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    // --- constants ---
    static constexpr float kMaxSeconds = 16.f;
    static constexpr float kBlockSec = 0.02f;   // 20 ms activity blocks
    static constexpr int kMaxRegions = 8;       // active-region descriptors
    static constexpr int kNumHeads = 2;         // playback heads
    static constexpr int kCells = 120;          // display resolution

    // --- circular stereo buffer + activity map ---
    float sr = 0.f;
    int bufLen = 0, blockLen = 0, numBlocks = 0;
    std::vector<float> bufL, bufR;
    int writePos = 0;

    struct Block {
        float energy = 0.f;   // rms of stored content, ±1 domain
        bool active = false;  // hysteretic gate
        int64_t frame = -1;   // last write frame
    };
    std::vector<Block> blocks;
    double blockAccum = 0.0;
    int blockAccumN = 0;
    int curBlock = 0;
    bool blockActiveState = false;

    // --- input analysis ---
    float fastEnv = 0.f, slowEnv = 0.f, refLevel = 0.02f;
    float transient = 0.f;

    // --- recollection ---
    struct Region {
        double startPos = 0;  // sample index into the buffer
        double length = 0;    // samples
        float energy = 0.f;
        float ageSec = 0.f;
        float score = 0.f;
    };
    Region regions[kMaxRegions];
    int numRegions = 0;
    int rebuildCounter = 0;
    float recallAccum = 0.f;
    float autoCooldown = 0.f;
    double lastPlayedStart = -1e9;

    struct Head {
        bool active = false;
        double base = 0.0;    // region start sample
        double len = 0.0;     // region length samples
        double phase = 0.0;   // 0..len read offset
        int dir = 1;          // +1 forward, -1 reverse
        float speed = 1.f;
        float amp = 1.f;
        float panL = 0.7f, panR = 0.7f;
        double age = 0.0;     // output samples elapsed
        float attackSamp = 96.f, releaseSamp = 240.f;
        float degrade = 0.f;  // AGE amount captured at start
        // per-head degradation state
        float lpState = 0.f;
        float holdVal = 0.f;
        int holdCnt = 0;
        float jitter = 0.f;
    };
    Head heads[kNumHeads];

    // --- smear / diffusion network ---
    vestigia_dsp::AllPass apL[4], apR[4];

    // --- feedback ---
    float fbL = 0.f, fbR = 0.f;
    vestigia_dsp::DCBlock dcL, dcR, dcWL, dcWR;

    // --- triggers / state ---
    dsp::SchmittTrigger freezeBtnTrig, freezeGateTrig;
    dsp::SchmittTrigger eventBtnTrig, eventCvTrig;
    dsp::SchmittTrigger clearBtnTrig, clearCvTrig;
    dsp::PulseGenerator eventPulse;
    bool frozenToggle = false;

    // --- metering / display ---
    float levelEnvL = 0.f, levelEnvR = 0.f;
    float chaosSmooth = 0.f;
    float recallFlash = 0.f;
    float cellEnergy[kCells] = {};
    float displayHead = 0.f;  // 0..1 write position for the display

    uint32_t seed = 0x1234567u;
    vestigia_dsp::Rng rng;

    // --- context-menu options ---
    int freezeMode = 1;   // 0 gate, 1 toggle (button latches)
    bool monoOut = false; // sum L/R equal-power on output

    Vestigia() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(MEMORY_PARAM, 0.f, 1.f, 0.4f, "Memory horizon", " s", kMaxSeconds / 0.05f, 0.05f);
        configParam(RECALL_PARAM, 0.f, 1.f, 0.35f, "Recall rate", "%", 0.f, 100.f);
        configParam(AGE_PARAM, 0.f, 1.f, 0.25f, "Age / degradation", "%", 0.f, 100.f);
        configParam(SMEAR_PARAM, 0.f, 1.f, 0.2f, "Smear", "%", 0.f, 100.f);
        configParam(FORGET_PARAM, 0.f, 1.f, 0.4f, "Forget", "%", 0.f, 100.f);
        configParam(TEMPER_PARAM, 0.f, 1.f, 0.15f, "Temper (instability)", "%", 0.f, 100.f);
        configParam(DIRECTION_PARAM, 0.f, 1.f, 0.f, "Reverse probability", "%", 0.f, 100.f);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Dry / wet", "%", 0.f, 100.f);
        configParam(OUTPUT_PARAM, 0.f, 1.f, 0.5f, "Output level");
        configSwitch(MODE_PARAM, 0.f, 2.f, 1.f, "Recollection mode",
            {"Listen", "Breathe", "Dream"});
        configSwitch(MEMMODE_PARAM, 0.f, 2.f, 0.f, "Memory mode",
            {"Oblivion", "Remanence", "Sediment"});
        configButton(FREEZE_PARAM, "Freeze");
        configButton(EVENT_PARAM, "Event (force recall)");
        configButton(CLEAR_PARAM, "Clear memory");
        configInput(IN_L_INPUT, "Audio L");
        configInput(IN_R_INPUT, "Audio R (normalled from L)");
        configInput(FREEZE_INPUT, "Freeze gate");
        configInput(EVENT_INPUT, "Event trigger");
        configInput(CLEAR_INPUT, "Clear trigger");
        configInput(MEMORY_CV_INPUT, "Memory CV");
        configInput(RECALL_CV_INPUT, "Recall CV");
        configInput(AGE_CV_INPUT, "Age CV");
        configInput(SMEAR_CV_INPUT, "Smear CV");
        configOutput(OUT_L_OUTPUT, "Audio L");
        configOutput(OUT_R_OUTPUT, "Audio R");
        configOutput(EVENT_OUTPUT, "Event (recall began)");
        configOutput(ENV_OUTPUT, "Input envelope");
        configOutput(CHAOS_OUTPUT, "Chaos / internal state");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
        rng.seed(seed);
    }

    void allocate(float newSr) {
        sr = newSr;
        blockLen = std::max(1, (int)(kBlockSec * sr));
        // keep the buffer an exact whole number of blocks so writePos/blockLen
        // never indexes one past the activity map
        numBlocks = (int)(kMaxSeconds * sr) / blockLen + 1;
        bufLen = numBlocks * blockLen;
        bufL.assign(bufLen, 0.f);
        bufR.assign(bufLen, 0.f);
        blocks.assign(numBlocks, Block());
        writePos = 0;
        curBlock = 0;
        blockAccum = 0.0; blockAccumN = 0;
        // smear all-pass delays: mutually non-harmonic, different per side
        const float msL[4] = {13.f, 29.f, 47.f, 67.f};
        const float msR[4] = {17.f, 31.f, 53.f, 73.f};
        for (int i = 0; i < 4; i++) {
            apL[i].init((int)(msL[i] * 0.001f * sr)); apL[i].g = 0.55f;
            apR[i].init((int)(msR[i] * 0.001f * sr)); apR[i].g = 0.55f;
        }
        clearMemory();
    }

    void clearMemory() {
        std::fill(bufL.begin(), bufL.end(), 0.f);
        std::fill(bufR.begin(), bufR.end(), 0.f);
        for (auto& b : blocks) { b.energy = 0.f; b.active = false; b.frame = -1; }
        for (auto& h : heads) h.active = false;
        for (int i = 0; i < 4; i++) { apL[i].reset(); apR[i].reset(); }
        for (int i = 0; i < kCells; i++) cellEnergy[i] = 0.f;
        numRegions = 0;
        fbL = fbR = 0.f;
        dcL.reset(); dcR.reset(); dcWL.reset(); dcWR.reset();
        blockActiveState = false;
        blockAccum = 0.0; blockAccumN = 0;
    }

    void onReset() override {
        frozenToggle = false;
        freezeMode = 1;
        monoOut = false;
        seed = 0x1234567u;
        rng.seed(seed);
        sr = 0.f;  // force re-alloc next process
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seed", json_integer((json_int_t)seed));
        json_object_set_new(root, "frozen", json_boolean(frozenToggle));
        json_object_set_new(root, "freezeMode", json_integer(freezeMode));
        json_object_set_new(root, "monoOut", json_boolean(monoOut));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        if ((j = json_object_get(root, "seed"))) {
            seed = (uint32_t)json_integer_value(j);
            rng.seed(seed);
        }
        if ((j = json_object_get(root, "frozen")))
            frozenToggle = json_boolean_value(j);
        if ((j = json_object_get(root, "freezeMode")))
            freezeMode = clamp((int)json_integer_value(j), 0, 1);
        if ((j = json_object_get(root, "monoOut")))
            monoOut = json_boolean_value(j);
    }

    // read a knob plus a normalled 0..1 CV nudge (unipolar, ±5V -> ±0.5)
    float macro(int pid, int cvid) {
        float v = params[pid].getValue();
        if (inputs[cvid].isConnected())
            v += inputs[cvid].getVoltage() * 0.1f;
        return clamp(v, 0.f, 1.f);
    }

    int blockIndex(int j) {  // wrap a possibly-negative block into range
        int m = j % numBlocks;
        return m < 0 ? m + numBlocks : m;
    }

    // rebuild the active-region list from the activity map, within horizon
    void rebuildRegions(float memorySec, int mode, int64_t frame) {
        int horizonBlocks = clamp((int)(memorySec / kBlockSec), 4, numBlocks - 1);
        int gapBridge = (mode == 2) ? 6 : (mode == 1 ? 4 : 2);  // dream bridges more
        int writeBlock = curBlock;
        int oldest = writeBlock - horizonBlocks + 1;

        numRegions = 0;
        int runStart = -1, gap = 0;
        double runEsum = 0; int runN = 0;

        auto closeRun = [&](int runEnd) {
            if (runStart < 0 || numRegions >= kMaxRegions) return;
            int lenBlocks = runEnd - runStart + 1;
            if (lenBlocks < 2) return;  // min ~40 ms
            Region& r = regions[numRegions];
            r.startPos = (double)blockIndex(runStart) * blockLen;
            r.length = (double)lenBlocks * blockLen;
            r.energy = runN ? (float)(runEsum / runN) : 0.f;
            // newer blocks sit at higher j: age from the run's newest edge
            r.ageSec = (float)(horizonBlocks - 1 - runEnd) * kBlockSec;
            float eNorm = clamp(r.energy / 0.2f, 0.f, 1.f);
            r.score = eNorm;
            numRegions++;
        };

        for (int k = 0; k < horizonBlocks; k++) {
            int j = oldest + k;
            const Block& b = blocks[blockIndex(j)];
            bool inHorizon = b.frame >= 0 && (frame - b.frame) <= (int64_t)(memorySec * sr) + blockLen;
            bool on = b.active && inHorizon;
            if (on) {
                if (runStart < 0) { runStart = k; runEsum = 0; runN = 0; }
                runEsum += b.energy; runN++;
                gap = 0;
            } else if (runStart >= 0) {
                if (++gap > gapBridge) {
                    closeRun(k - gap);
                    runStart = -1;
                }
            }
        }
        if (runStart >= 0) closeRun(horizonBlocks - 1);
    }

    // pick a region by weighted selection, honoring the recollection mode
    int selectRegion(int mode, float memorySec) {
        if (numRegions == 0) return -1;
        float weights[kMaxRegions];
        float total = 0.f;
        for (int i = 0; i < numRegions; i++) {
            const Region& r = regions[i];
            float ageNorm = clamp(r.ageSec / std::max(0.05f, memorySec), 0.f, 1.f);
            float ageW;
            if (mode == 2)              // dream: prefer older memories
                ageW = 0.25f + 0.75f * ageNorm;
            else                        // listen/breathe: prefer recent
                ageW = 1.f - 0.55f * ageNorm;
            float modeW = 1.f;
            if (mode == 0)              // listen: strongly favor energetic events
                modeW = 0.2f + r.score * r.score;
            else if (mode == 1)         // breathe: favor energy near current input
                modeW = 0.3f + 0.7f * (1.f - std::fabs(r.score - clamp(refLevel / 0.2f, 0.f, 1.f)));
            float cool = 1.f;
            if (std::fabs(r.startPos - lastPlayedStart) < 2.0 * blockLen)
                cool = 0.15f;           // cooldown on the just-played region
            float w = (0.1f + r.score) * ageW * modeW * cool * (0.4f + 0.6f * rng.uniform());
            weights[i] = std::max(0.f, w);
            total += weights[i];
        }
        if (total <= 0.f) return -1;
        float pick = rng.uniform() * total;
        for (int i = 0; i < numRegions; i++) {
            pick -= weights[i];
            if (pick <= 0.f) return i;
        }
        return numRegions - 1;
    }

    Head* freeHead() {
        for (auto& h : heads) if (!h.active) return &h;
        // else replace the head nearest completion
        Head* best = &heads[0];
        float bestRemain = 1e9f;
        for (auto& h : heads) {
            float remain = (float)((h.len - h.phase) / std::max(0.01f, h.speed));
            if (remain < bestRemain) { bestRemain = remain; best = &h; }
        }
        return best;
    }

    // attempt a recollection; returns true if a head actually started
    bool tryRecall(int mode, float memorySec, float ageAmt, float temper, float dirProb) {
        int ri = selectRegion(mode, memorySec);
        if (ri < 0) return false;
        Region& r = regions[ri];
        Head* h = freeHead();
        h->active = true;
        h->base = r.startPos;
        h->len = r.length;
        h->phase = 0.0;
        h->dir = (rng.uniform() < dirProb) ? -1 : 1;
        // speed: mostly 1x, perturbed by temper; age nudges it down a touch
        float sp = 1.f + temper * rng.bipolar() * 0.5f - ageAmt * 0.1f * rng.uniform();
        h->speed = clamp(sp, 0.25f, 2.f);
        h->amp = 1.f;
        // per-recollection stereo placement
        float pan = clamp(0.5f + temper * rng.bipolar() * 0.5f, 0.f, 1.f);
        h->panL = std::cos(pan * 0.5f * (float)M_PI) * 1.41421f;
        h->panR = std::sin(pan * 0.5f * (float)M_PI) * 1.41421f;
        h->age = 0.0;
        float fadeBase = (mode == 2) ? 0.030f : 0.012f;  // dream fades slower
        h->attackSamp = fadeBase * sr;
        h->releaseSamp = fadeBase * 1.6f * sr;
        h->degrade = ageAmt;
        h->lpState = 0.f; h->holdVal = 0.f; h->holdCnt = 0; h->jitter = 0.f;
        lastPlayedStart = r.startPos;
        recallFlash = 1.f;
        eventPulse.trigger(0.003f);
        return true;
    }

    // read one degraded sample from a head (mono), advancing it
    float readHead(Head& h, float ageAmt, float temper) {
        double off = h.phase;
        // temper positional jitter (smoothed brown-ish)
        h.jitter += (rng.bipolar() * temper * 6.f - h.jitter) * 0.02f;
        double idx = h.base + (h.dir > 0 ? off : (h.len - off)) + h.jitter;
        idx = std::fmod(idx, (double)bufLen);
        if (idx < 0) idx += bufLen;
        int i0 = (int)idx;
        int i1 = i0 + 1; if (i1 >= bufLen) i1 = 0;
        float frac = (float)(idx - i0);
        float s = bufL[i0] + (bufL[i1] - bufL[i0]) * frac;
        float sR = bufR[i0] + (bufR[i1] - bufR[i0]) * frac;
        float smp = 0.5f * (s + sR);

        // --- degradation chain, scaled by AGE ---
        if (ageAmt > 0.001f) {
            // sample-rate reduction (hold)
            int holdN = 1 + (int)(ageAmt * ageAmt * 14.f);
            if (h.holdCnt <= 0) { h.holdVal = smp; h.holdCnt = holdN; }
            h.holdCnt--;
            smp = h.holdVal;
            // bit-depth quantization
            float bits = 16.f - ageAmt * 12.f;
            float levels = std::pow(2.f, bits);
            smp = std::round(smp * levels) / levels;
            // progressive low-pass
            float fc = 800.f * std::pow(22.f, 1.f - ageAmt);
            float a = clamp(fc / sr * 6.2832f, 0.f, 1.f);
            h.lpState += a * (smp - h.lpState);
            smp = h.lpState;
        }

        // envelope: cosine attack, cosine release near the end
        float env = 1.f;
        if (h.age < h.attackSamp)
            env = 0.5f * (1.f - std::cos((float)M_PI * (float)h.age / h.attackSamp));
        double remainOut = (h.len - h.phase) / std::max(0.01, (double)h.speed);
        if (remainOut < h.releaseSamp)
            env *= 0.5f * (1.f - std::cos((float)M_PI * (float)remainOut / h.releaseSamp));

        h.phase += h.speed;
        h.age += 1.0;
        if (h.phase >= h.len) h.active = false;
        return smp * env * h.amp;
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != sr)
            allocate(args.sampleRate);

        // --- controls ---
        float memory = macro(MEMORY_PARAM, MEMORY_CV_INPUT);
        float recall = macro(RECALL_PARAM, RECALL_CV_INPUT);
        float ageAmt = macro(AGE_PARAM, AGE_CV_INPUT);
        float smear = macro(SMEAR_PARAM, SMEAR_CV_INPUT);
        float forget = params[FORGET_PARAM].getValue();
        float temper = params[TEMPER_PARAM].getValue();
        float dirProb = params[DIRECTION_PARAM].getValue();
        float mix = params[MIX_PARAM].getValue();
        float outLvl = params[OUTPUT_PARAM].getValue() * 2.f;  // unity at 0.5
        int mode = (int)std::round(params[MODE_PARAM].getValue());
        int memmode = (int)std::round(params[MEMMODE_PARAM].getValue());
        float memorySec = 0.05f * std::pow(kMaxSeconds / 0.05f, memory);

        // --- freeze / event / clear ---
        bool freezeGate = inputs[FREEZE_INPUT].isConnected() &&
                          freezeGateTrig.process(inputs[FREEZE_INPUT].getVoltage(), 0.1f, 1.f);
        // (freezeGateTrig used only for edge; read raw level for gate mode)
        bool gateHigh = inputs[FREEZE_INPUT].getVoltage() >= 1.f;
        if (freezeBtnTrig.process(params[FREEZE_PARAM].getValue()))
            frozenToggle = !frozenToggle;
        if (freezeGate) frozenToggle = !frozenToggle;  // rising edge also toggles
        bool frozen = (freezeMode == 0) ? (gateHigh || frozenToggle) : frozenToggle;

        if (clearBtnTrig.process(params[CLEAR_PARAM].getValue()) ||
            (inputs[CLEAR_INPUT].isConnected() &&
             clearCvTrig.process(inputs[CLEAR_INPUT].getVoltage(), 0.1f, 1.f)))
            clearMemory();

        bool forceEvent = eventBtnTrig.process(params[EVENT_PARAM].getValue()) ||
            (inputs[EVENT_INPUT].isConnected() &&
             eventCvTrig.process(inputs[EVENT_INPUT].getVoltage(), 0.1f, 1.f));

        // --- input ---
        float inL = inputs[IN_L_INPUT].getVoltage() * 0.2f;
        float inR = inputs[IN_R_INPUT].isConnected()
                        ? inputs[IN_R_INPUT].getVoltage() * 0.2f : inL;
        float inMono = 0.5f * (inL + inR);

        // input analysis: fast/slow envelope, transient, adaptive reference
        float aMag = std::fabs(inMono);
        float fa = (aMag > fastEnv) ? 0.3f : 0.02f;   // fast attack, medium release
        fastEnv += fa * (aMag - fastEnv);
        float sa = (aMag > slowEnv) ? 0.01f : 0.002f;
        slowEnv += sa * (aMag - slowEnv);
        transient = std::max(0.f, fastEnv - slowEnv);
        refLevel += 0.0005f * (fastEnv - refLevel);
        refLevel = std::max(refLevel, 0.001f);

        // --- write head (unless frozen) ---
        float wL = inL, wR = inR;
        if (!frozen) {
            // fold feedback into what gets committed to memory
            float fbGain = clamp((1.f - forget) * 0.9f, 0.f, 0.9f);
            float wiL = inL + fbL * fbGain;
            float wiR = inR + fbR * fbGain;
            switch (memmode) {
                case 0:  // oblivion: replace
                    wL = wiL; wR = wiR;
                    break;
                case 1:  // remanence: 30% old, 70% new
                    wL = bufL[writePos] * 0.30f + wiL * 0.70f;
                    wR = bufR[writePos] * 0.30f + wiR * 0.70f;
                    break;
                default: // sediment: accumulate + saturate + DC block
                    wL = dcL.process(vestigia_dsp::softSat(bufL[writePos] + wiL));
                    wR = dcR.process(vestigia_dsp::softSat(bufR[writePos] + wiR));
                    break;
            }
            if (!std::isfinite(wL)) wL = 0.f;
            if (!std::isfinite(wR)) wR = 0.f;
            bufL[writePos] = wL;
            bufR[writePos] = wR;
        } else {
            wL = bufL[writePos];
            wR = bufR[writePos];
        }

        // --- activity map accumulation ---
        float wMono = 0.5f * (wL + wR);
        blockAccum += (double)wMono * wMono;
        blockAccumN++;
        int nowBlock = writePos / blockLen;
        if (nowBlock != curBlock) {
            // finalize the block we just left (only if it was actually written)
            Block& b = blocks[curBlock];
            float e = blockAccumN ? std::sqrt((float)(blockAccum / blockAccumN)) : 0.f;
            b.energy = e;
            b.frame = args.frame;
            // hysteresis: open at -45 dBFS, close at -52 dBFS
            const float openT = 0.0056f, closeT = 0.0025f;
            if (!blockActiveState && e > openT) blockActiveState = true;
            else if (blockActiveState && e < closeT) blockActiveState = false;
            b.active = blockActiveState;
            cellEnergy[curBlock * kCells / numBlocks] = e;
            blockAccum = 0.0; blockAccumN = 0;
            curBlock = nowBlock;
        }

        // advance the (logical) write transport
        writePos++;
        if (writePos >= bufLen) writePos = 0;

        // --- periodically rebuild the region list ---
        if (--rebuildCounter <= 0) {
            rebuildCounter = 512;
            rebuildRegions(memorySec, mode, args.frame);
        }

        // --- recollection events ---
        float dt = args.sampleTime;
        if (autoCooldown > 0.f) autoCooldown -= dt;
        // base rate in events/sec, shaped by mode and input level
        float envNorm = clamp(fastEnv / (refLevel * 2.5f), 0.f, 1.f);
        float rate = recall * recall * 8.f;
        if (mode == 1) rate *= 0.25f + 1.75f * envNorm;       // breathe: with energy
        else if (mode == 2) rate *= 0.25f + 1.75f * (1.f - envNorm);  // dream: with silence
        recallAccum += rate * dt;
        bool autoEvent = false;
        if (recallAccum >= 1.f) { recallAccum -= 1.f; autoEvent = true; }
        if (recallAccum > 2.f) recallAccum = 2.f;
        // listen mode also fires on transients
        if (mode == 0 && transient > 0.04f && autoCooldown <= 0.f && recall > 0.02f) {
            autoEvent = true;
        }
        if (autoEvent && autoCooldown <= 0.f) {
            if (tryRecall(mode, memorySec, ageAmt, temper, dirProb))
                autoCooldown = 0.03f;
        }
        if (forceEvent)
            tryRecall(mode, memorySec, ageAmt, temper, dirProb);

        // --- playback heads -> recalled (pre-smear) ---
        float recL = 0.f, recR = 0.f;
        int liveHeads = 0;
        for (auto& h : heads) {
            if (!h.active) continue;
            liveHeads++;
            float s = readHead(h, h.degrade, temper);
            recL += s * h.panL;
            recR += s * h.panR;
        }

        // --- smear / diffusion ---
        float difL = recL, difR = recR;
        for (int i = 0; i < 4; i++) { difL = apL[i].process(difL); difR = apR[i].process(difR); }
        float wetL = recL + (difL - recL) * smear;
        float wetR = recR + (difR - recR) * smear;

        // --- feedback path (filtered, saturated, protected) ---
        float fLp = clamp(2000.f / sr * 6.2832f, 0.f, 1.f);  // gentle HF loss in loop
        fbL = dcWL.process(vestigia_dsp::softSat(wetL));
        fbR = dcWR.process(vestigia_dsp::softSat(wetR));
        fbL += fLp * (0.f - fbL) * 0.0f;  // (kept simple; softSat + dc block bound it)
        if (!std::isfinite(fbL)) fbL = 0.f;
        if (!std::isfinite(fbR)) fbR = 0.f;

        // --- equal-power dry/wet mix ---
        float dg = std::cos(mix * 0.5f * (float)M_PI);
        float wg = std::sin(mix * 0.5f * (float)M_PI);
        float outL = inL * dg + wetL * wg;
        float outR = inR * dg + wetR * wg;
        outL *= outLvl; outR *= outLvl;
        if (monoOut) {
            float m = 0.70710678f * (outL + outR);
            outL = outR = m;
        }
        outL = vestigia_dsp::softSat(outL);
        outR = vestigia_dsp::softSat(outR);
        if (!std::isfinite(outL)) outL = 0.f;
        if (!std::isfinite(outR)) outR = 0.f;

        outputs[OUT_L_OUTPUT].setVoltage(outL * 5.f);
        outputs[OUT_R_OUTPUT].setVoltage(outR * 5.f);

        // --- aux outputs ---
        outputs[EVENT_OUTPUT].setVoltage(eventPulse.process(dt) ? 10.f : 0.f);
        outputs[ENV_OUTPUT].setVoltage(clamp(fastEnv * 10.f, 0.f, 10.f));
        // chaos: blend of live-head activity and mean region age, smoothed, ±5V
        float meanAge = 0.f;
        for (int i = 0; i < numRegions; i++) meanAge += regions[i].ageSec;
        if (numRegions) meanAge /= numRegions;
        float chaosTarget = (numRegions ? clamp(meanAge / std::max(0.05f, memorySec), 0.f, 1.f) : 0.f)
                            + 0.25f * liveHeads;
        chaosSmooth += 0.001f * (chaosTarget - chaosSmooth);
        outputs[CHAOS_OUTPUT].setVoltage(clamp(chaosSmooth * 2.f - 1.f, -1.f, 1.f) * 5.f);

        // --- meters / lights ---
        levelEnvL += (std::fabs(outL) - levelEnvL) * 0.002f;
        levelEnvR += (std::fabs(outR) - levelEnvR) * 0.002f;
        lights[LEVEL_L_LIGHT].setBrightness(clamp(levelEnvL * 2.f, 0.f, 1.f));
        lights[LEVEL_R_LIGHT].setBrightness(clamp(levelEnvR * 2.f, 0.f, 1.f));
        lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);
        if (recallFlash > 0.f) recallFlash -= dt * 6.f;
        lights[EVENT_LIGHT].setBrightness(clamp(recallFlash, 0.f, 1.f));
        displayHead = (float)writePos / (float)bufLen;
    }
};


// linear "tape" display: block energies, active regions, write head, recall flash
struct VestigiaDisplay : TransparentWidget {
    Vestigia* module = nullptr;

    void draw(const DrawArgs& args) override {
        // dark bezel
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.5f);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) { TransparentWidget::drawLayer(args, layer); return; }
        nvgScissor(args.vg, 1, 1, box.size.x - 2, box.size.y - 2);
        float W = box.size.x, H = box.size.y;
        int n = Vestigia::kCells;
        float cw = W / n;
        for (int i = 0; i < n; i++) {
            float e = clamp(module->cellEnergy[i] * 3.5f, 0.f, 1.f);
            if (e <= 0.01f) continue;
            float h = e * (H - 4.f);
            // yellow bars for stored energy
            nvgBeginPath(args.vg);
            nvgRect(args.vg, i * cw, (H - h) * 0.5f, std::max(0.6f, cw - 0.3f), h);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, (int)(60 + 160 * e)));
            nvgFill(args.vg);
        }
        // write head marker
        float hx = module->displayHead * W;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, hx, 1);
        nvgLineTo(args.vg, hx, H - 1);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, module->frozenToggle ? 0x50 : 0xc0));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);
        // recall flash overlay
        if (module->recallFlash > 0.f) {
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0, 0, W, H, 2.5f);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0x40, 0x40,
                (int)(60 * clamp(module->recallFlash, 0.f, 1.f))));
            nvgFill(args.vg);
        }
        nvgResetScissor(args.vg);
        TransparentWidget::drawLayer(args, layer);
    }
};


struct VestigiaWidget : ModuleWidget {
    VestigiaWidget(Vestigia* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/vestigia.svg")));

// @layout:begin vestigia 111.76 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem MEMORY_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem RECALL_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem AGE_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem SMEAR_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem FORGET_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem TEMPER_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem DIRECTION_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MIX_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem OUTPUT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem MEMMODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem FREEZE_PARAM TL1105 2.6 param "" 0.0
// @elem EVENT_PARAM TL1105 2.6 param "" 0.0
// @elem CLEAR_PARAM TL1105 2.6 param "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FREEZE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem EVENT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLEAR_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MEMORY_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RECALL_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AGE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SMEAR_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem EVENT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CHAOS_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem FREEZE_LIGHT SmallLight 1.0 light "" 0.0
// @elem EVENT_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_MEMORY label 0.0 label "memory" 0.0 27.94 44.00
// @elem LABEL_RECALL label 0.0 label "recall" 0.0 55.88 44.00
// @elem LABEL_AGE label 0.0 label "age" 0.0 83.82 44.00
// @elem LABEL_SMEAR label 0.0 label "smear" 0.0 27.94 66.00
// @elem LABEL_FORGET label 0.0 label "forget" 0.0 55.88 66.00
// @elem LABEL_TEMPER label 0.0 label "temper" 0.0 83.82 66.00
// @elem LABEL_MODE label 0.0 label "mode" 0.0 11.00 52.50
// @elem LABEL_MEMMODE label 0.0 label "mem" 0.0 100.76 52.50
// @elem LABEL_DIR label 0.0 label "dir" 0.0 13.00 84.50
// @elem LABEL_MIX label 0.0 label "mix" 0.0 28.00 84.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 43.00 84.50
// @elem LABEL_FRZ label 0.0 label "frz" 0.0 64.00 83.00
// @elem LABEL_EVT label 0.0 label "evt" 0.0 80.00 83.00
// @elem LABEL_CLR label 0.0 label "clr" 0.0 96.00 83.00
// @elem LABEL_IN label 0.0 label "in" 0.0 8.00 99.50
// @elem LABEL_INR label 0.0 label "r" 0.0 19.00 99.50
// @elem LABEL_FRZIN label 0.0 label "frz" 0.0 30.00 99.50
// @elem LABEL_EVTIN label 0.0 label "evt" 0.0 41.00 99.50
// @elem LABEL_CLRIN label 0.0 label "clr" 0.0 52.00 99.50
// @elem LABEL_MEMCV label 0.0 label "mem" 0.0 63.00 99.50
// @elem LABEL_RECCV label 0.0 label "rec" 0.0 74.00 99.50
// @elem LABEL_AGECV label 0.0 label "age" 0.0 85.00 99.50
// @elem LABEL_SMRCV label 0.0 label "smr" 0.0 96.00 99.50
// @elem LABEL_OUTL label 0.0 label "L" 0.0 18.00 114.00
// @elem LABEL_OUTR label 0.0 label "R" 0.0 38.00 114.00
// @elem LABEL_EVTOUT label 0.0 label "evt" 0.0 58.00 114.00
// @elem LABEL_ENVOUT label 0.0 label "env" 0.0 78.00 114.00
// @elem LABEL_CHAOSOUT label 0.0 label "chaos" 0.0 98.00 114.00
// @elem BOX_OUTL panel_box 7.0 box "" 0.0 18.00 108.50
// @elem BOX_OUTR panel_box 7.0 box "" 0.0 38.00 108.50
// @elem BOX_EVTOUT panel_box 7.0 box "" 0.0 58.00 108.50
// @elem BOX_ENVOUT panel_box 7.0 box "" 0.0 78.00 108.50
// @elem BOX_CHAOSOUT panel_box 7.0 box "" 0.0 98.00 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 55.88 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(101.60f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(101.60f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(27.94f, 33.00f)), module, Vestigia::MEMORY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(55.88f, 33.00f)), module, Vestigia::RECALL_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(83.82f, 33.00f)), module, Vestigia::AGE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(27.94f, 55.00f)), module, Vestigia::SMEAR_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(55.88f, 55.00f)), module, Vestigia::FORGET_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(83.82f, 55.00f)), module, Vestigia::TEMPER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(13.00f, 76.00f)), module, Vestigia::DIRECTION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(28.00f, 76.00f)), module, Vestigia::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(43.00f, 76.00f)), module, Vestigia::OUTPUT_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(11.00f, 44.00f)), module, Vestigia::MODE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(100.76f, 44.00f)), module, Vestigia::MEMMODE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(64.00f, 76.00f)), module, Vestigia::FREEZE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(80.00f, 76.00f)), module, Vestigia::EVENT_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(96.00f, 76.00f)), module, Vestigia::CLEAR_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.00f, 92.00f)), module, Vestigia::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.00f, 92.00f)), module, Vestigia::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.00f, 92.00f)), module, Vestigia::FREEZE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.00f, 92.00f)), module, Vestigia::EVENT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.00f, 92.00f)), module, Vestigia::CLEAR_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(63.00f, 92.00f)), module, Vestigia::MEMORY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(74.00f, 92.00f)), module, Vestigia::RECALL_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(85.00f, 92.00f)), module, Vestigia::AGE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(96.00f, 92.00f)), module, Vestigia::SMEAR_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(18.00f, 106.50f)), module, Vestigia::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.00f, 106.50f)), module, Vestigia::OUT_R_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(58.00f, 106.50f)), module, Vestigia::EVENT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.00f, 106.50f)), module, Vestigia::ENV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(98.00f, 106.50f)), module, Vestigia::CHAOS_OUTPUT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(67.50f, 72.00f)), module, Vestigia::FREEZE_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(83.50f, 72.00f)), module, Vestigia::EVENT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(23.00f, 103.50f)), module, Vestigia::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(43.00f, 103.50f)), module, Vestigia::LEVEL_R_LIGHT));
        // @layout:end

        VestigiaDisplay* disp = new VestigiaDisplay;
        disp->module = module;
        disp->box.pos = mm2px(Vec(8.00f, 9.00f));
        disp->box.size = mm2px(Vec(95.76f, 14.00f));
        addChild(disp);
    }

    void appendContextMenu(Menu* menu) override {
        Vestigia* module = getModule<Vestigia>();
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Freeze button",
            {"Gate + toggle", "Toggle"}, &module->freezeMode));
        menu->addChild(createBoolPtrMenuItem("Mono output (equal-power sum)",
            "", &module->monoOut));
    }
};

Model* modelVestigia = createModel<Vestigia, VestigiaWidget>("vestigia");
