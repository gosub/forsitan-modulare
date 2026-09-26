// shapes.hpp - what nodi's context menu writes into its sliders and switches.
//
// Setups are the manual's quick-start patches as whole-module settings.
// Presets and transforms work on one bank at a time: the f(X) sliders, the
// threshold sliders, the direction switches or the group switches. All of it
// is pure: values in, values out, with the randomness handed in, so the probe
// checks every shape without Rack and the module applies each as one undo
// step.
//
// Slider values are the panel's, 0..1. Directions and groups are the
// engine's (RISE / OFF / FALL, 0..2 = A..C), not the switches' throws.

#pragma once

#include "nodi.hpp"

#include <algorithm>
#include <cmath>

namespace nodi {

// ---------------------------------------------------------------- f(X)

// Where 0 V sits in a range, as the base of every scale.
inline float rangeBase(int range) { return range == RANGE_BIPOLAR ? 0.f : rangeLow(range); }

// A voltage snapped to the nearest semitone and clamped into the range.
inline float snapSemitone(float v, int range) {
    float s = std::round(v * 12.f) / 12.f;
    float lo = rangeLow(range), hi = rangeHigh(range);
    while (s > hi + 1e-6f) s -= 1.f / 12.f;
    while (s < lo - 1e-6f) s += 1.f / 12.f;
    return s;
}

enum ValuePreset {
    VALUE_ZERO, VALUE_UP, VALUE_DOWN,
    VALUE_CHROMATIC, VALUE_MAJOR, VALUE_MINOR, VALUE_MAJOR_PENTA, VALUE_MINOR_PENTA,
    VALUE_TRIAD, VALUE_SEVENTH, VALUE_OCTAVES,
    VALUE_RANDOM_PENTA, VALUE_RANDOM,
    NUM_VALUE_PRESETS
};

inline const char* valuePresetName(int p) {
    static const char* n[NUM_VALUE_PRESETS] = {
        "All at 0 V", "Ramp up", "Ramp down",
        "Chromatic", "Major scale", "Minor scale", "Major pentatonic", "Minor pentatonic",
        "Major triad arpeggio", "Minor seventh arpeggio", "Octaves",
        "Random minor pentatonic", "Random"};
    return n[p];
}

inline bool valuePresetStartsGroup(int p) {
    return p == VALUE_CHROMATIC || p == VALUE_TRIAD || p == VALUE_RANDOM_PENTA;
}

// Semitones above the base, per stage.
inline const int* valueSemitones(int p) {
    static const int t[][kStages] = {
        {0, 1, 2, 3, 4, 5, 6, 7},          // chromatic
        {0, 2, 4, 5, 7, 9, 11, 12},        // major
        {0, 2, 3, 5, 7, 8, 10, 12},        // minor
        {0, 2, 4, 7, 9, 12, 14, 16},       // major pentatonic
        {0, 3, 5, 7, 10, 12, 15, 17},      // minor pentatonic
        {0, 4, 7, 12, 16, 19, 24, 28},     // major triad
        {0, 3, 7, 10, 12, 15, 19, 22},     // minor seventh
        {0, 12, 24, 12, 0, 12, 24, 12},    // octaves
    };
    return t[p - VALUE_CHROMATIC];
}

template <typename Uniform>
inline void valuePreset(int p, int range, float* v, Uniform uniform) {
    const float base = rangeBase(range);
    for (int k = 0; k < kStages; k++) {
        float volts;
        switch (p) {
            case VALUE_ZERO: volts = base; break;
            case VALUE_UP: volts = sliderVolts(k / 7.f, range); break;
            case VALUE_DOWN: volts = sliderVolts((7 - k) / 7.f, range); break;
            case VALUE_RANDOM_PENTA: {
                // Two octaves of the minor pentatonic, or as much as fits.
                static const int penta[10] = {0, 3, 5, 7, 10, 12, 15, 17, 19, 22};
                int i = std::min((int)(uniform() * 10.f), 9);
                volts = base + penta[i] / 12.f;
                break;
            }
            case VALUE_RANDOM: volts = sliderVolts(uniform(), range); break;
            default: volts = base + valueSemitones(p)[k] / 12.f; break;
        }
        if (p >= VALUE_CHROMATIC && p <= VALUE_RANDOM_PENTA) volts = snapSemitone(volts, range);
        v[k] = voltsSlider(volts, range);
    }
}

enum ValueTransform {
    VT_REVERSE, VT_ROTATE_LEFT, VT_ROTATE_RIGHT, VT_MIRROR,
    VT_SEMITONE_UP, VT_SEMITONE_DOWN, VT_OCTAVE_UP, VT_OCTAVE_DOWN, VT_SNAP,
    VT_SHUFFLE, VT_MUTATE,
    NUM_VALUE_TRANSFORMS
};

inline const char* valueTransformName(int t) {
    static const char* n[NUM_VALUE_TRANSFORMS] = {
        "Reverse", "Rotate left", "Rotate right", "Mirror",
        "Transpose up a semitone", "Transpose down a semitone",
        "Transpose up an octave", "Transpose down an octave", "Snap to semitones",
        "Shuffle", "Mutate"};
    return n[t];
}

inline bool valueTransformStartsGroup(int t) {
    return t == VT_SEMITONE_UP || t == VT_SHUFFLE;
}

// The shared moves of any bank of eight.
template <typename T>
inline void reverseBank(T* v) { std::reverse(v, v + kStages); }
template <typename T>
inline void rotateLeft(T* v) { std::rotate(v, v + 1, v + kStages); }
template <typename T>
inline void rotateRight(T* v) { std::rotate(v, v + kStages - 1, v + kStages); }

template <typename T, typename Uniform>
inline void shuffleBank(T* v, Uniform uniform) {
    for (int i = kStages - 1; i > 0; i--) {
        int j = std::min((int)(uniform() * (i + 1)), i);
        std::swap(v[i], v[j]);
    }
}

template <typename Uniform>
inline void valueTransform(int t, int range, float* v, Uniform uniform) {
    auto shift = [&](float volts) {
        for (int k = 0; k < kStages; k++) {
            float lo = rangeLow(range), hi = rangeHigh(range);
            float x = sliderVolts(v[k], range) + volts;
            v[k] = voltsSlider(std::min(std::max(x, lo), hi), range);
        }
    };
    switch (t) {
        case VT_REVERSE: reverseBank(v); break;
        case VT_ROTATE_LEFT: rotateLeft(v); break;
        case VT_ROTATE_RIGHT: rotateRight(v); break;
        case VT_MIRROR:
            for (int k = 0; k < kStages; k++) v[k] = 1.f - v[k];
            break;
        case VT_SEMITONE_UP: shift(1.f / 12.f); break;
        case VT_SEMITONE_DOWN: shift(-1.f / 12.f); break;
        case VT_OCTAVE_UP: shift(1.f); break;
        case VT_OCTAVE_DOWN: shift(-1.f); break;
        case VT_SNAP:
            for (int k = 0; k < kStages; k++)
                v[k] = voltsSlider(snapSemitone(sliderVolts(v[k], range), range), range);
            break;
        case VT_SHUFFLE: shuffleBank(v, uniform); break;
        case VT_MUTATE:
            // Two stages move a semitone or two, up or down, onto semitones.
            for (int n = 0; n < 2; n++) {
                int k = std::min((int)(uniform() * kStages), kStages - 1);
                int steps = uniform() < 0.5f ? 1 : 2;
                float d = (uniform() < 0.5f ? -steps : steps) / 12.f;
                float x = snapSemitone(sliderVolts(v[k], range) + d, range);
                v[k] = voltsSlider(x, range);
            }
            break;
    }
}

// ---------------------------------------------------------------- thresholds

enum ThresholdPreset {
    TH_EVEN, TH_SWING, TH_TRESILLO, TH_ACCELERANDO, TH_RITARDANDO, TH_RANDOM,
    NUM_THRESHOLD_PRESETS
};

inline const char* thresholdPresetName(int p) {
    static const char* n[NUM_THRESHOLD_PRESETS] = {
        "Even", "Swing (2:1)", "3-3-2", "Accelerando", "Ritardando", "Random"};
    return n[p];
}

// The rhythm each preset stands for, as relative step lengths.
template <typename Uniform>
inline void presetLengths(int p, float* len, Uniform uniform) {
    static const float tresillo[kStages] = {3, 3, 2, 3, 3, 2, 2, 2};
    for (int k = 0; k < kStages; k++) {
        switch (p) {
            case TH_SWING: len[k] = k % 2 == 0 ? 2.f : 1.f; break;
            case TH_TRESILLO: len[k] = tresillo[k]; break;
            case TH_ACCELERANDO: len[k] = (float)(kStages - k); break;
            case TH_RITARDANDO: len[k] = (float)(k + 1); break;
            case TH_RANDOM: len[k] = 0.2f + 0.8f * uniform(); break;
            default: len[k] = 1.f; break;
        }
    }
}

// Relative lengths as LENGTH sliders: their mean at half, none over the top.
inline void lengthsToSliders(const float* len, float* s) {
    float sum = 0.f, top = 0.f;
    for (int k = 0; k < kStages; k++) {
        sum += len[k];
        top = std::max(top, len[k]);
    }
    if (sum <= 0.f) {
        for (int k = 0; k < kStages; k++) s[k] = 0.f;
        return;
    }
    float scale = 0.5f * kStages / sum;
    if (top * scale > 1.f) scale = 1.f / top;
    for (int k = 0; k < kStages; k++) s[k] = len[k] * scale;
}

// LENGTH sliders as the POSIT. sliders that put every threshold where it was.
inline void lengthsToPositions(const float* len, float* pos) {
    float sum = 0.f;
    for (int k = 0; k < kStages; k++) sum += std::max(len[k], 0.f);
    float cum = 0.f;
    for (int k = 0; k < kStages; k++) {
        pos[k] = sum > 1e-6f ? cum / sum : 0.5f;
        cum += std::max(len[k], 0.f);
    }
}

// POSIT. sliders as LENGTH sliders. LENGTH always starts at the bottom and
// runs in stage order, so this is exact when stage 1 is at the bottom and the
// rest ascend. A stage below one before it cannot keep its place: it gets
// zero length, landing on the next stage in order, and a stage of zero
// length is the one passed over, so the stages in order all still play.
inline void positionsToLengths(const float* pos, float* s) {
    float at[kStages + 1];
    at[kStages] = 1.f;
    float top = 0.f;
    bool inOrder[kStages];
    for (int k = 0; k < kStages; k++) {
        inOrder[k] = k == 0 || pos[k] >= top;
        if (inOrder[k]) top = k == 0 ? 0.f : pos[k];
    }
    for (int k = kStages - 1; k >= 0; k--)
        at[k] = k == 0 ? 0.f : inOrder[k] ? pos[k] : at[k + 1];
    float len[kStages];
    float longest = 0.f;
    for (int k = 0; k < kStages; k++) {
        len[k] = std::max(at[k + 1] - at[k], 0.f);
        longest = std::max(longest, len[k]);
    }
    // Keep the ratios, which are all LENGTH reads: a slider for an eighth of
    // the space sits at half, unless the longest would then overflow.
    float scale = longest > 0.f ? std::min(4.f, 1.f / longest) : 0.f;
    for (int k = 0; k < kStages; k++) s[k] = len[k] * scale;
}

template <typename Uniform>
inline void thresholdPreset(int p, bool length, float* s, Uniform uniform) {
    float len[kStages];
    presetLengths(p, len, uniform);
    if (length) lengthsToSliders(len, s);
    else if (p == TH_RANDOM)
        for (int k = 0; k < kStages; k++) s[k] = uniform();
    else lengthsToPositions(len, s);
}

enum ThresholdTransform {
    TT_REVERSE, TT_ROTATE_LEFT, TT_ROTATE_RIGHT, TT_MUTATE,
    NUM_THRESHOLD_TRANSFORMS
};

inline const char* thresholdTransformName(int t) {
    static const char* n[NUM_THRESHOLD_TRANSFORMS] = {
        "Reverse", "Rotate left", "Rotate right", "Mutate"};
    return n[t];
}

template <typename Uniform>
inline void thresholdTransform(int t, float* s, Uniform uniform) {
    switch (t) {
        case TT_REVERSE: reverseBank(s); break;
        case TT_ROTATE_LEFT: rotateLeft(s); break;
        case TT_ROTATE_RIGHT: rotateRight(s); break;
        case TT_MUTATE:
            // Two sliders nudged by up to a tenth of their travel.
            for (int n = 0; n < 2; n++) {
                int k = std::min((int)(uniform() * kStages), kStages - 1);
                s[k] = std::min(std::max(s[k] + 0.2f * (uniform() - 0.5f), 0.f), 1.f);
            }
            break;
    }
}

// ---------------------------------------------------------------- switches

enum DirectionPreset {
    DIR_ALL_RISE, DIR_ALL_FALL, DIR_SEQUENCER, DIR_CRUSHER, DIR_ALTERNATE,
    NUM_DIRECTION_PRESETS
};

inline const char* directionPresetName(int p) {
    static const char* n[NUM_DIRECTION_PRESETS] = {
        "All rise", "All fall", "Sequencer: 1 fall, the rest rise",
        "Waveshaper: 1-4 rise, 5-8 fall", "Alternate rise and fall"};
    return n[p];
}

inline void directionPreset(int p, int* d) {
    for (int k = 0; k < kStages; k++) {
        switch (p) {
            case DIR_ALL_RISE: d[k] = RISE; break;
            case DIR_ALL_FALL: d[k] = FALL; break;
            case DIR_SEQUENCER: d[k] = k == 0 ? FALL : RISE; break;
            case DIR_CRUSHER: d[k] = k < 4 ? RISE : FALL; break;
            default: d[k] = k % 2 == 0 ? RISE : FALL; break;
        }
    }
}

enum SwitchTransform {
    ST_REVERSE, ST_ROTATE_LEFT, ST_ROTATE_RIGHT, ST_FLIP,
    NUM_SWITCH_TRANSFORMS
};

inline const char* directionTransformName(int t) {
    static const char* n[NUM_SWITCH_TRANSFORMS] = {
        "Reverse", "Rotate left", "Rotate right", "Swap rise and fall"};
    return n[t];
}

inline void directionTransform(int t, int* d) {
    switch (t) {
        case ST_REVERSE: reverseBank(d); break;
        case ST_ROTATE_LEFT: rotateLeft(d); break;
        case ST_ROTATE_RIGHT: rotateRight(d); break;
        case ST_FLIP:
            for (int k = 0; k < kStages; k++)
                d[k] = d[k] == RISE ? FALL : d[k] == FALL ? RISE : OFF;
            break;
    }
}

enum GroupPreset {
    GROUP_ALL_A, GROUP_CYCLE, GROUP_PAIRS, GROUP_THIRDS, GROUP_HALVES, GROUP_RANDOM,
    NUM_GROUP_PRESETS
};

inline const char* groupPresetName(int p) {
    static const char* n[NUM_GROUP_PRESETS] = {
        "All A", "A B C A B C A B", "A A B B C C A A", "A A A B B B C C", "A A A A B B B B",
        "Random"};
    return n[p];
}

template <typename Uniform>
inline void groupPreset(int p, int* g, Uniform uniform) {
    static const int pairs[kStages] = {0, 0, 1, 1, 2, 2, 0, 0};
    static const int thirds[kStages] = {0, 0, 0, 1, 1, 1, 2, 2};
    for (int k = 0; k < kStages; k++) {
        switch (p) {
            case GROUP_ALL_A: g[k] = 0; break;
            case GROUP_CYCLE: g[k] = k % kGroups; break;
            case GROUP_PAIRS: g[k] = pairs[k]; break;
            case GROUP_THIRDS: g[k] = thirds[k]; break;
            case GROUP_HALVES: g[k] = k < 4 ? 0 : 1; break;
            default: g[k] = std::min((int)(uniform() * kGroups), kGroups - 1); break;
        }
    }
}

inline const char* groupTransformName(int t) {
    static const char* n[NUM_SWITCH_TRANSFORMS] = {
        "Reverse", "Rotate left", "Rotate right", "Cycle A -> B -> C -> A"};
    return n[t];
}

inline void groupTransform(int t, int* g) {
    switch (t) {
        case ST_REVERSE: reverseBank(g); break;
        case ST_ROTATE_LEFT: rotateLeft(g); break;
        case ST_ROTATE_RIGHT: rotateRight(g); break;
        case ST_FLIP:
            for (int k = 0; k < kStages; k++) g[k] = (g[k] + 1) % kGroups;
            break;
    }
}

// ---------------------------------------------------------------- setups

enum SetupId {
    SETUP_LENGTH_SEQ, SETUP_POSIT_SEQ, SETUP_QUANTIZER, SETUP_VCO, SETUP_CRUSHER, SETUP_MIXER,
    NUM_SETUPS
};

inline const char* setupName(int s) {
    static const char* n[NUM_SETUPS] = {
        "Variable-length sequencer", "Variable-position sequencer",
        "Quantizer: four notes, either direction", "Graphic VCO",
        "Waveshaper / bitcrusher (audio into X)", "Temporal mixer (A, B, C into the switch)"};
    return n[s];
}

// Everything a setup sets. The clock rate is in Hz, on the range `fast`.
struct Setup {
    float hz = 0.25f;
    bool fast = false;
    bool once = false;
    bool length = true;
    int range = RANGE_HALF;
    float value[kStages];
    float threshold[kStages];
    int direction[kStages];
    int group[kStages];
};

inline void setup(int id, Setup& s) {
    auto none = []() { return 0.5f; };
    groupPreset(GROUP_CYCLE, s.group, none);
    switch (id) {
        case SETUP_LENGTH_SEQ:
        case SETUP_POSIT_SEQ:
            s.length = id == SETUP_LENGTH_SEQ;
            thresholdPreset(TH_EVEN, s.length, s.threshold, none);
            directionPreset(DIR_SEQUENCER, s.direction);
            valuePreset(VALUE_MAJOR, s.range, s.value, none);
            break;
        case SETUP_QUANTIZER: {
            // Four regions across the space, each entered from below through
            // a RISE stage at its bottom and from above through a FALL stage
            // at its top, both mapped to the region's note.
            s.length = false;
            static const int notes[4] = {0, 4, 7, 12};
            for (int r = 0; r < 4; r++) {
                s.threshold[r] = r / 4.f;
                s.direction[r] = RISE;
                s.threshold[4 + r] = (r + 1) / 4.f;
                s.direction[4 + r] = FALL;
                float v = voltsSlider(notes[r] / 12.f, s.range);
                s.value[r] = s.value[4 + r] = v;
            }
            break;
        }
        case SETUP_VCO:
            s.hz = 110.f;
            s.fast = true;
            s.range = RANGE_BIPOLAR;
            thresholdPreset(TH_EVEN, true, s.threshold, none);
            directionPreset(DIR_ALL_RISE, s.direction);
            for (int k = 0; k < kStages; k++)
                s.value[k] = 0.5f + 0.45f * std::sin(2.f * 3.14159265f * (k + 0.5f) / kStages);
            break;
        case SETUP_CRUSHER: {
            // The manual's two-bit bitcrusher, sliders mirrored about the middle.
            static const float at[kStages] = {0.2f, 0.4f, 0.6f, 0.8f, 0.8f, 0.6f, 0.4f, 0.2f};
            s.length = false;
            s.range = RANGE_BIPOLAR;
            directionPreset(DIR_CRUSHER, s.direction);
            for (int k = 0; k < kStages; k++) s.threshold[k] = s.value[k] = at[k];
            break;
        }
        case SETUP_MIXER:
            s.hz = 110.f;
            s.fast = true;
            for (int k = 0; k < kStages; k++) {
                s.threshold[k] = k < 3 ? 0.5f : 0.f;
                s.direction[k] = k < 3 ? RISE : OFF;
                s.group[k] = k < 3 ? k : 0;
                s.value[k] = 0.f;
            }
            break;
    }
}

}  // namespace nodi
