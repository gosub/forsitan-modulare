// sdt_material.hpp — a material axis for the modal resonator.
//
// Not part of the SDT, which leaves the modal frequencies, decays and gains to
// the patch. Four sets of them, and a continuous morph across the four, so
// "material" is one knob instead of eighteen numbers.
//
// Rubber is nearly harmonic and dead; wood is the same shape with more life;
// metal is a free bar, wide and inharmonic; glass sits between the two and
// rings the longest.
#pragma once

#include "sdt_common.hpp"
#include "sdt_resonator.hpp"

namespace sdt {

static const int kMaterialModes = 6;

struct Material {
    double ratio[kMaterialModes];
    double decay[kMaterialModes];
    double gain[kMaterialModes];
    double decayScale;
};

static const Material kMaterials[4] = {
    // rubber
    {{1.0, 1.41, 2.10, 2.90, 3.61, 4.40},
     {1.0, 0.80, 0.62, 0.50, 0.40, 0.33},
     {1.0, 0.55, 0.30, 0.18, 0.10, 0.06},
     0.25},
    // wood
    {{1.0, 1.72, 2.63, 3.42, 4.51, 5.30},
     {1.0, 0.90, 0.78, 0.66, 0.55, 0.46},
     {1.0, 0.70, 0.50, 0.36, 0.25, 0.17},
     1.0},
    // metal
    {{1.0, 2.76, 5.40, 8.93, 13.34, 18.64},
     {1.0, 0.95, 0.88, 0.80, 0.72, 0.64},
     {1.0, 0.85, 0.70, 0.56, 0.44, 0.33},
     3.0},
    // glass
    {{1.0, 2.32, 4.25, 6.63, 9.38, 12.22},
     {1.0, 0.97, 0.93, 0.88, 0.82, 0.75},
     {1.0, 0.80, 0.62, 0.47, 0.34, 0.24},
     5.0},
};

// Ratios and decay scales interpolate in the log domain so the sweep sounds
// even rather than crowding at one end.
inline void blendMaterial(double x, Material& out) {
    x = fclip(x, 0.0, 1.0) * 3.0;
    const int i = std::min(2, (int)x);
    const double t = x - i;
    const Material& a = kMaterials[i];
    const Material& b = kMaterials[i + 1];
    for (int m = 0; m < kMaterialModes; m++) {
        out.ratio[m] = std::exp((1.0 - t) * std::log(a.ratio[m]) + t * std::log(b.ratio[m]));
        out.decay[m] = (1.0 - t) * a.decay[m] + t * b.decay[m];
        out.gain[m] = (1.0 - t) * a.gain[m] + t * b.gain[m];
    }
    out.decayScale = std::exp((1.0 - t) * std::log(a.decayScale) + t * std::log(b.decayScale));
}

// Apply a material to a six-mode resonator at a given fundamental and decay.
inline void applyMaterial(Resonator& r, const Material& mat, double f0, double decay,
                          double gainScale = 1.0) {
    for (int m = 0; m < kMaterialModes; m++) {
        r.setFrequency(m, f0 * mat.ratio[m]);
        r.setDecay(m, decay * mat.decayScale * mat.decay[m]);
        r.setGain(0, m, gainScale * mat.gain[m]);
    }
}

}  // namespace sdt
