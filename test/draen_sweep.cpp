// draen_sweep - offline octave sweep of the dræn engine banks.
//
// For every engine it renders amp=1 audio at 27.5·2^k Hz (k = 0..7) and
// reports, as CSV on stdout: per-channel DC (mean), AC RMS (DC removed),
// peak, and any non-finite sample count. Used to calibrate the per-engine
// makeup gains and to catch DC leaks; see test/README.md.
//
//   ./draen_sweep [draen|hyf]     (default: both banks)
#include <rack.hpp>
#include "../src/draen_alt_engines.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
using namespace draen;

static void sweepBank(const char* bank, std::vector<std::unique_ptr<DroneEngine>> es) {
    const float sr = 48000.f, st = 1.f / sr;
    const int W = (int)(5 * sr);   // warmup, settles slow LFOs and reverbs
    const int N = (int)(8 * sr);   // measurement window
    std::vector<float> bl(N), br(N);
    for (auto& e : es) {
        for (int k = 0; k < 8; ++k) {
            float hz = 27.5f * (float)(1 << k);
            e->init(4242u, sr);
            for (int i = 0; i < W; ++i) { float l, r; e->process(hz, 1.f, st, l, r); }
            double sl = 0, sr_ = 0, ss = 0; float pk = 0; int nans = 0;
            for (int i = 0; i < N; ++i) {
                float l, r; e->process(hz, 1.f, st, l, r);
                if (!std::isfinite(l) || !std::isfinite(r)) { nans++; l = r = 0.f; }
                bl[i] = l; br[i] = r;
                sl += l; sr_ += r;
                pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
            }
            double ml = sl / N, mr = sr_ / N;
            for (int i = 0; i < N; ++i) {
                double a = bl[i] - ml, b = br[i] - mr;
                ss += a * a + b * b;
            }
            printf("%s,%s,%g,%.5f,%.5f,%.4f,%.3f,%d\n", bank, e->name(), hz,
                   ml, mr, std::sqrt(ss / (2.0 * N)), pk, nans);
            fflush(stdout);
        }
    }
}

int main(int argc, char** argv) {
    const char* which = (argc > 1) ? argv[1] : "";
    printf("bank,engine,hz,dcL,dcR,rms_ac,peak,nans\n");
    if (std::strcmp(which, "hyf") != 0)   sweepBank("draen", makeEngines());
    if (std::strcmp(which, "draen") != 0) sweepBank("hyf", makeAltEngines());
    return 0;
}
