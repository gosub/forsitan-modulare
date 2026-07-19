// imber_engine.hpp — the real-time half of the Haiku model: 8 looping
// players driven by 5 drunk-jittered clock divisions, position-as-routing
// on a unit field (nearest clock within reach drives a player; all FX
// within reach stack onto it), pair couplings between players (sync pos /
// sync loop / jump), the Skip and Micro voices, and the master lo-fi
// chain. The module owns params/serialization; this owns the sound.
#pragma once
#include "imber_dsp.hpp"
#include "imber_gen.hpp"
#include "imber_fx.hpp"
#include <memory>

namespace imber_engine {

using namespace imber_dsp;
using imber_gen::Bank;

enum Division { DIV_2N, DIV_4N, DIV_8N, DIV_16N, DIV_32N, DIV_COUNT };

static const int kPlayers = 8;
static const int kFxObjs = imber_fx::FX_KINDS;

// everything the panel controls, filled by the module each control tick
struct Params {
    float px[kPlayers], py[kPlayers];   // field position 0..1
    float chg[kPlayers];                // change prob (already expo-mapped)
    bool voiceOn[kPlayers];             // per-voice mute (panel bezel latch)
    // equal-power pan gains, computed with px at control rate: doing the
    // sin/cos per sample cost 16 trig calls every frame for a value that
    // only moves when a knob or its CV does
    float panL[kPlayers], panR[kPlayers];
    float speedMult[kPlayers];          // ½ / 1 / 2
    float clkMorph, fxMorph;            // constellation A→B
    float reach;                        // 0.05..0.7 field units
    float couple;                       // 0..1 pair-influence strength
    float bpm, spd, lpm;                // tempo, global speed, max loop s
    float skp, sks, scv;                // skip voice
    float pls, mch, mcv, mdv, plv;      // micro voice
    int microDiv;                       // 0..4 panel division
    float bit, nse, tap, rvl, vol;      // master faders (vol = linear gain)
    bool on;
    bool sparse;                        // engine mode: clocked articulation
    Params() {
        for (int i = 0; i < kPlayers; i++) {
            px[i] = py[i] = 0.5f;
            chg[i] = 0.1f;
            speedMult[i] = 1.f;
            voiceOn[i] = true;
            panL[i] = panR[i] = 0.70710678f;
        }
        clkMorph = fxMorph = 0.f;
        reach = 0.25f; couple = 1.f;
        bpm = 100.f; spd = 1.f; lpm = 2.f;
        skp = 0.15f; sks = 1.f; scv = 0.5f;
        pls = 0.3f; mch = 0.15f; mcv = 0.25f; mdv = 0.03f; plv = 0.5f;
        microDiv = 3;
        bit = nse = tap = 0.f; rvl = 0.4f; vol = 1.f;
        on = true;
        sparse = false;
    }
};

// a clock or FX marker on the field: two rolled positions, morphed live
struct FieldObj {
    float ax, ay, bx, by;   // constellation A and B
    float ex, ey;           // effective (lerped) position
    int tag;                // division or FxKind — fixed, never rerolled
};

struct Engine {
    float sr;
    std::shared_ptr<const Bank> bank, bankPrev;

    FieldObj clk[DIV_COUNT];
    FieldObj fxo[kFxObjs];

    // one head = one buffer being read through a loop window
    struct Head {
        int buf;
        float pos;            // samples into the loop window
        float loopInS, loopDurS;
        bool inPrevBank;
        Head() : buf(-1), pos(0.f), loopInS(0.f), loopDurS(0.5f),
                 inPrevBank(false) {}
    };

    struct Player {
        Head cur, prev;
        float xfade;          // 1 → still hearing prev, ramps to 0
        int boundClock;       // division index, -1 = unbound → silent
        float gain;           // bind fade
        imber_fx::PlayerFx fx;
        Urn urn;
        float actEnv;         // display activity
        // sparse mode: one drop = one pass through the loop window. The
        // clock edge opens it, the material running out closes it, and the
        // silence in between is where the sparseness comes from.
        bool grainActive;
        float grainLeft;      // samples of material left in this drop
        float dropGain;       // click-free gate envelope
        Player() : xfade(0.f), boundClock(-1), gain(0.f), actEnv(0.f),
                   grainActive(false), grainLeft(0.f), dropGain(0.f) {}
    };
    Player pl[kPlayers];
    uint8_t fxMask[kPlayers];

    // pair couplings (display + audio)
    struct Pair { int a, b, kind; };   // kind: 0 syncpos, 1 syncloop, 2 jump
    Pair pairs[kPlayers * (kPlayers - 1) / 2];
    int pairCount;

    // skip + micro voices
    struct OneShot {
        int buf;
        float pos, speed;
        bool active, inPrevBank;
        OneShot() : buf(-1), pos(0.f), speed(1.f), active(false),
                    inPrevBank(false) {}
    };
    OneShot skip, micro;
    Urn skipUrn, microUrn;
    int microEffDiv;
    int lastMicroDivParam;
    Drunk mcvWalk;

    // clocks
    double nominal[DIV_COUNT];
    float jitter[DIV_COUNT];
    double t;

    Rng timingRng, voiceRng;
    imber_fx::MasterChain master;

    int assignPhase;
    float masterGain;         // ON/OFF fade
    float rvlCached;

    static float divMult(int d) {
        static const float m[DIV_COUNT] = {2.f, 1.f, 0.5f, 0.25f, 0.125f};
        return m[d];
    }

    void init(float sampleRate, uint64_t seed) {
        sr = sampleRate;
        timingRng.seed(seed ^ 0x7115ull);
        voiceRng.seed(seed ^ 0xb0c3ull);
        for (int i = 0; i < kPlayers; i++) {
            pl[i] = Player();
            pl[i].fx.init(sr);
            fxMask[i] = 0;
        }
        skip = OneShot();
        micro = OneShot();
        microEffDiv = DIV_16N;
        lastMicroDivParam = -1;
        for (int d = 0; d < DIV_COUNT; d++) {
            nominal[d] = 0.0;
            jitter[d] = 0.f;
        }
        t = 0.0;
        master.init(sr, seed ^ 0x3a57ull);
        assignPhase = 0;
        masterGain = 0.f;
        pairCount = 0;
        rvlCached = -1.f;
    }

    // last-resort recovery: a non-finite sample escaped — flush every
    // stateful buffer so the poisoning cannot latch, keep the music state
    void recover() {
        for (int i = 0; i < kPlayers; i++) {
            Player& p = pl[i];
            p.fx.clearState();
            p.cur.pos = 0.f;
            p.prev.buf = -1;
            p.xfade = 0.f;
            p.gain = 0.f;
            p.actEnv = 0.f;
            p.grainActive = false;
            p.grainLeft = 0.f;
            p.dropGain = 0.f;
        }
        skip.active = false;
        micro.active = false;
        master.clearState();
        masterGain = 0.f;
    }

    // ------------------------------------------------- constellations ---

    // stratified placement: shuffled jittered grid cells, so a roll can't
    // pile every object into one corner and leave the field dead
    static void stratified(Rng& rng, int count, int gw, int gh,
                           float* xs, float* ys) {
        int cells[16];
        int n = gw * gh;
        for (int i = 0; i < n; i++) cells[i] = i;
        for (int i = n - 1; i > 0; i--) {
            int j = rng.irange(0, i);
            int tmp = cells[i]; cells[i] = cells[j]; cells[j] = tmp;
        }
        for (int i = 0; i < count; i++) {
            int cx = cells[i] % gw, cy = cells[i] / gw;
            xs[i] = clampf((cx + 0.15f + 0.7f * rng.uniform()) / gw, 0.02f, 0.98f);
            ys[i] = clampf((cy + 0.15f + 0.7f * rng.uniform()) / gh, 0.02f, 0.98f);
        }
    }

    void rerollClocks(Rng& rng) {
        float xs[DIV_COUNT], ys[DIV_COUNT];
        stratified(rng, DIV_COUNT, 3, 2, xs, ys);
        for (int i = 0; i < DIV_COUNT; i++) { clk[i].ax = xs[i]; clk[i].ay = ys[i]; }
        stratified(rng, DIV_COUNT, 3, 2, xs, ys);
        for (int i = 0; i < DIV_COUNT; i++) {
            clk[i].bx = xs[i]; clk[i].by = ys[i];
            clk[i].tag = i;
        }
    }

    void rerollFx(Rng& rng) {
        float xs[kFxObjs], ys[kFxObjs];
        stratified(rng, kFxObjs, 3, 3, xs, ys);
        for (int i = 0; i < kFxObjs; i++) { fxo[i].ax = xs[i]; fxo[i].ay = ys[i]; }
        stratified(rng, kFxObjs, 3, 3, xs, ys);
        for (int i = 0; i < kFxObjs; i++) {
            fxo[i].bx = xs[i]; fxo[i].by = ys[i];
            fxo[i].tag = i;
        }
    }

    static void nudgeObj(FieldObj& o, Rng& rng, float s) {
        o.ax = clampf(o.ax + rng.bipolar() * s, 0.f, 1.f);
        o.ay = clampf(o.ay + rng.bipolar() * s, 0.f, 1.f);
        o.bx = clampf(o.bx + rng.bipolar() * s, 0.f, 1.f);
        o.by = clampf(o.by + rng.bipolar() * s, 0.f, 1.f);
    }
    void nudgeClocks(Rng& rng) {
        for (int i = 0; i < DIV_COUNT; i++) nudgeObj(clk[i], rng, 0.03f);
    }
    void nudgeFx(Rng& rng) {
        for (int i = 0; i < kFxObjs; i++) nudgeObj(fxo[i], rng, 0.03f);
    }

    // ------------------------------------------------------ bank swap ---

    void setBank(std::shared_ptr<const Bank> b) {
        bankPrev = bank;
        bank = b;
        // crossfade every sounding player onto the new material
        for (int i = 0; i < kPlayers; i++) {
            Player& p = pl[i];
            if (p.cur.buf >= 0 && bankPrev) {
                p.prev = p.cur;
                p.prev.inPrevBank = true;
                p.xfade = 1.f;
            }
            if (p.cur.buf >= 0)
                loadHead(p.cur, p.urn);
        }
        if (skip.active) skip.inPrevBank = true;
        if (micro.buf >= 0) { micro.buf = -1; micro.active = false; }
    }

    void loadHead(Head& h, Urn& urn) {
        if (!bank || bank->loops.empty()) { h.buf = -1; return; }
        h.buf = urn.pick(voiceRng, (int)bank->loops.size());
        h.inPrevBank = false;
        float lenS = (float)bank->loops[h.buf].size() / sr;
        h.loopDurS = clampf(voiceRng.range(0.15f, 1.2f), 0.05f, lenS);
        h.loopInS = voiceRng.uniform() * std::max(0.f, lenS - h.loopDurS);
        h.pos = 0.f;
    }

    void loadPlayerSample(Player& p) {
        if (p.cur.buf >= 0) {
            p.prev = p.cur;
            p.xfade = 1.f;
        }
        loadHead(p.cur, p.urn);
        p.fx.reseed(voiceRng);
    }

    // ------------------------------------------------- event handling ---

    void walkLoop(Player& p, const Params& prm) {
        Head& h = p.cur;
        if (h.buf < 0 || !bank) return;
        const std::vector<float>& b = bufferFor(h);
        float lenS = (float)b.size() / sr;
        float maxDur = clampf(std::min(2.f, prm.lpm), 0.06f, lenS);
        h.loopDurS = clampf(h.loopDurS + timingRng.bipolar() * 0.2f, 0.05f, maxDur);
        h.loopInS = clampf(h.loopInS + timingRng.bipolar() * 0.2f,
                           0.f, std::max(0.f, lenS - h.loopDurS));
        p.actEnv = std::min(1.f, p.actEnv + 0.4f);
    }

    // sparse mode, on a clock edge: start a drop if the player is silent,
    // or top up the one already sounding. Topping up rather than
    // retriggering is what makes the mode degrade gracefully — once edges
    // arrive faster than the material lasts the gate never closes, and the
    // player is back to the continuous bed with no seam and no stutter.
    void openDrop(Player& p, const Params& prm, int i) {
        float rate = clampf(prm.spd, 0.05f, 2.f) * prm.speedMult[i];
        if (rate < 1e-4f) rate = 1e-4f;
        if (!p.grainActive) {
            p.cur.pos = 0.f;
            p.grainActive = true;
        }
        p.grainLeft = p.cur.loopDurS * sr / rate;
    }

    const std::vector<float>& bufferFor(const Head& h) {
        const Bank* bk = (h.inPrevBank && bankPrev) ? bankPrev.get() : bank.get();
        int i = h.buf;
        if (i < 0 || i >= (int)bk->loops.size()) i = 0;
        return bk->loops[i];
    }

    void fireDivision(int d, const Params& prm) {
        // players bound to this division churn their loop windows
        for (int i = 0; i < kPlayers; i++) {
            if (pl[i].boundClock != d) continue;
            walkLoop(pl[i], prm);
            if (prm.sparse) openDrop(pl[i], prm, i);
        }

        if (d == DIV_4N) {
            // auto sample change, evaluated every quarter
            for (int i = 0; i < kPlayers; i++)
                if (pl[i].boundClock >= 0 && timingRng.chance(prm.chg[i]))
                    loadPlayerSample(pl[i]);
        }
        if (d == DIV_8N && bank && !bank->skips.empty()
            && timingRng.chance(prm.skp)) {
            skip.buf = skipUrn.pick(voiceRng, (int)bank->skips.size());
            skip.pos = 0.f;
            skip.speed = prm.sks;
            skip.active = true;
            skip.inPrevBank = false;
        }
        if (d == DIV_32N) {
            if (timingRng.chance(prm.mdv))
                microEffDiv = timingRng.irange(0, DIV_COUNT - 1);
            mcvWalk.step(timingRng, 0.06f, -1.f, 1.f);
        }
        if (d == microEffDiv && bank && !bank->micros.empty()) {
            // two independent rolls: change the sound, then maybe play it
            float effChange = clampf(prm.mch + mcvWalk.v * prm.mcv, 0.f, 1.f);
            if (micro.buf < 0 || timingRng.chance(effChange))
                micro.buf = microUrn.pick(voiceRng, (int)bank->micros.size());
            if (timingRng.chance(prm.pls)) {
                micro.pos = 0.f;
                micro.speed = 1.f;
                micro.active = true;
                micro.inPrevBank = false;
            }
        }
    }

    // ---------------------------------------------- assignment + pairs ---

    void assignmentPass(const Params& prm) {
        for (int i = 0; i < DIV_COUNT; i++) {
            clk[i].ex = clk[i].ax + (clk[i].bx - clk[i].ax) * prm.clkMorph;
            clk[i].ey = clk[i].ay + (clk[i].by - clk[i].ay) * prm.clkMorph;
        }
        for (int i = 0; i < kFxObjs; i++) {
            fxo[i].ex = fxo[i].ax + (fxo[i].bx - fxo[i].ax) * prm.fxMorph;
            fxo[i].ey = fxo[i].ay + (fxo[i].by - fxo[i].ay) * prm.fxMorph;
        }
        float reach2 = prm.reach * prm.reach;
        for (int i = 0; i < kPlayers; i++) {
            Player& p = pl[i];
            int best = -1;
            float bestD = reach2;
            for (int c = 0; c < DIV_COUNT; c++) {
                float dx = prm.px[i] - clk[c].ex;
                float dy = prm.py[i] - clk[c].ey;
                float d2 = dx * dx + dy * dy;
                if (d2 <= bestD) { bestD = d2; best = c; }
            }
            bool wasBound = p.boundClock >= 0;
            p.boundClock = best;
            if (best >= 0 && (!wasBound || p.cur.buf < 0) && bank)
                if (p.cur.buf < 0)
                    loadPlayerSample(p);
            uint8_t mask = 0;
            for (int f = 0; f < kFxObjs; f++) {
                float dx = prm.px[i] - fxo[f].ex;
                float dy = prm.py[i] - fxo[f].ey;
                bool onFx = dx * dx + dy * dy <= reach2;
                p.fx.setTarget(f, onFx);
                if (onFx) mask |= (1 << f);
            }
            fxMask[i] = mask;
        }

        // pair couplings — position is composition
        pairCount = 0;
        if (prm.couple <= 0.001f) return;
        const float tol = 0.06f;
        for (int i = 0; i < kPlayers; i++) {
            if (pl[i].boundClock < 0 || pl[i].cur.buf < 0) continue;
            for (int j = i + 1; j < kPlayers; j++) {
                if (pl[j].boundClock < 0 || pl[j].cur.buf < 0) continue;
                float dx = std::fabs(prm.px[i] - prm.px[j]);
                float dy = std::fabs(prm.py[i] - prm.py[j]);
                int kind = -1;
                if (dx < tol) kind = 0;                       // sync pos
                else if (dy < tol) kind = 1;                  // sync loop
                else if (std::fabs(dx - dy) < tol) kind = 2;  // jump
                if (kind < 0) continue;
                if (pairCount < (int)(sizeof(pairs) / sizeof(pairs[0]))) {
                    pairs[pairCount].a = i;
                    pairs[pairCount].b = j;
                    pairs[pairCount].kind = kind;
                    pairCount++;
                }
                applyPair(pl[i], pl[j], kind, prm.couple);
            }
        }
    }

    void applyPair(Player& a, Player& b, int kind, float couple) {
        Head& ha = a.cur;
        Head& hb = b.cur;
        if (kind == 0) {
            // read heads drift toward a common playback phase
            float k = 0.005f * couple;
            float pa = ha.pos / std::max(1.f, ha.loopDurS * sr);
            float pb = hb.pos / std::max(1.f, hb.loopDurS * sr);
            float mean = 0.5f * (pa + pb);
            ha.pos += (mean - pa) * k * ha.loopDurS * sr;
            hb.pos += (mean - pb) * k * hb.loopDurS * sr;
        }
        else if (kind == 1) {
            // loop windows converge, at half the rate
            float k = 0.0025f * couple;
            float mi = 0.5f * (ha.loopInS + hb.loopInS);
            float md = 0.5f * (ha.loopDurS + hb.loopDurS);
            ha.loopInS += (mi - ha.loopInS) * k;
            hb.loopInS += (mi - hb.loopInS) * k;
            ha.loopDurS += (md - ha.loopDurS) * k;
            hb.loopDurS += (md - hb.loopDurS) * k;
        }
        else if (timingRng.chance(0.04f * couple)) {
            // jump: swap the two read positions
            float tmp = ha.pos;
            ha.pos = hb.pos;
            hb.pos = tmp;
        }
    }

    // ----------------------------------------------------- rendering ---

    float readHead(Head& h, float rate, bool reversed) {
        if (h.buf < 0 || !bank) return 0.f;
        // a poisoned head (NaN pos/window) must never reach the indexing
        if (!std::isfinite(h.pos) || !std::isfinite(h.loopDurS)
            || !std::isfinite(h.loopInS)) {
            h.pos = 0.f;
            h.loopInS = 0.f;
            h.loopDurS = 0.5f;
        }
        const std::vector<float>& b = bufferFor(h);
        int len = (int)b.size();
        float P = clampf(h.loopDurS * sr, 64.f, (float)len);
        float xf = std::min(0.025f * sr, P * 0.25f);
        if (h.pos >= P)
            h.pos -= (P - xf);
        if (h.pos < 0.f)
            h.pos = 0.f;
        float startS = h.loopInS * sr;
        float rp = reversed ? (P - 1.f - h.pos) : h.pos;
        float out = sampleAt(b, startS + rp);
        float ov = h.pos - (P - xf);
        if (ov > 0.f) {
            // equal-power seam: fade the window head in under the tail
            float u = ov / xf;
            float g1 = std::cos(u * kPi * 0.5f);
            float g2 = std::sin(u * kPi * 0.5f);
            float rp2 = reversed ? (P - 1.f - ov) : ov;
            out = out * g1 + sampleAt(b, startS + rp2) * g2;
        }
        h.pos += rate;
        return out;
    }

    static float sampleAt(const std::vector<float>& b, float idx) {
        int len = (int)b.size();
        if (len < 2) return 0.f;
        // NaN-proof clamp: !(idx >= 0) also catches non-finite indices
        if (!(idx >= 0.f)) idx = 0.f;
        else if (idx > (float)(len - 1)) idx = (float)(len - 1);
        int i0 = (int)idx;
        int i1 = std::min(i0 + 1, len - 1);
        float fr = idx - i0;
        return b[i0] + (b[i1] - b[i0]) * fr;
    }

    float renderOneShot(OneShot& o, const std::vector<std::vector<float> >& set,
                        const std::vector<std::vector<float> >* prevSet) {
        if (!o.active || o.buf < 0) return 0.f;
        if (!std::isfinite(o.pos)) {
            o.active = false;
            return 0.f;
        }
        const std::vector<std::vector<float> >& s =
            (o.inPrevBank && prevSet) ? *prevSet : set;
        int bi = o.buf < (int)s.size() ? o.buf : 0;
        const std::vector<float>& b = s[bi];
        if (o.pos >= (float)b.size() - 1.f) {
            o.active = false;
            return 0.f;
        }
        float v = sampleAt(b, o.pos);
        o.pos += o.speed;
        return v;
    }

    void process(const Params& prm, float& outL, float& outR,
                 float& skipOut, float& microOut, bool gate[DIV_COUNT]) {
        for (int d = 0; d < DIV_COUNT; d++)
            gate[d] = false;
        outL = outR = skipOut = microOut = 0.f;

        // ON/OFF fades the world out; clocks freeze while silent
        float gTgt = prm.on ? 1.f : 0.f;
        masterGain += (gTgt - masterGain) * (1.f / (0.05f * sr));
        if (!prm.on && masterGain < 0.001f)
            return;

        if (rvlCached < 0.f || std::fabs(prm.rvl - rvlCached) > 0.01f) {
            rvlCached = prm.rvl;
            float rt60 = 0.3f * std::pow(10.f / 0.3f, prm.rvl);
            for (int i = 0; i < kPlayers; i++)
                pl[i].fx.rvb.setDecay(rt60);
        }

        if (--assignPhase <= 0) {
            assignPhase = 2048;
            assignmentPass(prm);
        }

        // drunk clocks: every division fires at nominal + bounded-walk
        // jitter; the same jittered edges feed voices and gate outs
        t += 1.0;
        const double beatSamples = (60.0 / clampf(prm.bpm, 0.5f, 500.f)) * sr;
        for (int d = 0; d < DIV_COUNT; d++) {
            double interval = beatSamples * divMult(d);
            float lim = 0.25f * (float)(interval / sr);
            // a tempo jump must not strand the next edge. Speeding up used
            // to leave the old, slower deadline standing (2 bpm -> 2n is a
            // minute away), so the clock stayed dead until it elapsed; the
            // stale jitter offset was oversized for the new interval too.
            jitter[d] = clampf(jitter[d], -lim, lim);
            if (nominal[d] > t + interval)
                nominal[d] = t + interval;
            if (t >= nominal[d] + jitter[d] * sr) {
                nominal[d] += interval;
                if (nominal[d] + jitter[d] * sr < t)
                    nominal[d] = t + interval;   // catch up after rate/bpm jumps
                float step = std::min(0.2f, (float)(interval / sr) / 6.f);
                jitter[d] = clampf(jitter[d] + timingRng.bipolar() * step,
                                   -lim, lim);
                gate[d] = true;
                fireDivision(d, prm);
            }
        }

        if (!bank) return;

        // players
        float mixL = 0.f, mixR = 0.f;
        for (int i = 0; i < kPlayers; i++) {
            Player& p = pl[i];
            // a muted voice keeps its binding (so the panel can still show
            // which clock it would follow) and simply loses its gain
            float tg = (p.boundClock >= 0 && p.cur.buf >= 0
                        && prm.voiceOn[i]) ? 1.f : 0.f;
            p.gain += (tg - p.gain) * (1.f / (0.08f * sr));
            if (p.gain < 0.001f && tg == 0.f) {
                p.actEnv *= 0.9999f;
                continue;
            }
            // sparse: run down the drop, then hold silence until the next
            // edge. Original mode leaves the gate wide open forever.
            float dtg = 1.f;
            if (prm.sparse) {
                if (p.grainActive && (p.grainLeft -= 1.f) <= 0.f)
                    p.grainActive = false;
                dtg = p.grainActive ? 1.f : 0.f;
            }
            else
                p.grainActive = true;
            p.dropGain += (dtg - p.dropGain) * (1.f / (0.004f * sr));
            if (p.dropGain < 0.001f && dtg == 0.f) {
                p.actEnv *= 0.999f;
                continue;
            }
            float rate = clampf(prm.spd, 0.05f, 2.f) * prm.speedMult[i];
            bool rev = p.fx.reversed();
            float smp = readHead(p.cur, rate, rev);
            if (p.xfade > 0.f) {
                float old = readHead(p.prev, rate, rev);
                smp += (old - smp) * p.xfade;
                p.xfade -= 1.f / (0.03f * sr);
                if (p.xfade <= 0.f) {
                    p.xfade = 0.f;
                    p.prev.buf = -1;
                }
            }
            smp = p.fx.process(smp, voiceRng) * p.gain * p.dropGain;
            p.actEnv += (std::fabs(smp) * 2.f - p.actEnv) * 0.001f;
            mixL += smp * prm.panL[i];
            mixR += smp * prm.panR[i];
        }
        // sum/8 per the original, plus makeup gain so a typical field
        // lands near Rack levels before the master chain
        mixL *= 1.8f / kPlayers;
        mixR *= 1.8f / kPlayers;

        float sv = renderOneShot(skip, bank->skips,
                                 bankPrev ? &bankPrev->skips : nullptr);
        float mv = 0.f;
        if (micro.active) {
            const std::vector<float>& mb =
                bank->micros[micro.buf < (int)bank->micros.size() ? micro.buf : 0];
            if (!(micro.pos < (float)mb.size() - 1.f))   // also catches NaN
                micro.active = false;
            else {
                mv = sampleAt(mb, micro.pos);
                micro.pos += micro.speed;
            }
        }
        skipOut = sv * prm.scv;
        microOut = mv * prm.plv;
        mixL += (skipOut + microOut) * 0.7071f;
        mixR += (skipOut + microOut) * 0.7071f;

        master.process(mixL, mixR, prm.bit, prm.nse, prm.tap, prm.vol);
        outL = mixL * masterGain;
        outR = mixR * masterGain;
    }
};

} // namespace imber_engine
