// vespae_probe - measurement harness for the vespae (Wasp) filter.
//
// Not part of `make check` (smoke_vespae is). This one prints the numbers you
// need to judge whether the emulation still behaves like the circuit:
//
//   resp   : LP/BP/HP magnitude response, small-signal, for a few settings
//   q      : resonant peak gain and frequency vs. cutoff knob at max RES,
//            which should reproduce the Wasp's Q falling as it opens
//            (paper: Q ~ 94 at 400 Hz, ~ 6 at 10 kHz)
//   osc    : self-oscillation frequency/level vs. the cutoff knob
//   mix    : the LP/HP mix output - where its null sits vs. the knob
//   thd    : harmonic distortion vs. DRIVE and vs. GRIT
//   stress : extreme settings, checked for non-finite output and runaway
//
// Usage: ./vespae_probe [section ...]     (no args = all)

#include "smoke_harness.hpp"
#include "../src/vespae.cpp"

#include <vector>
#include <complex>

static const float PROBE_SR = 48000.f;

// Drive a fresh module for `settle` seconds, then measure `meas` seconds.
struct Probe {
    Vespae m;
    long frame = 0;

    void set(int p, float v) { m.params[p].setValue(v); }

    void connectIn() { m.inputs[Vespae::AUDIO_INPUT].channels = 1; }

    Module::ProcessArgs args() {
        Module::ProcessArgs a;
        a.sampleRate = PROBE_SR;
        a.sampleTime = 1.f / PROBE_SR;
        a.frame = frame++;
        return a;
    }
};

// RMS of `out` at the fundamental `f` (Goertzel-style single-bin DFT), plus
// total RMS, while feeding a sine of amplitude `amp` at `f`.
struct SineResult {
    double fundRms = 0, totalRms = 0, dc = 0, peak = 0;
    long nans = 0;
    double thd = 0;   // sqrt(sum of harmonics 2..8) / fundamental
};

static SineResult driveSine(Vespae& m, long& frame, float f, float amp,
                            int outId, double settleS, double measS) {
    const double w = 2.0 * M_PI * f / PROBE_SR;
    double ph = 0.0;
    const int nSettle = (int)(settleS * PROBE_SR);
    // measure over a whole number of cycles for a clean single-bin DFT
    const int cycles = std::max(4, (int)(measS * f));
    const int nMeas = (int)std::llround(cycles * PROBE_SR / f);

    m.inputs[Vespae::AUDIO_INPUT].channels = 1;
    for (int i = 0; i < nSettle; i++) {
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        Module::ProcessArgs a; a.sampleRate = PROBE_SR;
        a.sampleTime = 1.f / PROBE_SR; a.frame = frame++;
        m.process(a);
    }

    SineResult r;
    std::complex<double> bin[9];
    for (int h = 0; h <= 8; h++) bin[h] = 0.0;
    double sum = 0, sum2 = 0;
    for (int i = 0; i < nMeas; i++) {
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(amp * std::sin(ph));
        ph += w;
        Module::ProcessArgs a; a.sampleRate = PROBE_SR;
        a.sampleTime = 1.f / PROBE_SR; a.frame = frame++;
        m.process(a);
        double y = m.outputs[outId].getVoltage();
        if (!std::isfinite(y)) { r.nans++; y = 0; }
        sum += y; sum2 += y * y;
        r.peak = std::max(r.peak, std::fabs(y));
        for (int h = 1; h <= 8; h++) {
            double a2 = -2.0 * M_PI * h * i / (double)nMeas * cycles;
            bin[h] += y * std::complex<double>(std::cos(a2), std::sin(a2));
        }
    }
    r.dc = sum / nMeas;
    r.totalRms = std::sqrt(sum2 / nMeas);
    double mag[9];
    for (int h = 1; h <= 8; h++) mag[h] = std::abs(bin[h]) * 2.0 / nMeas / std::sqrt(2.0);
    r.fundRms = mag[1];
    double harm = 0;
    for (int h = 2; h <= 8; h++) harm += mag[h] * mag[h];
    r.thd = r.fundRms > 1e-9 ? std::sqrt(harm) / r.fundRms : 0.0;
    return r;
}

// ── magnitude response ──────────────────────────────────────────────────────
static void secResp() {
    printf("\n# resp: small-signal magnitude, cutoff knob 0.5 (=640 Hz nominal)\n");
    printf("res,f_hz,lp_db,bp_db,hp_db\n");
    const float freqs[] = {50, 100, 200, 320, 450, 560, 640, 740, 900, 1300,
                           2500, 5000, 10000};
    for (float res : {0.0f, 0.5f, 0.9f}) {
        for (float f : freqs) {
            double g[3];
            for (int o = 0; o < 3; o++) {
                Vespae m; long fr = 0;
                m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
                m.params[Vespae::RES_PARAM].setValue(res);
                m.params[Vespae::DRIVE_PARAM].setValue(0.5f);
                int id = (o == 0) ? Vespae::LP_OUTPUT
                       : (o == 1) ? Vespae::BP_OUTPUT : Vespae::HP_OUTPUT;
                // 20 mV in: deep in the linear regime
                SineResult r = driveSine(m, fr, f, 0.02f, id, 0.6, 0.3);
                g[o] = 20.0 * std::log10(std::max(r.fundRms, 1e-12) / (0.02 / std::sqrt(2.0)));
            }
            printf("%.1f,%.0f,%.2f,%.2f,%.2f\n", res, f, g[0], g[1], g[2]);
        }
    }
}

// ── Q vs cutoff ─────────────────────────────────────────────────────────────
static void secQ() {
    printf("\n# q: BP peak gain and frequency at RES 0.9 (below self-oscillation)\n");
    printf("cutoff_knob,f_nominal,f_peak,peak_db,Q_est\n");
    for (float knob : {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f}) {
        const float fNom = 20.f * std::pow(2.f, 10.f * knob);
        double best = -1e9, bestF = 0;
        // fine log sweep around the nominal cutoff
        for (int i = -14; i <= 14; i++) {
            float f = fNom * std::pow(2.f, i / 24.f);
            if (f > 0.4f * PROBE_SR) continue;
            Vespae m; long fr = 0;
            m.params[Vespae::CUTOFF_PARAM].setValue(knob);
            m.params[Vespae::RES_PARAM].setValue(0.9f);
            m.params[Vespae::DRIVE_PARAM].setValue(0.5f);
            SineResult r = driveSine(m, fr, f, 0.005f, Vespae::BP_OUTPUT, 1.2, 0.4);
            double db = 20.0 * std::log10(std::max(r.fundRms, 1e-12) / (0.005 / std::sqrt(2.0)));
            if (db > best) { best = db; bestF = f; }
        }
        // for a BP normalised to unity at resonance, peak gain == Q here
        printf("%.1f,%.0f,%.0f,%.2f,%.1f\n", knob, fNom, bestF, best,
               std::pow(10.0, best / 20.0));
    }
}

// ── self-oscillation ────────────────────────────────────────────────────────
static void secOsc() {
    printf("\n# osc: self-oscillation at RES = 1, no input\n");
    printf("cutoff_knob,f_nominal,grit,rms_v,peak_v,f_meas\n");
    for (float knob : {0.2f, 0.35f, 0.5f, 0.65f, 0.8f}) {
        for (float grit : {0.0f, 0.5f, 1.0f}) {
            Vespae m; long fr = 0;
            m.params[Vespae::CUTOFF_PARAM].setValue(knob);
            m.params[Vespae::RES_PARAM].setValue(1.f);
            m.params[Vespae::GRIT_PARAM].setValue(grit);
            for (int i = 0; i < (int)(3 * PROBE_SR); i++) m.process(makeArgs(fr++));
            Stats s;
            int zc = 0; float prev = 0;
            const int n = (int)(1 * PROBE_SR);
            for (int i = 0; i < n; i++) {
                m.process(makeArgs(fr++));
                float y = m.outputs[Vespae::BP_OUTPUT].getVoltage();
                s.add(y);
                if (prev <= 0.f && y > 0.f) zc++;
                prev = y;
            }
            printf("%.2f,%.0f,%.1f,%.4f,%.3f,%d\n", knob,
                   20.f * std::pow(2.f, 10.f * knob), grit, s.rms(), s.peak, zc);
        }
    }
}

// ── distortion ──────────────────────────────────────────────────────────────
static void secThd() {
    printf("\n# thd: LP output, 200 Hz sine, cutoff knob 0.6 (=1280 Hz)\n");
    printf("drive,grit,in_vpk,out_rms,thd_pct,dc_v\n");
    for (float drive : {0.2f, 0.5f, 0.8f, 1.0f}) {
        for (float grit : {0.0f, 0.5f, 1.0f}) {
            Vespae m; long fr = 0;
            m.params[Vespae::CUTOFF_PARAM].setValue(0.6f);
            m.params[Vespae::RES_PARAM].setValue(0.3f);
            m.params[Vespae::DRIVE_PARAM].setValue(drive);
            m.params[Vespae::GRIT_PARAM].setValue(grit);
            SineResult r = driveSine(m, fr, 200.f, 5.f, Vespae::LP_OUTPUT, 0.5, 0.4);
            printf("%.1f,%.1f,5.0,%.3f,%.2f,%.4f\n", drive, grit,
                   r.totalRms, 100.0 * r.thd, r.dc);
        }
    }
}

// ── the LP/HP mix output ────────────────────────────────────────────────────
// The A-124 manual describes the knob as running "from pure low pass via
// asymmetrical / symmetrical / asymmetrical notch to pure high pass". A
// linear crossfade (1-m)*LP + m*HP does exactly that: the null sits at
// fc*sqrt((1-m)/m), sliding down from above the cutoff to below it.
static void secMix() {
    printf("\n# mix: null frequency vs. knob, cutoff knob 0.5 (=640 Hz)\n");
    printf("mix,f_null_predicted,f_null_measured,null_db,passband_db\n");
    const float fc = 640.f;
    for (float mix : {0.15f, 0.25f, 0.5f, 0.75f, 0.85f}) {
        const float pred = fc * std::sqrt((1.f - mix) / mix);
        if (pred > 0.4f * PROBE_SR) { printf("%.2f,-,-,-,-\n", mix); continue; }
        double worst = 1e9, worstF = 0;
        for (int i = -18; i <= 18; i++) {
            float f = pred * std::pow(2.f, i / 24.f);
            if (f < 20.f || f > 0.4f * PROBE_SR) continue;
            Vespae m; long fr = 0;
            m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
            m.params[Vespae::RES_PARAM].setValue(0.2f);
            m.params[Vespae::DRIVE_PARAM].setValue(0.5f);
            m.params[Vespae::MIX_PARAM].setValue(mix);
            SineResult r = driveSine(m, fr, f, 0.02f, Vespae::MIX_OUTPUT, 0.6, 0.3);
            if (r.fundRms < worst) { worst = r.fundRms; worstF = f; }
        }
        // passband: well below the cutoff, where the LP share dominates
        Vespae mp; long frp = 0;
        mp.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
        mp.params[Vespae::RES_PARAM].setValue(0.2f);
        mp.params[Vespae::DRIVE_PARAM].setValue(0.5f);
        mp.params[Vespae::MIX_PARAM].setValue(mix);
        SineResult pb = driveSine(mp, frp, 40.f, 0.02f, Vespae::MIX_OUTPUT, 0.6, 0.3);
        const double ref = 0.02 / std::sqrt(2.0);
        printf("%.2f,%.0f,%.0f,%.1f,%.2f\n", mix, pred, worstF,
               20.0 * std::log10(std::max(worst, 1e-12) / ref),
               20.0 * std::log10(std::max(pb.fundRms, 1e-12) / ref));
    }

    // A deep notch is a small-signal luxury: once the OTAs saturate they drag
    // the cutoff around at twice the signal frequency and the null smears.
    printf("\n# mix: notch depth at the centre vs. input level\n");
    printf("in_vpk,null_rel_db\n");
    for (float amp : {0.02f, 0.1f, 0.5f, 1.f, 3.f, 5.f}) {
        Vespae mn; long frn = 0;
        mn.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
        mn.params[Vespae::RES_PARAM].setValue(0.2f);
        mn.params[Vespae::DRIVE_PARAM].setValue(0.4f);
        mn.params[Vespae::MIX_PARAM].setValue(0.5f);
        SineResult n = driveSine(mn, frn, fc, amp, Vespae::MIX_OUTPUT, 0.5, 0.4);
        Vespae mr; long frr = 0;
        mr.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
        mr.params[Vespae::RES_PARAM].setValue(0.2f);
        mr.params[Vespae::DRIVE_PARAM].setValue(0.4f);
        mr.params[Vespae::MIX_PARAM].setValue(0.5f);
        SineResult ref = driveSine(mr, frr, fc, amp, Vespae::LP_OUTPUT, 0.5, 0.4);
        printf("%.2f,%.1f\n", amp,
               20.0 * std::log10(std::max(n.totalRms, 1e-12)
                                 / std::max(ref.totalRms, 1e-12)));
    }

    printf("\n# mix endpoints vs. the dedicated LP and HP jacks (should match)\n");
    printf("f_hz,mix0_db,lp_db,mix1_db,hp_db\n");
    for (float f : {80.f, 320.f, 640.f, 1300.f, 5000.f}) {
        double g[4];
        const int outs[4] = {Vespae::MIX_OUTPUT, Vespae::LP_OUTPUT,
                             Vespae::MIX_OUTPUT, Vespae::HP_OUTPUT};
        const float mixv[4] = {0.f, 0.f, 1.f, 1.f};
        for (int i = 0; i < 4; i++) {
            Vespae m; long fr = 0;
            m.params[Vespae::CUTOFF_PARAM].setValue(0.5f);
            m.params[Vespae::RES_PARAM].setValue(0.2f);
            m.params[Vespae::DRIVE_PARAM].setValue(0.5f);
            m.params[Vespae::MIX_PARAM].setValue(mixv[i]);
            SineResult r = driveSine(m, fr, f, 0.02f, outs[i], 0.6, 0.3);
            g[i] = 20.0 * std::log10(std::max(r.fundRms, 1e-12) / (0.02 / std::sqrt(2.0)));
        }
        printf("%.0f,%.2f,%.2f,%.2f,%.2f\n", f, g[0], g[1], g[2], g[3]);
    }
}

// ── stress ──────────────────────────────────────────────────────────────────
static void secStress() {
    printf("\n# stress\n");
    Vespae m; long fr = 0;
    m.inputs[Vespae::AUDIO_INPUT].channels = 1;
    m.inputs[Vespae::VOCT_INPUT].channels = 1;
    m.inputs[Vespae::FM_INPUT].channels = 1;
    m.inputs[Vespae::RES_CV_INPUT].channels = 1;
    m.params[Vespae::RES_PARAM].setValue(1.f);
    m.params[Vespae::DRIVE_PARAM].setValue(1.f);
    m.params[Vespae::FM_PARAM].setValue(1.f);
    Stats s[5];
    const int n = (int)(20 * PROBE_SR);
    for (int i = 0; i < n; i++) {
        float t = (float)i / PROBE_SR;
        m.inputs[Vespae::AUDIO_INPUT].setVoltage(10.f * std::sin(2.f * M_PI * 137.f * t));
        m.inputs[Vespae::VOCT_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 3.1f * t));
        m.inputs[Vespae::FM_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 511.f * t));
        m.inputs[Vespae::RES_CV_INPUT].setVoltage(5.f * std::sin(2.f * M_PI * 0.7f * t));
        m.params[Vespae::CUTOFF_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.23f * t));
        m.params[Vespae::GRIT_PARAM].setValue(0.5f + 0.5f * std::sin(2.f * M_PI * 0.13f * t));
        m.process(makeArgs(fr++));
        for (int o = 0; o < 5; o++) s[o].add(m.outputs[Vespae::LP_OUTPUT + o].getVoltage());
    }
    const char* nm[5] = {"lp", "hp", "bp", "notch", "mix"};
    printf("out,nans,rms,peak\n");
    for (int o = 0; o < 5; o++)
        printf("%s,%ld,%.3f,%.3f\n", nm[o], s[o].nans, s[o].rms(), s[o].peak);
}

int main(int argc, char** argv) {
    rack::random::init();
    bool all = (argc < 2);
    auto want = [&](const char* s) {
        if (all) return true;
        for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], s)) return true;
        return false;
    };
    if (want("resp"))   secResp();
    if (want("q"))      secQ();
    if (want("osc"))    secOsc();
    if (want("mix"))    secMix();
    if (want("thd"))    secThd();
    if (want("stress")) secStress();
    return 0;
}
