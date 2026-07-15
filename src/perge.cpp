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
//   In    : IN L/R, PITCH/SUST/GLIT/FILT CV, CLOCK, FREEZE gate, TILT gate
//   Out   : OUT L/R
//   Menu  : grain cap (repeats capped to one tempo interval, on by default),
//           alternative routing (dry into FX), clock multiplier (1/4 .. x4)

#include "forsitan.hpp"

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
        LIGHTS_LEN
    };

    enum RepeatsMode { MODE_STD, MODE_REV, MODE_TAIL };

    // ── sampler state ────────────────────────────────────────────────────
    std::vector<float> bufL, bufR;
    int bufLen = 0;
    int writePos = 0;

    struct Slot {
        int start = 0;
        int len = 0;        // 0 = empty
        float peak = 0.f;
        int age = 0;        // repeats played since capture
    };
    Slot slots[kNumSlots];

    static Slot makeSlot(int start, int len, float peak, int age) {
        Slot s;
        s.start = start; s.len = len; s.peak = peak; s.age = age;
        return s;
    }

    struct Voice {
        bool active = false;
        int start = 0;
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
    int capStart = 0;
    int capLen = 0;
    float capPeak = 0.f;

    // scheduler
    float tickTimer = 0.f;      // samples until next tick
    long tickCount = 0;
    int burstLeft = 0;
    int burstDiv = 1;
    float clockPeriod = 0.f;    // measured external clock period (samples)
    float clockTimeout = 0.f;

    // tilt random modulation
    float tiltEnv = 0.f;
    float tiltTimer = 0.f;
    float tiltPitch = 0.f;      // semitones
    float tiltLofi = 0.f, tiltCrush = 0.f, tiltRvrb = 0.f, tiltSmear = 0.f;

    // ── fx state ─────────────────────────────────────────────────────────
    // lofi vibrato delay (per channel)
    std::vector<float> vibBuf[2];
    int vibPos = 0;
    float vibPhase = 0.f;
    float lofiLp[2] = {};
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
    int repeatsMode = MODE_STD;      // cached from REPEATSMODE_PARAM each block
    bool altRouting = false;         // context menu
    bool grainCap = true;            // cap grain playback to one tempo interval
    int clockMult = 2;               // index into kMults, default x1
    bool freezeLatch = false;
    float curSampleRate = 0.f;
    uint32_t noiseState = 0x9e3779b9u;
    dsp::SchmittTrigger clockTrigger;
    dsp::BooleanTrigger freezeButton;
    float outEnvL = 0.f, outEnvR = 0.f;

    static constexpr float kMults[5] = {0.25f, 0.5f, 1.f, 2.f, 4.f};

    Perge() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
        configParam(TEMPO_PARAM, 0.f, 1.f, 0.5f, "Tempo");
        configParam(PITCH_PARAM, -1.f, 1.f, 0.f, "Pitch (random octave/fifth)");
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.5f, "Sustain (top = freeze)");
        configParam(GLITCH_PARAM, -1.f, 1.f, 0.f, "Glitch / Dimension");
        configParam(LOFI_PARAM, -1.f, 1.f, 0.f, "Lofi / Crush");
        configParam(RVRB_PARAM, -1.f, 1.f, 0.f, "Reverb / Smear");
        configParam(FILTER_PARAM, -1.f, 1.f, 0.f, "Lowpass / Highpass");
        configParam(SENS_PARAM, 0.f, 1.f, 0.5f, "Sensibility (dynamics response)");
        configParam(THRESH_PARAM, 0.f, 1.f, 0.15f, "Threshold");
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.1f, "Repeat attack");
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.5f, "Repeat release");
        configParam(MOD_PARAM, 0.f, 1.f, 0.3f, "Lofi/crush modulation");
        configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Reverb/smear decay");
        configParam(SPREAD_PARAM, 0.f, 1.f, 0.3f, "Stereo spread");
        configParam(INFX_PARAM, 0.f, 1.f, 0.5f, "Dry into FX (alt routing)");
        configButton(FREEZE_PARAM, "Freeze");
        configButton(TILT_PARAM, "Tilt (momentary warble)");
        configSwitch(REPEATSMODE_PARAM, 0.f, 2.f, 0.f, "Repeats mode",
                     {"Standard", "Reverse", "Tail"});
        configInput(IN_L_INPUT, "Left audio");
        configInput(IN_R_INPUT, "Right audio (normalled to left)");
        configInput(PITCH_CV_INPUT, "Pitch CV");
        configInput(SUSTAIN_CV_INPUT, "Sustain CV");
        configInput(GLITCH_CV_INPUT, "Glitch/dimension CV");
        configInput(FILTER_CV_INPUT, "Filter CV");
        configInput(CLOCK_INPUT, "Tempo clock");
        configInput(FREEZE_GATE_INPUT, "Freeze gate");
        configInput(TILT_GATE_INPUT, "Tilt gate");
        configOutput(OUT_L_OUTPUT, "Left audio");
        configOutput(OUT_R_OUTPUT, "Right audio");
        configLight(CAPT_LIGHT, "Capturing");
        configLight(FREEZE_LIGHT, "Frozen");
        configLight(TILT_LIGHT, "Tilt");
        configLight(OUT_L_LIGHT, "Left level");
        configLight(OUT_R_LIGHT, "Right level");
        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);
    }

    void onReset() override {
        for (auto& s : slots) s = Slot{};
        for (auto& v : voices) v.active = false;
        env = 0.f;
        capturing = false;
        capLen = 0;
        tickTimer = 0.f;
        tickCount = 0;
        burstLeft = 0;
        clockPeriod = 0.f;
        clockTimeout = 0.f;
        tiltEnv = 0.f;
        freezeLatch = false;
        std::fill(bufL.begin(), bufL.end(), 0.f);
        std::fill(bufR.begin(), bufR.end(), 0.f);
        clearFx();
    }

    void clearFx() {
        for (int c = 0; c < 2; c++) {
            std::fill(vibBuf[c].begin(), vibBuf[c].end(), 0.f);
            lofiLp[c] = crushHold[c] = 0.f;
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

    void spawnVoice(const Slot& slot, float layerGain, float sens, float atkK,
                    float relK, float spread, float pitchK, float decayPerRepeat,
                    float sr, float grainCapSamples) {
        // steal the quietest inactive-or-oldest voice
        Voice* v = nullptr;
        double best = 1e18;
        for (auto& c : voices) {
            if (!c.active) { v = &c; break; }
            double left = c.dur - c.envPos;
            if (left < best) { best = left; v = &c; }
        }
        if (!v) return;

        float shift = randomShift(pitchK) + tiltEnv * tiltPitch;
        float rate = std::pow(2.f, shift / 12.f);
        // dynamics: sens 0 = uniform repeats, sens 1 = level tracks the hit
        float dynAmp = clamp(slot.peak / 0.4f, 0.05f, 1.f);
        float amp = crossfade(0.85f, dynAmp, sens);
        amp *= layerGain * std::pow(decayPerRepeat, (float)slot.age);
        if (amp < 0.003f) return;

        float pan = spread * (2.f * urand() - 1.f);
        v->active = true;
        v->start = slot.start;
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

        // ── params ───────────────────────────────────────────────────────
        float mix = params[MIX_PARAM].getValue();
        float pitchK = clamp(params[PITCH_PARAM].getValue()
                        + inputs[PITCH_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        float sustain = clamp(params[SUSTAIN_PARAM].getValue()
                        + inputs[SUSTAIN_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        float glitchK = clamp(params[GLITCH_PARAM].getValue()
                        + inputs[GLITCH_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        float filterK = clamp(params[FILTER_PARAM].getValue()
                        + inputs[FILTER_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
        float sens = params[SENS_PARAM].getValue();
        float thresh = 0.004f * std::pow(75.f, params[THRESH_PARAM].getValue());
        float atkK = params[ATTACK_PARAM].getValue();
        float relK = params[RELEASE_PARAM].getValue();
        float modK = params[MOD_PARAM].getValue();
        float decayK = params[DECAY_PARAM].getValue();
        float spread = params[SPREAD_PARAM].getValue();
        float infx = params[INFX_PARAM].getValue();
        repeatsMode = (int)std::round(params[REPEATSMODE_PARAM].getValue());

        float glitch = std::max(0.f, -glitchK);
        float dimens = std::max(0.f, glitchK);
        float lofiK = params[LOFI_PARAM].getValue();
        float lofi = std::max(0.f, -lofiK);
        float crush = std::max(0.f, lofiK);
        float rvrbK = params[RVRB_PARAM].getValue();
        float rvrb = std::max(0.f, -rvrbK);
        float smear = std::max(0.f, rvrbK);

        // ── freeze ───────────────────────────────────────────────────────
        if (freezeButton.process(params[FREEZE_PARAM].getValue() > 0.5f))
            freezeLatch = !freezeLatch;
        bool freezeGate = inputs[FREEZE_GATE_INPUT].getVoltage() >= 1.f;
        bool frozen = freezeLatch || freezeGate || sustain > 0.9f;

        // ── tilt: momentary random modulation ────────────────────────────
        bool tiltOn = params[TILT_PARAM].getValue() > 0.5f
                      || inputs[TILT_GATE_INPUT].getVoltage() >= 1.f;
        tiltEnv += ((tiltOn ? 1.f : 0.f) - tiltEnv) * (30.f / sr);
        if (tiltOn) {
            tiltTimer -= 1.f;
            if (tiltTimer <= 0.f) {
                tiltTimer = (0.12f + 0.18f * urand()) * sr;
                tiltPitch = 4.f * noise();
                tiltLofi = urand() * 0.35f;
                tiltCrush = urand() * 0.25f;
                tiltRvrb = urand() * 0.4f;
                tiltSmear = urand() * 0.4f;
            }
        }
        if (tiltEnv > 1e-3f) {
            lofi = crossfade(lofi, tiltLofi, tiltEnv);
            crush = crossfade(crush, tiltCrush, tiltEnv);
            rvrb = crossfade(rvrb, tiltRvrb, tiltEnv);
            smear = crossfade(smear, tiltSmear, tiltEnv);
        }

        // ── envelope follower & capture ──────────────────────────────────
        float lvl = std::max(std::fabs(inL), std::fabs(inR));
        float k = (lvl > env) ? 1.f - std::exp(-1.f / (0.001f * sr))
                              : 1.f - std::exp(-1.f / (0.120f * sr));
        env += (lvl - env) * k;

        if (!frozen) {
            bufL[writePos] = inL;
            bufR[writePos] = inR;

            if (!capturing && env > thresh) {
                capturing = true;
                capStart = writePos;
                capLen = 0;
                capPeak = env;
            }
            if (capturing) {
                capLen++;
                capPeak = std::max(capPeak, env);
                bool tooLong = capLen >= (int)(kMaxCapSeconds * sr);
                if (env < thresh * 0.5f || tooLong) {
                    capturing = false;
                    if (capLen >= (int)(0.03f * sr)) {
                        for (int i = kNumSlots - 1; i > 0; i--)
                            slots[i] = slots[i - 1];
                        slots[0] = makeSlot(capStart, capLen, capPeak, 0);
                    }
                    if (tooLong && env >= thresh) {   // keep listening
                        capturing = true;
                        capStart = writePos;
                        capLen = 0;
                        capPeak = env;
                    }
                }
            }
            if (++writePos >= bufLen) writePos = 0;

            // drop slots whose tape is about to be overwritten
            for (auto& s : slots) {
                if (!s.len) continue;
                int dist = writePos - s.start;
                if (dist < 0) dist += bufLen;
                if (dist > bufLen - (int)(0.1f * sr) - s.len) s.len = 0;
            }
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
        float tempoK = params[TEMPO_PARAM].getValue();
        // the multiplier scales the external clock only; the tempo knob
        // always means what it says
        float baseT = clocked ? clockPeriod / kMults[clockMult]
                              : (2.0f * std::pow(0.05f, tempoK)) * sr;  // 2s..100ms

        // sustain -> per-repeat gain (freeze pins it to 1)
        float decayPerRepeat = frozen ? 1.f
            : 0.25f + 0.745f * std::pow(clamp(sustain / 0.9f, 0.f, 1.f), 0.4f);

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

            // live capture counts as the newest material
            Slot live = slots[0];
            if (capturing && capLen >= (int)(0.05f * sr))
                live = makeSlot(capStart, capLen, capPeak,
                                slots[0].len ? slots[0].age : 0);

            float grainCapSamples = grainCap ? interval : 0.f;
            if (live.len)
                spawnVoice(live, 1.f, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            // extra layers, each on its own grid
            float dim1 = clamp(2.f * dimens, 0.f, 1.f);
            float dim2 = clamp(2.f * dimens - 1.f, 0.f, 1.f);
            if (dim1 > 0.f && slots[1].len && (tickCount % 2 == 0))
                spawnVoice(slots[1], dim1, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            if (dim2 > 0.f && slots[2].len && (tickCount % 3 == 0))
                spawnVoice(slots[2], dim2, sens, atkK, relK, spread, pitchK,
                           decayPerRepeat, sr, grainCapSamples);
            // frozen slots don't age: unfreezing resumes the decay train
            // from where it was instead of finding decay^age collapsed
            if (!frozen)
                for (auto& s : slots)
                    if (s.len) s.age++;
        }

        // ── render voices ────────────────────────────────────────────────
        float wetL = 0.f, wetR = 0.f;
        float tiltRate = (tiltEnv > 1e-3f)
            ? std::pow(2.f, tiltEnv * tiltPitch * 0.15f / 12.f) : 1.f;
        for (auto& v : voices) {
            if (!v.active) continue;
            double p = v.reverse ? (v.len - 1 - v.pos) : v.pos;
            double idx = v.start + p;
            if (idx >= bufLen) idx -= bufLen;
            if (idx < 0) idx += bufLen;
            float e;
            if (v.envPos < v.atk)
                e = (float)(v.envPos / std::max(v.atk, 1.0));
            else if (v.envPos > v.dur - v.rel)
                e = (float)((v.dur - v.envPos) / std::max(v.rel, 1.0));
            else
                e = 1.f;
            e = clamp(e, 0.f, 1.f);
            float g = v.amp * e * e;   // squared: soft corners
            wetL += readBuf(bufL, idx) * g * v.panL;
            wetR += readBuf(bufR, idx) * g * v.panR;
            v.pos += v.rate * tiltRate;
            v.envPos += 1.0;
            if (v.pos >= v.len - 1 || v.envPos >= v.dur)
                v.active = false;
        }

        // ── multi-effect section ─────────────────────────────────────────
        float fxL = wetL, fxR = wetR;
        if (altRouting) {
            fxL += inL * infx;
            fxR += inR * infx;
        }

        // lofi: pitch-LFO vibrato through a short delay, then darkening
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
                float& lp = lofiLp[c];
                float fc = 16000.f * std::pow(1000.f / 16000.f, lofi);
                float a = 1.f - std::exp(-2.f * (float)M_PI * fc / sr);
                lp += a * (wob - lp);
                float& x = (c == 0) ? fxL : fxR;
                x = crossfade(x, lp, std::min(1.f, lofi * 2.f));
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
            float damp = 1.f - std::exp(-2.f * (float)M_PI * 3000.f / sr);
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
        filtSmooth += (filterK - filtSmooth) * (50.f / sr);
        float fk = filtSmooth;
        if (std::fabs(fk) > 0.03f) {
            float fc = (fk < 0.f)
                ? 16000.f * std::pow(160.f / 16000.f, -fk)
                : 25.f * std::pow(2500.f / 25.f, fk);
            float gg = std::tan((float)M_PI * clamp(fc / sr, 1e-4f, 0.45f));
            float kq = 1.2f;
            float a1 = 1.f / (1.f + gg * (gg + kq));
            for (int c = 0; c < 2; c++) {
                float x = (c == 0) ? fxL : fxR;
                float hi = (x - (gg + kq) * svfBand[c] - svfLow[c]) * a1;
                float bp = gg * hi + svfBand[c];
                float lo = gg * bp + svfLow[c];
                svfBand[c] = gg * hi + bp;
                svfLow[c] = gg * bp + lo;
                float y = (fk < 0.f) ? lo : hi;
                float m = std::min(1.f, std::fabs(fk) * 4.f);
                if (c == 0) fxL = crossfade(x, y, m);
                else fxR = crossfade(x, y, m);
            }
        }

        // ── mix ──────────────────────────────────────────────────────────
        float dryL = inL, dryR = inR;
        if (altRouting) {
            dryL *= 1.f - infx;
            dryR *= 1.f - infx;
        }
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
        lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);
        lights[TILT_LIGHT].setBrightness(tiltEnv);
        outEnvL += (std::fabs(outL) - outEnvL) * 0.002f;
        outEnvR += (std::fabs(outR) - outEnvR) * 0.002f;
        lights[OUT_L_LIGHT].setBrightness(clamp(outEnvL, 0.f, 1.f));
        lights[OUT_R_LIGHT].setBrightness(clamp(outEnvR, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "altRouting", json_boolean(altRouting));
        json_object_set_new(root, "grainCap", json_boolean(grainCap));
        json_object_set_new(root, "clockMult", json_integer(clockMult));
        // freezeLatch is deliberately not saved: the buffer isn't either, so
        // a patch reloaded frozen would sit silent over empty tape
        return root;
    }

    void dataFromJson(json_t* root) override {
        // migrate the pre-panel-switch context-menu setting onto the param
        if (json_t* j = json_object_get(root, "repeatsMode"))
            params[REPEATSMODE_PARAM].setValue(clamp((int)json_integer_value(j), 0, 2));
        if (json_t* j = json_object_get(root, "altRouting"))
            altRouting = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "grainCap"))
            grainCap = json_boolean_value(j);
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
// @elem PITCH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SUSTAIN_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem GLITCH_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FILTER_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem IN_L_INPUT PJ301MPort 4.18 input "" 0.0
// @elem IN_R_INPUT PJ301MPort 4.18 input "" 0.0
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
// @elem LABEL_PITCHCV label 0.0 label "pitch" 0.0 21.40 87.50
// @elem LABEL_SUSTCV label 0.0 label "sust" 0.0 41.00 87.50
// @elem LABEL_GLITCV label 0.0 label "glit" 0.0 60.60 87.50
// @elem LABEL_FILTCV label 0.0 label "filt" 0.0 80.20 87.50
// @elem LABEL_INL label 0.0 label "in l" 0.0 12.80 103.50
// @elem LABEL_INR label 0.0 label "in r" 0.0 28.30 103.50
// @elem LABEL_CLOCK label 0.0 label "clock" 0.0 43.80 103.50
// @elem LABEL_FRZGATE label 0.0 label "frz" 0.0 59.30 103.50
// @elem LABEL_TILTGATE label 0.0 label "tilt" 0.0 74.80 103.50
// @elem LABEL_MODE label 0.0 label "mode" 0.0 90.30 103.50
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
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(21.40f, 80.00f)), module, Perge::PITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(41.00f, 80.00f)), module, Perge::SUSTAIN_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.60f, 80.00f)), module, Perge::GLITCH_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(80.20f, 80.00f)), module, Perge::FILTER_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.80f, 96.00f)), module, Perge::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.30f, 96.00f)), module, Perge::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.80f, 96.00f)), module, Perge::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(59.30f, 96.00f)), module, Perge::FREEZE_GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(74.80f, 96.00f)), module, Perge::TILT_GATE_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.80f, 112.00f)), module, Perge::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.80f, 112.00f)), module, Perge::OUT_R_OUTPUT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(69.80f, 112.00f)), module, Perge::FREEZE_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(88.80f, 112.00f)), module, Perge::TILT_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(90.30f, 96.00f)), module, Perge::REPEATSMODE_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(16.50f, 92.30f)), module, Perge::CAPT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(72.70f, 109.10f)), module, Perge::FREEZE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(91.70f, 109.10f)), module, Perge::TILT_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(17.80f, 109.00f)), module, Perge::OUT_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(36.80f, 109.00f)), module, Perge::OUT_R_LIGHT));
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
        menu->addChild(createBoolPtrMenuItem("Alternative routing (dry into FX)",
            "", &m->altRouting));
        menu->addChild(createMenuItem("Clear buffer", "", [m]() { m->onReset(); }));
    }
};

Model* modelPerge = createModel<Perge, PergeWidget>("perge");
