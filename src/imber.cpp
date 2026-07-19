// imber.cpp — VCV Rack 2 module
// imber (Latin: "rain shower, downpour") is a generative engine inspired
// by Giorgio Sancristoforo's Haiku, "a generative record" whose sample
// material is synthesized from nothing at every launch. The architecture
// follows a reverse-engineering of the original (see doc/imber.md); all
// sample material is procedurally generated at (re)seed time by a worker
// thread — nothing is loaded from disk, nothing ships.
//
// The model: 8 looping sample players live on a 2D field where position
// is meaning. Five clocks (2n/4n/8n/16n/32n, all drunk-jittered) and
// eight effects (rev/lpf/hpf/bpf/bit/dly/grn/rvb) also live on the field:
// a player sounds only when a clock is within REACH (nearest clock wins,
// its division sets how fast the player's loop window churns), and it
// picks up every effect within REACH, applied in Haiku's fixed order.
// Players that line up couple: vertically aligned pairs sync their read
// positions, horizontally aligned pairs sync their loop windows, and
// diagonal pairs occasionally swap read heads (COUPLE scales all three).
//
// One deliberate deviation from the original's mouse-driven canvas:
// clocks and effects are not placed by hand. Each owns two rolled
// positions (constellation A and B) and the CLK/FX MORPH knobs glide
// between them; REROLL rolls new constellations (stratified, so the
// field never goes dead), NUDGE drunk-steps them. Players are placed
// with per-player X/Y knobs plus two polyphonic CV inputs (channel N →
// player N, ±5 V spans the field; a mono cable moves everyone).
//
// On top: the Skip voice (CD-skip material on the 8th grid) and the
// Micro voice (tiny one-shots on a selectable division, with the
// trigger/change/variability trio collapsed into the INSTAB macro),
// then the master lo-fi chain (tube warmth → bitcrush → noise inject →
// tape wow/flutter → soft limiter at −1 dBFS).
//
// RND rerolls both constellations, randomizes every fader except VOL
// and reseeds the timing; CLR spreads the players back out; RESEED
// regenerates the whole sample bank in the background (the old bank
// keeps playing until the new one lands). The bank seed and the
// constellations are saved with the patch — unless the "Ephemeral"
// context-menu option is on, in which case every load rolls fresh
// ("permanence is an illusion").

#include "forsitan.hpp"
#include "imber/imber_engine.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>

struct Imber : Module {
    enum ParamId {
        X1_PARAM, X2_PARAM, X3_PARAM, X4_PARAM,
        X5_PARAM, X6_PARAM, X7_PARAM, X8_PARAM,
        Y1_PARAM, Y2_PARAM, Y3_PARAM, Y4_PARAM,
        Y5_PARAM, Y6_PARAM, Y7_PARAM, Y8_PARAM,
        CHG1_PARAM, CHG2_PARAM, CHG3_PARAM, CHG4_PARAM,
        CHG5_PARAM, CHG6_PARAM, CHG7_PARAM, CHG8_PARAM,
        SPD1_PARAM, SPD2_PARAM, SPD3_PARAM, SPD4_PARAM,
        SPD5_PARAM, SPD6_PARAM, SPD7_PARAM, SPD8_PARAM,
        CLKMORPH_PARAM, FXMORPH_PARAM, REACH_PARAM, COUPLE_PARAM,
        BPM_PARAM, SPD_PARAM, LPM_PARAM, SKP_PARAM,
        SKS_PARAM, SCV_PARAM, RVL_PARAM, VOL_PARAM,
        PLS_PARAM, INSTAB_PARAM, PLV_PARAM, DIV_PARAM,
        BIT_PARAM, NSE_PARAM, TAP_PARAM, ON_PARAM,
        RRC_PARAM, NDC_PARAM, RRF_PARAM, NDF_PARAM,
        RND_PARAM, RESEED_PARAM, CLR_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        XCV_INPUT, YCV_INPUT,
        CLKMORPH_INPUT, FXMORPH_INPUT, REACH_INPUT, COUPLE_INPUT,
        RRC_INPUT, NDC_INPUT, RRF_INPUT, NDF_INPUT,
        BPM_INPUT, SPD_INPUT, PLS_INPUT, BIT_INPUT,
        NSE_INPUT, TAP_INPUT, RVL_INPUT,
        RND_INPUT, RESEED_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        G2N_OUTPUT, G4N_OUTPUT, G8N_OUTPUT, G16N_OUTPUT, G32N_OUTPUT,
        SKIP_OUTPUT, MICRO_OUTPUT,
        LEFT_OUTPUT, RIGHT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        ENUMS(P1_LIGHT, 3), ENUMS(P2_LIGHT, 3), ENUMS(P3_LIGHT, 3),
        ENUMS(P4_LIGHT, 3), ENUMS(P5_LIGHT, 3), ENUMS(P6_LIGHT, 3),
        ENUMS(P7_LIGHT, 3), ENUMS(P8_LIGHT, 3),
        LEVEL_L_LIGHT, LEVEL_R_LIGHT,
        LIGHTS_LEN
    };

    static constexpr int kPlayers = imber_engine::kPlayers;
    static constexpr int kControlDiv = 64;

    // bank render in flight; the detached worker only touches this
    struct BankJob {
        std::atomic<bool> done;
        std::atomic<bool> abort;
        std::atomic<int> progress;
        std::shared_ptr<imber_gen::Bank> bank;
        BankJob() : done(false), abort(false), progress(0),
                    bank(new imber_gen::Bank()) {}
    };

    imber_engine::Engine eng;
    imber_engine::Params eParams;
    std::shared_ptr<BankJob> bankJob;
    // UI-safe mirror of the render progress (audio thread owns bankJob)
    std::atomic<int> bankProgressPct{-1};
    uint64_t bankSeed = 0;
    bool ephemeral = false;
    // 0 = original (continuous Haiku bed), 1 = sparse (clocked drops)
    int engineMode = 0;
    float sr = 0.f;

    imber_dsp::Rng constRng;   // constellation rolls
    dsp::SchmittTrigger rrcTrig, ndcTrig, rrfTrig, ndfTrig, rndTrig, reseedTrig;
    dsp::BooleanTrigger rrcBtn, ndcBtn, rrfBtn, ndfBtn, rndBtn, reseedBtn, clrBtn;
    dsp::PulseGenerator gatePulse[imber_engine::DIV_COUNT];
    float levelEnvL = 0.f, levelEnvR = 0.f;
    int controlPhase = 0;

    Imber() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        static const float spreadX[kPlayers] =
            {0.15f, 0.383f, 0.617f, 0.85f, 0.15f, 0.383f, 0.617f, 0.85f};
        static const float spreadY[kPlayers] =
            {0.3f, 0.3f, 0.3f, 0.3f, 0.7f, 0.7f, 0.7f, 0.7f};
        for (int i = 0; i < kPlayers; i++) {
            configParam(X1_PARAM + i, 0.f, 1.f, spreadX[i],
                        string::f("Player %d X", i + 1));
            configParam(Y1_PARAM + i, 0.f, 1.f, spreadY[i],
                        string::f("Player %d Y", i + 1));
            configParam(CHG1_PARAM + i, 0.f, 1.f, 0.464f,
                        string::f("Player %d change probability", i + 1));
            configSwitch(SPD1_PARAM + i, 0.f, 2.f, 1.f,
                         string::f("Player %d speed", i + 1),
                         {"1/2", "1", "2"});
        }
        configParam(CLKMORPH_PARAM, 0.f, 1.f, 0.f, "Clock constellation morph");
        configParam(FXMORPH_PARAM, 0.f, 1.f, 0.f, "FX constellation morph");
        configParam(REACH_PARAM, 0.05f, 0.7f, 0.25f, "Reach");
        configParam(COUPLE_PARAM, 0.f, 1.f, 1.f, "Couple");
        // stored as log2(bpm): exponential taper down to glacial tempos,
        // displayed in bpm (base-2 display), CV is 1 V/oct (doubles per volt)
        configParam(BPM_PARAM, std::log2(1.f), std::log2(250.f),
                    std::log2(100.f), "Tempo", " bpm", 2.f, 1.f);
        configParam(SPD_PARAM, 0.1f, 2.f, 1.f, "Speed", "x");
        configParam(LPM_PARAM, 0.1f, 3.f, 2.f, "Max loop length", " s");
        configParam(SKP_PARAM, 0.f, 1.f, 0.15f, "Skip probability", "%", 0.f, 100.f);
        configParam(SKS_PARAM, 0.5f, 2.f, 1.f, "Skip speed", "x");
        configParam(SCV_PARAM, 0.f, 1.f, 0.5f, "Skip level", "%", 0.f, 100.f);
        configParam(RVL_PARAM, 0.f, 1.f, 0.4f, "Reverb length", "%", 0.f, 100.f);
        // like the original's RND: randomize never touches volume or power
        configParam(VOL_PARAM, -38.f, 8.f, 0.f, "Volume", " dB")
            ->randomizeEnabled = false;
        configParam(PLS_PARAM, 0.f, 1.f, 0.3f, "Micro trigger probability", "%", 0.f, 100.f);
        configParam(INSTAB_PARAM, 0.f, 1.f, 0.25f, "Micro instability", "%", 0.f, 100.f);
        configParam(PLV_PARAM, 0.f, 1.f, 0.5f, "Micro level", "%", 0.f, 100.f);
        configSwitch(DIV_PARAM, 0.f, 4.f, 3.f, "Micro division",
                     {"2n", "4n", "8n", "16n", "32n"});
        configParam(BIT_PARAM, 0.f, 1.f, 0.f, "Master bitcrush", "%", 0.f, 100.f);
        configParam(NSE_PARAM, 0.f, 1.f, 0.f, "Noise inject", "%", 0.f, 100.f);
        configParam(TAP_PARAM, 0.f, 1.f, 0.f, "Tape wow/flutter/age", "%", 0.f, 100.f);
        configSwitch(ON_PARAM, 0.f, 1.f, 1.f, "Engine", {"Off", "On"})
            ->randomizeEnabled = false;
        configButton(RRC_PARAM, "Reroll clock constellations");
        configButton(NDC_PARAM, "Nudge clock constellations");
        configButton(RRF_PARAM, "Reroll FX constellations");
        configButton(NDF_PARAM, "Nudge FX constellations");
        configButton(RND_PARAM, "Randomize (constellations + faders)");
        configButton(RESEED_PARAM, "Reseed the sample bank");
        configButton(CLR_PARAM, "Spread players out");
        configInput(XCV_INPUT, "Player X (poly, channel N > player N, +-5 V spans the field)");
        configInput(YCV_INPUT, "Player Y (poly, channel N > player N, +-5 V spans the field)");
        configInput(CLKMORPH_INPUT, "Clock morph CV");
        configInput(FXMORPH_INPUT, "FX morph CV");
        configInput(REACH_INPUT, "Reach CV");
        configInput(COUPLE_INPUT, "Couple CV");
        configInput(RRC_INPUT, "Reroll clocks trigger");
        configInput(NDC_INPUT, "Nudge clocks trigger");
        configInput(RRF_INPUT, "Reroll FX trigger");
        configInput(NDF_INPUT, "Nudge FX trigger");
        configInput(BPM_INPUT, "Tempo CV (1 V/oct: +1 V doubles the tempo)");
        configInput(SPD_INPUT, "Speed CV");
        configInput(PLS_INPUT, "Micro trigger probability CV");
        configInput(BIT_INPUT, "Bitcrush CV");
        configInput(NSE_INPUT, "Noise CV");
        configInput(TAP_INPUT, "Tape CV");
        configInput(RVL_INPUT, "Reverb length CV");
        configInput(RND_INPUT, "Randomize trigger");
        configInput(RESEED_INPUT, "Reseed trigger");
        configOutput(G2N_OUTPUT, "2n gate (drunk-jittered)");
        configOutput(G4N_OUTPUT, "4n gate (drunk-jittered)");
        configOutput(G8N_OUTPUT, "8n gate (drunk-jittered)");
        configOutput(G16N_OUTPUT, "16n gate (drunk-jittered)");
        configOutput(G32N_OUTPUT, "32n gate (drunk-jittered)");
        configOutput(SKIP_OUTPUT, "Skip voice");
        configOutput(MICRO_OUTPUT, "Micro voice");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");

        uint64_t t = (uint64_t)std::chrono::steady_clock::now()
                         .time_since_epoch().count();
        constRng.seed(t);
        eng.init(48000.f, t);
        eng.rerollClocks(constRng);
        eng.rerollFx(constRng);
    }

    ~Imber() override {
        if (bankJob)
            bankJob->abort.store(true);
    }

    void startBank(float sampleRate, uint64_t seed) {
        if (bankJob)
            return;
        bankSeed = seed;
        std::shared_ptr<BankJob> j(new BankJob());
        bankJob = j;
        std::thread([j, sampleRate, seed]() {
            imber_gen::buildBank(*j->bank, seed, sampleRate,
                                 &j->progress, &j->abort);
            j->done.store(true);
        }).detach();
    }

    void spreadPlayers() {
        static const float sx[kPlayers] =
            {0.15f, 0.383f, 0.617f, 0.85f, 0.15f, 0.383f, 0.617f, 0.85f};
        for (int i = 0; i < kPlayers; i++) {
            params[X1_PARAM + i].setValue(sx[i]);
            params[Y1_PARAM + i].setValue(i < 4 ? 0.3f : 0.7f);
        }
    }

    // RND, faithful to the original: both constellations rerolled, every
    // fader except VOL randomized, timing reseeded
    void bigRandom() {
        eng.rerollClocks(constRng);
        eng.rerollFx(constRng);
        eng.timingRng.seed(constRng.next());
        imber_dsp::Rng& r = constRng;
        params[BPM_PARAM].setValue(r.range(std::log2(60.f), std::log2(180.f)));
        params[SPD_PARAM].setValue(r.range(0.1f, 2.f));
        params[LPM_PARAM].setValue(r.range(0.1f, 3.f));
        params[SKP_PARAM].setValue(r.uniform());
        params[SKS_PARAM].setValue(r.range(0.5f, 2.f));
        params[SCV_PARAM].setValue(r.uniform());
        params[PLS_PARAM].setValue(r.uniform());
        params[INSTAB_PARAM].setValue(r.uniform());
        params[PLV_PARAM].setValue(r.uniform());
        params[DIV_PARAM].setValue((float)r.irange(0, 4));
        float u = r.uniform();
        params[BIT_PARAM].setValue(u * u);   // lofi faders biased low
        u = r.uniform();
        params[NSE_PARAM].setValue(u * u);
        u = r.uniform();
        params[TAP_PARAM].setValue(u * u);
        params[RVL_PARAM].setValue(r.uniform());
        params[CLKMORPH_PARAM].setValue(r.uniform());
        params[FXMORPH_PARAM].setValue(r.uniform());
        params[REACH_PARAM].setValue(r.range(0.15f, 0.5f));
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "ephemeral", json_boolean(ephemeral));
        json_object_set_new(rootJ, "engineMode", json_integer(engineMode));
        if (!ephemeral) {
            json_object_set_new(rootJ, "bankSeed",
                                json_integer((json_int_t)bankSeed));
            json_t* cl = json_array();
            for (int i = 0; i < imber_engine::DIV_COUNT; i++) {
                json_array_append_new(cl, json_real(eng.clk[i].ax));
                json_array_append_new(cl, json_real(eng.clk[i].ay));
                json_array_append_new(cl, json_real(eng.clk[i].bx));
                json_array_append_new(cl, json_real(eng.clk[i].by));
            }
            json_object_set_new(rootJ, "clocks", cl);
            json_t* fx = json_array();
            for (int i = 0; i < imber_engine::kFxObjs; i++) {
                json_array_append_new(fx, json_real(eng.fxo[i].ax));
                json_array_append_new(fx, json_real(eng.fxo[i].ay));
                json_array_append_new(fx, json_real(eng.fxo[i].bx));
                json_array_append_new(fx, json_real(eng.fxo[i].by));
            }
            json_object_set_new(rootJ, "fx", fx);
        }
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j;
        if ((j = json_object_get(rootJ, "ephemeral")))
            ephemeral = json_boolean_value(j);
        if ((j = json_object_get(rootJ, "engineMode")))
            engineMode = (int)json_integer_value(j);
        if (ephemeral)
            return;   // roll everything fresh, as saved nothing
        if ((j = json_object_get(rootJ, "bankSeed")))
            bankSeed = (uint64_t)json_integer_value(j);
        json_t* cl = json_object_get(rootJ, "clocks");
        if (cl && json_array_size(cl) >= 4 * imber_engine::DIV_COUNT)
            for (int i = 0; i < imber_engine::DIV_COUNT; i++) {
                eng.clk[i].ax = json_real_value(json_array_get(cl, 4 * i));
                eng.clk[i].ay = json_real_value(json_array_get(cl, 4 * i + 1));
                eng.clk[i].bx = json_real_value(json_array_get(cl, 4 * i + 2));
                eng.clk[i].by = json_real_value(json_array_get(cl, 4 * i + 3));
            }
        json_t* fx = json_object_get(rootJ, "fx");
        if (fx && json_array_size(fx) >= 4 * imber_engine::kFxObjs)
            for (int i = 0; i < imber_engine::kFxObjs; i++) {
                eng.fxo[i].ax = json_real_value(json_array_get(fx, 4 * i));
                eng.fxo[i].ay = json_real_value(json_array_get(fx, 4 * i + 1));
                eng.fxo[i].bx = json_real_value(json_array_get(fx, 4 * i + 2));
                eng.fxo[i].by = json_real_value(json_array_get(fx, 4 * i + 3));
            }
    }

    void updateControls() {
        // buttons + triggers
        if (rrcBtn.process(params[RRC_PARAM].getValue() > 0.5f)
            | rrcTrig.process(inputs[RRC_INPUT].getVoltage(), 0.1f, 1.f))
            eng.rerollClocks(constRng);
        if (ndcBtn.process(params[NDC_PARAM].getValue() > 0.5f)
            | ndcTrig.process(inputs[NDC_INPUT].getVoltage(), 0.1f, 1.f))
            eng.nudgeClocks(constRng);
        if (rrfBtn.process(params[RRF_PARAM].getValue() > 0.5f)
            | rrfTrig.process(inputs[RRF_INPUT].getVoltage(), 0.1f, 1.f))
            eng.rerollFx(constRng);
        if (ndfBtn.process(params[NDF_PARAM].getValue() > 0.5f)
            | ndfTrig.process(inputs[NDF_INPUT].getVoltage(), 0.1f, 1.f))
            eng.nudgeFx(constRng);
        if (rndBtn.process(params[RND_PARAM].getValue() > 0.5f)
            | rndTrig.process(inputs[RND_INPUT].getVoltage(), 0.1f, 1.f))
            bigRandom();
        if (reseedBtn.process(params[RESEED_PARAM].getValue() > 0.5f)
            | reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f))
            startBank(sr, constRng.next());
        if (clrBtn.process(params[CLR_PARAM].getValue() > 0.5f))
            spreadPlayers();

        using imber_dsp::clampf;
        imber_engine::Params& p = eParams;
        int chans = std::max(inputs[XCV_INPUT].getChannels(),
                             inputs[YCV_INPUT].getChannels());
        for (int i = 0; i < kPlayers; i++) {
            // a mono CV cable shifts every player; poly addresses them
            float xcv = inputs[XCV_INPUT].getChannels() > 1
                ? inputs[XCV_INPUT].getVoltage(i) : inputs[XCV_INPUT].getVoltage();
            float ycv = inputs[YCV_INPUT].getChannels() > 1
                ? inputs[YCV_INPUT].getVoltage(i) : inputs[YCV_INPUT].getVoltage();
            (void)chans;
            p.px[i] = clampf(params[X1_PARAM + i].getValue() + xcv / 10.f, 0.f, 1.f);
            p.py[i] = clampf(params[Y1_PARAM + i].getValue() + ycv / 10.f, 0.f, 1.f);
            float chg = params[CHG1_PARAM + i].getValue();
            p.chg[i] = chg * chg * chg;   // exponential: low is genuinely rare
            int s = (int)params[SPD1_PARAM + i].getValue();
            p.speedMult[i] = s == 0 ? 0.5f : (s == 2 ? 2.f : 1.f);
        }
        p.clkMorph = clampf(params[CLKMORPH_PARAM].getValue()
                            + inputs[CLKMORPH_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.fxMorph = clampf(params[FXMORPH_PARAM].getValue()
                           + inputs[FXMORPH_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.reach = clampf(params[REACH_PARAM].getValue()
                         + inputs[REACH_INPUT].getVoltage() / 10.f * 0.65f,
                         0.05f, 0.7f);
        p.couple = clampf(params[COUPLE_PARAM].getValue()
                          + inputs[COUPLE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.bpm = clampf(std::pow(2.f, params[BPM_PARAM].getValue()
                                     + inputs[BPM_INPUT].getVoltage()),
                       1.f, 500.f);
        p.spd = clampf(params[SPD_PARAM].getValue()
                       + inputs[SPD_INPUT].getVoltage() / 5.f, 0.1f, 2.f);
        p.lpm = params[LPM_PARAM].getValue();
        p.skp = params[SKP_PARAM].getValue();
        p.sks = params[SKS_PARAM].getValue();
        p.scv = params[SCV_PARAM].getValue();
        p.pls = clampf(params[PLS_PARAM].getValue()
                       + inputs[PLS_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        float inst = params[INSTAB_PARAM].getValue();
        p.mch = std::pow(inst, 1.5f);
        p.mcv = inst;
        p.mdv = 0.5f * inst * inst;
        p.plv = params[PLV_PARAM].getValue();
        p.microDiv = (int)params[DIV_PARAM].getValue();
        p.bit = clampf(params[BIT_PARAM].getValue()
                       + inputs[BIT_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.nse = clampf(params[NSE_PARAM].getValue()
                       + inputs[NSE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.tap = clampf(params[TAP_PARAM].getValue()
                       + inputs[TAP_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.rvl = clampf(params[RVL_PARAM].getValue()
                       + inputs[RVL_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        p.vol = std::pow(10.f, params[VOL_PARAM].getValue() / 20.f);
        p.on = params[ON_PARAM].getValue() > 0.5f;
        p.sparse = engineMode == 1;

        // panel division change overrides the drifted one
        if (p.microDiv != eng.lastMicroDivParam) {
            eng.lastMicroDivParam = p.microDiv;
            eng.microEffDiv = p.microDiv;
        }

        // player LEDs: color = bound division, brightness = activity
        static const float divColor[5][3] = {
            {1.f, 0.25f, 0.25f},    // 2n
            {1.f, 0.65f, 0.1f},     // 4n
            {1.f, 1.f, 0.2f},       // 8n
            {0.3f, 1.f, 0.3f},      // 16n
            {0.35f, 0.6f, 1.f},     // 32n
        };
        for (int i = 0; i < kPlayers; i++) {
            int d = eng.pl[i].boundClock;
            float a = d >= 0
                ? 0.25f + 0.75f * imber_dsp::clampf(eng.pl[i].actEnv, 0.f, 1.f)
                : 0.f;
            for (int c = 0; c < 3; c++)
                lights[P1_LIGHT + 3 * i + c].setBrightness(
                    d >= 0 ? divColor[d][c] * a : 0.f);
        }
    }

    void process(const ProcessArgs& args) override {
        if (sr != args.sampleRate) {
            sr = args.sampleRate;
            uint64_t keep = bankSeed;
            eng.init(sr, constRng.next());
            bankSeed = keep;
        }
        if (!eng.bank && !bankJob)
            startBank(sr, bankSeed ? bankSeed : constRng.next());
        if (bankJob && bankJob->done.load()) {
            eng.setBank(bankJob->bank);
            bankSeed = bankJob->bank->seed;
            bankJob.reset();
        }
        if (bankJob) {
            int total = imber_gen::kBankLoops + imber_gen::kBankSkips
                        + imber_gen::kBankMicros;
            bankProgressPct.store(100 * bankJob->progress.load() / total);
        }
        else
            bankProgressPct.store(-1);

        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        float outL, outR, skipV, microV;
        bool gates[imber_engine::DIV_COUNT];
        eng.process(eParams, outL, outR, skipV, microV, gates);
        if (!std::isfinite(outL + outR + skipV + microV)) {
            eng.recover();
            outL = outR = skipV = microV = 0.f;
        }

        for (int d = 0; d < imber_engine::DIV_COUNT; d++) {
            if (gates[d])
                gatePulse[d].trigger(2e-3f);
            outputs[G2N_OUTPUT + d].setVoltage(
                gatePulse[d].process(args.sampleTime) ? 10.f : 0.f);
        }
        outputs[SKIP_OUTPUT].setVoltage(skipV * 5.f);
        outputs[MICRO_OUTPUT].setVoltage(microV * 5.f);
        float vL = outL * 5.f;
        float vR = outR * 5.f;
        outputs[LEFT_OUTPUT].setVoltage(vL);
        outputs[RIGHT_OUTPUT].setVoltage(vR);
        levelEnvL += (std::fabs(vL * 0.2f) - levelEnvL) * 0.002f;
        levelEnvR += (std::fabs(vR * 0.2f) - levelEnvR) * 0.002f;
        lights[LEVEL_L_LIGHT].setBrightness(clamp(levelEnvL, 0.f, 1.f));
        lights[LEVEL_R_LIGHT].setBrightness(clamp(levelEnvR, 0.f, 1.f));
    }
};

// ------------------------------------------------------ field display ---

struct ImberDisplay : TransparentWidget {
    Imber* module = nullptr;

    void draw(const DrawArgs& args) override {
        // frame + background (also in the browser preview)
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.f);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(0x55, 0x55, 0x55));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);
        TransparentWidget::draw(args);
    }

    Vec fieldPos(float x, float y) {
        return Vec(3.f + x * (box.size.x - 6.f), 3.f + y * (box.size.y - 6.f));
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1)
            return;
        if (!module) {
            drawDemo(args);
            return;
        }
        nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
        imber_engine::Engine& e = module->eng;
        imber_engine::Params& p = module->eParams;
        float reachPx = p.reach * (box.size.x - 6.f);

        static const NVGcolor divCol[5] = {
            nvgRGB(0xff, 0x40, 0x40), nvgRGB(0xff, 0xa6, 0x1a),
            nvgRGB(0xff, 0xff, 0x33), nvgRGB(0x4d, 0xff, 0x4d),
            nvgRGB(0x59, 0x99, 0xff)};
        static const char* divName[5] = {"2n", "4n", "8n", "16n", "32n"};
        static const char* fxName[8] =
            {"rev", "lpf", "hpf", "bpf", "bit", "dly", "grn", "rvb"};

        nvgFontSize(args.vg, 8.f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        // fx squares + reach circles
        for (int i = 0; i < imber_engine::kFxObjs; i++) {
            Vec c = fieldPos(e.fxo[i].ex, e.fxo[i].ey);
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, c.x, c.y, reachPx);
            nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x14));
            nvgStrokeWidth(args.vg, 0.7f);
            nvgStroke(args.vg);
            nvgBeginPath(args.vg);
            nvgRect(args.vg, c.x - 3.f, c.y - 3.f, 6.f, 6.f);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x90));
            nvgFill(args.vg);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0xb0));
            nvgText(args.vg, c.x, c.y - 7.f, fxName[i], NULL);
        }
        // clock diamonds + reach circles
        for (int i = 0; i < imber_engine::DIV_COUNT; i++) {
            Vec c = fieldPos(e.clk[i].ex, e.clk[i].ey);
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, c.x, c.y, reachPx);
            nvgStrokeColor(args.vg, nvgRGBA(divCol[i].r * 255, divCol[i].g * 255,
                                            divCol[i].b * 255, 0x28));
            nvgStrokeWidth(args.vg, 0.8f);
            nvgStroke(args.vg);
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, c.x, c.y - 4.5f);
            nvgLineTo(args.vg, c.x + 4.5f, c.y);
            nvgLineTo(args.vg, c.x, c.y + 4.5f);
            nvgLineTo(args.vg, c.x - 4.5f, c.y);
            nvgClosePath(args.vg);
            nvgFillColor(args.vg, divCol[i]);
            nvgFill(args.vg);
            nvgFillColor(args.vg, nvgRGBA(0xe5, 0xe5, 0xe5, 0xc0));
            nvgText(args.vg, c.x, c.y - 8.f, divName[i], NULL);
        }
        // connections: player -> bound clock, player -> fx in reach
        for (int i = 0; i < Imber::kPlayers; i++) {
            Vec pc = fieldPos(p.px[i], p.py[i]);
            int bc = e.pl[i].boundClock;
            if (bc >= 0) {
                Vec cc = fieldPos(e.clk[bc].ex, e.clk[bc].ey);
                nvgBeginPath(args.vg);
                nvgMoveTo(args.vg, pc.x, pc.y);
                nvgLineTo(args.vg, cc.x, cc.y);
                nvgStrokeColor(args.vg, nvgRGBA(divCol[bc].r * 255,
                    divCol[bc].g * 255, divCol[bc].b * 255, 0x70));
                nvgStrokeWidth(args.vg, 1.f);
                nvgStroke(args.vg);
            }
            for (int f = 0; f < imber_engine::kFxObjs; f++) {
                if (!(e.fxMask[i] & (1 << f)))
                    continue;
                Vec fc = fieldPos(e.fxo[f].ex, e.fxo[f].ey);
                nvgBeginPath(args.vg);
                nvgMoveTo(args.vg, pc.x, pc.y);
                nvgLineTo(args.vg, fc.x, fc.y);
                nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0x38));
                nvgStrokeWidth(args.vg, 0.6f);
                nvgStroke(args.vg);
            }
        }
        // pair couplings
        static const NVGcolor pairCol[3] = {
            nvgRGB(0xff, 0xd5, 0x00), nvgRGB(0x2a, 0xd5, 0xc8),
            nvgRGB(0xff, 0x7a, 0x2a)};
        for (int k = 0; k < e.pairCount; k++) {
            Vec a = fieldPos(p.px[e.pairs[k].a], p.py[e.pairs[k].a]);
            Vec b = fieldPos(p.px[e.pairs[k].b], p.py[e.pairs[k].b]);
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, a.x, a.y);
            nvgLineTo(args.vg, b.x, b.y);
            NVGcolor c = pairCol[e.pairs[k].kind];
            nvgStrokeColor(args.vg, nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 0x80));
            nvgStrokeWidth(args.vg, 1.2f);
            nvgStroke(args.vg);
        }
        // players
        for (int i = 0; i < Imber::kPlayers; i++) {
            Vec pc = fieldPos(p.px[i], p.py[i]);
            float act = imber_dsp::clampf(e.pl[i].actEnv, 0.f, 1.f);
            bool bound = e.pl[i].boundClock >= 0;
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, pc.x, pc.y, 4.f);
            nvgFillColor(args.vg, bound
                ? nvgRGBA(0xf9, 0xf9, 0xf9, 0x60 + (int)(0x9f * act))
                : nvgRGBA(0x99, 0x99, 0x99, 0x50));
            nvgFill(args.vg);
            char n[4];
            snprintf(n, sizeof(n), "%d", i + 1);
            nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
            nvgText(args.vg, pc.x, pc.y, n, NULL);
        }
        // bank render progress
        int pct = module->bankProgressPct.load();
        if (pct >= 0) {
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 4.f, box.size.y - 6.f,
                    (box.size.x - 8.f) * pct / 100.f, 2.5f);
            nvgFillColor(args.vg, nvgRGB(0xff, 0xd5, 0x00));
            nvgFill(args.vg);
        }
        nvgResetScissor(args.vg);
    }

    void drawDemo(const DrawArgs& args) {
        // static constellation for the module browser
        imber_dsp::Rng r;
        r.seed(7);
        nvgFontSize(args.vg, 8.f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < 5; i++) {
            Vec c = fieldPos(r.uniform(), r.uniform());
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, c.x, c.y - 4.5f);
            nvgLineTo(args.vg, c.x + 4.5f, c.y);
            nvgLineTo(args.vg, c.x, c.y + 4.5f);
            nvgLineTo(args.vg, c.x - 4.5f, c.y);
            nvgClosePath(args.vg);
            nvgFillColor(args.vg, nvgRGBA(0xff, 0xd5, 0x00, 0xa0));
            nvgFill(args.vg);
        }
        for (int i = 0; i < 8; i++) {
            Vec c = fieldPos(r.uniform(), r.uniform());
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, c.x, c.y, 4.f);
            nvgFillColor(args.vg, nvgRGBA(0xf9, 0xf9, 0xf9, 0x90));
            nvgFill(args.vg);
        }
    }
};

struct ImberWidget : ModuleWidget {
    ImberWidget(Imber* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/imber.svg")));

        ImberDisplay* display = new ImberDisplay();
        display->module = module;
        display->box.pos = mm2px(Vec(4.f, 12.f));
        display->box.size = mm2px(Vec(54.f, 48.f));
        addChild(display);

// @layout:begin imber 182.88 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem X1_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X3_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X4_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X5_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X6_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X7_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem X8_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y1_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y3_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y4_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y5_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y6_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y7_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem Y8_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG1_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG2_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG3_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG4_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG5_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG6_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG7_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem CHG8_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPD1_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD2_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD3_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD4_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD5_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD6_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD7_PARAM CKSSThree 2.3 param "" 0.0
// @elem SPD8_PARAM CKSSThree 2.3 param "" 0.0
// @elem CLKMORPH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem FXMORPH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem REACH_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem COUPLE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BPM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SPD_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem LPM_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SKP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SKS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem SCV_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem RVL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem VOL_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PLS_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem INSTAB_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem PLV_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem DIV_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem BIT_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem NSE_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem TAP_PARAM RoundBlackKnob 4.8 param "" 0.0
// @elem ON_PARAM CKSS 2.0 param "" 0.0
// @elem RRC_PARAM TL1105 2.6 param "" 0.0
// @elem NDC_PARAM TL1105 2.6 param "" 0.0
// @elem RRF_PARAM TL1105 2.6 param "" 0.0
// @elem NDF_PARAM TL1105 2.6 param "" 0.0
// @elem RND_PARAM TL1105 2.6 param "" 0.0
// @elem RESEED_PARAM TL1105 2.6 param "" 0.0
// @elem CLR_PARAM TL1105 2.6 param "" 0.0
// @elem XCV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem YCV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem CLKMORPH_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FXMORPH_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REACH_INPUT PJ301MPort 4.18 input "" 0.0
// @elem COUPLE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RRC_INPUT PJ301MPort 4.18 input "" 0.0
// @elem NDC_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RRF_INPUT PJ301MPort 4.18 input "" 0.0
// @elem NDF_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BPM_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPD_INPUT PJ301MPort 4.18 input "" 0.0
// @elem PLS_INPUT PJ301MPort 4.18 input "" 0.0
// @elem BIT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem NSE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TAP_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RVL_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RND_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RESEED_INPUT PJ301MPort 4.18 input "" 0.0
// @elem G2N_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem G4N_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem G8N_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem G16N_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem G32N_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem SKIP_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem MICRO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem P1_LIGHT SmallLight 1.5 light "" 0.0
// @elem P2_LIGHT SmallLight 1.5 light "" 0.0
// @elem P3_LIGHT SmallLight 1.5 light "" 0.0
// @elem P4_LIGHT SmallLight 1.5 light "" 0.0
// @elem P5_LIGHT SmallLight 1.5 light "" 0.0
// @elem P6_LIGHT SmallLight 1.5 light "" 0.0
// @elem P7_LIGHT SmallLight 1.5 light "" 0.0
// @elem P8_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_L_LIGHT SmallLight 1.5 light "" 0.0
// @elem LEVEL_R_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_ROWX label 0.0 label "x" 0.0 63.00 18.60
// @elem LABEL_ROWY label 0.0 label "y" 0.0 63.00 30.60
// @elem LABEL_ROWCHG label 0.0 label "chg" 0.0 63.00 42.60
// @elem LABEL_ROWSPD label 0.0 label "spd" 0.0 63.00 54.60
// @elem LABEL_CLKM label 0.0 label "clk" 0.0 9.00 74.50
// @elem LABEL_FXM label 0.0 label "fx" 0.0 20.50 74.50
// @elem LABEL_RCH label 0.0 label "rch" 0.0 32.00 74.50
// @elem LABEL_CPL label 0.0 label "cpl" 0.0 43.50 74.50
// @elem LABEL_XCV label 0.0 label "xcv" 0.0 55.00 73.50
// @elem LABEL_CVC label 0.0 label "cv" 0.0 9.00 88.50
// @elem LABEL_CVF label 0.0 label "cv" 0.0 20.50 88.50
// @elem LABEL_CVR label 0.0 label "cv" 0.0 32.00 88.50
// @elem LABEL_CVCP label 0.0 label "cv" 0.0 43.50 88.50
// @elem LABEL_YCV label 0.0 label "ycv" 0.0 55.00 88.50
// @elem LABEL_BPM label 0.0 label "bpm" 0.0 69.00 74.50
// @elem LABEL_SPD label 0.0 label "spd" 0.0 80.20 74.50
// @elem LABEL_LPM label 0.0 label "lpm" 0.0 91.40 74.50
// @elem LABEL_SKP label 0.0 label "skp" 0.0 102.60 74.50
// @elem LABEL_SKS label 0.0 label "sks" 0.0 113.80 74.50
// @elem LABEL_SCV label 0.0 label "scv" 0.0 125.00 74.50
// @elem LABEL_RVL label 0.0 label "rvl" 0.0 136.20 74.50
// @elem LABEL_VOL label 0.0 label "vol" 0.0 147.40 74.50
// @elem LABEL_PLS label 0.0 label "pls" 0.0 69.00 89.50
// @elem LABEL_INSTAB label 0.0 label "inst" 0.0 80.20 89.50
// @elem LABEL_PLV label 0.0 label "plv" 0.0 91.40 89.50
// @elem LABEL_DIV label 0.0 label "div" 0.0 102.60 89.50
// @elem LABEL_BIT label 0.0 label "bit" 0.0 113.80 89.50
// @elem LABEL_NSE label 0.0 label "nse" 0.0 125.00 89.50
// @elem LABEL_TAP label 0.0 label "tap" 0.0 136.20 89.50
// @elem LABEL_ON label 0.0 label "on" 0.0 147.40 89.50
// @elem LABEL_RRC label 0.0 label "rr clk" 0.0 11.25 103.00
// @elem LABEL_NDC label 0.0 label "nd clk" 0.0 30.25 103.00
// @elem LABEL_RRF label 0.0 label "rr fx" 0.0 49.25 103.00
// @elem LABEL_NDF label 0.0 label "nd fx" 0.0 68.25 103.00
// @elem LABEL_BPMCV label 0.0 label "bpm" 0.0 83.00 103.00
// @elem LABEL_SPDCV label 0.0 label "spd" 0.0 93.00 103.00
// @elem LABEL_PLSCV label 0.0 label "pls" 0.0 103.00 103.00
// @elem LABEL_BITCV label 0.0 label "bit" 0.0 113.00 103.00
// @elem LABEL_NSECV label 0.0 label "nse" 0.0 123.00 103.00
// @elem LABEL_TAPCV label 0.0 label "tap" 0.0 133.00 103.00
// @elem LABEL_RVLCV label 0.0 label "rvl" 0.0 143.00 103.00
// @elem LABEL_RND label 0.0 label "rnd" 0.0 158.50 73.00
// @elem LABEL_RNDTRG label 0.0 label "trg" 0.0 169.50 73.50
// @elem LABEL_RESEED label 0.0 label "seed" 0.0 158.50 88.00
// @elem LABEL_RSDTRG label 0.0 label "trg" 0.0 169.50 88.50
// @elem LABEL_CLR label 0.0 label "clr" 0.0 158.50 102.50
// @elem LABEL_G2N label 0.0 label "2n" 0.0 11.00 117.50
// @elem LABEL_G4N label 0.0 label "4n" 0.0 26.50 117.50
// @elem LABEL_G8N label 0.0 label "8n" 0.0 42.00 117.50
// @elem LABEL_G16N label 0.0 label "16n" 0.0 57.50 117.50
// @elem LABEL_G32N label 0.0 label "32n" 0.0 73.00 117.50
// @elem LABEL_SKIP label 0.0 label "skip" 0.0 106.50 117.50
// @elem LABEL_MICRO label 0.0 label "micro" 0.0 122.00 117.50
// @elem LABEL_L label 0.0 label "L" 0.0 137.50 117.50
// @elem LABEL_R label 0.0 label "R" 0.0 153.00 117.50
// @elem BOX_G2N panel_box 7.0 box "" 0.0 11.00 112.00
// @elem BOX_G4N panel_box 7.0 box "" 0.0 26.50 112.00
// @elem BOX_G8N panel_box 7.0 box "" 0.0 42.00 112.00
// @elem BOX_G16N panel_box 7.0 box "" 0.0 57.50 112.00
// @elem BOX_G32N panel_box 7.0 box "" 0.0 73.00 112.00
// @elem BOX_SKIP panel_box 7.0 box "" 0.0 106.50 112.00
// @elem BOX_MICRO panel_box 7.0 box "" 0.0 122.00 112.00
// @elem BOX_L panel_box 7.0 box "" 0.0 137.50 112.00
// @elem BOX_R panel_box 7.0 box "" 0.0 153.00 112.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 91.44 122.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(172.72f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(172.72f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(74.00f, 17.50f)), module, Imber::X1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(86.40f, 17.50f)), module, Imber::X2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(98.80f, 17.50f)), module, Imber::X3_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(111.20f, 17.50f)), module, Imber::X4_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(123.60f, 17.50f)), module, Imber::X5_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(136.00f, 17.50f)), module, Imber::X6_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(148.40f, 17.50f)), module, Imber::X7_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(160.80f, 17.50f)), module, Imber::X8_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(74.00f, 29.50f)), module, Imber::Y1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(86.40f, 29.50f)), module, Imber::Y2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(98.80f, 29.50f)), module, Imber::Y3_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(111.20f, 29.50f)), module, Imber::Y4_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(123.60f, 29.50f)), module, Imber::Y5_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(136.00f, 29.50f)), module, Imber::Y6_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(148.40f, 29.50f)), module, Imber::Y7_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(160.80f, 29.50f)), module, Imber::Y8_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(74.00f, 41.50f)), module, Imber::CHG1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(86.40f, 41.50f)), module, Imber::CHG2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(98.80f, 41.50f)), module, Imber::CHG3_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(111.20f, 41.50f)), module, Imber::CHG4_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(123.60f, 41.50f)), module, Imber::CHG5_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(136.00f, 41.50f)), module, Imber::CHG6_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(148.40f, 41.50f)), module, Imber::CHG7_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(160.80f, 41.50f)), module, Imber::CHG8_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(74.00f, 53.50f)), module, Imber::SPD1_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(86.40f, 53.50f)), module, Imber::SPD2_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(98.80f, 53.50f)), module, Imber::SPD3_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(111.20f, 53.50f)), module, Imber::SPD4_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(123.60f, 53.50f)), module, Imber::SPD5_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(136.00f, 53.50f)), module, Imber::SPD6_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(148.40f, 53.50f)), module, Imber::SPD7_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(160.80f, 53.50f)), module, Imber::SPD8_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(9.00f, 66.00f)), module, Imber::CLKMORPH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20.50f, 66.00f)), module, Imber::FXMORPH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(32.00f, 66.00f)), module, Imber::REACH_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(43.50f, 66.00f)), module, Imber::COUPLE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(69.00f, 66.00f)), module, Imber::BPM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80.20f, 66.00f)), module, Imber::SPD_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(91.40f, 66.00f)), module, Imber::LPM_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(102.60f, 66.00f)), module, Imber::SKP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(113.80f, 66.00f)), module, Imber::SKS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(125.00f, 66.00f)), module, Imber::SCV_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(136.20f, 66.00f)), module, Imber::RVL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(147.40f, 66.00f)), module, Imber::VOL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(69.00f, 81.00f)), module, Imber::PLS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80.20f, 81.00f)), module, Imber::INSTAB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(91.40f, 81.00f)), module, Imber::PLV_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(102.60f, 81.00f)), module, Imber::DIV_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(113.80f, 81.00f)), module, Imber::BIT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(125.00f, 81.00f)), module, Imber::NSE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(136.20f, 81.00f)), module, Imber::TAP_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(147.40f, 81.00f)), module, Imber::ON_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(7.00f, 95.50f)), module, Imber::RRC_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(26.00f, 95.50f)), module, Imber::NDC_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(45.00f, 95.50f)), module, Imber::RRF_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(64.00f, 95.50f)), module, Imber::NDF_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(158.50f, 66.00f)), module, Imber::RND_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(158.50f, 81.00f)), module, Imber::RESEED_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(158.50f, 95.50f)), module, Imber::CLR_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 66.00f)), module, Imber::XCV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(55.00f, 81.00f)), module, Imber::YCV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.00f, 81.00f)), module, Imber::CLKMORPH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.50f, 81.00f)), module, Imber::FXMORPH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.00f, 81.00f)), module, Imber::REACH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.50f, 81.00f)), module, Imber::COUPLE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.50f, 95.50f)), module, Imber::RRC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(34.50f, 95.50f)), module, Imber::NDC_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.50f, 95.50f)), module, Imber::RRF_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.50f, 95.50f)), module, Imber::NDF_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(83.00f, 95.50f)), module, Imber::BPM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(93.00f, 95.50f)), module, Imber::SPD_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(103.00f, 95.50f)), module, Imber::PLS_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(113.00f, 95.50f)), module, Imber::BIT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(123.00f, 95.50f)), module, Imber::NSE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(133.00f, 95.50f)), module, Imber::TAP_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(143.00f, 95.50f)), module, Imber::RVL_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(169.50f, 66.00f)), module, Imber::RND_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(169.50f, 81.00f)), module, Imber::RESEED_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(11.00f, 110.00f)), module, Imber::G2N_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(26.50f, 110.00f)), module, Imber::G4N_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(42.00f, 110.00f)), module, Imber::G8N_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(57.50f, 110.00f)), module, Imber::G16N_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(73.00f, 110.00f)), module, Imber::G32N_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(106.50f, 110.00f)), module, Imber::SKIP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(122.00f, 110.00f)), module, Imber::MICRO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(137.50f, 110.00f)), module, Imber::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(153.00f, 110.00f)), module, Imber::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(79.50f, 11.50f)), module, Imber::P1_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(91.90f, 11.50f)), module, Imber::P2_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(104.30f, 11.50f)), module, Imber::P3_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(116.70f, 11.50f)), module, Imber::P4_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(129.10f, 11.50f)), module, Imber::P5_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(141.50f, 11.50f)), module, Imber::P6_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(153.90f, 11.50f)), module, Imber::P7_LIGHT));
        addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(166.30f, 11.50f)), module, Imber::P8_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(142.50f, 107.00f)), module, Imber::LEVEL_L_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(158.00f, 107.00f)), module, Imber::LEVEL_R_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Imber* module = getModule<Imber>();
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem(
            "Engine",
            {"original (continuous bed)", "sparse (clocked drops)"},
            &module->engineMode));
        menu->addChild(createBoolPtrMenuItem(
            "Ephemeral (reroll bank + constellations on load)",
            "", &module->ephemeral));
    }
};

Model* modelImber = createModel<Imber, ImberWidget>("imber");
