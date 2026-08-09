// crepitus_probe — measurement harness for the fracture point process.
//
// What it reports:
//   - event rate and level across the DRIVE x CRIT plane, which is what the
//     two knobs are for and how their ranges were chosen;
//   - the avalanche statistics: how bursty the texture is (envelope crest)
//     and where the process parks its own integrity, which is the claim that
//     the supercritical setting self-organises rather than running away;
//   - level against the remaining knobs;
//   - CPU.
//
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/crepitus.cpp"

#include <chrono>
#include <vector>

struct Run {
    double rms = 0, peak = 0, crest = 0, integrity = 0, integrityMin = 1;
    int events = 0;
    double seconds = 0;
};

static Run render(Crepitus& m, double settleS, double measS) {
    long fr = 0;
    for (int i = 0; i < (int)(settleS * SR); i++) m.process(makeArgs(fr++));
    Run r;
    r.seconds = measS;
    double s2 = 0, env = 0, envSum = 0, envMax = 0, iSum = 0;
    const int n = (int)(measS * SR);
    bool prev = false;
    for (int i = 0; i < n; i++) {
        m.process(makeArgs(fr++));
        const double y = m.outputs[Crepitus::AUDIO_OUTPUT].getVoltage();
        s2 += y * y;
        r.peak = std::max(r.peak, std::fabs(y));
        env += (std::fabs(y) - env) * 0.0005;
        envSum += env;
        envMax = std::max(envMax, env);
        iSum += m.engine.integrity;
        r.integrityMin = std::min(r.integrityMin, m.engine.integrity);
        const bool e = m.outputs[Crepitus::EVENT_OUTPUT].getVoltage() > 5.f;
        if (e && !prev) r.events++;
        prev = e;
    }
    r.rms = std::sqrt(s2 / n);
    r.crest = envMax / std::max(envSum / n, 1e-12);
    r.integrity = iSum / n;
    return r;
}

static void drivePlane() {
    printf("\n# event rate (per second) and level over drive x crit\n");
    const float crits[] = {0.f, 0.6f, 1.f, 1.6f, 2.5f};
    printf("drive\\crit");
    for (float c : crits) printf("%14.1f", c);
    printf("\n");
    for (float d : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        printf("%10.2f", d);
        for (float c : crits) {
            Crepitus m;
            m.params[Crepitus::DRIVE_PARAM].setValue(d);
            m.params[Crepitus::CRIT_PARAM].setValue(c);
            const Run r = render(m, 1.0, 4.0);
            printf("  %6.0f/%-5.2f", r.events / r.seconds, r.rms);
        }
        printf("\n");
    }
}

static void avalanches() {
    printf("\n# avalanche shape at a slow drive: crest > 1 is bursty,\n");
    printf("# and above criticality the process should park at integrity ~ 1/crit\n");
    printf("crit    ev/s     crest    integrity  1/crit   min\n");
    for (float c : {0.3f, 0.9f, 1.2f, 1.6f, 2.0f, 2.5f, 3.0f}) {
        Crepitus m;
        m.params[Crepitus::DRIVE_PARAM].setValue(0.3f);
        m.params[Crepitus::CRIT_PARAM].setValue(c);
        const Run r = render(m, 2.0, 10.0);
        printf("%-7.1f %-8.0f %-8.2f %-10.3f %-8.3f %.3f\n", c, r.events / r.seconds, r.crest,
               r.integrity, c > 1.f ? 1.f / c : 1.f, r.integrityMin);
    }
}

static void knobSweep(const char* name, int id, float lo, float hi) {
    printf("\n# %s\n", name);
    printf("value    ev/s     rms      peak\n");
    for (int i = 0; i <= 4; i++) {
        const float v = lo + (hi - lo) * i / 4.f;
        Crepitus m;
        m.params[id].setValue(v);
        const Run r = render(m, 1.0, 3.0);
        printf("%-8.2f %-8.0f %-8.3f %.3f\n", v, r.events / r.seconds, r.rms, r.peak);
    }
}

static void cpu() {
    Crepitus m;
    m.params[Crepitus::DRIVE_PARAM].setValue(0.8f);
    m.params[Crepitus::CRIT_PARAM].setValue(1.2f);
    long fr = 0;
    for (int i = 0; i < 4800; i++) m.process(makeArgs(fr++));
    const int n = (int)(SR * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) m.process(makeArgs(fr++));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n# cpu: %.2f%% of one core at %g Hz (dense setting)\n", 100.0 * dt / (n / SR), SR);
}

int main() {
    rack::random::init();
    drivePlane();
    avalanches();
    knobSweep("energy", Crepitus::ENERGY_PARAM, 0.f, 1.f);
    knobSweep("hardness", Crepitus::HARD_PARAM, 0.f, 1.f);
    knobSweep("fragment size", Crepitus::SIZE_PARAM, 0.f, 1.f);
    knobSweep("decay", Crepitus::DECAY_PARAM, 0.f, 1.f);
    knobSweep("material", Crepitus::MAT_PARAM, 0.f, 1.f);
    cpu();
    return 0;
}
