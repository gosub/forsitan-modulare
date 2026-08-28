// antrum_probe - measurement harness for the antrum reverb.
// Renders impulse responses and reports the numbers a reverb is judged by:
// RT60 against the decay knob, echo density, damping and stereo width.
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/antrum.cpp"

static void setKnobs(Antrum& m, float size, float decay, float absorb, float depth) {
    m.params[Antrum::MIX_PARAM].setValue(1.f);
    m.params[Antrum::SIZE_PARAM].setValue(size);
    m.params[Antrum::DECAY_PARAM].setValue(decay);
    m.params[Antrum::ABSORB_PARAM].setValue(absorb);
    m.params[Antrum::DEPTH_PARAM].setValue(depth);
}

// render an impulse response into l/r, after letting the smoothers settle
static void impulse(Antrum& m, std::vector<float>& l, std::vector<float>& r, double secs) {
    long frame = 0;
    for (int i = 0; i < (int)(0.3 * SR); i++) m.process(makeArgs(frame++));
    int n = (int)(secs * SR);
    l.resize(n);
    r.resize(n);
    for (int i = 0; i < n; i++) {
        m.inputs[Antrum::LEFT_INPUT].setVoltage(i == 0 ? 5.f : 0.f);
        m.process(makeArgs(frame++));
        l[i] = m.outputs[Antrum::LEFT_OUTPUT].getVoltage();
        r[i] = m.outputs[Antrum::RIGHT_OUTPUT].getVoltage();
    }
}

// -60 dB point of the Schroeder backward energy integral, in seconds
static double rt60(const std::vector<float>& x) {
    std::vector<double> e(x.size());
    double acc = 0.0;
    for (int i = (int)x.size() - 1; i >= 0; i--) {
        acc += (double)x[i] * x[i];
        e[i] = acc;
    }
    if (e[0] <= 0.0) return 0.0;
    for (size_t i = 0; i < e.size(); i++)
        if (10.0 * std::log10(e[i] / e[0]) < -60.0)
            return (double)i / SR;
    return -1.0;   // never got there inside the window
}

// echoes per second above a fraction of the running peak, first 200 ms
static double density(const std::vector<float>& x) {
    int n = std::min((int)x.size(), (int)(0.2 * SR));
    float peak = 0.f;
    for (int i = 0; i < n; i++) peak = std::max(peak, std::fabs(x[i]));
    int count = 0;
    for (int i = 1; i < n - 1; i++)
        if (std::fabs(x[i]) > peak * 0.05f && std::fabs(x[i]) > std::fabs(x[i-1])
            && std::fabs(x[i]) >= std::fabs(x[i+1]))
            count++;
    return count / 0.2;
}

// crude spectral centroid via zero crossings of the tail
static double brightness(const std::vector<float>& x, double fromSec) {
    int a = (int)(fromSec * SR), n = (int)x.size();
    if (a >= n - 1) return 0.0;
    int zc = 0;
    for (int i = a + 1; i < n; i++)
        if ((x[i] >= 0.f) != (x[i-1] >= 0.f)) zc++;
    return zc * SR / (2.0 * (n - a));
}

static double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    double sa = 0, sb = 0, sab = 0;
    for (size_t i = 0; i < a.size(); i++) {
        sa += (double)a[i] * a[i];
        sb += (double)b[i] * b[i];
        sab += (double)a[i] * b[i];
    }
    return (sa > 0 && sb > 0) ? sab / std::sqrt(sa * sb) : 0.0;
}

int main() {
    rack::random::init();
    printf("test,setting,rt60_s,density_per_s,tail_hz,l_r_corr\n");

    const float decays[] = {0.f, 0.15f, 0.3f, 0.6f, 0.9f, 1.0f};
    for (float d : decays) {
        Antrum m;
        setKnobs(m, 0.706f, d, 0.5f, 0.f);
        std::vector<float> l, r;
        impulse(m, l, r, 30.0);
        printf("decay,%.2f,%.3f,%.0f,%.0f,%.3f\n", d, rt60(l), density(l),
               brightness(l, 0.5), correlation(l, r));
    }
    const float sizes[] = {0.f, 0.35f, 0.706f, 1.f};
    for (float s : sizes) {
        Antrum m;
        setKnobs(m, s, 0.75f, 0.5f, 0.f);
        std::vector<float> l, r;
        impulse(m, l, r, 30.0);
        printf("size,%.2f,%.3f,%.0f,%.0f,%.3f\n", s, rt60(l), density(l),
               brightness(l, 0.5), correlation(l, r));
    }
    const float absorbs[] = {0.f, 0.33f, 0.66f, 1.f};
    for (float a : absorbs) {
        Antrum m;
        setKnobs(m, 0.706f, 0.75f, a, 0.f);
        std::vector<float> l, r;
        impulse(m, l, r, 30.0);
        printf("absorb,%.2f,%.3f,%.0f,%.0f,%.3f\n", a, rt60(l), density(l),
               brightness(l, 0.5), correlation(l, r));
    }
    const float depths[] = {-1.f, -0.5f, 0.5f, 1.f};
    for (float dp : depths) {
        Antrum m;
        setKnobs(m, 0.706f, 0.75f, 0.5f, dp);
        std::vector<float> l, r;
        impulse(m, l, r, 30.0);
        printf("depth,%.2f,%.3f,%.0f,%.0f,%.3f\n", dp, rt60(l), density(l),
               brightness(l, 0.5), correlation(l, r));
    }
    return 0;
}
