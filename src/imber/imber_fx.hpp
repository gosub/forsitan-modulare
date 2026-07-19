// imber_fx.hpp — the per-player effect chain and the master lo-fi chain.
// Per-player FX follow Haiku's fixed apply order (filters/quantise before
// spatial): LPF → HPF → BPF → BIT → DLY → GRN → RVB; REV is handled in
// the player's read direction. Exact designs from the reverse-engineering
// notes: 375 ms / 0.5 fb delay, Schroeder 4-comb + 2-allpass reverb with
// RT60 0.3–10 s, 1.5 s granular ring with 3 re-rolled taps, BPF with 3
// log-random centers re-rolled on sample load.
#pragma once
#include "imber_dsp.hpp"

namespace imber_fx {

using namespace imber_dsp;

enum FxKind { FX_REV, FX_LPF, FX_HPF, FX_BPF, FX_BIT, FX_DLY, FX_GRN, FX_RVB,
              FX_KINDS };

// ----------------------------------------------------- Schroeder reverb ---

struct Schroeder {
    std::vector<float> comb[4];
    std::vector<float> ap[2];
    int cIdx[4], aIdx[2];
    float fb[4];
    float sr;
    Schroeder() : sr(0.f) {
        for (int i = 0; i < 4; i++) { cIdx[i] = 0; fb[i] = 0.8f; }
        aIdx[0] = aIdx[1] = 0;
    }
    void init(float sampleRate) {
        sr = sampleRate;
        static const float combMs[4] = {29.7f, 37.1f, 41.1f, 43.7f};
        static const float apMs[2] = {5.0f, 1.7f};
        for (int i = 0; i < 4; i++) {
            comb[i].assign(std::max(2, (int)(combMs[i] * 0.001f * sr)), 0.f);
            cIdx[i] = 0;
            fb[i] = 0.8f;
        }
        for (int i = 0; i < 2; i++) {
            ap[i].assign(std::max(2, (int)(apMs[i] * 0.001f * sr)), 0.f);
            aIdx[i] = 0;
        }
    }
    void setDecay(float rt60) {
        for (int i = 0; i < 4; i++) {
            float d = (float)comb[i].size() / sr;
            fb[i] = std::pow(10.f, -3.f * d / clampf(rt60, 0.1f, 20.f));
        }
    }
    float process(float x) {
        float s = 0.f;
        for (int i = 0; i < 4; i++) {
            float v = comb[i][cIdx[i]];
            comb[i][cIdx[i]] = x + v * fb[i];
            if (++cIdx[i] >= (int)comb[i].size()) cIdx[i] = 0;
            s += v;
        }
        s *= 0.25f;
        for (int i = 0; i < 2; i++) {
            float v = ap[i][aIdx[i]];
            float y = -0.7f * s + v;
            ap[i][aIdx[i]] = s + 0.7f * y;
            if (++aIdx[i] >= (int)ap[i].size()) aIdx[i] = 0;
            s = y;
        }
        return s;
    }
};

// ------------------------------------------------------ per-player chain ---

struct PlayerFx {
    float en[FX_KINDS];    // enable ramps (proximity fades FX in/out)
    float tgt[FX_KINDS];
    float sr;
    float rampInc;

    PlayerFx() : sr(0.f), rampInc(0.f), lpfFc(0.f), hpfFc(0.f), bpfQ(1.f),
                 bits(6.f), bitLv(64.f), dlyIdx(0), dlySamp(1), grnIdx(0), tapXf(1.f),
                 grnClock(0), grnRollAt(1),
                 dlyIdle(0), grnIdle(0), rvbIdle(0),
                 dlyTail(1), grnTail(1), rvbTail(1) {
        for (int i = 0; i < FX_KINDS; i++)
            en[i] = tgt[i] = 0.f;
        for (int i = 0; i < 3; i++) {
            bpfFc[i] = 500.f;
            tapOld[i] = tapNew[i] = 1;
        }
    }

    Biquad lpf, hpf, bpf[3];
    float lpfFc, hpfFc, bpfFc[3], bpfQ;
    float bits, bitLv;   // bitLv = 2^bits, refreshed with bits
    OnePoleHp bitHp;

    std::vector<float> dly;
    int dlyIdx, dlySamp;

    std::vector<float> grn;
    int grnIdx;
    int tapOld[3], tapNew[3];
    float tapXf;           // 0 → old set, 1 → new set
    int grnClock, grnRollAt;

    Schroeder rvb;

    // The spatial trio keeps running after its FX leaves reach so the tail
    // rings out instead of being chopped. Once the tail is inaudible there
    // is nothing left to ring, so each one parks itself: idle counts
    // samples since the FX switched off, and the limits are one full tail
    // (delay 375 ms at 0.5 fb, the 1.5 s granular ring, reverb RT60 max).
    int dlyIdle, grnIdle, rvbIdle;
    int dlyTail, grnTail, rvbTail;

    void init(float sampleRate) {
        sr = sampleRate;
        rampInc = 1.f / (0.05f * sr);
        for (int i = 0; i < FX_KINDS; i++)
            en[i] = tgt[i] = 0.f;
        dlySamp = (int)(0.375f * sr);
        dly.assign(dlySamp + 4, 0.f);
        dlyIdx = 0;
        grn.assign((int)(1.5f * sr), 0.f);
        grnIdx = 0;
        tapXf = 1.f;
        grnClock = 0;
        grnRollAt = (int)(0.16f * sr);
        rvb.init(sr);
        bits = 6.f;
        bitLv = 64.f;
        lpfFc = 1200.f;
        hpfFc = 500.f;
        bpfQ = 6.f;
        for (int i = 0; i < 3; i++) {
            bpfFc[i] = 500.f * (i + 1);
            tapOld[i] = tapNew[i] = 1 + i * 1000;
        }
        bitHp.setTau(150.f, sr);
        dlyTail = (int)(4.f * sr);      // 375 ms at 0.5 fb -> -60 dB in ~3.8 s
        grnTail = (int)(1.6f * sr);     // must exceed the 1.5 s ring, so it
                                        // parks holding silence, not old audio
        rvbTail = (int)(12.f * sr);     // RT60 tops out at 10 s
        dlyIdle = dlyTail;
        grnIdle = grnTail;
        rvbIdle = rvbTail;
        rollFilters();
    }

    // re-rolled at init and on every sample load, log-distributed (Haiku
    // re-rolls the BPF centers per load; we extend that to LPF/HPF/BIT so
    // every sample wears the effect differently)
    void reseed(Rng& rng) {
        lpfFc = 300.f * std::pow(10.f, rng.uniform() * 1.1f);    // 300–3.8k
        hpfFc = 150.f * std::pow(10.f, rng.uniform() * 1.2f);    // 150–2.4k
        for (int i = 0; i < 3; i++)
            bpfFc[i] = 200.f * std::pow(10.f, rng.uniform() * 1.5f); // 200–6.3k
        bpfQ = rng.range(4.f, 12.f);
        bits = rng.range(3.f, 8.f);
        bitLv = std::pow(2.f, bits);
        rollFilters();
    }

    void rollFilters() {
        lpf.setLp(lpfFc, 0.9f, sr);
        hpf.setHp(hpfFc, 0.9f, sr);
        for (int i = 0; i < 3; i++)
            bpf[i].setBp(bpfFc[i], bpfQ, sr);
    }

    void setTarget(int kind, bool on) { tgt[kind] = on ? 1.f : 0.f; }
    bool reversed() const { return en[FX_REV] > 0.5f; }

    // flush every stateful buffer (recovery path: a non-finite value in
    // any feedback structure would otherwise latch forever)
    void clearState() {
        lpf.reset();
        hpf.reset();
        for (int i = 0; i < 3; i++)
            bpf[i].reset();
        bitHp.reset();
        std::fill(dly.begin(), dly.end(), 0.f);
        std::fill(grn.begin(), grn.end(), 0.f);
        for (int i = 0; i < 4; i++)
            std::fill(rvb.comb[i].begin(), rvb.comb[i].end(), 0.f);
        for (int i = 0; i < 2; i++)
            std::fill(rvb.ap[i].begin(), rvb.ap[i].end(), 0.f);
        dlyIdle = dlyTail;
        grnIdle = grnTail;
        rvbIdle = rvbTail;
    }

    void rollGrainTaps(Rng& rng) {
        for (int i = 0; i < 3; i++) {
            tapOld[i] = tapNew[i];
            tapNew[i] = (int)(rng.range(0.03f, 1.4f) * sr);
        }
        tapXf = 0.f;
    }

    float process(float x, Rng& rng) {
        for (int i = 0; i < FX_KINDS; i++) {
            if (en[i] < tgt[i]) en[i] = std::min(tgt[i], en[i] + rampInc);
            else if (en[i] > tgt[i]) en[i] = std::max(tgt[i], en[i] - rampInc);
        }

        if (en[FX_LPF] > 0.f)
            x += en[FX_LPF] * (lpf.process(x) - x);
        if (en[FX_HPF] > 0.f)
            x += en[FX_HPF] * (hpf.process(x) - x);
        if (en[FX_BPF] > 0.f) {
            float w = 0.f;
            for (int i = 0; i < 3; i++)
                w += bpf[i].process(x);
            x += en[FX_BPF] * (w * 1.2f + 0.25f * x - x);
        }
        if (en[FX_BIT] > 0.f) {
            float c = bitHp.process(crushLv(x, bitLv));
            x += en[FX_BIT] * (c - x);
        }

        // spatial trio runs continuously so tails ring out across
        // proximity changes; only the mix follows the ramp
        dlyIdle = en[FX_DLY] > 0.f ? 0 : dlyIdle + 1;
        if (dlyIdle < dlyTail) {
            int r = dlyIdx - dlySamp;
            if (r < 0) r += (int)dly.size();
            float wet = dly[r];
            dly[dlyIdx] = x * en[FX_DLY] + wet * 0.5f;
            if (++dlyIdx >= (int)dly.size()) dlyIdx = 0;
            x += wet * 0.7f;
        }
        grnIdle = en[FX_GRN] > 0.f ? 0 : grnIdle + 1;
        if (grnIdle < grnTail) {
            grn[grnIdx] = x * en[FX_GRN];
            if (++grnIdx >= (int)grn.size()) grnIdx = 0;
            if (++grnClock >= grnRollAt) {
                grnClock = 0;
                rollGrainTaps(rng);
            }
            if (tapXf < 1.f)
                tapXf = std::min(1.f, tapXf + 1.f / (0.16f * sr));
            float w = 0.f;
            int n = (int)grn.size();
            for (int i = 0; i < 3; i++) {
                int ro = grnIdx - tapOld[i];
                if (ro < 0) ro += n;
                int rn = grnIdx - tapNew[i];
                if (rn < 0) rn += n;
                w += grn[ro] * (1.f - tapXf) + grn[rn] * tapXf;
            }
            x += w * 0.4f;
        }
        {
            rvbIdle = en[FX_RVB] > 0.f ? 0 : rvbIdle + 1;
            float wet = rvbIdle < rvbTail ? rvb.process(x * en[FX_RVB]) : 0.f;
            x += wet * 0.6f;
        }
        return x;
    }
};

// --------------------------------------------------------- master chain ---

struct MasterChain {
    float sr;
    // tape mod
    std::vector<float> tapeL, tapeR;
    int tapeIdx;
    float wowPh, flutPh, agePh;
    // noise inject
    Biquad noiseBp;
    float noiseEnv;
    // bitcrush
    OnePoleHp crushHpL, crushHpR;
    // limiter
    float limEnv;
    float limAtk, limRel;   // depend only on sr; never recompute per sample
    Rng rng;

    void init(float sampleRate, uint64_t seed) {
        sr = sampleRate;
        int n = (int)(0.06f * sr);
        tapeL.assign(n, 0.f);
        tapeR.assign(n, 0.f);
        tapeIdx = 0;
        wowPh = flutPh = agePh = 0.f;
        rng.seed(seed);
        noiseBp.setBp(rng.range(800.f, 3000.f), 2.f, sr);
        noiseEnv = 0.f;
        crushHpL.setTau(120.f, sr);
        crushHpR.setTau(120.f, sr);
        limEnv = 0.f;
        limAtk = 1.f - std::exp(-1.f / (0.002f * sr));
        limRel = 1.f - std::exp(-1.f / (0.2f * sr));
    }

    void clearState() {
        std::fill(tapeL.begin(), tapeL.end(), 0.f);
        std::fill(tapeR.begin(), tapeR.end(), 0.f);
        noiseBp.reset();
        crushHpL.reset();
        crushHpR.reset();
        noiseEnv = 0.f;
        limEnv = 0.f;
    }

    inline float readTape(const std::vector<float>& t, float delaySamp) {
        float rp = (float)tapeIdx - delaySamp;
        int n = (int)t.size();
        while (rp < 0.f) rp += n;
        int i0 = (int)rp;
        int i1 = i0 + 1;
        if (i1 >= n) i1 = 0;
        float fr = rp - i0;
        return t[i0] + (t[i1] - t[i0]) * fr;
    }

    // bit/nse/tap 0..1 faders, vol = linear gain (up to +8 dB), all
    // pre-limit; ceiling −1 dBFS on the ±1 internal scale
    void process(float& l, float& r, float bit, float nse, float tap, float vol) {
        // TubeWarmth — always on, gentle
        l = 0.65f * l + 0.35f * std::tanh(l * 1.8f);
        r = 0.65f * r + 0.35f * std::tanh(r * 1.8f);

        // Bitcrush + TPDF dither + post-HP
        if (bit > 0.001f) {
            float lv = std::pow(2.f, 16.f - 12.f * bit);
            float lsb = 1.f / lv;          // 0.5^bits == 1 / 2^bits
            float dl = (rng.uniform() - rng.uniform()) * lsb;
            float dr = (rng.uniform() - rng.uniform()) * lsb;
            float cl = crushHpL.process(crushLv(l + dl, lv));
            float cr = crushHpR.process(crushLv(r + dr, lv));
            l += bit * (cl - l);
            r += bit * (cr - r);
        }

        // NoiseInject — band-limited, ducked open by the signal itself
        if (nse > 0.001f) {
            float e = std::max(std::fabs(l), std::fabs(r));
            noiseEnv += (e - noiseEnv) * (e > noiseEnv ? 0.01f : 0.0005f);
            float gate = clampf(noiseEnv * 6.f - 0.05f, 0.f, 1.f);
            float n = noiseBp.process(rng.bipolar()) * 2.f;
            l += n * nse * 0.35f * gate;
            r += n * nse * 0.35f * gate;
        }

        // TapeMod — wow + flutter on a modulated delay line, slow age AM.
        // The delay line keeps being fed even when TAP is off, so turning
        // it up never reads a stale buffer, but the four sines and the two
        // interpolated reads are skipped -- TAP defaults to 0, and this
        // whole stage used to run flat out to be multiplied by zero.
        {
            tapeL[tapeIdx] = l;
            tapeR[tapeIdx] = r;
            if (tap > 0.001f) {
                wowPh += kTau * 0.8f / sr;
                flutPh += kTau * 8.5f / sr;
                agePh += kTau * 0.07f / sr;
                if (wowPh > kTau) wowPh -= kTau;
                if (flutPh > kTau) flutPh -= kTau;
                if (agePh > kTau) agePh -= kTau;
                float depth = tap * (0.0025f * std::sin(wowPh)
                                     + 0.00018f * std::sin(flutPh));
                float base = 0.03f * sr;
                float dl = base * (1.f + depth * 30.f);
                float outL = readTape(tapeL, dl);
                float outR = readTape(tapeR, dl * 1.001f);
                float age = 1.f - tap * 0.25f
                            * (0.5f + 0.5f * std::sin(agePh));
                l += tap * (outL * age - l);
                r += tap * (outR * age - r);
            }
            if (++tapeIdx >= (int)tapeL.size()) tapeIdx = 0;
        }

        // volume into the soft limiter, ceiling −1 dBFS
        l *= vol;
        r *= vol;
        float peak = std::max(std::fabs(l), std::fabs(r));
        limEnv += (peak - limEnv) * (peak > limEnv ? limAtk : limRel);
        float g = limEnv > 0.891f ? 0.891f / limEnv : 1.f;
        l = softLimit(l * g);
        r = softLimit(r * g);
    }
};

} // namespace imber_fx
