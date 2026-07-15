// perge_render — offline renderer: feed a mono float32 stream through
// Perge::process() and write interleaved stereo float32. Lets us A/B the
// module against real pedal audio outside of Rack.
//
//   perge_render <in.f32mono> <out.f32stereo> [KEY=value ...]
//
// KEYs are param names (MIX, TEMPO, PITCH, SUSTAIN, GLITCH, LOFI, RVRB,
// FILTER, SENS, THRESH, ATTACK, RELEASE, MOD, DECAY, SPREAD, INFX) or
// the menu members repeatsMode / clockMult / grainCap / seed /
// tiltStart / tiltEnd / freezeAt (seconds). Input is +-1; we drive
// IN_L at +-5 V and read OUT_L/R back to +-1.

#include <rack.hpp>
rack::plugin::Plugin* pluginInstance = nullptr;
#include "../src/perge.cpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

static const float SR = 48000.f;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: perge_render in out [KEY=val ...]\n"); return 2; }
    rack::random::init();

    // read mono float32 input
    FILE* fi = fopen(argv[1], "rb");
    if (!fi) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    std::vector<float> in;
    { float s; while (fread(&s, sizeof(float), 1, fi) == 1) in.push_back(s); }
    fclose(fi);

    std::map<std::string, float> P;
    float tiltStart = -1, tiltEnd = -1, freezeAt = -1;
    int seed = 12345;
    for (int i = 3; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) continue;
        std::string k(argv[i], eq - argv[i]);
        float v = atof(eq + 1);
        if (k == "tiltStart") tiltStart = v;
        else if (k == "tiltEnd") tiltEnd = v;
        else if (k == "freezeAt") freezeAt = v;
        else if (k == "seed") seed = (int)v;
        else P[k] = v;
    }

    Perge m;
    m.noiseState = (uint32_t)seed ? (uint32_t)seed : 1u;
    m.inputs[Perge::IN_L_INPUT].channels = 1;
    m.outputs[Perge::OUT_L_OUTPUT].channels = 1;
    m.outputs[Perge::OUT_R_OUTPUT].channels = 1;

    struct NP { const char* n; int id; };
    NP names[] = {
        {"MIX",Perge::MIX_PARAM},{"TEMPO",Perge::TEMPO_PARAM},{"PITCH",Perge::PITCH_PARAM},
        {"SUSTAIN",Perge::SUSTAIN_PARAM},{"GLITCH",Perge::GLITCH_PARAM},{"LOFI",Perge::LOFI_PARAM},
        {"RVRB",Perge::RVRB_PARAM},{"FILTER",Perge::FILTER_PARAM},{"SENS",Perge::SENS_PARAM},
        {"THRESH",Perge::THRESH_PARAM},{"ATTACK",Perge::ATTACK_PARAM},{"RELEASE",Perge::RELEASE_PARAM},
        {"MOD",Perge::MOD_PARAM},{"DECAY",Perge::DECAY_PARAM},{"SPREAD",Perge::SPREAD_PARAM},
        {"INFX",Perge::INFX_PARAM},
    };
    for (auto& np : names) {
        auto it = P.find(np.n);
        if (it != P.end()) m.params[np.id].setValue(it->second);
    }
    // repeatsMode is a param now (cached each block), so set the param
    if (P.count("repeatsMode"))
        m.params[Perge::REPEATSMODE_PARAM].setValue((float)(int)P["repeatsMode"]);
    if (P.count("clockMult"))   m.clockMult   = (int)P["clockMult"];
    if (P.count("grainCap"))    m.grainCap    = P["grainCap"] > 0.5f;

    long freezeFrame = freezeAt >= 0 ? (long)(freezeAt * SR) : -1;
    long tiltA = tiltStart >= 0 ? (long)(tiltStart * SR) : -1;
    long tiltB = tiltEnd   >= 0 ? (long)(tiltEnd   * SR) : -1;

    FILE* fo = fopen(argv[2], "wb");
    long n = (long)in.size();
    // render input length + 6 s tail so repeats/reverb ring out
    long total = n + (long)(6 * SR);
    for (long i = 0; i < total; i++) {
        float x = (i < n) ? in[i] : 0.f;
        m.inputs[Perge::IN_L_INPUT].setVoltage(5.f * x);
        if (freezeFrame >= 0 && i == freezeFrame)
            m.params[Perge::SUSTAIN_PARAM].setValue(1.f);
        bool tilt = (tiltA >= 0 && i >= tiltA && i < tiltB);
        m.params[Perge::TILT_PARAM].setValue(tilt ? 1.f : 0.f);

        Module::ProcessArgs a; a.sampleRate = SR; a.sampleTime = 1.f/SR; a.frame = i;
        m.process(a);
        float oL = m.outputs[Perge::OUT_L_OUTPUT].getVoltage() / 5.f;
        float oR = m.outputs[Perge::OUT_R_OUTPUT].getVoltage() / 5.f;
        fwrite(&oL, sizeof(float), 1, fo);
        fwrite(&oR, sizeof(float), 1, fo);
    }
    fclose(fo);
    fprintf(stderr, "rendered %ld frames (%.1fs)\n", total, total / SR);
    return 0;
}
