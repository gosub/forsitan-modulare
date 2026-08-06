// caligo_probe — measurement harness for the Greyhole port. Not run by
// `make check`; this is the tool for hearing-by-numbers when the engine is
// touched. Prints CSV-ish tables.
//
//   ./caligo_probe            everything
//   ./caligo_probe echoes     echo arrival times and per-pass loss
//   ./caligo_probe decay      RT60 vs feedback and damping
//   ./caligo_probe density    diffusion spread vs diff and size
//   ./caligo_probe stereo     channel correlation vs spin
//   ./caligo_probe drift      the size random walk's excursion
//   ./caligo_probe sr         the same measurements at 44.1/48/96/192 kHz
//   ./caligo_probe cost       per-sample cost

#include "smoke_harness.hpp"

#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include "../src/caligo.cpp"

// The module reads its sample rate from ProcessArgs, so a probe at a different
// rate just hands it different args. SR from the harness is only the default.
static float probeSr = 48000.f;

static Module::ProcessArgs argsAt(long frame) {
    Module::ProcessArgs a;
    a.sampleRate = probeSr;
    a.sampleTime = 1.f / probeSr;
    a.frame = frame;
    return a;
}

static uint32_t rs = 0x2545F491u;
static float noise() {
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return ((float)(rs >> 8) * (1.f / 16777216.f) - 0.5f) * 10.f;
}

struct Setup {
    float time = 0.4f, size = 1.f, diff = 0.707f, fb = 0.9f, damp = 0.f;
    float mod = 0.1f, rate = 2.f, mix = 1.f, spin = 1.5707963f, drift = 0.f;
    bool tape = false;
};

// knob position for each parameter's taper (mirrors caligo.cpp)
static float kTime(float s) { return std::log(s / 0.01f) / std::log(1600.f); }
static float kSize(float s) { return std::log(s / 0.5f) / std::log(8.f); }
static float kDiff(float g) { return std::pow(g / 0.95f, 1.f / 0.8f); }
static float kRate(float h) { return std::log(h / 0.02f) / std::log(500.f); }

static void apply(Caligo& m, const Setup& s) {
    m.params[Caligo::TIME_PARAM].setValue(clamp(kTime(s.time), 0.f, 1.f));
    m.params[Caligo::SIZE_PARAM].setValue(clamp(kSize(s.size), 0.f, 1.f));
    m.params[Caligo::DIFF_PARAM].setValue(clamp(kDiff(s.diff), 0.f, 1.f));
    m.params[Caligo::FEEDBACK_PARAM].setValue(s.fb);
    m.params[Caligo::DAMP_PARAM].setValue(s.damp / 0.99f);
    m.params[Caligo::MOD_PARAM].setValue(s.mod);
    m.params[Caligo::RATE_PARAM].setValue(clamp(kRate(s.rate), 0.f, 1.f));
    m.params[Caligo::MIX_PARAM].setValue(s.mix);
    m.params[Caligo::SPIN_PARAM].setValue(s.spin / 1.5707963f);
    m.params[Caligo::DRIFT_PARAM].setValue(s.drift);
    m.p.tape = s.tape;
}

// impulse response, captured after the smoothers and the long delay's first
// crossfade have settled
static void impulse(const Setup& s, double secs, std::vector<float>& l,
                    std::vector<float>& r) {
    Caligo m;
    apply(m, s);
    long frame = 0;
    int warm = (int)(1.5f * probeSr);
    for (int i = 0; i < warm; i++) {
        m.inputs[Caligo::IN_L_INPUT].setVoltage(0.f);
        m.process(argsAt(frame++));
    }
    int n = (int)(secs * probeSr);
    l.assign(n, 0.f);
    r.assign(n, 0.f);
    for (int i = 0; i < n; i++) {
        float in = (i == 0) ? 5.f : 0.f;
        m.inputs[Caligo::IN_L_INPUT].setVoltage(in);
        m.inputs[Caligo::IN_R_INPUT].setVoltage(in);
        m.process(argsAt(frame++));
        l[i] = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
        r[i] = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
    }
}

// RMS envelope in `winMs` blocks
static std::vector<float> envelope(const std::vector<float>& x, float winMs) {
    int w = std::max(1, (int)(winMs * 0.001f * probeSr));
    std::vector<float> e;
    for (size_t k = 0; k + w <= x.size(); k += w) {
        double s = 0;
        for (int j = 0; j < w; j++) s += (double)x[k + j] * x[k + j];
        e.push_back((float)std::sqrt(s / w));
    }
    return e;
}

// ------------------------------------------------------------------- echoes

// The diffuser is 24 delay lines in series, so it contributes a fixed latency
// of its own on top of the long delay: the loop period is time + that, and the
// first thing out of the module is the diffuser latency alone.
static float diffuserLatencyMs(float size) {
    Setup s;
    s.size = size;
    s.diff = 0.f;                 // collapse the diffuser to a plain chain
    s.fb = 0.f;
    s.mod = 0.f;
    std::vector<float> l, r;
    impulse(s, 0.5, l, r);
    float pk = 0.f;
    size_t at = 0;
    for (size_t i = 0; i < l.size(); i++)
        if (std::fabs(l[i]) > pk) { pk = std::fabs(l[i]); at = i; }
    return (float)at * 1000.f / probeSr;
}

static void probeEchoes() {
    printf("\n== echo arrivals and per-pass loss (diff 0.2, mod 0, wet)\n");
    float lat = diffuserLatencyMs(1.f);
    printf("diffuser latency at size 1.0: %.1f ms — the loop period is\n"
           "time + that, and the first arrival is that alone\n", lat);
    printf("time_ms fb   period_ms  arrivals_ms                        pass_loss_dB\n");
    const float times[] = {12.f, 90.f, 400.f, 1200.f};
    const float fbs[] = {0.5f, 0.9f, 1.0f};
    for (float t : times) {
        float period = t + lat;
        for (float fb : fbs) {
            Setup s;
            s.time = t * 0.001f;
            s.fb = fb;
            s.diff = 0.2f;
            s.mod = 0.f;
            std::vector<float> l, r;
            impulse(s, std::min(9.0, 6.0 * period * 0.001 + 0.5), l, r);
            float win = std::max(0.5f, period * 0.04f);
            std::vector<float> e = envelope(l, win);
            printf("%7.0f %.2f %10.1f  ", t, fb, period);
            float first = 0.f, last = 0.f;
            int npass = 0;
            for (int k = 0; k <= 4; k++) {
                double centre = lat + k * period;          // ms
                int a = (int)((centre - period * 0.35) / win);
                int b = (int)((centre + period * 0.35) / win);
                if (b >= (int)e.size()) break;
                float pk = 0.f;
                int at = a;
                for (int i = std::max(a, 0); i <= b; i++)
                    if (e[i] > pk) { pk = e[i]; at = i; }
                if (pk < 1e-4f) break;
                printf("%.0f(%.3f) ", at * win, pk);
                if (k == 0) first = pk;
                last = pk;
                npass = k;
            }
            if (npass > 0)
                printf("  %+.2f\n", 20.f * std::log10(last / first) / npass);
            else
                printf("  n/a\n");
        }
    }
    printf("expected pass loss = 20*log10(fb): %.2f / %.2f / %.2f dB\n",
           20 * std::log10(0.5f), 20 * std::log10(0.9f), 0.f);
}

// -------------------------------------------------------------------- decay

static float rt60(const std::vector<float>& x) {
    std::vector<float> e = envelope(x, 20.f);
    float pk = 0.f;
    size_t at = 0;
    for (size_t i = 0; i < e.size(); i++)
        if (e[i] > pk) { pk = e[i]; at = i; }
    float target = pk * 0.001f;
    for (size_t i = at; i < e.size(); i++)
        if (e[i] < target)
            return (float)i * 0.02f;
    return -1.f;   // still ringing at the end of the capture
}

static void probeDecay() {
    printf("\n== RT60 (s), -1 = still ringing after the capture\n");
    printf("        damp:");
    const float damps[] = {0.f, 0.3f, 0.6f, 0.9f};
    for (float d : damps) printf("  %5.2f", d);
    printf("\nfb\n");
    const float fbs[] = {0.5f, 0.7f, 0.9f, 0.98f, 1.f, 1.2f};
    for (float fb : fbs) {
        printf("%.2f        ", fb);
        for (float d : damps) {
            Setup s;
            s.fb = fb;
            s.damp = d;
            std::vector<float> l, r;
            impulse(s, 25.0, l, r);
            printf("  %5.2f", rt60(l));
        }
        printf("\n");
    }
}

// ------------------------------------------------------------------ density

// how long the diffuser smears a single impulse: time from the first arrival
// to the point the envelope has fallen 20 dB inside one delay period
static void probeDensity() {
    printf("\n== diffusion spread within one 400 ms pass (fb 0, mod 0)\n");
    printf("diff  size  spread_ms  peak_V  crest_dB\n");
    const float diffs[] = {0.f, 0.2f, 0.45f, 0.707f, 0.95f};
    const float sizes[] = {0.5f, 1.f, 2.f, 4.f};
    for (float g : diffs) {
        for (float sz : sizes) {
            Setup s;
            s.diff = g;
            s.size = sz;
            s.fb = 0.f;
            s.mod = 0.f;
            std::vector<float> l, r;
            impulse(s, 0.4, l, r);
            std::vector<float> e = envelope(l, 1.f);
            float pk = 0.f;
            for (float v : e) pk = std::max(pk, v);
            int lastAbove = 0;
            for (size_t i = 0; i < e.size(); i++)
                if (e[i] > pk * 0.1f) lastAbove = (int)i;
            double rmsAll = 0;
            for (float v : l) rmsAll += (double)v * v;
            rmsAll = std::sqrt(rmsAll / l.size());
            float samplePk = 0.f;
            for (float v : l) samplePk = std::max(samplePk, std::fabs(v));
            printf("%.3f %5.1f  %9d  %6.3f  %+7.1f\n", g, sz, lastAbove, samplePk,
                   20.f * std::log10(samplePk / (rmsAll + 1e-12)));
        }
    }
}

// ------------------------------------------------------------------- stereo

static void probeStereo() {
    printf("\n== channel correlation vs spin (mono noise in, wet)\n");
    printf("spin_deg  corr    L/R_rms_dB\n");
    for (int i = 0; i <= 6; i++) {
        float t = (float)i / 6.f;
        Caligo m;
        Setup s;
        apply(m, s);
        m.params[Caligo::SPIN_PARAM].setValue(t);
        m.params[Caligo::MOD_PARAM].setValue(0.f);
        long frame = 0;
        for (int k = 0; k < (int)(2.0 * probeSr); k++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
            m.process(argsAt(frame++));
        }
        double sa = 0, sb = 0, sab = 0;
        for (int k = 0; k < (int)(4.0 * probeSr); k++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
            m.process(argsAt(frame++));
            double l = m.outputs[Caligo::OUT_L_OUTPUT].getVoltage();
            double r = m.outputs[Caligo::OUT_R_OUTPUT].getVoltage();
            sa += l * l; sb += r * r; sab += l * r;
        }
        printf("%8.1f  %+.3f  %+9.2f\n", t * 90.f,
               sab / (std::sqrt(sa * sb) + 1e-30),
               10.0 * std::log10((sa + 1e-30) / (sb + 1e-30)));
    }
}

// -------------------------------------------------------------------- drift

static void probeDrift() {
    printf("\n== drift: size excursion over 60 s (size knob at 1.0)\n");
    printf("drift  min_size  max_size  mean  span_%%\n");
    const float amounts[] = {0.f, 0.25f, 0.5f, 1.f};
    for (float d : amounts) {
        Caligo m;
        Setup s;
        s.drift = d;
        apply(m, s);
        long frame = 0;
        float lo = 1e9f, hi = -1e9f;
        double sum = 0;
        int cnt = 0;
        for (int i = 0; i < (int)(60.0 * probeSr); i++) {
            m.inputs[Caligo::IN_L_INPUT].setVoltage(noise() * 0.1f);
            m.process(argsAt(frame++));
            if (i % 480 == 0) {
                float v = m.engine.sizeEff;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                cnt++;
            }
        }
        printf("%.2f  %8.3f  %8.3f  %.3f  %6.1f\n", d, lo, hi, sum / cnt,
               100.f * (hi - lo));
    }
}

// ---------------------------------------------------------------------- sr

static void probeSampleRates() {
    printf("\n== the same room at four sample rates\n");
    printf("The prime lengths are rescaled by SR/44100, so these columns are\n"
           "the invariant the port adds: the original specifies samples, and a\n"
           "naive port halves the diffuser's duration from 48 to 96 kHz.\n");
    printf("     sr  shortest_line_ms  diffuser_latency_ms  max_time_s\n");
    const float rates[] = {44100.f, 48000.f, 96000.f, 192000.f};
    for (float r : rates) {
        probeSr = r;
        Caligo probe;
        apply(probe, Setup());
        long f = 0;
        for (int i = 0; i < 64; i++) probe.process(argsAt(f++));
        // the loop length of a level is fdelay1a(prime-1) plus the explicit
        // one-sample delays the original puts in the forward and feedback
        // paths, so the effective length is the prime itself
        float firstMs = (probe.engine.level[0][0].dL + 1.f) * 1000.f / r;
        printf("%7.0f  %16.4f  %19.2f  %10.3f\n", r, firstMs,
               diffuserLatencyMs(1.f), probe.engine.maxTimeSec());
    }
    probeSr = 48000.f;
}

// -------------------------------------------------------------------- cost

static void probeCost() {
    printf("\n== cost (defaults, wet, 20 s of audio)\n");
    Caligo m;
    apply(m, Setup());
    long frame = 0;
    int n = (int)(20.0 * probeSr);
    clock_t t0 = clock();
    for (int i = 0; i < n; i++) {
        m.inputs[Caligo::IN_L_INPUT].setVoltage(noise());
        m.process(argsAt(frame++));
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    size_t diffuserFrames = 0;
    for (int i = 0; i < caligo_dsp::kStages; i++)
        for (int j = 0; j < caligo_dsp::kNest; j++)
            diffuserFrames += m.engine.level[i][j].lineL.buf.size()
                            + m.engine.level[i][j].lineR.buf.size();
    printf("  %.3f s for %.1f s of audio = %.2f%% of one core at %.0f Hz\n",
           secs, n / probeSr, 100.0 * secs / (n / probeSr), probeSr);
    printf("  buffers: diffuser %.2f MB, long delay %.2f MB stereo\n",
           diffuserFrames * 4.0 / 1048576.0,
           m.engine.longL.n * 8.0 / 1048576.0);
}

int main(int argc, char** argv) {
    rack::random::init();
    bool all = (argc < 2);
    auto want = [&](const char* k) {
        if (all) return true;
        for (int i = 1; i < argc; i++)
            if (!std::strcmp(argv[i], k)) return true;
        return false;
    };
    if (want("echoes")) probeEchoes();
    if (want("decay")) probeDecay();
    if (want("density")) probeDensity();
    if (want("stereo")) probeStereo();
    if (want("drift")) probeDrift();
    if (want("sr")) probeSampleRates();
    if (want("cost")) probeCost();
    return 0;
}
