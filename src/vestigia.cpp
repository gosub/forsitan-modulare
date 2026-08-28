// vestigia.cpp - VCV Rack 2 module
// vestigia (Latin: "traces, footprints") is a stereo memory effect. It
// continuously writes the incoming signal into an endless tape loop, keeps
// a parallel block-based activity map of where meaningful sound actually
// lives, and recalls those regions - during pauses, on transients, or by
// probability - degrading each memory a little more every time it returns.
//
// It is not a delay. The write head never stops (except FREEZE); what the
// present does to the past is set by the MEMORY MODE:
//   oblivion   - the present replaces the past
//   remanence  - the present rewrites the past but leaves traces (retention)
//   sediment   - the present accumulates over the past through saturation
//
// The recollection engine consults the activity map, never arbitrary buffer
// positions, so silence is never recalled. Three recollection modes shape
// WHEN memories return:
//   listen   - event-centered, triggered by transients, prefers recent
//   breathe  - recall follows the input envelope, density and similarity
//   dream    - recall rises as the input falls quiet; older, reversed, longer
//
// This build implements the full Vestigium design document (v0.2), beyond
// the MVP: a memory-descriptor pool with per-region integrity and
// wear-per-recollection, sonic-similarity weighting, Hermite interpolation
// that degrades toward nearest-neighbour with AGE, dropouts, a modulated
// all-pass diffusion network with crossfeed, cross-fed protected feedback,
// event-centred / region / sub-region selection with mode-dependent
// padding, every macro CV, a MEMORY OUT tap, the full context menu
// (quality, buffer size, retention, sediment amount/character, freeze
// behaviour, mono output, seed, safety, bypass/patch memory) and the seven
// factory presets. The audio buffer is optionally saved with the patch;
// otherwise only the seed survives a reload, keeping behaviour reproducible.

#include "forsitan.hpp"
#include <vector>
#include <string>

namespace vestigia_dsp {

// deterministic RNG (xorshift32) - musical randomness seeded from the patch
struct Rng {
    uint32_t s = 0x1234567u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
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

// tanh-flavoured "tape" curve and a wavefolder, for sediment character
inline float tapeSat(float x) { return std::tanh(x * 1.3f); }
inline float fold(float x) {
    // reflective wavefolder, bounded to ±1
    float y = x;
    for (int i = 0; i < 4; i++) {
        if (y > 1.f) y = 2.f - y;
        else if (y < -1.f) y = -2.f - y;
        else break;
    }
    return y;
}

// 4-point Hermite interpolation
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((((a * t) - b) * t + c) * t + x0);
}

// one-pole DC blocker
struct DCBlock {
    float x1 = 0.f, y1 = 0.f;
    float process(float x) { float y = x - x1 + 0.995f * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0.f; }
};

// Schroeder all-pass for the smear / diffusion network
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

// minimal base64 for optional buffer persistence
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline std::string b64encode(const int16_t* p, size_t n) {
    const uint8_t* d = (const uint8_t*)p;
    size_t len = n * 2;
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = d[i] << 16;
        if (i + 1 < len) v |= d[i + 1] << 8;
        if (i + 2 < len) v |= d[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? B64[v & 63] : '=';
    }
    return out;
}
inline void b64decode(const std::string& s, int16_t* p, size_t n) {
    int8_t inv[256]; for (int i = 0; i < 256; i++) inv[i] = -1;
    for (int i = 0; i < 64; i++) inv[(uint8_t)B64[i]] = i;
    uint8_t* d = (uint8_t*)p;
    size_t len = n * 2, oi = 0;
    uint32_t v = 0; int bits = 0;
    for (char c : s) {
        int8_t x = inv[(uint8_t)c];
        if (x < 0) continue;
        v = (v << 6) | x; bits += 6;
        if (bits >= 8) { bits -= 8; if (oi < len) d[oi++] = (v >> bits) & 0xFF; }
    }
}

} // namespace vestigia_dsp

struct Vestigia : Module {
    enum ParamId {
        MEMORY_PARAM, RECALL_PARAM, AGE_PARAM, SMEAR_PARAM, FORGET_PARAM,
        TEMPER_PARAM, DIRECTION_PARAM, MIX_PARAM, OUTPUT_PARAM,
        HARMONY_PARAM,  // 0 = consonant (unison/octaves), 1 = inharmonic
        MODE_PARAM,     // 0 listen, 1 breathe, 2 dream
        MEMMODE_PARAM,  // 0 oblivion, 1 remanence, 2 sediment
        FREEZE_PARAM, EVENT_PARAM, CLEAR_PARAM,
        SOURCE_PARAM,   // recall listens to: 0 dry, 1 mix, 2 wet
        FB_PARAM,       // feedback: wet back into the record path
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT, IN_R_INPUT,
        FREEZE_INPUT, EVENT_INPUT, CLEAR_INPUT,
        MEMORY_CV_INPUT, RECALL_CV_INPUT, AGE_CV_INPUT, SMEAR_CV_INPUT,
        FORGET_CV_INPUT, TEMPER_CV_INPUT, DIRECTION_CV_INPUT,
        MIX_CV_INPUT, OUTPUT_CV_INPUT, HARMONY_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_L_OUTPUT, OUT_R_OUTPUT,
        MEMORY_OUTPUT, EVENT_OUTPUT, ENV_OUTPUT, CHAOS_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        FREEZE_LIGHT, EVENT_LIGHT, LEVEL_L_LIGHT, LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    static constexpr float kMaxSeconds = 32.f;  // allocation ceiling
    static constexpr int kMaxRegions = 16;
    static constexpr int kMaxHeads = 4;
    static constexpr int kMaxDescriptors = 16;
    static constexpr int kCells = 120;

    // --- circular stereo buffer + activity map ---
    float sr = 0.f;
    int bufLen = 0, blockLen = 0, numBlocks = 0;
    float bufferSeconds = 16.f;
    std::vector<float> bufL, bufR;
    int writePos = 0;

    struct Block {
        float energy = 0.f;
        float transient = 0.f;
        float zcr = 0.f;
        bool active = false;
        int64_t frame = -1;
    };
    std::vector<Block> blocks;
    double blockAccum = 0.0;
    float blockPeakTrans = 0.f;
    int blockZC = 0;
    float blockPrevSign = 0.f;
    int blockAccumN = 0;
    int curBlock = 0;
    bool blockActiveState = false;

    // --- input analysis ---
    float fastEnv = 0.f, slowEnv = 0.f, refLevel = 0.02f;
    float transient = 0.f, inZcr = 0.f;
    float inPrevSign = 0.f; float inZcAccum = 0.f;

    // --- regions ---
    struct Region {
        double startPos = 0, length = 0;
        float energy = 0.f, transient = 0.f, zcr = 0.f;
        float ageSec = 0.f, score = 0.f;
        double peakOffset = 0.0;   // samples from start to the strongest block
    };
    Region regions[kMaxRegions];
    int numRegions = 0;
    int rebuildCounter = 0;

    // --- memory descriptors (integrity + wear per recollection) ---
    struct Fragment {
        double startPos = 0, length = 0;
        int recallCount = 0;
        float degradation = 0.f;  // added wear, accumulates per recall
        float integrity = 1.f;    // 1 = still matches when captured
        int64_t lastFrame = -1;
        bool valid = false;
    };
    Fragment frags[kMaxDescriptors];
    double lastPlayedStart = -1e9;
    float recallAccum = 0.f, autoCooldown = 0.f, recallThresh = 1.f;
    // previous-sample mix / wet, for the "recall listens to" sources
    float senseMixMono = 0.f, senseWetMono = 0.f;

    struct Head {
        bool active = false;
        double base = 0.0, len = 0.0, phase = 0.0;
        int dir = 1;
        float speed = 1.f, amp = 1.f, panL = 0.7f, panR = 0.7f;
        double age = 0.0;
        float attackSamp = 96.f, releaseSamp = 240.f;
        float degrade = 0.f;
        float lpState = 0.f, holdVal = 0.f;
        int holdCnt = 0, dropCnt = 0;
        float dropGain = 1.f, wowPhase = 0.f;
        float jitter = 0.f;
        int fragIdx = -1;
    };
    Head heads[kMaxHeads];
    int activeHeads() const { return quality == 0 ? 2 : (quality == 2 ? 4 : 3); }

    // --- smear / diffusion + feedback ---
    vestigia_dsp::AllPass apL[4], apR[4];
    float smearLfo = 0.f;
    float fbL = 0.f, fbR = 0.f;
    vestigia_dsp::DCBlock dcL, dcR, dcWL, dcWR;

    // --- triggers / state ---
    dsp::SchmittTrigger freezeBtnTrig, freezeGateTrig;
    dsp::SchmittTrigger eventBtnTrig, eventCvTrig;
    dsp::SchmittTrigger clearBtnTrig, clearCvTrig;
    dsp::PulseGenerator eventPulse;
    bool frozenToggle = false;

    // --- metering / display ---
    float levelEnvL = 0.f, levelEnvR = 0.f, chaosSmooth = 0.f, chaosHeld = 0.f;
    float recallFlash = 0.f;
    float cellEnergy[kCells] = {};
    float displayHead = 0.f;

    uint32_t seed = 0x1234567u;
    vestigia_dsp::Rng rng;
    bool needReseedOnAdd = false;

    // --- context-menu options ---
    int quality = 1;         // 0 eco, 1 standard, 2 high
    int bufferSizeIdx = 2;   // 0:4  1:8  2:16  3:32 s
    int retentionIdx = 1;    // 0:.10 1:.30 2:.50 3:.70
    int sedimentAmtIdx = 1;  // 0:.5 1:1 2:1.5 3:2
    int sedimentSatIdx = 0;  // 0 soft, 1 tape, 2 fold
    int freezeMode = 1;      // 0 gate, 1 toggle, 2 toggle on rising edge
    int monoMode = 0;        // 0 stereo, 1 left only, 2 sum, 3 equal-power sum
    int seedMode = 0;        // 0 random on load, 1 fixed, 2 (reseed action)
    bool chaosStepped = false, chaosBipolar = true;
    bool feedbackLimiter = true, softClip = true, preserveTails = true;
    bool bypassClearsMemory = false, saveMemoryWithPatch = false;
    bool temperTiming = true;   // temper scatters the recall clock

    int numDescriptors() const { return quality == 0 ? 4 : (quality == 2 ? 16 : 8); }
    int regionCap() const { return quality == 0 ? 6 : (quality == 2 ? 16 : 10); }
    float retention() const { const float r[4] = {0.10f, 0.30f, 0.50f, 0.70f}; return r[retentionIdx]; }
    float sedimentAmt() const { const float a[4] = {0.5f, 1.f, 1.5f, 2.f}; return a[sedimentAmtIdx]; }

    Vestigia() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(MEMORY_PARAM, 0.f, 1.f, 0.4f, "Memory horizon", " s", bufferSeconds / 0.05f, 0.05f);
        configParam(RECALL_PARAM, 0.f, 1.f, 0.35f, "Recall rate", "%", 0.f, 100.f);
        configParam(AGE_PARAM, 0.f, 1.f, 0.25f, "Age / degradation", "%", 0.f, 100.f);
        configParam(SMEAR_PARAM, 0.f, 1.f, 0.2f, "Smear", "%", 0.f, 100.f);
        configParam(FORGET_PARAM, 0.f, 1.f, 0.4f, "Forget (memory persistence)", "%", 0.f, 100.f);
        configParam(TEMPER_PARAM, 0.f, 1.f, 0.15f, "Temper (instability)", "%", 0.f, 100.f);
        configParam(DIRECTION_PARAM, 0.f, 1.f, 0.f, "Reverse probability", "%", 0.f, 100.f);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Dry / wet", "%", 0.f, 100.f);
        configParam(OUTPUT_PARAM, 0.f, 1.f, 0.5f, "Output level");
        configParam(HARMONY_PARAM, 0.f, 1.f, 0.25f, "Harmony (consonant to inharmonic)", "%", 0.f, 100.f);
        configSwitch(MODE_PARAM, 0.f, 2.f, 1.f, "Recollection mode", {"Listen", "Breathe", "Dream"});
        configSwitch(MEMMODE_PARAM, 0.f, 2.f, 0.f, "Memory mode", {"Oblivion", "Remanence", "Sediment"});
        configButton(FREEZE_PARAM, "Freeze");
        configButton(EVENT_PARAM, "Event (force recall)");
        configButton(CLEAR_PARAM, "Clear memory");
        configSwitch(SOURCE_PARAM, 0.f, 2.f, 0.f, "Recall source",
            {"Dry (input)", "Mix", "Wet (self-triggering)"});
        configParam(FB_PARAM, 0.f, 1.1f, 0.3f, "Feedback (wet into record path)", "%", 0.f, 100.f);
        configInput(IN_L_INPUT, "Audio L");
        configInput(IN_R_INPUT, "Audio R (normalled from L)");
        configInput(FREEZE_INPUT, "Freeze gate");
        configInput(EVENT_INPUT, "Event trigger");
        configInput(CLEAR_INPUT, "Clear trigger");
        configInput(MEMORY_CV_INPUT, "Memory CV");
        configInput(RECALL_CV_INPUT, "Recall CV");
        configInput(AGE_CV_INPUT, "Age CV");
        configInput(SMEAR_CV_INPUT, "Smear CV");
        configInput(FORGET_CV_INPUT, "Forget CV");
        configInput(TEMPER_CV_INPUT, "Temper CV");
        configInput(DIRECTION_CV_INPUT, "Direction CV");
        configInput(MIX_CV_INPUT, "Mix CV");
        configInput(OUTPUT_CV_INPUT, "Output level CV");
        configInput(HARMONY_CV_INPUT, "Harmony CV");
        configOutput(OUT_L_OUTPUT, "Audio L");
        configOutput(OUT_R_OUTPUT, "Audio R");
        configOutput(MEMORY_OUTPUT, "Recalled memory (pre-smear)");
        configOutput(EVENT_OUTPUT, "Event (recall began)");
        configOutput(ENV_OUTPUT, "Envelope of the recall source (dry/mix/wet)");
        configOutput(CHAOS_OUTPUT, "Chaos / internal state");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
        rng.seed(seed);
    }

    void updateHorizonDisplay() {
        if (ParamQuantity* pq = getParamQuantity(MEMORY_PARAM))
            pq->displayBase = bufferSeconds / 0.05f;
    }

    void allocate(float newSr) {
        sr = newSr;
        static const float bs[4] = {4.f, 8.f, 16.f, 32.f};
        bufferSeconds = bs[clamp(bufferSizeIdx, 0, 3)];
        blockLen = std::max(1, (int)(0.02f * sr));
        numBlocks = (int)(bufferSeconds * sr) / blockLen + 1;
        bufLen = numBlocks * blockLen;
        bufL.assign(bufLen, 0.f);
        bufR.assign(bufLen, 0.f);
        blocks.assign(numBlocks, Block());
        writePos = 0; curBlock = 0;
        blockAccum = 0.0; blockAccumN = 0; blockPeakTrans = 0.f; blockZC = 0;
        const float msL[4] = {13.f, 29.f, 47.f, 67.f};
        const float msR[4] = {17.f, 31.f, 53.f, 73.f};
        for (int i = 0; i < 4; i++) {
            apL[i].init((int)(msL[i] * 0.001f * sr)); apL[i].g = 0.55f;
            apR[i].init((int)(msR[i] * 0.001f * sr)); apR[i].g = 0.55f;
        }
        updateHorizonDisplay();
        clearMemory();
    }

    void clearMemory() {
        std::fill(bufL.begin(), bufL.end(), 0.f);
        std::fill(bufR.begin(), bufR.end(), 0.f);
        for (auto& b : blocks) b = Block();
        for (auto& h : heads) h.active = false;
        for (auto& f : frags) f = Fragment();
        for (int i = 0; i < 4; i++) { apL[i].reset(); apR[i].reset(); }
        for (int i = 0; i < kCells; i++) cellEnergy[i] = 0.f;
        numRegions = 0;
        fbL = fbR = 0.f;
        dcL.reset(); dcR.reset(); dcWL.reset(); dcWR.reset();
        blockActiveState = false;
        blockAccum = 0.0; blockAccumN = 0; blockPeakTrans = 0.f; blockZC = 0;
    }

    void onReset() override {
        frozenToggle = false;
        quality = 1; bufferSizeIdx = 2; retentionIdx = 1;
        sedimentAmtIdx = 1; sedimentSatIdx = 0;
        freezeMode = 1; monoMode = 0; seedMode = 0;
        chaosStepped = false; chaosBipolar = true;
        feedbackLimiter = softClip = preserveTails = true;
        bypassClearsMemory = saveMemoryWithPatch = false;
        temperTiming = true;
        seed = 0x1234567u; rng.seed(seed);
        sr = 0.f;
    }

    void onRandomize() override { reseed(); }

    void reseed() {
        seed = (uint32_t)(random::u32() | 1u);
        rng.seed(seed);
    }

    void onAdd(const AddEvent& e) override {
        Module::onAdd(e);
        if (seedMode == 0) { needReseedOnAdd = true; }  // random on load
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "seed", json_integer((json_int_t)seed));
        json_object_set_new(root, "frozen", json_boolean(frozenToggle));
        json_object_set_new(root, "quality", json_integer(quality));
        json_object_set_new(root, "bufferSizeIdx", json_integer(bufferSizeIdx));
        json_object_set_new(root, "retentionIdx", json_integer(retentionIdx));
        json_object_set_new(root, "sedimentAmtIdx", json_integer(sedimentAmtIdx));
        json_object_set_new(root, "sedimentSatIdx", json_integer(sedimentSatIdx));
        json_object_set_new(root, "freezeMode", json_integer(freezeMode));
        json_object_set_new(root, "monoMode", json_integer(monoMode));
        json_object_set_new(root, "seedMode", json_integer(seedMode));
        json_object_set_new(root, "chaosStepped", json_boolean(chaosStepped));
        json_object_set_new(root, "chaosBipolar", json_boolean(chaosBipolar));
        json_object_set_new(root, "feedbackLimiter", json_boolean(feedbackLimiter));
        json_object_set_new(root, "softClip", json_boolean(softClip));
        json_object_set_new(root, "preserveTails", json_boolean(preserveTails));
        json_object_set_new(root, "bypassClearsMemory", json_boolean(bypassClearsMemory));
        json_object_set_new(root, "saveMemoryWithPatch", json_boolean(saveMemoryWithPatch));
        json_object_set_new(root, "temperTiming", json_boolean(temperTiming));
        if (saveMemoryWithPatch && bufLen > 0) {
            std::vector<int16_t> q(bufLen * 2);
            for (int i = 0; i < bufLen; i++) {
                q[i] = (int16_t)clamp((int)std::lround(bufL[i] * 32767.f), -32768, 32767);
                q[bufLen + i] = (int16_t)clamp((int)std::lround(bufR[i] * 32767.f), -32768, 32767);
            }
            json_object_set_new(root, "bufSr", json_real(sr));
            json_object_set_new(root, "buf",
                json_string(vestigia_dsp::b64encode(q.data(), q.size()).c_str()));
        }
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        auto geti = [&](const char* k, int& v) { if ((j = json_object_get(root, k))) v = (int)json_integer_value(j); };
        auto getb = [&](const char* k, bool& v) { if ((j = json_object_get(root, k))) v = json_boolean_value(j); };
        if ((j = json_object_get(root, "seed"))) { seed = (uint32_t)json_integer_value(j); rng.seed(seed); }
        getb("frozen", frozenToggle);
        geti("quality", quality); geti("bufferSizeIdx", bufferSizeIdx);
        geti("retentionIdx", retentionIdx); geti("sedimentAmtIdx", sedimentAmtIdx);
        geti("sedimentSatIdx", sedimentSatIdx); geti("freezeMode", freezeMode);
        geti("monoMode", monoMode); geti("seedMode", seedMode);
        getb("chaosStepped", chaosStepped); getb("chaosBipolar", chaosBipolar);
        getb("feedbackLimiter", feedbackLimiter); getb("softClip", softClip);
        getb("preserveTails", preserveTails);
        getb("bypassClearsMemory", bypassClearsMemory);
        getb("saveMemoryWithPatch", saveMemoryWithPatch);
        getb("temperTiming", temperTiming);
        quality = clamp(quality, 0, 2); bufferSizeIdx = clamp(bufferSizeIdx, 0, 3);
        retentionIdx = clamp(retentionIdx, 0, 3); sedimentAmtIdx = clamp(sedimentAmtIdx, 0, 3);
        sedimentSatIdx = clamp(sedimentSatIdx, 0, 2); freezeMode = clamp(freezeMode, 0, 2);
        monoMode = clamp(monoMode, 0, 3); seedMode = clamp(seedMode, 0, 2);
        sr = 0.f;  // re-alloc to honor bufferSizeIdx
        // restore buffer after the next allocate() has sized the buffers
        pendingBufStr.clear();
        if (saveMemoryWithPatch && (j = json_object_get(root, "buf")))
            pendingBufStr = json_string_value(j);
    }
    // buffer restore is deferred until allocate() has sized the buffers
    std::string pendingBufStr;

    void restorePendingBuffer() {
        if (pendingBufStr.empty()) return;
        std::vector<int16_t> q(bufLen * 2);
        vestigia_dsp::b64decode(pendingBufStr, q.data(), q.size());
        for (int i = 0; i < bufLen; i++) {
            bufL[i] = q[i] / 32767.f;
            bufR[i] = q[bufLen + i] / 32767.f;
        }
        pendingBufStr.clear();
    }

    float macro(int pid, int cvid) {
        float v = params[pid].getValue();
        if (inputs[cvid].isConnected())
            v += inputs[cvid].getVoltage() * 0.1f;
        return clamp(v, 0.f, 1.f);
    }

    int blockIndex(int j) { int m = j % numBlocks; return m < 0 ? m + numBlocks : m; }

    float readBuf(const std::vector<float>& b, double idx) {
        int i0 = (int)idx;
        float t = (float)(idx - i0);
        int im1 = i0 - 1, i1 = i0 + 1, i2 = i0 + 2;
        if (im1 < 0) im1 += bufLen;
        if (i1 >= bufLen) i1 -= bufLen;
        if (i2 >= bufLen) i2 -= bufLen;
        return vestigia_dsp::hermite(b[im1], b[i0], b[i1], b[i2], t);
    }
    float readBufL(double idx) { return readBuf(bufL, idx); }
    float readBufR(double idx) { return readBuf(bufR, idx); }

    void rebuildRegions(float memorySec, int mode, int64_t frame) {
        int horizonBlocks = clamp((int)(memorySec / 0.02f), 4, numBlocks - 1);
        int gapBridge = (mode == 2) ? 6 : (mode == 1 ? 4 : 2);
        int writeBlock = curBlock;
        int oldest = writeBlock - horizonBlocks + 1;
        numRegions = 0;
        int runStart = -1, gap = 0;
        double eSum = 0; float tMax = 0, zSum = 0; int rn = 0;
        float peakE = 0; int peakK = 0;

        auto closeRun = [&](int runEnd) {
            if (runStart < 0 || numRegions >= regionCap()) return;
            int lenBlocks = runEnd - runStart + 1;
            if (lenBlocks < 2) return;
            Region& r = regions[numRegions];
            r.startPos = (double)blockIndex(oldest + runStart) * blockLen;
            r.length = (double)lenBlocks * blockLen;
            r.energy = rn ? (float)(eSum / rn) : 0.f;
            r.transient = tMax;
            r.zcr = rn ? zSum / rn : 0.f;
            r.ageSec = (float)(horizonBlocks - 1 - runEnd) * 0.02f;
            r.peakOffset = (double)(peakK - runStart) * blockLen;
            r.score = clamp(r.energy / 0.2f, 0.f, 1.f);
            numRegions++;
        };
        for (int k = 0; k < horizonBlocks; k++) {
            const Block& b = blocks[blockIndex(oldest + k)];
            bool inHorizon = b.frame >= 0 && (frame - b.frame) <= (int64_t)(memorySec * sr) + blockLen;
            // keep a guard band behind the write head: the newest blocks are
            // being (or were just) overwritten, and reading across that seam
            // clicks, so never let a region reach them
            bool on = b.active && inHorizon && (k < horizonBlocks - 2);
            if (on) {
                if (runStart < 0) { runStart = k; eSum = 0; tMax = 0; zSum = 0; rn = 0; peakE = 0; peakK = k; }
                eSum += b.energy; tMax = std::max(tMax, b.transient); zSum += b.zcr; rn++;
                if (b.energy > peakE) { peakE = b.energy; peakK = k; }
                gap = 0;
            } else if (runStart >= 0) {
                if (++gap > gapBridge) { closeRun(k - gap); runStart = -1; }
            }
        }
        if (runStart >= 0) closeRun(horizonBlocks - 1);
    }

    int findFragment(double startPos, double length, bool create) {
        int nd = numDescriptors();
        int best = -1; double bestD = blockLen * 1.5;
        for (int i = 0; i < nd; i++) {
            if (!frags[i].valid) continue;
            double d = std::fabs(frags[i].startPos - startPos);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0 || !create) return best;
        // allocate a free or the least-recently-used descriptor
        int slot = -1; int64_t oldest = INT64_MAX;
        for (int i = 0; i < nd; i++) {
            if (!frags[i].valid) { slot = i; break; }
            if (frags[i].lastFrame < oldest) { oldest = frags[i].lastFrame; slot = i; }
        }
        frags[slot] = Fragment();
        frags[slot].startPos = startPos; frags[slot].length = length;
        frags[slot].integrity = 1.f; frags[slot].valid = true;
        return slot;
    }

    // integrity: descriptors decay as the write head passes over them;
    // forget scales how fast they fade (its "memory persistence" role)
    void decayIntegrity(int memmode, float forget, bool frozen) {
        if (frozen) return;
        int nd = numDescriptors();
        double wp = writePos;
        float fscale = 0.5f + forget * 1.5f;  // 0.5x..2x the base decay rate
        for (int i = 0; i < nd; i++) {
            Fragment& f = frags[i];
            if (!f.valid) continue;
            double rel = wp - f.startPos;
            if (rel < 0) rel += bufLen;
            if (rel < f.length) {
                float base = (memmode == 0) ? 0.9990f : (memmode == 1 ? 0.99995f : 0.9997f);
                f.integrity *= 1.f - (1.f - base) * fscale;
                if (f.integrity < 0.02f) f.valid = false;
            }
        }
    }

    int selectRegion(int mode, float memorySec, float forget) {
        if (numRegions == 0) return -1;
        float weights[kMaxRegions]; float total = 0.f;
        float inZ = clamp(inZcr, 0.f, 1.f);
        float inE = clamp(refLevel / 0.2f, 0.f, 1.f);
        for (int i = 0; i < numRegions; i++) {
            const Region& r = regions[i];
            float ageNorm = clamp(r.ageSec / std::max(0.05f, memorySec), 0.f, 1.f);
            float ageW = (mode == 2) ? (0.25f + 0.75f * ageNorm) : (1.f - 0.55f * ageNorm);
            float modeW = 1.f;
            if (mode == 0) modeW = 0.15f + r.transient * 3.f + r.score * r.score;  // listen: events
            else if (mode == 1) {                                                  // breathe: similarity
                float eSim = 1.f - std::fabs(r.score - inE);
                float zSim = 1.f - std::fabs(r.zcr - inZ);
                modeW = 0.3f + 0.7f * (0.6f * eSim + 0.4f * zSim);
            }
            float cool = (std::fabs(r.startPos - lastPlayedStart) < 2.0 * blockLen) ? 0.15f : 1.f;
            // descriptor integrity + recall-count penalty
            float integ = 1.f, rc = 0.f;
            int fi = findFragment(r.startPos, r.length, false);
            if (fi >= 0) { integ = 0.3f + 0.7f * frags[fi].integrity; rc = (float)frags[fi].recallCount; }
            // forget strengthens the recall-count penalty: forgotten memories
            // are avoided after fewer replays
            float wear = 1.f / (1.f + (0.15f + forget * 0.7f) * rc);
            float w = (0.1f + r.score) * ageW * modeW * cool * integ * wear * (0.4f + 0.6f * rng.uniform());
            weights[i] = std::max(0.f, w); total += weights[i];
        }
        if (total <= 0.f) return -1;
        float pick = rng.uniform() * total;
        for (int i = 0; i < numRegions; i++) { pick -= weights[i]; if (pick <= 0.f) return i; }
        return numRegions - 1;
    }

    // pitch ratio for a recollection: quantized to musical intervals whose
    // pool widens from consonant (unison + octaves) toward inharmonic as
    // harmony rises; near the top a continuous detune is layered on so the
    // memories drift genuinely out of tune.
    float pitchRatio(float harmony) {
        // semitone pool, ordered roughly consonant -> dissonant
        static const int POOL[16] = {0, 12, -12, 7, 5, 19, 4, 9, 3, 8, 2, 10, 6, 1, 11, -7};
        int allowed = clamp(3 + (int)std::lround(harmony * 13.f), 3, 16);
        int semi = POOL[(int)(rng.uniform() * allowed) % allowed];
        float ratio = std::pow(2.f, semi / 12.f);
        if (harmony > 0.8f) {
            float cents = rng.bipolar() * (harmony - 0.8f) / 0.2f * 55.f;
            ratio *= std::pow(2.f, cents / 1200.f);
        }
        return ratio;
    }

    // return a free head, or one already in its release tail (stealing a head
    // mid-note clicks); nullptr if every active head is still sounding.
    Head* freeHead() {
        int n = activeHeads();
        for (int i = 0; i < n; i++) if (!heads[i].active) return &heads[i];
        Head* best = nullptr; float bestRemain = 1e9f;
        for (int i = 0; i < n; i++) {
            float remain = (float)((heads[i].len - heads[i].phase) / std::max(0.01, (double)heads[i].speed));
            if (remain < bestRemain) { bestRemain = remain; best = &heads[i]; }
        }
        if (best && bestRemain < best->releaseSamp) return best;  // already fading out
        return nullptr;
    }

    bool tryRecall(int mode, float memory01, float memorySec, float ageAmt, float temper, float dirProb, float recall01, float harmony, float forget) {
        int ri = selectRegion(mode, memorySec, forget);
        if (ri < 0) return false;
        Head* h = freeHead();
        if (!h) return false;  // all heads busy: skip rather than steal (clicks)
        Region& r = regions[ri];
        // region selection strategy + mode-dependent padding
        float preRoll = (mode == 0 ? 0.005f : mode == 1 ? 0.015f : 0.030f) * sr;
        float postRoll = (mode == 0 ? 0.020f : mode == 1 ? 0.050f : 0.100f) * sr;
        double base = r.startPos, len = r.length;
        bool subRegion = (recall01 > 0.7f || memory01 < 0.25f) && len > 0.15 * sr;
        if (mode == 0) {
            // event-centered: a short window around the strongest block
            double win = clamp((float)len, 0.05f * sr, 0.4f * sr);
            base = r.startPos + r.peakOffset - win * 0.5;
            len = win;
        } else if (subRegion) {
            double sub = len * rng.range(0.3f, 0.7f);
            base = r.startPos + rng.uniform() * (len - sub);
            len = sub;
        }
        base -= preRoll; len += preRoll + postRoll;
        if (len > bufLen - 4) len = bufLen - 4;
        base = std::fmod(base, (double)bufLen); if (base < 0) base += bufLen;

        h->active = true;
        h->base = base; h->len = len; h->phase = 0.0;
        h->dir = (rng.uniform() < dirProb) ? -1 : 1;
        // pitch quantized to musical intervals; temper adds only a tiny wow
        float ratio = pitchRatio(harmony);
        h->speed = clamp(ratio * (1.f + temper * rng.bipolar() * 0.02f), 0.25f, 4.f);
        h->amp = 1.f;
        float pan = clamp(0.5f + temper * rng.bipolar() * 0.5f, 0.f, 1.f);
        h->panL = std::cos(pan * 0.5f * (float)M_PI) * 1.41421f;
        h->panR = std::sin(pan * 0.5f * (float)M_PI) * 1.41421f;
        h->age = 0.0;
        // fades, clamped so short fragments never get a truncated (clicky) edge
        float fadeBase = (mode == 2) ? 0.030f : 0.012f;
        float outLen = (float)(len / std::max(0.25, (double)h->speed));
        h->attackSamp = std::min(fadeBase * sr, outLen * 0.4f);
        h->releaseSamp = std::min(fadeBase * 1.6f * sr, outLen * 0.4f);
        h->lpState = 0.f; h->holdVal = 0.f; h->holdCnt = 0; h->dropCnt = 0;
        h->dropGain = 1.f; h->wowPhase = rng.uniform(); h->jitter = 0.f;
        // descriptor: bump recall count + wear, use its accumulated degradation
        int fi = findFragment(r.startPos, r.length, true);
        frags[fi].recallCount++;
        frags[fi].degradation = std::min(1.f, frags[fi].degradation + ageAmt * 0.06f);
        h->fragIdx = fi;
        h->degrade = clamp(ageAmt + frags[fi].degradation, 0.f, 1.f);
        lastPlayedStart = r.startPos;
        recallFlash = 1.f;
        eventPulse.trigger(0.003f);
        return true;
    }

    float readHead(Head& h, float temper) {
        float ageAmt = h.degrade;
        double off = h.phase;
        h.jitter += (rng.bipolar() * temper * 6.f - h.jitter) * 0.02f;
        double idx = h.base + (h.dir > 0 ? off : (h.len - off)) + h.jitter;
        idx = std::fmod(idx, (double)bufLen); if (idx < 0) idx += bufLen;
        float hi = 0.5f * (readBufL(idx) + readBufR(idx));  // Hermite (clean)
        // degradation
        float smp = hi;
        if (ageAmt > 0.001f) {
            // interpolation quality only collapses toward nearest-neighbour at
            // high age, so low/mid AGE stays smooth (no stair-step "clicks")
            float nblend = clamp((ageAmt - 0.4f) / 0.6f, 0.f, 1.f);
            if (nblend > 0.f) { int i0 = (int)idx; float nn = 0.5f * (bufL[i0] + bufR[i0]); smp = hi + (nn - hi) * nblend; }
            int holdN = 1 + (int)(ageAmt * ageAmt * 14.f);
            if (holdN > 1) { if (h.holdCnt <= 0) { h.holdVal = smp; h.holdCnt = holdN; } h.holdCnt--; smp = h.holdVal; }
            float bits = 16.f - ageAmt * 12.f;
            float levels = std::pow(2.f, bits);
            smp = std::round(smp * levels) / levels;
            float fc = 800.f * std::pow(22.f, 1.f - ageAmt);
            float a = clamp(fc / sr * 6.2832f, 0.f, 1.f);
            h.lpState += a * (smp - h.lpState); smp = h.lpState;
            // dropouts at high age
            if (h.dropCnt > 0) h.dropCnt--;
            else if (ageAmt > 0.6f && rng.uniform() < (ageAmt - 0.6f) * 0.0008f)
                h.dropCnt = (int)(rng.range(0.003f, 0.02f) * sr);
        }
        // dropouts fade in/out (ramping, not a hard zero) to stay click-free
        h.dropGain += ((h.dropCnt > 0 ? 0.f : 1.f) - h.dropGain) * 0.02f;
        smp *= h.dropGain;
        float env = 1.f;
        if (h.age < h.attackSamp)
            env = 0.5f * (1.f - std::cos((float)M_PI * (float)h.age / h.attackSamp));
        double remainOut = (h.len - h.phase) / std::max(0.01, (double)h.speed);
        if (remainOut < h.releaseSamp)
            env *= 0.5f * (1.f - std::cos((float)M_PI * (float)remainOut / h.releaseSamp));
        // gentle tape wow (age/temper driven), a slow ±<1% pitch waver, not detune
        h.wowPhase += 2.5f / sr; if (h.wowPhase >= 1.f) h.wowPhase -= 1.f;
        float wow = 1.f + std::sin(2.f * (float)M_PI * h.wowPhase) * (ageAmt * 0.4f + temper * 0.3f) * 0.008f;
        h.phase += h.speed * wow; h.age += 1.0;
        if (h.phase >= h.len) h.active = false;
        return smp * env * h.amp;
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != sr) {
            allocate(args.sampleRate);
            restorePendingBuffer();
        }
        if (needReseedOnAdd) { reseed(); needReseedOnAdd = false; }

        float memory01 = macro(MEMORY_PARAM, MEMORY_CV_INPUT);
        float recall = macro(RECALL_PARAM, RECALL_CV_INPUT);
        float ageAmt = macro(AGE_PARAM, AGE_CV_INPUT);
        float smear = macro(SMEAR_PARAM, SMEAR_CV_INPUT);
        float forget = macro(FORGET_PARAM, FORGET_CV_INPUT);
        float temper = macro(TEMPER_PARAM, TEMPER_CV_INPUT);
        float dirProb = macro(DIRECTION_PARAM, DIRECTION_CV_INPUT);
        float harmony = macro(HARMONY_PARAM, HARMONY_CV_INPUT);
        float mix = macro(MIX_PARAM, MIX_CV_INPUT);
        float outLvl = macro(OUTPUT_PARAM, OUTPUT_CV_INPUT) * 2.f;
        int mode = (int)std::round(params[MODE_PARAM].getValue());
        int memmode = (int)std::round(params[MEMMODE_PARAM].getValue());
        int senseSource = (int)std::round(params[SOURCE_PARAM].getValue());
        float fb = params[FB_PARAM].getValue();
        float memorySec = 0.05f * std::pow(bufferSeconds / 0.05f, memory01);

        // freeze
        bool freezeEdge = inputs[FREEZE_INPUT].isConnected() &&
                          freezeGateTrig.process(inputs[FREEZE_INPUT].getVoltage(), 0.1f, 1.f);
        bool gateHigh = inputs[FREEZE_INPUT].getVoltage() >= 1.f;
        if (freezeBtnTrig.process(params[FREEZE_PARAM].getValue())) frozenToggle = !frozenToggle;
        if (freezeEdge && freezeMode != 0) frozenToggle = !frozenToggle;
        bool frozen = (freezeMode == 0) ? (gateHigh || frozenToggle) : frozenToggle;

        if (clearBtnTrig.process(params[CLEAR_PARAM].getValue()) ||
            (inputs[CLEAR_INPUT].isConnected() &&
             clearCvTrig.process(inputs[CLEAR_INPUT].getVoltage(), 0.1f, 1.f)))
            clearMemory();

        bool forceEvent = eventBtnTrig.process(params[EVENT_PARAM].getValue()) ||
            (inputs[EVENT_INPUT].isConnected() &&
             eventCvTrig.process(inputs[EVENT_INPUT].getVoltage(), 0.1f, 1.f));

        float inL = inputs[IN_L_INPUT].getVoltage() * 0.2f;
        float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() * 0.2f : inL;
        float inMono = 0.5f * (inL + inR);

        // input analysis - the engine can listen to the dry input, the
        // wet recollections, or the mix (wet/mix use the previous sample,
        // so "wet" gives a self-triggering feedback in the recall logic)
        float sense = (senseSource == 1) ? senseMixMono : (senseSource == 2) ? senseWetMono : inMono;
        float aMag = std::fabs(sense);
        fastEnv += ((aMag > fastEnv) ? 0.3f : 0.02f) * (aMag - fastEnv);
        slowEnv += ((aMag > slowEnv) ? 0.01f : 0.002f) * (aMag - slowEnv);
        transient = std::max(0.f, fastEnv - slowEnv);
        refLevel += 0.0005f * (fastEnv - refLevel); refLevel = std::max(refLevel, 0.001f);
        float sign = sense >= 0.f ? 1.f : -1.f;
        inZcAccum += (((sign != inPrevSign) ? 1.f : 0.f) - inZcAccum) * 0.001f;
        inPrevSign = sign;
        inZcr = clamp(inZcAccum * 40.f, 0.f, 1.f);

        decayIntegrity(memmode, forget, frozen);

        // write head
        float wL = inL, wR = inR;
        if (!frozen) {
            // dedicated FB trimpot sets the wet-into-record feedback amount
            float fbGain = fb;
            if (feedbackLimiter) fbGain = std::min(fbGain, 1.f);
            float wiL = inL + fbL * fbGain;
            float wiR = inR + fbR * fbGain;
            float ret = retention();
            switch (memmode) {
                case 0: wL = wiL; wR = wiR; break;
                case 1: wL = bufL[writePos] * ret + wiL * (1.f - ret);
                        wR = bufR[writePos] * ret + wiR * (1.f - ret); break;
                default: {
                    float amt = sedimentAmt();
                    float sL = bufL[writePos] + wiL * amt;
                    float sR = bufR[writePos] + wiR * amt;
                    float (*sat)(float) = sedimentSatIdx == 1 ? vestigia_dsp::tapeSat
                                        : sedimentSatIdx == 2 ? vestigia_dsp::fold
                                        : vestigia_dsp::softSat;
                    wL = dcL.process(sat(sL)); wR = dcR.process(sat(sR));
                } break;
            }
            if (!std::isfinite(wL)) wL = 0.f;
            if (!std::isfinite(wR)) wR = 0.f;
            // headroom guard so a hot feedback loop can't run the buffer away
            wL = clamp(wL, -2.5f, 2.5f); wR = clamp(wR, -2.5f, 2.5f);
            bufL[writePos] = wL; bufR[writePos] = wR;
        } else { wL = bufL[writePos]; wR = bufR[writePos]; }

        // activity accumulation
        float wMono = 0.5f * (wL + wR);
        blockAccum += (double)wMono * wMono;
        float ws = wMono >= 0.f ? 1.f : -1.f;
        if (ws != blockPrevSign) blockZC++;
        blockPrevSign = ws;
        blockPeakTrans = std::max(blockPeakTrans, transient);
        blockAccumN++;
        int nowBlock = writePos / blockLen;
        if (nowBlock != curBlock) {
            Block& b = blocks[curBlock];
            float e = blockAccumN ? std::sqrt((float)(blockAccum / blockAccumN)) : 0.f;
            b.energy = e; b.transient = blockPeakTrans;
            b.zcr = blockAccumN ? clamp((float)blockZC / blockAccumN * 40.f, 0.f, 1.f) : 0.f;
            b.frame = args.frame;
            const float openT = 0.0056f, closeT = 0.0025f;
            if (!blockActiveState && e > openT) blockActiveState = true;
            else if (blockActiveState && e < closeT) blockActiveState = false;
            b.active = blockActiveState;
            cellEnergy[curBlock * kCells / numBlocks] = e;
            blockAccum = 0.0; blockAccumN = 0; blockPeakTrans = 0.f; blockZC = 0;
            curBlock = nowBlock;
        }
        writePos++; if (writePos >= bufLen) writePos = 0;

        if (--rebuildCounter <= 0) { rebuildCounter = 512; rebuildRegions(memorySec, mode, args.frame); }

        // recollection events
        float dt = args.sampleTime;
        if (autoCooldown > 0.f) autoCooldown -= dt;
        float envNorm = clamp(fastEnv / (refLevel * 2.5f), 0.f, 1.f);
        float rate = recall * recall * 8.f;
        if (mode == 1) rate *= 0.25f + 1.75f * envNorm;
        else if (mode == 2) rate *= 0.25f + 1.75f * (1.f - envNorm);
        recallAccum += rate * dt;
        bool autoEvent = false;
        // temper can scatter the event clock: each interval's threshold is
        // re-rolled ±(temper·0.7), so gaps stretch and squeeze but the
        // average rate is preserved (context-menu switchable)
        if (recallAccum >= recallThresh) {
            recallAccum -= recallThresh;
            autoEvent = true;
            recallThresh = temperTiming ? clamp(1.f + temper * rng.bipolar() * 0.7f, 0.25f, 1.75f) : 1.f;
        }
        if (recallAccum > 3.f) recallAccum = 3.f;
        if (mode == 0 && transient > 0.04f && autoCooldown <= 0.f && recall > 0.02f) autoEvent = true;
        if (autoEvent && autoCooldown <= 0.f) {
            if (tryRecall(mode, memory01, memorySec, ageAmt, temper, dirProb, recall, harmony, forget)) autoCooldown = 0.03f;
        }
        if (forceEvent) tryRecall(mode, memory01, memorySec, ageAmt, temper, dirProb, recall, harmony, forget);

        // playback heads
        float recL = 0.f, recR = 0.f; int liveHeads = 0;
        for (auto& h : heads) {
            if (!h.active) continue;
            liveHeads++;
            if (h.fragIdx >= 0 && frags[h.fragIdx].valid) frags[h.fragIdx].lastFrame = args.frame;
            float s = readHead(h, temper);
            recL += s * h.panL; recR += s * h.panR;
        }

        // modulated all-pass diffusion + crossfeed
        smearLfo += 0.15f * dt; if (smearLfo >= 1.f) smearLfo -= 1.f;
        float lfo = std::sin(2.f * (float)M_PI * smearLfo);
        for (int i = 0; i < 4; i++) {
            float m = 0.08f * lfo * std::sin((float)(i + 1) * 1.7f);
            apL[i].g = 0.55f + m; apR[i].g = 0.55f - m;
        }
        float difL = recL, difR = recR;
        for (int i = 0; i < 4; i++) { difL = apL[i].process(difL); difR = apR[i].process(difR); }
        float cf = 0.3f * smear;
        float dL = difL + cf * difR, dR = difR + cf * difL;
        float wetL = recL + (dL - recL) * smear;
        float wetR = recR + (dR - recR) * smear;

        // protected, cross-fed feedback
        float crossfb = 0.25f;
        fbL = dcWL.process(vestigia_dsp::softSat(wetL + crossfb * wetR));
        fbR = dcWR.process(vestigia_dsp::softSat(wetR + crossfb * wetL));
        if (feedbackLimiter) { fbL = clamp(fbL, -1.2f, 1.2f); fbR = clamp(fbR, -1.2f, 1.2f); }
        if (!std::isfinite(fbL)) fbL = 0.f;
        if (!std::isfinite(fbR)) fbR = 0.f;

        // equal-power dry/wet
        float dg = std::cos(mix * 0.5f * (float)M_PI), wg = std::sin(mix * 0.5f * (float)M_PI);
        float mixL = inL * dg + wetL * wg, mixR = inR * dg + wetR * wg;
        // capture what the engine may listen to next sample (±1 domain)
        senseMixMono = 0.5f * (mixL + mixR);
        senseWetMono = 0.5f * (wetL + wetR);
        if (!std::isfinite(senseMixMono)) senseMixMono = 0.f;
        if (!std::isfinite(senseWetMono)) senseWetMono = 0.f;
        float outL = mixL * outLvl, outR = mixR * outLvl;
        switch (monoMode) {
            case 1: outR = outL; break;
            case 2: { float m = outL + outR; outL = outR = m; } break;
            case 3: { float m = 0.70710678f * (outL + outR); outL = outR = m; } break;
            default: break;
        }
        if (softClip) { outL = vestigia_dsp::softSat(outL); outR = vestigia_dsp::softSat(outR); }
        else { outL = clamp(outL, -2.f, 2.f); outR = clamp(outR, -2.f, 2.f); }
        if (!std::isfinite(outL)) outL = 0.f;
        if (!std::isfinite(outR)) outR = 0.f;

        outputs[OUT_L_OUTPUT].setVoltage(outL * 5.f);
        outputs[OUT_R_OUTPUT].setVoltage(outR * 5.f);
        outputs[MEMORY_OUTPUT].setVoltage(clamp(0.5f * (recL + recR), -1.5f, 1.5f) * 5.f);
        outputs[EVENT_OUTPUT].setVoltage(eventPulse.process(dt) ? 10.f : 0.f);
        outputs[ENV_OUTPUT].setVoltage(clamp(fastEnv * 10.f, 0.f, 10.f));

        // chaos out: mean region age blended with head activity
        float meanAge = 0.f;
        for (int i = 0; i < numRegions; i++) meanAge += regions[i].ageSec;
        if (numRegions) meanAge /= numRegions;
        float chaosTarget = (numRegions ? clamp(meanAge / std::max(0.05f, memorySec), 0.f, 1.f) : 0.f)
                            + 0.25f * liveHeads;
        chaosSmooth += 0.001f * (chaosTarget - chaosSmooth);
        if (chaosStepped) { if (recallFlash > 0.99f) chaosHeld = chaosSmooth; }
        else chaosHeld = chaosSmooth;
        float chaosOut = chaosBipolar ? (clamp(chaosHeld, 0.f, 1.f) * 2.f - 1.f) * 5.f
                                      : clamp(chaosHeld, 0.f, 1.f) * 10.f;
        outputs[CHAOS_OUTPUT].setVoltage(chaosOut);

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


struct VestigiaDisplay : TransparentWidget {
    Vestigia* module = nullptr;
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.5f);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10)); nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55)); nvgStrokeWidth(args.vg, 1.f); nvgStroke(args.vg);
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) { TransparentWidget::drawLayer(args, layer); return; }
        nvgScissor(args.vg, 1, 1, box.size.x - 2, box.size.y - 2);
        float W = box.size.x, H = box.size.y; int n = Vestigia::kCells; float cw = W / n;
        for (int i = 0; i < n; i++) {
            float e = clamp(module->cellEnergy[i] * 3.5f, 0.f, 1.f);
            if (e <= 0.01f) continue;
            float h = e * (H - 4.f);
            nvgBeginPath(args.vg);
            nvgRect(args.vg, i * cw, (H - h) * 0.5f, std::max(0.6f, cw - 0.3f), h);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, (int)(60 + 160 * e))); nvgFill(args.vg);
        }
        float hx = module->displayHead * W;
        nvgBeginPath(args.vg); nvgMoveTo(args.vg, hx, 1); nvgLineTo(args.vg, hx, H - 1);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, module->frozenToggle ? 0x50 : 0xc0));
        nvgStrokeWidth(args.vg, 1.f); nvgStroke(args.vg);
        if (module->recallFlash > 0.f) {
            nvgBeginPath(args.vg); nvgRoundedRect(args.vg, 0, 0, W, H, 2.5f);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0x40, 0x40, (int)(60 * clamp(module->recallFlash, 0.f, 1.f))));
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

// @layout:begin vestigia 132.08 128.5 title=2.8
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
// @elem HARMONY_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem MODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem MEMMODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem FREEZE_PARAM TL1105 2.6 param "" 0.0
// @elem EVENT_PARAM TL1105 2.6 param "" 0.0
// @elem CLEAR_PARAM TL1105 2.6 param "" 0.0
// @elem SOURCE_PARAM CKSSThree 2.3 param "" 0.0
// @elem FB_PARAM Trimpot 3.03 param "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.01 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FREEZE_INPUT PJ301MPort 4.01 input "" 0.0
// @elem EVENT_INPUT PJ301MPort 4.01 input "" 0.0
// @elem CLEAR_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MEMORY_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem RECALL_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem AGE_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem SMEAR_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem FORGET_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem TEMPER_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem DIRECTION_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem MIX_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem OUTPUT_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem HARMONY_CV_INPUT PJ301MPort 4.01 input "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem MEMORY_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem EVENT_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem ENV_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem CHAOS_OUTPUT PJ301MPort 4.01 output "" 0.0
// @elem FREEZE_LIGHT SmallLight 1.0 light "" 0.0
// @elem EVENT_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.0 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.0 light "" 0.0
// @elem LABEL_MEMORY label 0.0 label "memory" 0.0 33.00 43.00
// @elem LABEL_RECALL label 0.0 label "recall" 0.0 66.04 43.00
// @elem LABEL_AGE label 0.0 label "age" 0.0 99.08 43.00
// @elem LABEL_SMEAR label 0.0 label "smear" 0.0 33.00 65.00
// @elem LABEL_FORGET label 0.0 label "forget" 0.0 66.04 65.00
// @elem LABEL_TEMPER label 0.0 label "temper" 0.0 99.08 65.00
// @elem LABEL_MODE label 0.0 label "mode" 0.0 10.00 39.50
// @elem LABEL_MEMMODE label 0.0 label "mem" 0.0 10.00 61.50
// @elem LABEL_SOURCE label 0.0 label "src" 0.0 122.00 39.50
// @elem LABEL_FB label 0.0 label "fb" 0.0 122.00 59.50
// @elem LABEL_DIR label 0.0 label "dir" 0.0 16.50 84.00
// @elem LABEL_MIX label 0.0 label "mix" 0.0 38.10 84.00
// @elem LABEL_OUT label 0.0 label "out" 0.0 59.70 84.00
// @elem LABEL_HARM label 0.0 label "harm" 0.0 81.30 84.00
// @elem LABEL_FRZ label 0.0 label "frz" 0.0 99.00 82.00
// @elem LABEL_EVT label 0.0 label "evt" 0.0 111.00 82.00
// @elem LABEL_CLR label 0.0 label "clr" 0.0 123.00 82.00
// @elem LABEL_MEMCV label 0.0 label "mem" 0.0 10.00 101.50
// @elem LABEL_RECCV label 0.0 label "rec" 0.0 24.00 101.50
// @elem LABEL_AGECV label 0.0 label "age" 0.0 38.00 101.50
// @elem LABEL_SMRCV label 0.0 label "smr" 0.0 52.00 101.50
// @elem LABEL_FRGCV label 0.0 label "frg" 0.0 66.00 101.50
// @elem LABEL_TMPCV label 0.0 label "tmp" 0.0 80.00 101.50
// @elem LABEL_FRZIN label 0.0 label "frz" 0.0 99.00 101.50
// @elem LABEL_EVTIN label 0.0 label "evt" 0.0 111.00 101.50
// @elem LABEL_CLRIN label 0.0 label "clr" 0.0 123.00 101.50
// @elem LABEL_IN label 0.0 label "in l" 0.0 10.00 117.00
// @elem LABEL_INR label 0.0 label "in r" 0.0 22.00 117.00
// @elem LABEL_OUTL label 0.0 label "L" 0.0 107.20 117.00
// @elem LABEL_OUTR label 0.0 label "R" 0.0 124.00 117.00
// @elem LABEL_MEMOUT label 0.0 label "mem" 0.0 40.00 117.00
// @elem LABEL_EVTOUT label 0.0 label "evt" 0.0 56.80 117.00
// @elem LABEL_ENVOUT label 0.0 label "env" 0.0 73.60 117.00
// @elem LABEL_CHAOSOUT label 0.0 label "chaos" 0.0 90.40 117.00
// @elem BOX_OUTL panel_box 7.0 box "" 0.0 107.20 111.00
// @elem BOX_OUTR panel_box 7.0 box "" 0.0 124.00 111.00
// @elem BOX_MEMOUT panel_box 7.0 box "" 0.0 40.00 111.00
// @elem BOX_EVTOUT panel_box 7.0 box "" 0.0 56.80 111.00
// @elem BOX_ENVOUT panel_box 7.0 box "" 0.0 73.60 111.00
// @elem BOX_CHAOSOUT panel_box 7.0 box "" 0.0 90.40 111.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 66.04 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(121.92f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(121.92f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(33.00f, 32.00f)), module, Vestigia::MEMORY_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(66.04f, 32.00f)), module, Vestigia::RECALL_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(99.08f, 32.00f)), module, Vestigia::AGE_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(33.00f, 54.00f)), module, Vestigia::SMEAR_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(66.04f, 54.00f)), module, Vestigia::FORGET_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(99.08f, 54.00f)), module, Vestigia::TEMPER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(11.00f, 75.00f)), module, Vestigia::DIRECTION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(32.60f, 75.00f)), module, Vestigia::MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(54.20f, 75.00f)), module, Vestigia::OUTPUT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(75.80f, 75.00f)), module, Vestigia::HARMONY_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(10.00f, 31.00f)), module, Vestigia::MODE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(10.00f, 53.00f)), module, Vestigia::MEMMODE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(122.00f, 31.00f)), module, Vestigia::SOURCE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(122.00f, 53.00f)), module, Vestigia::FB_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(99.00f, 75.00f)), module, Vestigia::FREEZE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(111.00f, 75.00f)), module, Vestigia::EVENT_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(123.00f, 75.00f)), module, Vestigia::CLEAR_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.00f, 75.00f)), module, Vestigia::DIRECTION_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.60f, 75.00f)), module, Vestigia::MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.20f, 75.00f)), module, Vestigia::OUTPUT_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(86.80f, 75.00f)), module, Vestigia::HARMONY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 94.00f)), module, Vestigia::MEMORY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(24.00f, 94.00f)), module, Vestigia::RECALL_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.00f, 94.00f)), module, Vestigia::AGE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.00f, 94.00f)), module, Vestigia::SMEAR_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.00f, 94.00f)), module, Vestigia::FORGET_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(80.00f, 94.00f)), module, Vestigia::TEMPER_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(99.00f, 94.00f)), module, Vestigia::FREEZE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(111.00f, 94.00f)), module, Vestigia::EVENT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(123.00f, 94.00f)), module, Vestigia::CLEAR_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.00f, 109.00f)), module, Vestigia::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.00f, 109.00f)), module, Vestigia::IN_R_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(107.20f, 109.00f)), module, Vestigia::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(124.00f, 109.00f)), module, Vestigia::OUT_R_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.00f, 109.00f)), module, Vestigia::MEMORY_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(56.80f, 109.00f)), module, Vestigia::EVENT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(73.60f, 109.00f)), module, Vestigia::ENV_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(90.40f, 109.00f)), module, Vestigia::CHAOS_OUTPUT));
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(102.50f, 71.00f)), module, Vestigia::FREEZE_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(114.50f, 71.00f)), module, Vestigia::EVENT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(112.20f, 106.00f)), module, Vestigia::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(129.00f, 106.00f)), module, Vestigia::LEVEL_R_LIGHT));
        // @layout:end

        VestigiaDisplay* disp = new VestigiaDisplay;
        disp->module = module;
        disp->box.pos = mm2px(Vec(8.00f, 9.00f));
        disp->box.size = mm2px(Vec(116.08f, 13.00f));
        addChild(disp);
    }

    void appendContextMenu(Menu* menu) override {
        Vestigia* m = getModule<Vestigia>();
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Quality",
            {"Eco (4 descriptors)", "Standard (8)", "High (16)"}, &m->quality));
        menu->addChild(createIndexPtrSubmenuItem("Buffer size",
            {"4 s", "8 s", "16 s", "32 s"}, &m->bufferSizeIdx));
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Remanence retention",
            {"10%", "30%", "50%", "70%"}, &m->retentionIdx));
        menu->addChild(createIndexPtrSubmenuItem("Sediment input amount",
            {"0.5x", "1.0x", "1.5x", "2.0x"}, &m->sedimentAmtIdx));
        menu->addChild(createIndexPtrSubmenuItem("Sediment saturation",
            {"Soft", "Tape", "Fold"}, &m->sedimentSatIdx));
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Temper scatters recall timing", "", &m->temperTiming));
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Freeze behavior",
            {"Gate", "Toggle", "Toggle on rising edge"}, &m->freezeMode));
        menu->addChild(createIndexPtrSubmenuItem("Mono output",
            {"Stereo", "Left only", "Stereo sum", "Equal-power sum"}, &m->monoMode));
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Random seed",
            {"Random on load", "Fixed"}, &m->seedMode));
        menu->addChild(createMenuItem("Reseed now", "", [m]() { m->reseed(); }));
        menu->addChild(createBoolPtrMenuItem("Chaos out: stepped", "", &m->chaosStepped));
        menu->addChild(createBoolPtrMenuItem("Chaos out: bipolar", "", &m->chaosBipolar));
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Safety: feedback limiter", "", &m->feedbackLimiter));
        menu->addChild(createBoolPtrMenuItem("Safety: soft clipping", "", &m->softClip));
        menu->addChild(createBoolPtrMenuItem("Preserve tails on bypass", "", &m->preserveTails));
        menu->addChild(createBoolPtrMenuItem("Bypass clears memory", "", &m->bypassClearsMemory));
        menu->addChild(createBoolPtrMenuItem("Save memory with patch", "", &m->saveMemoryWithPatch));
    }
};

Model* modelVestigia = createModel<Vestigia, VestigiaWidget>("vestigia");
