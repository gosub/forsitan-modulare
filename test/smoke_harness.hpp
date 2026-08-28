// smoke_harness - shared scaffolding for the per-module smoke tests.
//
// Each smoke_<module>.cpp includes this header, then the one src/<module>.cpp
// it exercises, drives that module's process() directly and checks for
// non-finite samples, runaway levels and basic expected behavior
// (self-oscillation, loop decay, pluck response).
//
// Prints one CSV row per check: module,check,value,pass
// Exits nonzero if any check fails. SMOKE_MAIN() supplies the main().
#pragma once

#include "../src/forsitan.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

// pluginInstance is normally defined in forsitan.cpp; the harness never
// loads assets so a null plugin is fine.
rack::plugin::Plugin* pluginInstance = nullptr;

static const float SR = 48000.f;
static int failures = 0;

static void report(const char* mod, const char* check, double value, bool pass) {
    printf("%s,%s,%g,%s\n", mod, check, value, pass ? "PASS" : "FAIL");
    if (!pass) failures++;
}

struct Stats {
    double sum = 0, sum2 = 0;
    float peak = 0;
    long nans = 0;
    long n = 0;
    void add(float v) {
        if (!std::isfinite(v)) { nans++; v = 0.f; }
        sum += v; sum2 += v * v;
        peak = std::max(peak, std::fabs(v));
        n++;
    }
    double rms() const { return n ? std::sqrt(sum2 / n) : 0.0; }
};

static Module::ProcessArgs makeArgs(long frame) {
    Module::ProcessArgs args;
    args.sampleRate = SR;
    args.sampleTime = 1.f / SR;
    args.frame = frame;
    return args;
}

// Emits main(). Pass the test entry points to run, in order.
// `--no-header` suppresses the CSV header so `make check` can print it once.
#define SMOKE_MAIN(...)                                              \
    int main(int argc, char** argv) {                                \
        bool header = true;                                          \
        for (int i = 1; i < argc; i++)                               \
            if (!std::strcmp(argv[i], "--no-header")) header = false; \
        rack::random::init();                                        \
        if (header) printf("module,check,value,pass\n");             \
        void (*fns[])() = {__VA_ARGS__};                             \
        for (auto fn : fns) fn();                                    \
        return failures ? 1 : 0;                                     \
    }
