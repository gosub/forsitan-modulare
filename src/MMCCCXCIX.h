#pragma once
// PT2399.h — VCV Rack 2 plugin
// Core DSP ported from schollz/pt2399-sc (onebitdelay) with additions:
//   - Feedback send/return jack with dry/wet mix
//   - CV inputs for all main parameters
//
// Signal path:
//   input
//     → [inputFilter2nd + inputPole1]  pre-ADC biquad+pole LP (brightness)
//     → [softClip]                     VCO-dependent saturation
//     → [ΔΣ 2nd order, leaky]          real delta-sigma at dsmClockHz
//     → [BitRing 44000×OS bits]        the actual chip RAM
//     → [ZOH → demodState 2-pole]      demodulation inside DSM loop
//     → [outputFilter2nd]              post-DAC biquad LP
//     → [dcBlock]                      HP ~10 Hz on wet signal
//     → SEND OUTPUT ──→ (external fx) ──→ RETURN INPUT
//     → [feedbackHpf]                  HP on feedback path (cap model)
//     → mix(internal_fb, return_fb, fbLoopMix)
//     → back to input sum

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pt2399 {

constexpr float kPi  = 3.14159265358979f;
constexpr double kPiD = 3.14159265358979;

inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

inline float dbToGain(float db, float minusInfDb = -96.f) {
    if (db <= minusInfDb) return 0.f;
    return std::pow(10.f, db * 0.05f);
}

// ─── Biquad lowpass (direct form II transposed) ───────────────────────────────
class Biquad {
public:
    void reset() { z1_ = z2_ = 0.f; }

    void setLowpass(float sr, float fc, float q) {
        fc = clampf(fc, 1.f, sr * 0.49f);
        q  = std::max(1e-6f, q);
        const float w0    = 2.f * kPi * fc / sr;
        const float cosw0 = std::cos(w0);
        const float sinw0 = std::sin(w0);
        const float alpha = sinw0 / (2.f * q);
        const float b0    = (1.f - cosw0) * 0.5f;
        const float b1    =  1.f - cosw0;
        const float b2    = (1.f - cosw0) * 0.5f;
        const float a0    =  1.f + alpha;
        const float a1    = -2.f * cosw0;
        const float a2    =  1.f - alpha;
        b0_ = b0/a0; b1_ = b1/a0; b2_ = b2/a0;
        a1_ = a1/a0; a2_ = a2/a0;
    }

    float process(float x) {
        const float y = b0_*x + z1_;
        z1_ = b1_*x - a1_*y + z2_;
        z2_ = b2_*x - a2_*y;
        return y;
    }

private:
    float b0_=1,b1_=0,b2_=0,a1_=0,a2_=0,z1_=0,z2_=0;
};

// ─── Linear parameter smoother (avoids zipper noise on knob changes) ─────────
class LinearSmoother {
public:
    void reset(float sr, float secs, float init = 0.f) {
        current_ = target_ = init;
        step_ = 1.f / std::max(1.f, sr * std::max(0.f, secs));
    }
    void setTarget(float t) { target_ = t; }
    float next() {
        current_ += (target_ - current_) * step_;
        return current_;
    }
    float current() const { return current_; }
private:
    float current_=0, target_=0, step_=1;
};

// ─── One-pole compressor (soft limiting on output) ────────────────────────────
class OnePoleCompressor {
public:
    void prepare(float sr) {
        sr_ = std::max(1.f, sr);
        env_ = 0.f;
        const float atkSec = 0.008f, relSec = 0.220f;
        atkC_ = std::exp(-1.f / (sr_ * atkSec));
        relC_ = std::exp(-1.f / (sr_ * relSec));
    }
    void reset() { env_ = 0.f; }
    float process(float x) {
        const float a = std::fabs(x);
        env_ = ((a > env_) ? atkC_ : relC_) * env_ + (1.f - ((a > env_) ? atkC_ : relC_)) * a;
        float gain = 1.f;
        if (env_ > thresh_)
            gain = std::pow(env_ / thresh_, 1.f/ratio_ - 1.f);
        return x * gain;
    }
private:
    float sr_=48000, thresh_=dbToGain(-18.f), ratio_=4.f;
    float env_=0, atkC_=0, relC_=0;
};

// ─── PT2399 DSP core (schollz / onebitdelay, ported verbatim + extensions) ───
class PT2399Core {
public:
    explicit PT2399Core(int oversampling = 16) : osFactor_(oversampling) {}

    void prepare(double sr) {
        fs_ = std::max(1.0, sr);
        inputFilter2nd_.setLowpass(float(fs_), inputOutputFcHz_, 0.9f);
        inputPole1Alpha_ = 1.f - std::exp(-2.f * kPi * inputOutputFcHz_ / float(fs_));
        dcBlockR_ = 1.f - 2.f * kPi * 10.f / float(fs_);
        bitRing_.resize(44000 * osFactor_);
        updateVCO();
        updateDemodAlpha();
        updateFeedbackHpf();
        reset();
    }

    void reset() {
        i1_=i2_=0; ramPhase_=0; dacBit_=0;
        bitRing_.reset();
        zohBit_=0; ramHoldValue_=0;
        demodState_=demodState2_=0;
        inputFilter2nd_.reset(); outputFilter2nd_.reset();
        inputPole1State_=0;
        dcBlockX1_=dcBlockY1_=0;
        feedbackSample_=0;
        feedbackHpfX1_=feedbackHpfY1_=0;
        prevInput_=0;
        rngState_=0x12345678u;
    }

    // ── setters ──────────────────────────────────────────────────────────────
    void setDelayResistanceKOhm(float rK) {
        rKOhm_ = clampf(rK, 0.5f, 100.f);
        updateVCO(); updateDemodAlpha();
    }
    void setFeedback(float g)          { feedbackGain_ = clampf(g, 0.f, 2.f); }
    void setFeedbackHighPassHz(float hz){ feedbackHpfHz_ = clampf(hz, 10.f, 440.f); updateFeedbackHpf(); }
    void setC3nF(float nF)             { c3nF_ = clampf(nF, 22.f, 150.f); integGain_ = 100.f / c3nF_; }
    void setC6nF(float nF)             { c6nF_ = clampf(nF, 22.f, 150.f); updateDemodAlpha(); }
    void setBoostActivated(bool b)     { boostActivated_ = b; updateVCO(); }
    void setBrightness(float amt) {
        brightness_ = clampf(amt, 0.f, 1.f);
        inputOutputFcHz_ = interpLog(kBaseInFc, kMaxInFc, brightness_);
        demodFcScale_    = interpLog(kBaseDemodFc, kMaxDemodFc, brightness_);
        updateOutputFilter();
        updateDemodAlpha();
        inputFilter2nd_.setLowpass(float(fs_), inputOutputFcHz_, 0.9f);
        inputPole1Alpha_ = 1.f - std::exp(-2.f * kPi * inputOutputFcHz_ / float(fs_));
    }

    // ── main process ─────────────────────────────────────────────────────────
    // externalFeedback: signal coming back from the send/return loop
    // fbLoopConnected:  true when return jack is patched
    // fbLoopMix:        0=internal feedback only, 1=external feedback only
    float processSample(float input, float externalFeedback, bool fbLoopConnected, float fbLoopMix) {
        // --- build feedback signal ---
        float internalFb = feedbackSample_ * feedbackGain_;
        float usedFb;
        if (fbLoopConnected) {
            // mix internal (uneffected) and external (effected) feedback
            usedFb = internalFb * (1.f - fbLoopMix) + externalFeedback * feedbackGain_ * fbLoopMix;
        } else {
            usedFb = internalFb;  // normalled: bypass external loop
        }

        float summed  = input + usedFb;

        // --- pre-ADC filter chain ---
        float filtered = inputFilter2nd_.process(summed);
        inputPole1State_ += inputPole1Alpha_ * (filtered - inputPole1State_);
        filtered = inputPole1State_;
        filtered = softClip(filtered, clipDrive_);

        // --- run delta-sigma at dsmClockHz, decimate to fs ---
        float acc = 0.f;
        int   n   = 0;
        const double phaseStep = dsmClockHz_ / fs_;
        ramPhase_ += phaseStep;
        while (ramPhase_ >= 1.0) {
            ramPhase_ -= 1.0;
            // linear interpolation of input for sub-sample accuracy
            float t = clampf(float(1.0 - ramPhase_ / phaseStep), 0.f, 1.f);
            float interpIn = prevInput_ + t * (filtered - prevInput_);
            runDeltaSigmaTick(interpIn);
            acc += demodState_;
            ++n;
        }
        prevInput_ = filtered;

        float wet = (n > 0) ? acc / float(n) : demodState_;

        // --- post-DAC output filter ---
        wet = outputFilter2nd_.process(wet);

        // --- DC block on wet signal ---
        const float dcOut = wet - dcBlockX1_ + dcBlockR_ * dcBlockY1_;
        dcBlockX1_ = wet; dcBlockY1_ = dcOut;
        wet = dcOut;

        // scale output
        const float wetOut = wet * outputLevelTrim_;

        // --- update feedback for next sample ---
        // processFeedbackHpf models the coupling cap in the PT2399 circuit
        feedbackSample_ = processFeedbackHpf(wet * feedbackCompensation_);

        return wetOut;
    }

    // expose the pre-HPF feedback signal so the module can send it to the loop
    float getFeedbackPreHpf() const { return feedbackSample_; }

private:
    // ── BitRing: 44000*OS bits packed in uint32_t[] ───────────────────────
    struct BitRing {
        std::vector<uint32_t> data;
        int numBits=0, writePos=0;
        void resize(int bits) {
            numBits = std::max(1, bits);
            data.assign(size_t((numBits+31)/32), 0u);
            writePos=0;
        }
        void reset() { std::fill(data.begin(),data.end(),0u); writePos=0; }
        int readOldest() const {
            return int((data[size_t(writePos>>5)] >> (writePos&31)) & 1u);
        }
        void writeBit(int b) {
            const int w=writePos>>5, bt=writePos&31;
            if (b) data[size_t(w)] |=  (1u<<bt);
            else   data[size_t(w)] &= ~(1u<<bt);
            if (++writePos >= numBits) writePos=0;
        }
    };

    static float softClip(float x, float drive) {
        const float d = std::max(1e-3f, drive);
        return std::tanh(d*x) / d;
    }
    static float interpLog(float a, float b, float t) {
        return std::max(1e-6f,a) * std::pow(std::max(1e-6f,b)/std::max(1e-6f,a), clampf(t,0.f,1.f));
    }

    float processFeedbackHpf(float x) {
        const float y = feedbackHpfA_ * (feedbackHpfY1_ + x - feedbackHpfX1_);
        feedbackHpfX1_=x; feedbackHpfY1_=y;
        return y;
    }

    float nextDither() {
        // xorshift32 TPDF dither
        constexpr float kAmt = 0.02f;
        rngState_ ^= rngState_<<13; rngState_ ^= rngState_>>17; rngState_ ^= rngState_<<5;
        const float u1 = float(int32_t(rngState_)) * (1.f/2147483648.f);
        rngState_ ^= rngState_<<13; rngState_ ^= rngState_>>17; rngState_ ^= rngState_<<5;
        const float u2 = float(int32_t(rngState_)) * (1.f/2147483648.f);
        return (u1+u2)*0.5f*kAmt;
    }

    void runDeltaSigmaTick(float input) {
        constexpr float kDacLevel=0.7f, k1=0.8f, k2=0.4f;
        constexpr float kLeak1=0.9995f, kLeak2=0.9990f;
        const float dacFb  = dacBit_ ? kDacLevel : -kDacLevel;
        const float error  = input*inScale_ - dacFb + nextDither();
        i1_ = (i1_ + error*k1*integGain_) * kLeak1;
        i2_ = (i2_ + i1_*k2) * kLeak2;
        dacBit_ = (i2_ >= 0.f) ? 1 : 0;
        const int oldBit = bitRing_.readOldest();
        bitRing_.writeBit(dacBit_);
        zohBit_       = oldBit;
        ramHoldValue_ = zohBit_ ? kDacLevel : -kDacLevel;
        demodState_  += demodAlphaTick_ * (ramHoldValue_ - demodState_);
        demodState2_ += demodAlphaTick_ * (demodState_   - demodState2_);
    }

    void updateVCO() {
        const float delayMs  = 11.46f * rKOhm_ + 29.7f;
        const double fVcoHz  = 683210.0 / double(delayMs) * 1e3;  // Hz
        const double fRamHz  = fVcoHz / 15.5;
        dsmClockHz_ = fRamHz * osFactor_;
        const float delayNorm = (delayMs - 31.f) / (346.f - 31.f);
        inScale_ = 0.68f - 0.08f * delayNorm;
        feedbackCompensation_ = 1.f / std::max(0.06f, inScale_);
        clipDrive_ = boostActivated_ ? (1.f + 2.f*delayNorm) : (0.50f + 0.90f*delayNorm);
        updateOutputFilter();
    }
    void updateOutputFilter() {
        outputFilter2nd_.setLowpass(float(fs_), inputOutputFcHz_, 0.707f);
    }
    void updateDemodAlpha() {
        const float fc = demodFcScale_ / c6nF_;
        if (dsmClockHz_ > 0.0) {
            demodAlphaTick_ = 1.f - float(std::exp(-2.0*kPiD * double(fc) / dsmClockHz_));
        } else {
            demodAlphaTick_ = 1.f;
        }
    }
    void updateFeedbackHpf() {
        if (fs_ <= 0.0) { feedbackHpfA_=1.f; return; }
        const float rc = 1.f / (2.f*kPi*feedbackHpfHz_);
        const float dt = 1.f / float(fs_);
        feedbackHpfA_ = rc / (rc + dt);
    }

    // ── constants ─────────────────────────────────────────────────────────
    static constexpr float kBaseInFc   = 7000.f,  kMaxInFc    = 14000.f;
    static constexpr float kBaseDemodFc = 220000.f, kMaxDemodFc = 250000.f;

    // ── state ──────────────────────────────────────────────────────────────
    int    osFactor_;
    double fs_ = 48000.0;

    float  brightness_        = 0.f;
    float  inputOutputFcHz_   = kBaseInFc;
    float  demodFcScale_      = kBaseDemodFc;
    float  inScale_           = 0.6f;
    float  feedbackCompensation_ = 1.f/0.6f;
    float  clipDrive_         = 1.5f;
    bool   boostActivated_    = false;
    float  outputLevelTrim_   = 1.45f;

    float  feedbackGain_      = 0.f;
    float  feedbackHpfHz_     = 10.f;
    float  feedbackHpfA_      = 1.f;
    float  feedbackHpfX1_     = 0.f, feedbackHpfY1_ = 0.f;

    float  rKOhm_             = 10.f;
    double dsmClockHz_        = 305484.0 * 8;
    double ramPhase_          = 0.0;
    float  i1_=0, i2_=0;
    float  integGain_         = 1.f;
    int    dacBit_            = 0;
    float  c3nF_              = 100.f;
    float  c6nF_              = 100.f;
    BitRing bitRing_;
    int    zohBit_            = 0;
    float  ramHoldValue_      = 0.f;
    float  demodState_        = 0.f, demodState2_ = 0.f;
    float  demodAlphaTick_    = 0.01f;

    Biquad inputFilter2nd_;
    float  inputPole1Alpha_   = 0.f;
    float  inputPole1State_   = 0.f;
    Biquad outputFilter2nd_;

    float  dcBlockR_          = 0.999f;
    float  dcBlockX1_=0, dcBlockY1_=0;
    float  feedbackSample_    = 0.f;
    float  prevInput_         = 0.f;
    uint32_t rngState_        = 0x12345678u;
};

} // namespace pt2399
