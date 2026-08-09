// ruina_probe — measurement harness for the object under load.
//
// What it reports:
//   - lifetime against load and toughness, which is the module's contract:
//     the cube law on the load and the seconds on the knob;
//   - the lifetime distribution against brittleness, as a coefficient of
//     variation, which is the whole point of the Weibull draw: at the low end
//     the object may go at any moment, at the high end it goes when due;
//   - the acoustic emission ramp: event rate and level over the last part of
//     an object's life, i.e. the thing the patch listens for;
//   - the collapse: how long it lasts and how loud;
//   - CPU.
//
// Built by `make all`, not run by `make check`.

#include "smoke_harness.hpp"
#include "../src/ruina.cpp"

#include <chrono>
#include <vector>

// Run until the object fails, returning the lifetime in seconds (or the cap).
static double lifetime(Ruina& m, double capS, long& fr) {
    const int cap = (int)(capS * SR);
    for (int i = 0; i < cap; i++) {
        m.process(makeArgs(fr++));
        if (m.outputs[Ruina::BREAK_OUTPUT].getVoltage() > 5.f) return i / (double)SR;
    }
    return capS;
}

static void lifeVsLoad() {
    printf("\n# mean lifetime (s) against load, toughness 0.5 (nominally 3.2 s at full load)\n");
    printf("load    life(s)  cube-law prediction\n");
    for (float l : {0.3f, 0.4f, 0.6f, 0.8f, 1.0f}) {
        double sum = 0;
        const int reps = 6;
        for (int k = 0; k < reps; k++) {
            Ruina m;
            m.params[Ruina::LOAD_PARAM].setValue(l);
            m.params[Ruina::BRIT_PARAM].setValue(1.f);   // least random
            long fr = 0;
            sum += lifetime(m, 400.0, fr);
        }
        printf("%-7.2f %-8.2f %.2f\n", l, sum / reps, 3.16 / (l * l * l));
    }
}

static void lifeVsTough() {
    printf("\n# mean lifetime (s) against toughness, load 1.0\n");
    printf("tough   life(s)  knob says\n");
    for (float t : {0.f, 0.25f, 0.5f, 0.75f}) {
        double sum = 0;
        const int reps = 6;
        for (int k = 0; k < reps; k++) {
            Ruina m;
            m.params[Ruina::LOAD_PARAM].setValue(1.f);
            m.params[Ruina::TOUGH_PARAM].setValue(t);
            m.params[Ruina::BRIT_PARAM].setValue(1.f);
            long fr = 0;
            sum += lifetime(m, 400.0, fr);
        }
        printf("%-7.2f %-8.2f %.2f\n", t, sum / reps, 0.1 * std::pow(1000.0, t));
    }
}

static void lifeSpread() {
    printf("\n# lifetime spread against brittleness (load 1, tough 0.3)\n");
    printf("# CV is the coefficient of variation; Weibull(k) predicts it exactly\n");
    printf("brit    k        mean(s)  cv       weibull cv\n");
    for (float b : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        const double k = 0.5 * std::pow(40.0, b);
        std::vector<double> lives;
        for (int i = 0; i < 24; i++) {
            Ruina m;
            m.params[Ruina::LOAD_PARAM].setValue(1.f);
            m.params[Ruina::TOUGH_PARAM].setValue(0.3f);
            m.params[Ruina::BRIT_PARAM].setValue(b);
            long fr = 0;
            lives.push_back(lifetime(m, 200.0, fr));
        }
        double s = 0, s2 = 0;
        for (double v : lives) { s += v; s2 += v * v; }
        const double mean = s / lives.size();
        const double sd = std::sqrt(std::max(0.0, s2 / lives.size() - mean * mean));
        // cv of Weibull(k) = sqrt(G(1+2/k)/G(1+1/k)^2 - 1)
        const double g1 = std::tgamma(1.0 + 1.0 / k), g2 = std::tgamma(1.0 + 2.0 / k);
        printf("%-7.2f %-8.2f %-8.2f %-8.3f %.3f\n", b, k, mean, sd / std::max(mean, 1e-9),
               std::sqrt(std::max(0.0, g2 / (g1 * g1) - 1.0)));
    }
}

static void emissionRamp() {
    printf("\n# acoustic emission through one object's life (load 1, tough 0.5)\n");
    printf("# the rate climbs with damage squared and the events start to correlate\n");
    printf("strain(V)  ev/s     rms\n");
    Ruina m;
    m.params[Ruina::LOAD_PARAM].setValue(1.f);
    m.params[Ruina::BRIT_PARAM].setValue(1.f);
    long fr = 0;
    double nextReport = 1.0, s2 = 0;
    int events = 0, n = 0;
    const int refractoryFull = (int)(m.engine.refractory * SR);
    for (int i = 0; i < (int)(400 * SR); i++) {
        m.process(makeArgs(fr++));
        const double y = m.outputs[Ruina::AUDIO_OUTPUT].getVoltage();
        s2 += y * y;
        n++;
        // The refractory latch is at its full value only on the sample an
        // event fired, which is a cleaner count than thresholding the audio.
        if (m.engine.refractoryLeft == refractoryFull) events++;
        if (m.outputs[Ruina::BREAK_OUTPUT].getVoltage() > 5.f) break;
        if (m.outputs[Ruina::STRAIN_OUTPUT].getVoltage() >= nextReport) {
            printf("%-10.1f %-8.0f %.3f\n", nextReport, events / (n / (double)SR),
                   std::sqrt(s2 / n));
            nextReport += 1.0;
            events = 0;
            s2 = 0;
            n = 0;
        }
    }
}

static void collapse() {
    printf("\n# the collapse itself\n");
    Ruina m;
    m.params[Ruina::LOAD_PARAM].setValue(1.f);
    m.params[Ruina::TOUGH_PARAM].setValue(0.f);
    m.params[Ruina::BRIT_PARAM].setValue(1.f);
    long fr = 0;
    lifetime(m, 60.0, fr);
    double s2 = 0, peak = 0;
    int n = 0;
    while (m.failing && n < (int)(4 * SR)) {
        m.process(makeArgs(fr++));
        const double y = m.outputs[Ruina::AUDIO_OUTPUT].getVoltage();
        s2 += y * y;
        peak = std::max(peak, std::fabs(y));
        n++;
    }
    printf("duration %.1f ms, rms %.2f V, peak %.2f V\n", 1000.0 * n / SR,
           std::sqrt(s2 / std::max(1, n)), peak);
}

static void cpu() {
    Ruina m;
    m.params[Ruina::LOAD_PARAM].setValue(0.9f);
    m.params[Ruina::TOUGH_PARAM].setValue(0.6f);
    long fr = 0;
    for (int i = 0; i < (int)(4 * SR); i++) m.process(makeArgs(fr++));
    const int n = (int)(SR * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) m.process(makeArgs(fr++));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n# cpu: %.2f%% of one core at %g Hz\n", 100.0 * dt / (n / SR), SR);
}

int main() {
    rack::random::init();
    lifeVsLoad();
    lifeVsTough();
    lifeSpread();
    emissionRamp();
    collapse();
    cpu();
    return 0;
}
