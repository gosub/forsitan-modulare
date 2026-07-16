// perge.cpp — VCV Rack 2 module
// perge (Latin imperative: "carry on!, keep going!") is a stereo dynamic
// sampler and multi-effect modeled on the AC noises / BunkerNoise CONTINUA
// pedal (whose name is the Italian for the same exhortation), reimplemented
// from scratch from its public documentation. It listens to how you play: input crossing the
// threshold is captured as a sample, and repeats of that sample are spawned
// on a tempo grid, each with its own envelope, random quantized pitch shift
// (octave, then fifth), and per-repeat decay. The wet bus then runs through
// a multi-effect section: lofi (pitch-LFO wobble + darkening) / crush
// (downsampling), reverb / smear (diffusion), and a LP/HP tilt filter.
//
// Repeats engine:
//   - THRS gates which input is analyzed; a capture runs while the envelope
//     stays hot and becomes the newest sample slot (up to 3 slots coexist).
//   - Every tempo tick spawns repeats: slot 0 on every tick; DIMENS brings
//     slots 1 and 2 in as extra layers on their own rhythmic patterns.
//   - SENS sets how much the captured hit level drives repeat loudness.
//   - SUSTAIN sets per-repeat decay; past ~90% the engine FREEZEs (slots
//     are held, capture and buffer writes stop).
//   - GLITCH inserts sudden tempo accelerations (burst subdivisions).
//   - PITCH (inactive at noon): CW random octave-up then fifth-above,
//     CCW the mirror downward, randomized per repeat.
//   - Repeats mode (panel switch): standard, reverse, or tail (swell).
//   - TILT (momentary): random modulation of pitch and the FX amounts.
//
// Controls:
//   Knobs : MIX, TEMPO, PITCH, SUSTAIN (big); GLT/DIM, LOFI/CRSH,
//           RVRB/SMR, LP/HP (bipolar, inactive at noon)
//   Trims : SENS, THRS, ATK, REL, MOD, DCAY, SPRD, INFX
//   Btns  : FREEZE (latch), TILT (momentary)
//   Switch: MODE (std/rev/tail repeats)
//   In    : IN L/R, TEMPO/PITCH/SUST/GLIT/FILT CV, CLOCK, CAPT gate
//           (forced capture), FREEZE gate, TILT gate
//   Out   : OUT L/R
//   Menu  : grain cap (repeats capped to one tempo interval, on by default),
//           clock multiplier (1/4 .. x4)

#include "forsitan.hpp"
#include <memory>
#include <algorithm>

static constexpr float kBufSeconds = 8.f;
static constexpr float kMaxCapSeconds = 2.f;
static constexpr int kNumVoices = 16;
static constexpr int kNumSlots = 3;

struct Perge : Module {
    enum ParamId {
        MIX_PARAM,
        TEMPO_PARAM,
        PITCH_PARAM,
        SUSTAIN_PARAM,
        GLITCH_PARAM,   // bipolar: ccw glitch, cw dimension
        LOFI_PARAM,     // bipolar: ccw lofi, cw crush
        RVRB_PARAM,     // bipolar: ccw reverb, cw smear
        FILTER_PARAM,   // bipolar: ccw lowpass, cw highpass
        SENS_PARAM,
        THRESH_PARAM,
        ATTACK_PARAM,
        RELEASE_PARAM,
        MOD_PARAM,
        DECAY_PARAM,
        SPREAD_PARAM,
        INFX_PARAM,
        FREEZE_PARAM,
        TILT_PARAM,
        REPEATSMODE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT,
        IN_R_INPUT,
        PITCH_CV_INPUT,
        SUSTAIN_CV_INPUT,
        GLITCH_CV_INPUT,
        FILTER_CV_INPUT,
        CLOCK_INPUT,
        FREEZE_GATE_INPUT,
        TILT_GATE_INPUT,
        TEMPO_CV_INPUT,
        CAPTURE_GATE_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_L_OUTPUT,
        OUT_R_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        CAPT_LIGHT,
        FREEZE_LIGHT,
        TILT_LIGHT,
        OUT_L_LIGHT,
        OUT_R_LIGHT,
        TICK_LIGHT,
        LIGHTS_LEN
    };

    enum RepeatsMode { MODE_STD, MODE_REV, MODE_TAIL };

    // ── sampler state ────────────────────────────────────────────────────
    std::vector<float> bufL, bufR;
    int bufLen = 0;
    int writePos = 0;

    // committed captures own a copy of their audio, so they survive the
    // ring buffer lapping their tape (dimension layers and freeze can
    // reach back arbitrarily far). Only the live, still-open capture is
    // played straight from the ring (audio == null, ringStart valid)
    struct SlotAudio {
        std::vector<float> l, r;
    };
    struct Slot {
        std::shared_ptr<SlotAudio> audio;
        int ringStart = 0;
        int len = 0;        // 0 = empty
        float peak = 0.f;
        int age = 0;        // repeats played since capture
    };
    Slot slots[kNumSlots];

    static Slot makeSlot(int ringStart, int len, float peak, int age) {
        Slot s;
        s.ringStart = ringStart; s.len = len; s.peak = peak; s.age = age;
        return s;
    }

    struct Voice {
        bool active = false;
        std::shared_ptr<SlotAudio> audio;  // keeps a dropped slot alive
        int start = 0;      // ring offset when audio == null
        int len = 0;
        double pos = 0.0;   // 0..len in buffer samples
        float rate = 1.f;
        float amp = 0.f;
        float panL = 1.f, panR = 1.f;
        double envPos = 0.0;  // 0..dur in output samples
        double dur = 1.0;
        double atk = 1.0, rel = 1.0;
        bool reverse = false;
        bool tail = false;
    };
    Voice voices[kNumVoices];

    // envelope follower / capture
    float env = 0.f;
    bool capturing = false;
    bool capForced = false;     // capture held open by the capture gate
    int capStart = 0;
    int capLen = 0;
    float capPeak = 0.f;
    dsp::SchmittTrigger capGateTrigger;

    // scheduler
    float tickTimer = 0.f;      // samples until next tick
    bool resyncPending = false; // a capture committed: restart the grid
    long tickCount = 0;
    int burstLeft = 0;
    int burstDiv = 1;
    float clockPeriod = 0.f;    // measured external clock period (samples)
    float clockTimeout = 0.f;

    // tilt random modulation: each press rolls ONE new set of values
    // across the full range of the pitch and FX knobs and holds it
    // while down; tiltEnv fades the swap in and out. A momentary
    // different pedal per press, not a continuous wobble
    float tiltEnv = 0.f;
    bool tiltWasOn = false;
    float tiltPitch = 0.f;      // rolled pitch-knob value (-1..1)
    float tiltLofiK = 0.f;      // rolled lofi/crush knob value (-1..1)
    float tiltRvrbK = 0.f;      // rolled rvrb/smear knob value (-1..1)

    // ── fx state ─────────────────────────────────────────────────────────
    // lofi vibrato delay (per channel)
    std::vector<float> vibBuf[2];
    int vibPos = 0;
    float vibPhase = 0.f;
    float lofiLp[2] = {};
    float lofiHp[2] = {};
    // crush
    float crushPhase = 0.f;
    float crushHold[2] = {};
    // smear allpasses: 4 per channel
    static constexpr int kAp = 4;
    std::vector<float> apBuf[2][kAp];
    int apPos[2][kAp] = {};
    // reverb: 4 combs + 1 allpass per channel
    static constexpr int kComb = 4;
    std::vector<float> combBuf[2][kComb];
    int combPos[2][kComb] = {};
    float combLp[2][kComb] = {};
    std::vector<float> revApBuf[2];
    int revApPos[2] = {};
    // tilt filter (SVF per channel)
    float svfLow[2] = {}, svfBand[2] = {};
    float filtSmooth = 0.f;

    // ── housekeeping ─────────────────────────────────────────────────────
    int repeatsMode = MODE_STD;      // cached from REPEATSMODE_PARAM
    bool grainCap = true;            // cap grain playback to one tempo interval
    enum { RESYNC_OFF, RESYNC_END, RESYNC_START };
    int resyncMode = RESYNC_END;     // captures restart the tempo grid
    int clockMult = 2;               // index into kMults, default x1
    bool freezeLatch = false;
    bool sustainFrozen = false;      // freeze-zone state, with hysteresis
    float curSampleRate = 0.f;
    uint32_t noiseState = 0x9e3779b9u;
    dsp::SchmittTrigger clockTrigger;
    dsp::BooleanTrigger freezeButton;
    float outEnvL = 0.f, outEnvR = 0.f;

    // ── cached params (transcendentals run every kParamDiv samples) ──────
    static constexpr int kParamDiv = 64;
    dsp::ClockDivider paramDivider;
    bool paramsDirty = true;
    float mix = 0.5f, pitchK = 0.f, sustain = 0.5f, filterK = 0.f;
    float sens = 0.5f, thresh = 0.01f, atkK = 0.1f, relK = 0.5f;
    float modK = 0.3f, decayK = 0.5f, spread = 0.3f, infx = 0.5f;
    float glitch = 0.f, dimens = 0.f;
    float lofiBase = 0.f, crushBase = 0.f, rvrbBase = 0.f, smearBase = 0.f;
    float knobT = 0.f;             // tempo knob mapped to samples
    float decayUnfrozen = 0.9f;    // per-repeat gain when not frozen
    float lofiA = 0.01f;           // lofi darkening one-pole coefficient
    float lofiHpA = 0.001f;        // lofi bass-cut one-pole coefficient
    float lofiDrv = 1.f, lofiMk = 1.f;  // lofi soft-clip drive / makeup
    float filtG = 0.f, filtA1 = 1.f;
    bool filtLP = true;
    // constants per sample rate
    float envAtkC = 0.5f, envRelC = 0.01f, revDamp = 0.3f;
    float ledC = 0.002f;
    float tickFlash = 0.f;      // tempo tick LED pulse
    float tickC = 0.999f;

    // stolen-voice declick: the cut voice's last output decays here
    float declickL = 0.f, declickR = 0.f;
    float declickC = 0.99f;

    static constexpr float kMults[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f};

    // displays lo + span * v^2 (the atk/rel millisecond curves)
    struct MsSquaredQuantity : ParamQuantity {
        float lo = 0.f, span = 1.f;
        float getDisplayValue() override {
            float v = getValue();
            return lo + span * v * v;
        }
        void setDisplayValue(float dv) override {
            setValue(std::sqrt(clamp((dv - lo) / span, 0.f, 1.f)));
        }
    };

    Perge() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
        configParam(TEMPO_PARAM, 0.f, 1.f, 0.5f, "Tempo", " ms", 0.05f, 2000.f);
        configParam(PITCH_PARAM, -1.f, 1.f, 0.f, "Pitch (random octave/fifth)", "%", 0.f, 100.f);
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.5f, "Sustain (top = freeze)", "%", 0.f, 100.f);
        configParam(GLITCH_PARAM, -1.f, 1.f, 0.f, "Glitch / Dimension", "%", 0.f, 100.f);
        configParam(LOFI_PARAM, -1.f, 1.f, 0.f, "Lofi / Crush", "%", 0.f, 100.f);
        configParam(RVRB_PARAM, -1.f, 1.f, 0.f, "Reverb / Smear", "%", 0.f, 100.f);
        configParam(FILTER_PARAM, -1.f, 1.f, 0.f, "Lowpass / Highpass", "%", 0.f, 100.f);
        configParam(SENS_PARAM, 0.f, 1.f, 0.5f, "Sensitivity (dynamics response)", "%", 0.f, 100.f);
        configParam(THRESH_PARAM, 0.f, 1.f, 0.6f, "Threshold", " V", 200.f, 0.025f);
        {
            auto* q = configParam<MsSquaredQuantity>(ATTACK_PARAM, 0.f, 1.f, 0.1f, "Repeat attack", " ms");
            q->lo = 1.f; q->span = 799.f;
        }
        {
            auto* q = configParam<MsSquaredQuantity>(RELEASE_PARAM, 0.f, 1.f, 0.5f, "Repeat release", " ms");
            q->lo = 5.f; q->span = 1495.f;
        }
        configParam(MOD_PARAM, 0.f, 1.f, 0.3f, "Lofi/crush modulation", "%", 0.f, 100.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Reverb/smear decay", "%", 0.f, 100.f);
        configParam(SPREAD_PARAM, 0.f, 1.f, 0.3f, "Stereo spread", "%", 0.f, 100.f);
        configParam(INFX_PARAM, 0.f, 1.f, 0.f, "Dry into FX", "%", 0.f, 100.f);
        configButton(FREEZE_PARAM, "Freeze");
        configButton(TILT_PARAM, "Tilt (momentary warble)");
        configSwitch(REPEATSMODE_PARAM, 0.f, 2.f, 0.f, "Repeats mode",
                     {"Standard", "Reverse", "Tail"});
        configInput(IN_L_INPUT, "Left audio");
        configInput(IN_R_INPUT, "Right audio (normalled to left)");
        configInput(TEMPO_CV_INPUT, "Tempo CV");
        configInput(PITCH_CV_INPUT, "Pitch CV");
        configInput(SUSTAIN_CV_INPUT, "Sustain CV");
        configInput(GLITCH_CV_INPUT, "Glitch/dimension CV");
        configInput(FILTER_CV_INPUT, "Filter CV");
        configInput(CLOCK_INPUT, "Tempo clock");
        configInput(CAPTURE_GATE_INPUT, "Capture gate (forces a capture while high)");
        configInput(FREEZE_GATE_INPUT, "Freeze gate");
        configInput(TILT_GATE_INPUT, "Tilt gate");
        configOutput(OUT_L_OUTPUT, "Left audio");
        configOutput(OUT_R_OUTPUT, "Right audio");
        configLight(CAPT_LIGHT, "Capturing");
        configLight(FREEZE_LIGHT, "Frozen");
        configLight(TILT_LIGHT, "Tilt");
        configLight(OUT_L_LIGHT, "Left level");
        configLight(OUT_R_LIGHT, "Right level");
        configLight(TICK_LIGHT, "Tempo tick");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
        paramDivider.setDivision(kParamDiv);
    }

    void onReset() override {
        for (auto& s : slots) s = Slot{};
        for (auto& v : voices) { v.active = false; v.audio.reset(); }
        env = 0.f;
        capturing = false;
        capForced = false;
        capLen = 0;
        tickTimer = 0.f;
        tickCount = 0;
        tickFlash = 0.f;
        burstLeft = 0;
        clockPeriod = 0.f;
        clockTimeout = 0.f;
        tiltEnv = 0.f;
        tiltWasOn = false;
        tiltPitch = tiltLofiK = tiltRvrbK = 0.f;
        freezeLatch = false;
        sustainFrozen = false;
        declickL = declickR = 0.f;
        std::fill(bufL.begin(), bufL.end(), 0.f);
        std::fill(bufR.begin(), bufR.end(), 0.f);
        clearFx();
    }

    void clearFx() {
        for (int c = 0; c < 2; c++) {
            std::fill(vibBuf[c].begin(), vibBuf[c].end(), 0.f);
            lofiLp[c] = lofiHp[c] = crushHold[c] = 0.f;
            for (int i = 0; i < kAp; i++)
                std::fill(apBuf[c][i].begin(), apBuf[c][i].end(), 0.f);
            for (int i = 0; i < kComb; i++) {
                std::fill(combBuf[c][i].begin(), combBuf[c][i].end(), 0.f);
                combLp[c][i] = 0.f;
            }
            std::fill(revApBuf[c].begin(), revApBuf[c].end(), 0.f);
            svfLow[c] = svfBand[c] = 0.f;
        }
    }

    // bipolar knobs are "inactive at noon": a small deadzone keeps an
    // imperfectly centered knob truly inactive (pitch and filter have
    // their own equivalent thresholds)
    static float centerDead(float x) {
        return (std::fabs(x) < 0.04f) ? 0.f : x;
    }

    float noise() {
        uint32_t& s = noiseState;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }
    float urand() { return noise() * 0.5f + 0.5f; }

    void allocate(float sr) {
        curSampleRate = sr;
        bufLen = (int)(kBufSeconds * sr);
        bufL.assign(bufLen, 0.f);
        bufR.assign(bufLen, 0.f);
        vibBuf[0].assign((int)(0.02f * sr) + 4, 0.f);
        vibBuf[1].assign((int)(0.02f * sr) + 4, 0.f);
        // mutually prime-ish diffusion times, right channel slightly detuned
        static const float apMs[kAp] = {7.9f, 13.7f, 21.3f, 31.1f};
        for (int i = 0; i < kAp; i++) {
            apBuf[0][i].assign((int)(apMs[i] * 1e-3f * sr) + 1, 0.f);
            apBuf[1][i].assign((int)(apMs[i] * 1.07f * 1e-3f * sr) + 1, 0.f);
        }
        static const float combMs[kComb] = {29.7f, 37.1f, 41.1f, 43.7f};
        for (int i = 0; i < kComb; i++) {
            combBuf[0][i].assign((int)(combMs[i] * 1e-3f * sr) + 1, 0.f);
            combBuf[1][i].assign((int)(combMs[i] * 1.013f * 1e-3f * sr) + 1, 0.f);
        }
        revApBuf[0].assign((int)(5.0f * 1e-3f * sr) + 1, 0.f);
        revApBuf[1].assign((int)(5.3f * 1e-3f * sr) + 1, 0.f);
        envAtkC = 1.f - std::exp(-1.f / (0.001f * sr));
        envRelC = 1.f - std::exp(-1.f / (0.120f * sr));
        revDamp = 1.f - std::exp(-2.f * (float)M_PI * 3000.f / sr);
        declickC = std::exp(-1.f / (0.0015f * sr));   // ~1.5 ms fade
        ledC = 1.f - std::exp(-1.f / (0.010f * sr));  // ~10 ms level LEDs
        tickC = std::exp(-1.f / (0.040f * sr));       // ~40 ms tick flash
        paramsDirty = true;
        onReset();
    }

    // random quantized pitch shift: |p| < 0.5 fades in the octave,
    // beyond that the octave is certain and a fifth past it fades in
    float randomShift(float p) {
        if (std::fabs(p) < 0.04f) return 0.f;
        float ap = std::fabs(p);
        float semis;
        if (ap < 0.5f)
            semis = (urand() < ap * 2.f) ? 12.f : 0.f;
        else
            semis = 12.f + ((urand() < (ap - 0.5f) * 2.f) ? 7.f : 0.f);
        return (p < 0.f) ? -semis : semis;
    }

    // a finished capture becomes the newest slot (too-short ones dropped)
    void commitCapture(float sr) {
        if (capLen < (int)(0.03f * sr)) return;
        auto audio = std::make_shared<SlotAudio>();
        audio->l.resize(capLen);
        audio->r.resize(capLen);
        int first = std::min(capLen, bufLen - capStart);
        std::copy_n(bufL.begin() + capStart, first, audio->l.begin());
        std::copy_n(bufR.begin() + capStart, first, audio->r.begin());
        if (first < capLen) {
            std::copy_n(bufL.begin(), capLen - first, audio->l.begin() + first);
            std::copy_n(bufR.begin(), capLen - first, audio->r.begin() + first);
        }
        for (int i = kNumSlots - 1; i > 0; i--)
            slots[i] = slots[i - 1];
        slots[0] = makeSlot(capStart, capLen, capPeak, 0);
        slots[0].audio = audio;
        if (resyncMode == RESYNC_END) resyncPending = true;
    }

    // instantaneous stereo contribution of a voice (no state advance)
    // interpolated read from an owned slot buffer (no wrap)
    static float readSlot(const std::vector<float>& b, double idx) {
        int n = (int)b.size();
        int i0 = (int)idx;
        if (i0 < 0) i0 = 0;
        if (i0 > n - 1) i0 = n - 1;
        int i1 = std::min(i0 + 1, n - 1);
        float f = (float)(idx - i0);
        return b[i0] + f * (b[i1] - b[i0]);
    }

    void voiceSample(const Voice& v, float& outL, float& outR) {
        double p = v.reverse ? (v.len - 1 - v.pos) : v.pos;
        float e;
        if (v.envPos < v.atk)
            e = (float)(v.envPos / std::max(v.atk, 1.0));
        else if (v.envPos > v.dur - v.rel)
            e = (float)((v.dur - v.envPos) / std::max(v.rel, 1.0));
        else
            e = 1.f;
        e = clamp(e, 0.f, 1.f);
        float g = v.amp * e * e;   // squared: soft corners
        if (v.audio) {
            outL = readSlot(v.audio->l, p) * g * v.panL;
            outR = readSlot(v.audio->r, p) * g * v.panR;
        } else {
            double idx = v.start + p;
            if (idx >= bufLen) idx -= bufLen;
            if (idx < 0) idx += bufLen;
            outL = readBuf(bufL, idx) * g * v.panL;
            outR = readBuf(bufR, idx) * g * v.panR;
        }
    }

    void spawnVoice(const Slot& slot, float layerGain, float sens, float atkK,
                    float relK, float spread, float pitchK, float decayPerRepeat,
                    float sr, float grainCapSamples) {
        // steal the voice closest to the end of its envelope: the least
        // audible candidate, and the declick accumulator hides the cut
        Voice* v = nullptr;
        double best = 1e18;
        for (auto& c : voices) {
            if (!c.active) { v = &c; break; }
            double left = c.dur - c.envPos;
            if (left < best) { best = left; v = &c; }
        }
        if (!v) return;

        float shift = randomShift(pitchK);
        float rate = std::pow(2.f, shift / 12.f);
        // dynamics: sens 0 = uniform repeats, sens 1 = level tracks the hit
        float dynAmp = clamp(slot.peak / 0.4f, 0.05f, 1.f);
        float amp = crossfade(0.85f, dynAmp, sens);
        amp *= layerGain * std::pow(decayPerRepeat, (float)slot.age);
        if (amp < 0.003f) return;

        float pan = spread * (2.f * urand() - 1.f);
        if (v->active) {
            // stolen mid-note: hand its instantaneous output to the declick
            // accumulator so the cut fades instead of stepping to zero
            float sL, sR;
            voiceSample(*v, sL, sR);
            declickL += sL;
            declickR += sR;
        }
        v->active = true;
        v->audio = slot.audio;
        v->start = slot.ringStart;
        v->len = slot.len;
        // optionally cap the played grain to one tempo interval so repeats
        // stay short and discrete instead of replaying the whole note
        if (grainCapSamples > 0.f) {
            int maxLen = (int)(grainCapSamples * rate);
            if (maxLen >= (int)(0.02f * sr) && v->len > maxLen)
                v->len = maxLen;
        }
        v->pos = 0.0;
        v->rate = rate;
        v->amp = amp;
        v->panL = std::min(1.f, 1.f - pan);
        v->panR = std::min(1.f, 1.f + pan);
        v->reverse = (repeatsMode == MODE_REV);
        v->tail = (repeatsMode == MODE_TAIL);
        v->dur = v->len / (double)rate;
        if (v->tail) {
            v->atk = 0.75 * v->dur;
            v->rel = 0.25 * v->dur;
        } else {
            v->atk = (1.f + 799.f * atkK * atkK) * 1e-3f * sr;   // 1..800 ms
            v->rel = (5.f + 1495.f * relK * relK) * 1e-3f * sr;  // 5..1500 ms
            double ar = v->atk + v->rel;
            if (ar > v->dur) {   // squeeze the envelope into short repeats
                v->atk *= v->dur / ar;
                v->rel *= v->dur / ar;
            }
        }
        v->envPos = 0.0;
    }

    // knob/CV mapping and the transcendentals derived from it; runs every
    // kParamDiv samples (1.3 ms at 48 kHz), plenty for hand and CV rates
    void updateParams(float sr) {
        mix = params[MIX_PARAM].getValue();
        pitchK = clamp(params[PITCH_PARAM].getValue()
                        + inputs[PITCH_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        if (tiltEnv > 1e-3f) pitchK = crossfade(pitchK, tiltPitch, tiltEnv);
        sustain = clamp(params[SUSTAIN_PARAM].getValue()
                        + inputs[SUSTAIN_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        float glitchK = centerDead(clamp(params[GLITCH_PARAM].getValue()
                        + inputs[GLITCH_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f));
        filterK = clamp(params[FILTER_PARAM].getValue()
                        + inputs[FILTER_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        sens = params[SENS_PARAM].getValue();
        // 25 mV .. 5 V at the jack: the top must clear the envelope bed of
        // hot, reverb-heavy material so note attacks stay separable from it
        thresh = 0.005f * std::pow(200.f, params[THRESH_PARAM].getValue());
        atkK = params[ATTACK_PARAM].getValue();
        relK = params[RELEASE_PARAM].getValue();
        modK = params[MOD_PARAM].getValue();
        decayK = params[DECAY_PARAM].getValue();
        spread = params[SPREAD_PARAM].getValue();
        infx = params[INFX_PARAM].getValue();
        repeatsMode = (int)std::round(params[REPEATSMODE_PARAM].getValue());

        glitch = std::max(0.f, -glitchK);
        dimens = std::max(0.f, glitchK);
        float lofiK = centerDead(params[LOFI_PARAM].getValue());
        float rvrbK = centerDead(params[RVRB_PARAM].getValue());
        // tilt fades the rolled knob values in over the panel settings:
        // one bipolar value per knob, so lofi/crush and rvrb/smear stay
        // mutually exclusive, exactly like a hand turning the controls
        if (tiltEnv > 1e-3f) {
            lofiK = crossfade(lofiK, tiltLofiK, tiltEnv);
            rvrbK = crossfade(rvrbK, tiltRvrbK, tiltEnv);
        }
        lofiBase = std::max(0.f, -lofiK);
        crushBase = std::max(0.f, lofiK);
        rvrbBase = std::max(0.f, -rvrbK);
        smearBase = std::max(0.f, rvrbK);

        float tempoK = clamp(params[TEMPO_PARAM].getValue()
                        + inputs[TEMPO_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        knobT = (2.0f * std::pow(0.05f, tempoK)) * sr;  // 2s..100ms
        // sustain -> per-repeat gain (freeze pins it to 1)
        decayUnfrozen = 0.25f + 0.745f * std::pow(clamp(sustain / 0.9f, 0.f, 1.f), 0.4f);

        // the band narrows from both ends as lofi deepens: darken toward
        // 1 kHz and thin the bass toward 450 Hz, old-gramophone style,
        // instead of only muffling. At small amounts the highpass sits
        // at 20 Hz and the first half of the throw stays warm/dark
        float fc = 16000.f * std::pow(1000.f / 16000.f, lofiBase);
        lofiA = 1.f - std::exp(-2.f * (float)M_PI * fc / sr);
        float hfc = 20.f * std::pow(450.f / 20.f, lofiBase);
        lofiHpA = 1.f - std::exp(-2.f * (float)M_PI * hfc / sr);
        // saturation drive, normalized so a nominal 0.5 level stays at
        // unity gain: quieter material squeezes up, peaks fold over
        lofiDrv = 1.f + 3.f * lofiBase;
        float s0 = 0.5f * lofiDrv;
        lofiMk = 0.5f / (s0 * (27.f + s0 * s0) / (27.f + 9.f * s0 * s0));
        float ffc = (filtSmooth < 0.f)
            ? 16000.f * std::pow(160.f / 16000.f, -filtSmooth)
            : 25.f * std::pow(2500.f / 25.f, filtSmooth);
        filtG = std::tan((float)M_PI * clamp(ffc / sr, 1e-4f, 0.45f));
        filtA1 = 1.f / (1.f + filtG * (filtG + 1.2f));
        filtLP = filtSmooth < 0.f;
    }

    float readBuf(const std::vector<float>& b, double idx) {
        int i0 = (int)idx;
        float f = (float)(idx - i0);
        int i1 = i0 + 1;
        if (i0 >= bufLen) i0 -= bufLen;
        if (i1 >= bufLen) i1 -= bufLen;
        return b[i0] + f * (b[i1] - b[i0]);
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (sr != curSampleRate)
            allocate(sr);

        float inL = inputs[IN_L_INPUT].getVoltage() * 0.2f;   // ±5V -> ±1
        float inR = inputs[IN_R_INPUT].isConnected()
                        ? inputs[IN_R_INPUT].getVoltage() * 0.2f : inL;

        // ── params (block rate) ──────────────────────────────────────────
        if (paramDivider.process() || paramsDirty) {
            updateParams(sr);
            paramsDirty = false;
        }
        float lofi = lofiBase;
        float crush = crushBase;
        float rvrb = rvrbBase;
        float smear = smearBase;

        // ── freeze ───────────────────────────────────────────────────────
        if (freezeButton.process(params[FREEZE_PARAM].getValue() > 0.5f))
            freezeLatch = !freezeLatch;
        bool freezeGate = inputs[FREEZE_GATE_INPUT].getVoltage() >= 1.f;
        // hysteresis so CV riding the freeze edge doesn't chatter
        if (sustain > 0.9f) sustainFrozen = true;
        else if (sustain < 0.87f) sustainFrozen = false;
        bool frozen = freezeLatch || freezeGate || sustainFrozen;

        // ── tilt: momentary random re-roll of the knobs ──────────────────
        bool tiltOn = params[TILT_PARAM].getValue() > 0.5f
                      || inputs[TILT_GATE_INPUT].getVoltage() >= 1.f;
        if (tiltOn && !tiltWasOn) {
            tiltPitch = noise();
            tiltLofiK = noise();
            tiltRvrbK = noise();
            paramsDirty = true;
        }
        tiltWasOn = tiltOn;
        tiltEnv += ((tiltOn ? 1.f : 0.f) - tiltEnv) * (30.f / sr);

        // ── envelope follower & capture ──────────────────────────────────
        float lvl = std::max(std::fabs(inL), std::fabs(inR));
        env += (lvl - env) * ((lvl > env) ? envAtkC : envRelC);

        if (!frozen) {
            bufL[writePos] = inL;
            bufR[writePos] = inR;

            // capture gate: rising edge forces a capture, the gate holds it
            // open regardless of level, the falling edge commits it
            bool capGateRose = capGateTrigger.process(
                inputs[CAPTURE_GATE_INPUT].getVoltage(), 0.1f, 1.f);
            bool capGateHigh = capGateTrigger.isHigh();
            if (capGateRose) {
                if (capturing) commitCapture(sr);
                capturing = true;
                capForced = true;
                capStart = writePos;
                capLen = 0;
                capPeak = env;
                if (resyncMode == RESYNC_START) resyncPending = true;
            }
            if (!capturing && env > thresh) {
                capturing = true;
                capForced = false;
                capStart = writePos;
                capLen = 0;
                capPeak = env;
                if (resyncMode == RESYNC_START) resyncPending = true;
            }
            if (capturing) {
                capLen++;
                capPeak = std::max(capPeak, env);
                bool tooLong = capLen >= (int)(kMaxCapSeconds * sr);
                // end at 70% of the arming level: 3 dB of hysteresis keeps
                // wet material committing note-aligned captures (6 dB made
                // dense beds hold the capture open forever); the 120 ms
                // follower release and the 30 ms minimum guard flutter
                bool done = capForced ? !capGateHigh : env < thresh * 0.7f;
                if (done || tooLong) {
                    capturing = false;
                    commitCapture(sr);
                    if (tooLong && (capForced ? capGateHigh : env >= thresh)) {
                        capturing = true;   // keep listening
                        capStart = writePos;
                        capLen = 0;
                        capPeak = env;
                    } else {
                        capForced = false;
                    }
                }
            }
            if (++writePos >= bufLen) writePos = 0;
            // committed slots own their audio, so nothing to drop when
            // the ring laps; only the live capture reads the ring, and
            // it is at most 2 s behind the write head
        }

        // ── tempo / scheduler ────────────────────────────────────────────
        if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
            if (clockTimeout > 0.f)
                clockPeriod = clamp(clockTimeout, 0.02f * sr, 8.f * sr);
            clockTimeout = 0.f;
        } else {
            clockTimeout += 1.f;
            if (clockTimeout > 10.f * sr) clockPeriod = 0.f;   // clock gone
        }
        bool clocked = inputs[CLOCK_INPUT].isConnected() && clockPeriod > 0.f;
        // the multiplier scales the external clock only; the tempo knob
        // always means what it says
        float baseT = clocked ? clockPeriod / kMults[clockMult] : knobT;

        float decayPerRepeat = frozen ? 1.f : decayUnfrozen;

        // pedal-style resync: a capture restarts the grid at its commit
        // (note end: the first repeat lands one full interval after the
        // note) or at its onset (note start: repeats in rhythm with the
        // attack), instead of a random phase of the free-running clock
        if (resyncPending) {
            resyncPending = false;
            tickTimer = baseT;
        }

        tickTimer -= 1.f;
        if (tickTimer <= 0.f) {
            float interval = baseT;
            if (burstLeft > 0) {
                burstLeft--;
                interval = baseT / burstDiv;
            } else if (glitch > 0.f && urand() < glitch * 0.45f) {
                burstDiv = (urand() < 0.5f) ? 2 : 4;
                burstLeft = 1 + (int)(urand() * 3.f);
                interval = baseT / burstDiv;
            }
            tickTimer += interval;
            tickCount++;
            tickFlash = 1.f;

            // live capture counts as the newest material
            Slot live = slots[0];
            if (capturing && capLen >= (int)(0.05f * sr))
                live = makeSlot(capStart, capLen, capPeak,
                                slots[0].len ? slots[0].age : 0);

            float grainCapSamples = grainCap ? interval : 0.f;
            if (live.len)
                spawnVoice(live, 1.f, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            // extra layers, each on its own grid. They decay with the
            // CURRENT train's age, not their own: a slot is many ticks
            // old by the time it is "previous" and decay^ownAge had
            // already faded the layers to nothing. Each new capture
            // re-fires the whole ensemble and sustain fades it together
            float dim1 = clamp(2.f * dimens, 0.f, 1.f);
            float dim2 = clamp(2.f * dimens - 1.f, 0.f, 1.f);
            int trainAge = live.len ? live.age : 0;
            if (dim1 > 0.f && slots[1].len && (tickCount % 2 == 0)) {
                Slot s = slots[1];
                s.age = trainAge;
                spawnVoice(s, dim1, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            }
            if (dim2 > 0.f && slots[2].len && (tickCount % 3 == 0)) {
                Slot s = slots[2];
                s.age = trainAge;
                spawnVoice(s, dim2, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            }
            // frozen slots don't age: unfreezing resumes the decay train
            // from where it was instead of finding decay^age collapsed
            if (!frozen)
                for (auto& s : slots)
                    if (s.len) s.age++;
        }

        // ── render voices ────────────────────────────────────────────────
        float wetL = 0.f, wetR = 0.f;
        for (auto& v : voices) {
            if (!v.active) continue;
            float sL, sR;
            voiceSample(v, sL, sR);
            wetL += sL;
            wetR += sR;
            v.pos += v.rate;
            v.envPos += 1.0;
            if (v.pos >= v.len - 1 || v.envPos >= v.dur) {
                v.active = false;
                v.audio.reset();   // let a replaced slot's audio go
            }
        }
        wetL += declickL;
        wetR += declickR;
        declickL *= declickC;
        declickR *= declickC;

        // ── multi-effect section ─────────────────────────────────────────
        // infx diverts dry signal into the FX bus (0 = classic routing)
        float fxL = wetL + inL * infx;
        float fxR = wetR + inR * infx;

        // lofi: pitch-LFO vibrato through a short delay, then a soft
        // clip into the narrowing band
        if (lofi > 1e-3f) {
            vibPhase += (3.5f + 4.f * modK) / sr;
            if (vibPhase >= 1.f) vibPhase -= 1.f;
            float depth = lofi * (0.3f + 0.7f * modK) * 0.004f * sr;
            float dly = 2.f + depth * (1.f + std::sin(2.f * (float)M_PI * vibPhase));
            int vn = (int)vibBuf[0].size();
            vibBuf[0][vibPos] = fxL;
            vibBuf[1][vibPos] = fxR;
            for (int c = 0; c < 2; c++) {
                float rp = vibPos - dly;
                if (rp < 0.f) rp += vn;
                int i0 = (int)rp;
                float f = rp - i0;
                int i1 = (i0 + 1) % vn;
                float wob = vibBuf[c][i0] + f * (vibBuf[c][i1] - vibBuf[c][i0]);
                float w = clamp(wob * lofiDrv, -3.f, 3.f); // tanh-ish soft clip
                wob = (w * (27.f + w * w) / (27.f + 9.f * w * w)) * lofiMk;
                float& lp = lofiLp[c];
                lp += lofiA * (wob - lp);
                float& hp = lofiHp[c];
                hp += lofiHpA * (lp - hp);
                float& x = (c == 0) ? fxL : fxR;
                x = crossfade(x, lp - hp, std::min(1.f, lofi * 2.f));
            }
            vibPos = (vibPos + 1) % vn;
        }

        // crush: sample-rate reduction with a hint of quantize
        if (crush > 1e-3f) {
            float holdN = 1.f + crush * crush * (50.f + 20.f * modK) * (sr / 48000.f);
            crushPhase += 1.f;
            if (crushPhase >= holdN) {
                crushPhase -= holdN;
                float q = std::pow(2.f, 12.f - 8.f * crush);
                crushHold[0] = std::round(fxL * q) / q;
                crushHold[1] = std::round(fxR * q) / q;
            }
            float cm = std::min(1.f, crush * 3.f);
            fxL = crossfade(fxL, crushHold[0], cm);
            fxR = crossfade(fxR, crushHold[1], cm);
        }

        // smear: series allpass diffusion
        if (smear > 1e-3f) {
            float g = 0.45f + 0.3f * smear + 0.2f * decayK;
            float dL = fxL, dR = fxR;
            for (int c = 0; c < 2; c++) {
                float x = (c == 0) ? dL : dR;
                for (int i = 0; i < kAp; i++) {
                    auto& b = apBuf[c][i];
                    int& p = apPos[c][i];
                    float y = b[p];
                    float w = x + y * g;
                    b[p] = w;
                    x = y - w * g;
                    if (++p >= (int)b.size()) p = 0;
                }
                if (c == 0) dL = x; else dR = x;
            }
            fxL = crossfade(fxL, dL, smear);
            fxR = crossfade(fxR, dR, smear);
        }

        // reverb: dark schroeder, decay sets the tail
        if (rvrb > 1e-3f) {
            float fb = 0.72f + 0.255f * decayK;
            float damp = revDamp;
            float rL = 0.f, rR = 0.f;
            for (int c = 0; c < 2; c++) {
                float x = (c == 0) ? fxL : fxR;
                float acc = 0.f;
                for (int i = 0; i < kComb; i++) {
                    auto& b = combBuf[c][i];
                    int& p = combPos[c][i];
                    float y = b[p];
                    combLp[c][i] += damp * (y - combLp[c][i]);
                    b[p] = x + combLp[c][i] * fb;
                    if (++p >= (int)b.size()) p = 0;
                    acc += y;
                }
                acc *= 0.25f;
                auto& ab = revApBuf[c];
                int& ap = revApPos[c];
                float ay = ab[ap];
                float aw = acc + ay * 0.5f;
                ab[ap] = aw;
                acc = ay - aw * 0.5f;
                if (++ap >= (int)ab.size()) ap = 0;
                if (c == 0) rL = acc; else rR = acc;
            }
            // cross-blend for width
            fxL += rvrb * (rL * 0.85f + rR * 0.15f) * 1.4f;
            fxR += rvrb * (rR * 0.85f + rL * 0.15f) * 1.4f;
        }

        // tilt filter: lowpass left of noon, highpass right
        // (coefficients follow filtSmooth at block rate in updateParams)
        filtSmooth += (filterK - filtSmooth) * (50.f / sr);
        float fk = filtSmooth;
        if (std::fabs(fk) > 0.03f) {
            float gg = filtG;
            float a1 = filtA1;
            for (int c = 0; c < 2; c++) {
                float x = (c == 0) ? fxL : fxR;
                float hi = (x - (gg + 1.2f) * svfBand[c] - svfLow[c]) * a1;
                float bp = gg * hi + svfBand[c];
                float lo = gg * bp + svfLow[c];
                svfBand[c] = gg * hi + bp;
                svfLow[c] = gg * bp + lo;
                float y = filtLP ? lo : hi;
                float m = std::min(1.f, std::fabs(fk) * 4.f);
                if (c == 0) fxL = crossfade(x, y, m);
                else fxR = crossfade(x, y, m);
            }
        }

        // ── mix ──────────────────────────────────────────────────────────
        float dryL = inL * (1.f - infx);
        float dryR = inR * (1.f - infx);
        float dg = (mix <= 0.5f) ? 1.f : 2.f * (1.f - mix);
        float wg = (mix >= 0.5f) ? 1.f : 2.f * mix;
        float outL = dryL * dg + fxL * wg;
        float outR = dryR * dg + fxR * wg;
        if (!std::isfinite(outL)) { outL = 0.f; clearFx(); }
        if (!std::isfinite(outR)) { outR = 0.f; clearFx(); }
        outL = clamp(outL, -2.f, 2.f);
        outR = clamp(outR, -2.f, 2.f);

        // mono-out sums; unconnected right in already normalled from left
        if (outputs[OUT_L_OUTPUT].isConnected() && !outputs[OUT_R_OUTPUT].isConnected())
            outL = (outL + outR) * 0.7f;
        outputs[OUT_L_OUTPUT].setVoltage(5.f * outL);
        outputs[OUT_R_OUTPUT].setVoltage(5.f * outR);

        lights[CAPT_LIGHT].setBrightness(capturing ? 1.f : 0.f);
        tickFlash *= tickC;
        lights[TICK_LIGHT].setBrightness(tickFlash);
        lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);
        lights[TILT_LIGHT].setBrightness(tiltEnv);
        outEnvL += (std::fabs(outL) - outEnvL) * ledC;
        outEnvR += (std::fabs(outR) - outEnvR) * ledC;
        lights[OUT_L_LIGHT].setBrightness(clamp(outEnvL, 0.f, 1.f));
        lights[OUT_R_LIGHT].setBrightness(clamp(outEnvR, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "grainCap", json_boolean(grainCap));
        json_object_set_new(root, "resyncMode", json_integer(resyncMode));
        json_object_set_new(root, "clockMult", json_integer(clockMult));
        // freezeLatch is deliberately not saved: the buffer isn't either, so
        // a patch reloaded frozen would sit silent over empty tape
        return root;
    }

    void dataFromJson(json_t* root) override {
        // migrate the pre-panel-switch context-menu setting onto the param
        if (json_t* j = json_object_get(root, "repeatsMode"))
            params[REPEATSMODE_PARAM].setValue(clamp((int)json_integer_value(j), 0, 2));
        // migrate the removed "alternative routing" menu switch: infx now
        // acts directly, so patches saved with the switch off get infx 0
        if (json_t* j = json_object_get(root, "altRouting")) {
            if (!json_boolean_value(j))
                params[INFX_PARAM].setValue(0.f);
        }
        if (json_t* j = json_object_get(root, "grainCap"))
            grainCap = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "resyncMode"))
            resyncMode = clamp((int)json_integer_value(j), 0, 2);
        if (json_t* j = json_object_get(root, "clockMult"))
            clockMult = clamp((int)json_integer_value(j), 0, 4);
    }
};

constexpr float Perge::kMults[5];

struct PergeWidget : ModuleWidget {
    PergeWidget(Perge* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/perge.svg")));

// @layout:begin perge 101.6 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem MIX_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem TEMPO_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem PITCH_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem SUSTAIN_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem GLITCH_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem LOFI_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem RVRB_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FILTER_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SENS_PARAM Trimpot 2.5 param "" 0.0
// @elem THRESH_PARAM Trimpot 2.5 param "" 0.0
// @elem ATTACK_PARAM Trimpot 2.5 param "" 0.0
// @elem RELEASE_PARAM Trimpot 2.5 param "" 0.0
// @elem MOD_PARAM Trimpot 2.5 param "" 0.0
// @elem DECAY_PARAM Trimpot 2.5 param "" 0.0
// @elem SPREAD_PARAM Trimpot 2.5 param "" 0.0
// @elem INFX_PARAM Trimpot 2.5 param "" 0.0
// @elem TEMPO_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PITCH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SUSTAIN_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem GLITCH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FILTER_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.18 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CAPTURE_GATE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CLOCK_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FREEZE_GATE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TILT_GATE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem OUT_L_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem OUT_R_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem FREEZE_PARAM TL1105 2.0 param "" 0.0
// @elem TILT_PARAM TL1105 2.0 param "" 0.0
// @elem REPEATSMODE_PARAM CKSSThree 2.3 param "" 0.0
// @elem CAPT_LIGHT SmallLight 1.5 light "" 0.0
// @elem FREEZE_LIGHT SmallLight 1.5 light "" 0.0
// @elem TILT_LIGHT SmallLight 1.5 light "" 0.0
// @elem OUT_L_LIGHT SmallLight 1.5 light "" 0.0
// @elem OUT_R_LIGHT SmallLight 1.5 light "" 0.0
// @elem TICK_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_MIX label 0.0 label "mix" 0.0 21.40 33.50
// @elem LABEL_TEMPO label 0.0 label "tempo" 0.0 41.00 33.50
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 60.60 33.50
// @elem LABEL_SUSTAIN label 0.0 label "sustain" 0.0 80.20 33.50
// @elem LABEL_GLITCH label 0.0 label "glt/dim" 0.0 21.40 53.50
// @elem LABEL_LOFI label 0.0 label "lofi/crs" 0.0 41.00 53.50
// @elem LABEL_RVRB label 0.0 label "rvrb/smr" 0.0 60.60 53.50
// @elem LABEL_FILTER label 0.0 label "lp/hp" 0.0 80.20 53.50
// @elem LABEL_SENS label 0.0 label "sens" 0.0 10.90 68.00
// @elem LABEL_THRESH label 0.0 label "thrs" 0.0 22.30 68.00
// @elem LABEL_ATTACK label 0.0 label "atk" 0.0 33.70 68.00
// @elem LABEL_RELEASE label 0.0 label "rel" 0.0 45.10 68.00
// @elem LABEL_MOD label 0.0 label "mod" 0.0 56.50 68.00
// @elem LABEL_DECAY label 0.0 label "dcay" 0.0 67.90 68.00
// @elem LABEL_SPREAD label 0.0 label "sprd" 0.0 79.30 68.00
// @elem LABEL_INFX label 0.0 label "infx" 0.0 90.70 68.00
// @elem LABEL_TEMPOCV label 0.0 label "tempo" 0.0 16.80 87.50
// @elem LABEL_PITCHCV label 0.0 label "pitch" 0.0 33.80 87.50
// @elem LABEL_SUSTCV label 0.0 label "sust" 0.0 50.80 87.50
// @elem LABEL_GLITCV label 0.0 label "glit" 0.0 67.80 87.50
// @elem LABEL_FILTCV label 0.0 label "filt" 0.0 84.80 87.50
// @elem LABEL_INL label 0.0 label "in l" 0.0 12.80 103.50
// @elem LABEL_INR label 0.0 label "in r" 0.0 25.70 103.50
// @elem LABEL_CAPT label 0.0 label "capt" 0.0 38.60 103.50
// @elem LABEL_CLOCK label 0.0 label "clock" 0.0 51.50 103.50
// @elem LABEL_FRZGATE label 0.0 label "frz" 0.0 64.40 103.50
// @elem LABEL_TILTGATE label 0.0 label "tilt" 0.0 77.30 103.50
// @elem LABEL_MODE label 0.0 label "mode" 0.0 90.20 103.50
// @elem LABEL_OUTL label 0.0 label "out l" 0.0 12.80 119.50
// @elem LABEL_OUTR label 0.0 label "out r" 0.0 31.80 119.50
// @elem LABEL_FREEZE label 0.0 label "freeze" 0.0 69.80 119.00
// @elem LABEL_TILT label 0.0 label "tilt" 0.0 88.80 119.00
// @elem BOX_OUTL panel_box 7.0 box "" 0.0 12.80 114.00
// @elem BOX_OUTR panel_box 7.0 box "" 0.0 31.80 114.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 50.80 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(93.98f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(21.40f, 22.00f)), module, Perge::MIX_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(41.00f, 22.00f)), module, Perge::TEMPO_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(60.60f, 22.00f)), module, Perge::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(80.20f, 22.00f)), module, Perge::SUSTAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(21.40f, 45.00f)), module, Perge::GLITCH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(41.00f, 45.00f)), module, Perge::LOFI_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60.60f, 45.00f)), module, Perge::RVRB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80.20f, 45.00f)), module, Perge::FILTER_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(10.90f, 62.00f)), module, Perge::SENS_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(22.30f, 62.00f)), module, Perge::THRESH_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(33.70f, 62.00f)), module, Perge::ATTACK_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(45.10f, 62.00f)), module, Perge::RELEASE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(56.50f, 62.00f)), module, Perge::MOD_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(67.90f, 62.00f)), module, Perge::DECAY_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(79.30f, 62.00f)), module, Perge::SPREAD_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(90.70f, 62.00f)), module, Perge::INFX_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.80f, 80.00f)), module, Perge::TEMPO_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.80f, 80.00f)), module, Perge::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(50.80f, 80.00f)), module, Perge::SUSTAIN_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(67.80f, 80.00f)), module, Perge::GLITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(84.80f, 80.00f)), module, Perge::FILTER_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.80f, 96.00f)), module, Perge::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.70f, 96.00f)), module, Perge::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38.60f, 96.00f)), module, Perge::CAPTURE_GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(51.50f, 96.00f)), module, Perge::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(64.40f, 96.00f)), module, Perge::FREEZE_GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(77.30f, 96.00f)), module, Perge::TILT_GATE_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.80f, 112.00f)), module, Perge::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.80f, 112.00f)), module, Perge::OUT_R_OUTPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(69.80f, 112.00f)), module, Perge::FREEZE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(88.80f, 112.00f)), module, Perge::TILT_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(90.20f, 96.00f)), module, Perge::REPEATSMODE_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(42.30f, 92.30f)), module, Perge::CAPT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(72.70f, 109.10f)), module, Perge::FREEZE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(91.70f, 109.10f)), module, Perge::TILT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(17.80f, 109.00f)), module, Perge::OUT_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(36.80f, 109.00f)), module, Perge::OUT_R_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(55.20f, 92.30f)), module, Perge::TICK_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Perge* m = dynamic_cast<Perge*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Clock multiplier",
            {"1/4", "1/2", "x1", "x2", "x4"}, &m->clockMult));
        menu->addChild(createBoolPtrMenuItem("Cap grain to tempo interval",
            "", &m->grainCap));
        menu->addChild(createIndexPtrSubmenuItem("Resync grid to capture",
            {"Off", "Note end", "Note start"}, &m->resyncMode));
        menu->addChild(createMenuItem("Clear buffer", "", [m]() { m->onReset(); }));
    }
};

Model* modelPerge = createModel<Perge, PergeWidget>("perge");
