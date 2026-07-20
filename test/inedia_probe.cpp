// inedia_probe — audition harness for the starved-clock engine.
//
// Renders internally-generated test material through inedia::StarvedClock at a
// range of battery settings and writes 16-bit mono WAVs, plus a CSV of what the
// clock actually did. Standalone: no Rack, no input assets.
//
//   inedia_probe [outdir]        (default: ./inedia_out)
//
// Material is a drum pattern and a plucked arpeggio, because those two show
// opposite halves of the effect: percussion smears its transients and loses
// rhythmic grid, sustained tones droop in pitch and bloom back.

#include "../src/inedia_engine.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cmath>

static const float SR = 48000.f;

// --------------------------------------------------------------------- WAV
static void writeWav(const std::string& path, const std::vector<float>& x) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    uint32_t n = (uint32_t)x.size(), rate = (uint32_t)SR;
    uint32_t dataBytes = n * 2, riff = 36 + dataBytes;
    uint16_t ch = 1, bits = 16, fmt = 1, align = 2;
    uint32_t byteRate = rate * align, sub1 = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sub1, 4, 1, f); fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (float v : x) {
        int s = (int)lrintf(inedia::clampf(v, -1.f, 1.f) * 32767.f);
        int16_t o = (int16_t)s;
        fwrite(&o, 2, 1, f);
    }
    fclose(f);
    printf("  wrote %s (%.1f s)\n", path.c_str(), n / SR);
}

// ---------------------------------------------------------------- material
static uint32_t rng = 22222;
static float noise() {
    rng = rng * 1664525u + 1013904223u;
    return (float)((int32_t)(rng >> 8) - 8388608) / 8388608.f;
}

static void addKick(std::vector<float>& b, size_t at, float amp) {
    float ph = 0.f;
    for (int i = 0; i < (int)(0.35f * SR) && at + i < b.size(); i++) {
        float t = i / SR;
        float env = std::exp(-t * 14.f);
        float hz = 55.f + 110.f * std::exp(-t * 45.f);
        ph += hz / SR;
        b[at + i] += amp * env * std::sin(2.f * M_PI * ph);
    }
}

static void addSnare(std::vector<float>& b, size_t at, float amp) {
    float lp = 0.f;
    for (int i = 0; i < (int)(0.2f * SR) && at + i < b.size(); i++) {
        float t = i / SR;
        float env = std::exp(-t * 24.f);
        float n = noise();
        lp += (n - lp) * 0.5f;
        b[at + i] += amp * env * (0.7f * lp + 0.3f * std::sin(2.f * M_PI * 190.f * t));
    }
}

static void addHat(std::vector<float>& b, size_t at, float amp) {
    float hp = 0.f, prev = 0.f;
    for (int i = 0; i < (int)(0.06f * SR) && at + i < b.size(); i++) {
        float t = i / SR;
        float env = std::exp(-t * 90.f);
        float n = noise();
        hp = 0.85f * (hp + n - prev);
        prev = n;
        b[at + i] += amp * env * hp;
    }
}

static std::vector<float> makeDrums(float seconds) {
    std::vector<float> b((size_t)(seconds * SR), 0.f);
    float bpm = 112.f, step = 60.f / bpm / 2.f; // eighth notes
    int steps = (int)(seconds / step);
    for (int s = 0; s < steps; s++) {
        size_t at = (size_t)(s * step * SR);
        int p = s % 8;
        if (p == 0 || p == 3 || p == 6) addKick(b, at, 0.9f);
        if (p == 2 || p == 6) addSnare(b, at, 0.7f);
        addHat(b, at, (p % 2) ? 0.25f : 0.4f);
    }
    return b;
}

static std::vector<float> makePluck(float seconds) {
    std::vector<float> b((size_t)(seconds * SR), 0.f);
    const float notes[] = {110.f, 164.81f, 220.f, 261.63f, 220.f, 164.81f};
    float step = 0.4f;
    int n = (int)(seconds / step);
    for (int k = 0; k < n; k++) {
        size_t at = (size_t)(k * step * SR);
        float hz = notes[k % 6];
        float ph = 0.f;
        for (int i = 0; i < (int)(1.2f * SR) && at + i < b.size(); i++) {
            float t = i / SR;
            float env = std::exp(-t * 3.2f);
            ph += hz / SR;
            if (ph >= 1.f) ph -= 1.f;
            b[at + i] += 0.5f * env * (2.f * ph - 1.f); // saw
        }
    }
    return b;
}

// ------------------------------------------------------------------ render
// pinned = fraction of samples the clock sat clamped at kRatioFloor. High values
// mean the sag loop has stalled instead of drooping and recovering, which is the
// failure mode the DC blocker exists to prevent.
struct Stats { float ratioMin, ratioMean, pinned, peak; };

static Stats render(const std::vector<float>& in, std::vector<float>& out,
                    float battery, float delayMs, float feedback, float mix,
                    float tailSeconds) {
    inedia::StarvedClock sc;
    sc.baseRatio = 0.5f; // fresh-battery clock ~24 kHz: a cheap chip, not hi-fi
    float innerRate = sc.baseRatio * SR;
    int maxInner = (int)(2.0f * innerRate);
    sc.init(SR, maxInner);
    // Delay time is fixed in INNER samples; convert against the nominal rate.
    sc.inner.delaySamples = delayMs * 0.001f * innerRate;
    sc.inner.feedback = feedback;
    sc.inner.damp = 0.35f;

    // The one macro knob: a flatter battery both slows the clock outright and
    // makes it far more sensitive to what the audio is doing.
    sc.droop  = 0.50f * battery;
    sc.starve = 3.0f * battery;

    size_t total = in.size() + (size_t)(tailSeconds * SR);
    out.assign(total, 0.f);

    float rMin = 1.f, rSum = 0.f, peak = 0.f;
    long pinned = 0;
    for (size_t i = 0; i < total; i++) {
        float dry = i < in.size() ? in[i] : 0.f;
        float wet = sc.process(dry);
        float y = (1.f - mix) * dry + mix * wet;
        out[i] = y;
        rMin = std::min(rMin, sc.ratio);
        rSum += sc.ratio;
        if (sc.ratio <= inedia::kRatioFloor + 1e-6f) pinned++;
        peak = std::max(peak, std::fabs(y));
    }
    return {rMin, rSum / (float)total, (float)pinned / (float)total, peak};
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "inedia_out";
    std::string mk = "mkdir -p '" + dir + "'";
    if (system(mk.c_str()) != 0) { fprintf(stderr, "mkdir failed\n"); return 2; }

    struct Src { const char* name; std::vector<float> buf; };
    std::vector<Src> sources;
    sources.push_back({"drums", makeDrums(6.f)});
    sources.push_back({"pluck", makePluck(6.f)});

    const float batteries[] = {0.f, 0.3f, 0.6f, 0.85f, 1.f};

    printf("source,battery,ratio_min,ratio_mean,pinned,peak\n");
    for (auto& s : sources) {
        writeWav(dir + "/" + s.name + "_source.wav", s.buf);
        for (float b : batteries) {
            std::vector<float> out;
            Stats st = render(s.buf, out, b, 320.f, 0.45f, 0.5f, 3.f);
            char fn[256];
            snprintf(fn, sizeof(fn), "%s/%s_batt%02d.wav", dir.c_str(), s.name, (int)(b * 100));
            writeWav(fn, out);
            printf("%s,%.2f,%.3f,%.3f,%.3f,%.3f\n", s.name, b, st.ratioMin, st.ratioMean, st.pinned, st.peak);
        }
    }
    return 0;
}
