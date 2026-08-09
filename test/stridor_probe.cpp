// stridor_probe — measurement harness for the friction voice.
//
// What it reports:
//   - output level across the force/velocity plane, which is how the output
//     gain was chosen;
//   - the creak-to-squeal transition: slip rate and spectral centroid as
//     velocity rises, so the bifurcation is visible as a number;
//   - the material sweep: centroid and decay per material position;
//   - CPU, since the contact solver bisects and the cost is patch-dependent.
//
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/stridor.cpp"

#include <chrono>
#include <vector>

static void setK(Stridor& m, int id, float v) { m.params[id].setValue(v); }

// Run the module and collect the audio output, after a settling time.
static std::vector<float> render(Stridor& m, double settleS, double measS) {
    long fr = 0;
    for (int i = 0; i < (int)(settleS * SR); i++) m.process(makeArgs(fr++));
    std::vector<float> buf((size_t)(measS * SR));
    for (size_t i = 0; i < buf.size(); i++) {
        m.process(makeArgs(fr++));
        buf[i] = m.outputs[Stridor::AUDIO_OUTPUT].getVoltage();
    }
    return buf;
}

static double rmsOf(const std::vector<float>& b) {
    double s = 0;
    for (float v : b) s += (double)v * v;
    return std::sqrt(s / std::max<size_t>(1, b.size()));
}

// Cheap spectral centroid: zero-crossing rate is a good enough stand-in for
// "is it creaking or squealing", and needs no FFT.
static double zcr(const std::vector<float>& b) {
    int z = 0;
    for (size_t i = 1; i < b.size(); i++)
        if ((b[i] > 0.f) != (b[i - 1] > 0.f)) z++;
    return z * SR / 2.0 / std::max<size_t>(1, b.size());
}

static int slipCount(Stridor& m, double seconds) {
    long fr = 0;
    int n = 0;
    bool prev = false;
    for (int i = 0; i < (int)(seconds * SR); i++) {
        m.process(makeArgs(fr++));
        const bool s = m.outputs[Stridor::SLIP_OUTPUT].getVoltage() > 5.f;
        if (s && !prev) n++;
        prev = s;
    }
    return (int)(n / seconds);
}

static void levelPlane() {
    printf("\n# level across force x velocity (Vrms at the default gain)\n");
    printf("force\\vel");
    const float vels[] = {0.05f, 0.15f, 0.35f, 0.6f, 1.0f};
    for (float v : vels) printf("%9.2f", v);
    printf("\n");
    for (float f : {0.1f, 0.3f, 0.5f, 0.8f, 1.0f}) {
        printf("%9.2f", f);
        for (float v : vels) {
            Stridor m;
            setK(m, Stridor::FORCE_PARAM, f);
            setK(m, Stridor::VEL_PARAM, v);
            const auto b = render(m, 0.3, 0.7);
            printf("%9.3f", rmsOf(b));
        }
        printf("\n");
    }
}

static void creakToSqueal() {
    printf("\n# creak to squeal: velocity sweep at force 0.6\n");
    printf("vel      rms      zcr(Hz)   slips/s\n");
    for (float v : {0.02f, 0.05f, 0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 1.0f}) {
        Stridor m;
        setK(m, Stridor::FORCE_PARAM, 0.6f);
        setK(m, Stridor::VEL_PARAM, v);
        const auto b = render(m, 0.3, 1.0);
        Stridor m2;
        setK(m2, Stridor::FORCE_PARAM, 0.6f);
        setK(m2, Stridor::VEL_PARAM, v);
        printf("%-8.2f %-8.3f %-9.0f %d\n", v, rmsOf(b), zcr(b), slipCount(m2, 1.0));
    }
}

static void materialSweep() {
    printf("\n# material sweep (rubber -> wood -> metal -> glass)\n");
    printf("mat      rms      zcr(Hz)\n");
    for (int i = 0; i <= 6; i++) {
        const float x = i / 6.f;
        Stridor m;
        setK(m, Stridor::MAT_PARAM, x);
        setK(m, Stridor::FORCE_PARAM, 0.6f);
        setK(m, Stridor::VEL_PARAM, 0.3f);
        const auto b = render(m, 0.3, 1.0);
        printf("%-8.2f %-8.3f %-9.0f\n", x, rmsOf(b), zcr(b));
    }
}

static void cpu() {
    Stridor m;
    setK(m, Stridor::FORCE_PARAM, 0.8f);
    setK(m, Stridor::VEL_PARAM, 0.5f);
    long fr = 0;
    for (int i = 0; i < 4800; i++) m.process(makeArgs(fr++));
    const int n = (int)(SR * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) m.process(makeArgs(fr++));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n# cpu: %.2f%% of one core at %g Hz\n", 100.0 * dt / (n / SR), SR);
}

int main() {
    rack::random::init();
    levelPlane();
    creakToSqueal();
    materialSweep();
    cpu();
    return 0;
}
