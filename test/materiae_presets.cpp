// materiae_presets — load the factory presets into a real module and measure
// what comes out.
//
//     ./materiae_presets [dir]        default ../presets/materiae
//     ./materiae_presets --wav <dir>  also write one WAV per preset
//
// The presets are written by tools/presets/gen_materiae_presets.py in
// engineering units and converted to knob positions there. This reads them
// back through the module itself -- real ParamQuantities, real mapParams, real
// engine -- so what it prints is what a user gets when they pick the preset,
// not what the generator meant.
//
// A factory bank is worth having only if the presets are all audible, all
// distinct and all finite; the last column is the pairwise check.

#include "smoke_harness.hpp"
#include "../src/materiae.cpp"

#include <complex>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>

static const int kFFT = 8192;

static void fft(std::vector<std::complex<float>>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.f * 3.14159265358979f / (float)len;
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (int k = 0; k < len / 2; k++) {
                std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static std::vector<float> spectrum(const std::vector<float>& x) {
    std::vector<std::complex<float>> a(kFFT, std::complex<float>(0.f, 0.f));
    for (int i = 0; i < kFFT && i < (int)x.size(); i++) {
        float w = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * i / (kFFT - 1));
        a[i] = std::complex<float>(x[i] * w, 0.f);
    }
    fft(a);
    std::vector<float> m(kFFT / 2);
    float sum = 1e-12f;
    for (int i = 0; i < kFFT / 2; i++) { m[i] = std::abs(a[i]); sum += m[i]; }
    for (int i = 0; i < kFFT / 2; i++) m[i] /= sum;
    return m;
}

static float centroid(const std::vector<float>& m) {
    float num = 0.f, den = 1e-12f;
    for (int i = 0; i < (int)m.size(); i++) {
        num += (float)i * SR / (float)kFFT * m[i];
        den += m[i];
    }
    return num / den;
}

static float specDist(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.f;
    for (size_t i = 0; i < a.size(); i++) d += std::fabs(a[i] - b[i]);
    return d;
}

static void writeWav(const std::string& path, const std::vector<float>& x) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t)x.size(), rate = (uint32_t)SR, bytes = n * 2;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(rate); u32(rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; i++)
        u16((uint16_t)(int16_t)std::lrint(clamp(x[i] * 0.2f, -1.f, 1.f) * 30000.f));
    fclose(f);
}

struct Preset {
    std::string name, path;
};

static std::vector<Preset> listPresets(const std::string& dir) {
    std::vector<Preset> out;
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "cannot open %s\n", dir.c_str()); return out; }
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".vcvm") == 0)
            out.push_back({n.substr(0, n.size() - 5), dir + "/" + n});
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

// Apply a .vcvm the way Rack does: params by id, then the module's own data.
static bool load(Materiae& m, const std::string& path) {
    json_error_t err;
    json_t* root = json_load_file(path.c_str(), 0, &err);
    if (!root) { fprintf(stderr, "%s: %s\n", path.c_str(), err.text); return false; }
    json_t* params = json_object_get(root, "params");
    size_t i;
    json_t* v;
    json_array_foreach(params, i, v) {
        int id = (int)json_integer_value(json_object_get(v, "id"));
        float val = (float)json_number_value(json_object_get(v, "value"));
        if (id >= 0 && id < Materiae::PARAMS_LEN) m.params[id].setValue(val);
    }
    json_t* data = json_object_get(root, "data");
    if (data) m.dataFromJson(data);
    json_decref(root);
    return true;
}

int main(int argc, char** argv) {
    std::string dir = "../presets/materiae", wavDir;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavDir = argv[++i];
        else dir = argv[i];
    }

    std::vector<Preset> presets = listPresets(dir);
    if (presets.empty()) { fprintf(stderr, "no presets in %s\n", dir.c_str()); return 2; }

    printf("preset,rms,peak,dc,decay_ms,centroid_hz,finite\n");
    std::vector<std::vector<float>> spec;
    std::vector<std::string> names;
    int bad = 0;

    for (const Preset& pr : presets) {
        Materiae m;
        if (!load(m, pr.path)) { bad++; continue; }

        Stats s;
        std::vector<float> x;
        long frame = 0;
        m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
        m.process(makeArgs(frame++));
        m.inputs[Materiae::TRIG_INPUT].setVoltage(5.f);
        for (int i = 0; i < 32; i++) m.process(makeArgs(frame++));
        m.inputs[Materiae::TRIG_INPUT].setVoltage(0.f);
        for (int i = 0; i < (int)(2.5f * SR); i++) {
            m.process(makeArgs(frame++));
            float v = m.outputs[Materiae::AUDIO_OUTPUT].getVoltage();
            s.add(v);
            x.push_back(v);
        }

        // time from the strike to 60 dB below the peak, held for 5 ms
        float thr = s.peak * 0.001f;
        int hold = (int)(0.005f * SR), run = 0, last = 0;
        for (int i = 0; i < (int)x.size(); i++) {
            if (std::fabs(x[i]) > thr) { run = 0; last = i; }
            else if (++run > hold) break;
        }

        std::vector<float> mg = spectrum(x);
        spec.push_back(mg);
        names.push_back(pr.name);
        printf("%s,%.4f,%.4f,%+.5f,%.0f,%.1f,%d\n", pr.name.c_str(), s.rms(),
               s.peak, s.sum / std::max(1L, s.n), (double)last * 1000.0 / SR,
               centroid(mg), s.nans == 0 ? 1 : 0);

        if (s.nans) { fprintf(stderr, "%s: non-finite output\n", pr.name.c_str()); bad++; }
        if (s.rms() < 0.02) { fprintf(stderr, "%s: inaudible (rms %.4f)\n", pr.name.c_str(), s.rms()); bad++; }
        if (s.peak > 5.01f) { fprintf(stderr, "%s: over the declared swing (%.2f V)\n", pr.name.c_str(), s.peak); bad++; }
        if (!wavDir.empty()) writeWav(wavDir + "/materiae_" + pr.name + ".wav", x);
    }

    // A bank of eight is only worth eight slots if no two of them are the
    // same sound.
    float worst = 9.f;
    std::string wa, wb;
    for (size_t i = 0; i < spec.size(); i++)
        for (size_t j = i + 1; j < spec.size(); j++) {
            float d = specDist(spec[i], spec[j]);
            if (d < worst) { worst = d; wa = names[i]; wb = names[j]; }
        }
    printf("\nclosest pair: %s / %s at %.3f\n", wa.c_str(), wb.c_str(), worst);
    if (worst < 0.4f) { fprintf(stderr, "two presets are near-identical\n"); bad++; }

    printf("problems: %d\n", bad);
    return bad ? 1 : 0;
}
