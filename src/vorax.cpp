// vorax.cpp — VCV Rack 2 module
// vorax (Latin: "voracious, all-devouring") is a port of Audrey II, the
// feedback drone "horrorscape" synthesizer by Synthux Academy (design:
// Roey Tsemah, firmware: Nick Donadson / Infrasonic Audio), from the
// MIT-licensed firmware at https://github.com/FedeRepic/audrey-daisy.
//
// No oscillator makes sound. A Karplus-Strong string is fed a constant
// -90 dBFS white noise seed inside a feedback loop:
//
//   noise + fb ─> KS string ─> overdrive ─> LPF ─> HPF ─> reverb ─┬─> out
//        ^                                                        │
//        └──── "body" delay (1..100 ms, R offset −4 smp) <── gain ┘
//                                                                 │
//                              echo send ─> tape echo (BPF+clip) ─┘
//
// Raising the feedback gain past unity self-excites the loop; the string
// length sets the fundamental, everything else in the loop shapes the
// timbre. The echo delay sits outside the loop, each repeat bandpassed
// at 800 Hz and soft-clipped for telephone-like tape degradation, with
// feedback up to 1.5 for endlessly saturating swells.
//
// The reverb is a port of ReverbSc from DaisySP-LGPL (Sean Costello /
// Istvan Varga / Paul Batchelor, LGPL-2.1).
//
// Controls:
//   Knobs : PITCH, FEEDBACK (the mysterious big knob), BODY, LPF, HPF,
//           VERB MIX, VERB DECAY, ECHO SEND, ECHO TIME, ECHO FB, VOLUME
//   Switch: HALF (instantly halves echo time for doppler warps)
//   In    : audio IN (injected into the loop), V/OCT, FB CV, LPF CV, TIME CV
//   Out   : L, R
//   Light : LEVEL (output amplitude)

#include "forsitan.hpp"

namespace vorax_dsp {

// ---------------------------------------------------------------- utility

inline float softClip(float x) {
    if (x < -3.f) return -1.f;
    if (x > 3.f) return 1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

inline float dbToLin(float db) {
    return std::pow(10.f, db * 0.05f);
}

inline float noteToFreq(float nn) {
    return 440.f * std::pow(2.f, (nn - 69.f) / 12.f);
}

// one-pole smoothing coefficient with tau time constant `time_s`
inline float onepoleCoef(float timeS, float rate) {
    if (timeS <= 0.f || rate <= 0.f) return 1.f;
    return std::min(1.f / (timeS * rate), 1.f);
}

// t60-style coefficient as used by the firmware's ParameterRegistry
inline float onepoleCoefT60(float timeS, float rate) {
    return onepoleCoef(timeS * 0.1447597f, rate);
}

// anti-exponential tension curve (firmware's ftension, factor -3 on decay)
inline float ftension(float in, float factor) {
    if (factor == 0.f) return in;
    return std::expm1(in * factor) / std::expm1(factor);
}

// -------------------------------------------------------------- DelayLine
// Same semantics as daisysp::DelayLine (write decrements, read looks back)

struct DelayLine {
    std::vector<float> buf;
    int wp = 0;

    void init(int n) {
        buf.assign(std::max(n, 8), 0.f);
        wp = 0;
    }
    void write(float s) {
        buf[wp] = s;
        if (--wp < 0) wp += (int)buf.size();
    }
    float read(float delay) const {
        int n = (int)buf.size();
        int d = (int)delay;
        float frac = delay - (float)d;
        float a = buf[(wp + 1 + d) % n];
        float b = buf[(wp + 1 + d + 1) % n];
        return a + (b - a) * frac;
    }
    float readHermite(float delay) const {
        int n = (int)buf.size();
        int d = (int)delay;
        float f = delay - (float)d;
        int t = wp + 1 + d + n;
        float xm1 = buf[(t - 1) % n];
        float x0 = buf[t % n];
        float x1 = buf[(t + 1) % n];
        float x2 = buf[(t + 2) % n];
        float c = (x1 - xm1) * 0.5f;
        float v = x0 - x1;
        float w = c + v;
        float a = w + v + (x2 - x0) * 0.5f;
        float bNeg = w + a;
        return (((a * f) - bNeg) * f + c) * f + x0;
    }
};

// ----------------------------------------------------------------- Biquad
// 2nd order TDF2 section, coefficients per the firmware's BiquadFilters

struct Biquad {
    enum Type { LOWPASS, HIGHPASS, BANDPASS };
    Type type = LOWPASS;
    float sr = 48000.f;
    float q = 0.9f;
    float fc = -1.f;
    float b0 = 0.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float s1[2] = {}, s2[2] = {};

    void init(Type t, float sampleRate, float Q) {
        type = t;
        sr = sampleRate;
        q = Q;
        fc = -1.f;
        s1[0] = s1[1] = s2[0] = s2[1] = 0.f;
    }
    void setCutoff(float cutoffHz) {
        cutoffHz = clamp(cutoffHz, 1.f, sr * 0.497f);
        if (cutoffHz == fc) return;
        fc = cutoffHz;
        float K = std::tan(M_PI * fc / sr);
        float Ksq = K * K;
        float norm = 1.f / (1.f + K / q + Ksq);
        switch (type) {
            case HIGHPASS:
                b0 = norm;
                b1 = -2.f * b0;
                b2 = b0;
                break;
            case BANDPASS:
                b0 = (K / q) * norm;
                b1 = 0.f;
                b2 = -b0;
                break;
            default:
                b0 = Ksq * norm;
                b1 = 2.f * b0;
                b2 = b0;
                break;
        }
        a1 = 2.f * (Ksq - 1.f) * norm;
        a2 = (1.f - K / q + Ksq) * norm;
    }
    float process(float in, int ch) {
        float y = b0 * in + s1[ch];
        s1[ch] = s2[ch] + in * b1 - a1 * y;
        s2[ch] = b2 * in - a2 * y;
        if (!std::isfinite(y) || !std::isfinite(s1[ch]) || !std::isfinite(s2[ch])) {
            s1[ch] = s2[ch] = 0.f;
            y = 0.f;
        }
        return y;
    }
    void processStereo(float& l, float& r) {
        l = process(l, 0);
        r = process(r, 1);
    }
};

// ---------------------------------------------------------- KarplusString
// Port of the firmware's KarplusString (itself a trimmed daisysp/Mutable
// string): tuned delay line + one-pole damping filter + DC blocker, with
// a linear-interp upsampler for pitches below the delay line length.

struct KarplusString {
    DelayLine line;
    float sr = 48000.f;
    float frequency = 0.01f;   // normalized freq (Hz / sr)
    int lineSize = 8192;
    // one-pole "Tone" damping filter at 8 kHz
    float toneC1 = 0.f, toneC2 = 0.f, toneState = 0.f;
    // DC blocker
    float dcX = 0.f, dcY = 0.f;
    // upsampler state
    float srcPhase = 0.f;
    float outSample[2] = {};

    void init(float sampleRate) {
        sr = sampleRate;
        // keep the 48k lower pitch bound (~5.9 Hz) across sample rates
        lineSize = std::max(8192, (int)(sr * 8192.f / 48000.f) + 4);
        line.init(lineSize);
        float b = 2.f - std::cos(2.f * M_PI * 8000.f / sr);
        toneC2 = b - std::sqrt(b * b - 1.f);
        toneC1 = 1.f - toneC2;
        toneState = 0.f;
        dcX = dcY = 0.f;
        srcPhase = 0.f;
        outSample[0] = outSample[1] = 0.f;
        setFreq(440.f);
    }
    void setFreq(float freqHz) {
        frequency = clamp(freqHz / sr, 0.f, 0.25f);
    }
    float process(float in) {
        float delay = 1.f / frequency;
        delay = clamp(delay, 4.f, (float)lineSize - 4.f);

        float srcRatio = delay * frequency;
        if (srcRatio >= 0.9999f) {
            srcPhase = 1.f;
            srcRatio = 1.f;
        }

        srcPhase += srcRatio;
        if (srcPhase > 1.f) {
            srcPhase -= 1.f;
            float s = line.readHermite(delay);
            s += in;
            s = clamp(s, -20.f, 20.f);
            // DC block: y = x - x1 + 0.995 y1
            float y = s - dcX + 0.995f * dcY;
            dcX = s;
            dcY = y;
            s = y * 0.8f;
            // damping filter
            toneState = toneC1 * s + toneC2 * toneState;
            s = toneState;
            line.write(s);
            outSample[1] = outSample[0];
            outSample[0] = s;
        }
        return outSample[1] + (outSample[0] - outSample[1]) * srcPhase;
    }
};

// -------------------------------------------------------------- Overdrive
// daisysp::Overdrive (after Mutable Instruments), drive fixed at 0.4

struct Overdrive {
    float preGain = 0.f, postGain = 0.f;

    void setDrive(float drive) {
        drive = clamp(drive, 0.f, 1.f);
        float d = 2.f * drive;
        float d2 = d * d;
        float preA = d * 0.5f;
        float preB = d2 * d2 * d * 24.f;
        preGain = preA + (preB - preA) * d2;
        float squashed = d * (2.f - d);
        postGain = 1.f / softClip(0.33f + squashed * (preGain - 0.33f));
    }
    float process(float in) {
        return softClip(preGain * in) * postGain;
    }
};

// -------------------------------------------------------------- EchoDelay
// Tape-ish echo: full-wet, unbounded feedback but each repeat is
// bandpassed at 800 Hz and soft-clipped.

struct EchoDelay {
    DelayLine line;
    Biquad bpf;
    float sr = 48000.f;
    float timeCurrent = 0.5f, timeTarget = 0.5f, smoothCoef = 0.f;
    float feedback = 0.5f;
    float maxDelayS = 5.f;

    void init(float sampleRate) {
        sr = sampleRate;
        line.init((int)(maxDelayS * sr) + 4);
        bpf.init(Biquad::BANDPASS, sr, 1.f);
        bpf.setCutoff(800.f);
        smoothCoef = onepoleCoef(0.5f, sr);
        timeCurrent = timeTarget = 0.5f;
    }
    void setDelayTime(float timeS) {
        timeTarget = clamp(timeS, 0.f, maxDelayS);
    }
    float process(float in) {
        timeCurrent += (timeTarget - timeCurrent) * smoothCoef;
        float delaySamp = clamp(timeCurrent * sr, 1.f, (float)line.buf.size() - 4.f);
        float out = line.read(delaySamp);
        out = bpf.process(out, 0);
        out = softClip(out);
        line.write(out * feedback + in);
        return out;
    }
};

// --------------------------------------------------------------- ReverbSc
// Port of ReverbSc from DaisySP-LGPL (Sean Costello's Csound reverb, via
// Istvan Varga / Paul Batchelor): 8 modulated delay lines in a feedback
// junction. Buffers heap-allocated and scaled to the engine sample rate.

struct ReverbSc {
    static constexpr float kOutputGain = 0.35f;
    static constexpr float kJpScale = 0.25f;
    // {delay time s, random variation s, variation freq 1/s, seed}
    static constexpr float kParams[8][4] = {
        {2473.f / 48000.f, 0.0010f, 3.100f, 1966.f},
        {2767.f / 48000.f, 0.0011f, 3.500f, 29491.f},
        {3217.f / 48000.f, 0.0017f, 1.110f, 22937.f},
        {3557.f / 48000.f, 0.0006f, 3.973f, 9830.f},
        {3907.f / 48000.f, 0.0010f, 2.341f, 20643.f},
        {4127.f / 48000.f, 0.0011f, 1.897f, 22937.f},
        {2143.f / 48000.f, 0.0017f, 0.891f, 29491.f},
        {1933.f / 48000.f, 0.0006f, 3.221f, 14417.f}};
    static constexpr int kPosShift = 28;
    static constexpr int64_t kPosScale = 0x10000000;
    static constexpr int64_t kPosMask = 0x0FFFFFFF;

    struct Dl {
        int writePos = 0, bufferSize = 0, readPos = 0;
        int64_t readPosFrac = 0, readPosFracInc = 0;
        int seedVal = 0, randLineCnt = 0;
        float filterState = 0.f;
        std::vector<float> buf;
    };

    float sr = 48000.f;
    float feedback = 0.97f, lpfreq = 10000.f;
    float dampFact = 1.f, prvLpfreq = 0.f;
    Dl dl[8];

    int maxSamples(int n) const {
        float maxDel = kParams[n][0] + kParams[n][1] * 1.125f;
        return (int)(maxDel * sr + 16.5f);
    }

    void nextRandomLineseg(Dl& lp, int n) {
        if (lp.seedVal < 0) lp.seedVal += 0x10000;
        lp.seedVal = (lp.seedVal * 15625 + 1) & 0xFFFF;
        if (lp.seedVal >= 0x8000) lp.seedVal -= 0x10000;
        lp.randLineCnt = (int)(sr / kParams[n][2] + 0.5f);
        float prvDel = (float)lp.writePos;
        prvDel -= (float)lp.readPos + (float)lp.readPosFrac / (float)kPosScale;
        while (prvDel < 0.f) prvDel += (float)lp.bufferSize;
        prvDel /= sr;
        float nxtDel = (float)lp.seedVal * kParams[n][1] / 32768.f;
        nxtDel = kParams[n][0] + nxtDel;
        float phsInc = (prvDel - nxtDel) / (float)lp.randLineCnt;
        phsInc = phsInc * sr + 1.f;
        lp.readPosFracInc = (int64_t)(phsInc * kPosScale + 0.5f);
    }

    void initDelayLine(Dl& lp, int n) {
        lp.bufferSize = maxSamples(n);
        lp.buf.assign(lp.bufferSize, 0.f);
        lp.writePos = 0;
        lp.seedVal = (int)(kParams[n][3] + 0.5f);
        float readPos = (float)lp.seedVal * kParams[n][1] / 32768.f;
        readPos = kParams[n][0] + readPos;
        readPos = (float)lp.bufferSize - readPos * sr;
        lp.readPos = (int)readPos;
        lp.readPosFrac = (int64_t)((readPos - (float)lp.readPos) * kPosScale + 0.5f);
        lp.filterState = 0.f;
        nextRandomLineseg(lp, n);
    }

    void init(float sampleRate) {
        sr = sampleRate;
        dampFact = 1.f;
        prvLpfreq = 0.f;
        for (int i = 0; i < 8; i++)
            initDelayLine(dl[i], i);
    }

    void process(float in1, float in2, float& out1, float& out2) {
        if (lpfreq != prvLpfreq) {
            prvLpfreq = lpfreq;
            float d = 2.f - std::cos(prvLpfreq * 2.f * (float)M_PI / sr);
            dampFact = d - std::sqrt(d * d - 1.f);
        }

        float aInL = 0.f, aOutL = 0.f, aOutR = 0.f;
        for (int n = 0; n < 8; n++)
            aInL += dl[n].filterState;
        aInL *= kJpScale;
        float aInR = aInL + in2;
        aInL += in1;

        for (int n = 0; n < 8; n++) {
            Dl& lp = dl[n];
            int bufferSize = lp.bufferSize;

            lp.buf[lp.writePos] = (n & 1 ? aInR : aInL) - lp.filterState;
            if (++lp.writePos >= bufferSize) lp.writePos -= bufferSize;

            if (lp.readPosFrac >= kPosScale) {
                lp.readPos += (int)(lp.readPosFrac >> kPosShift);
                lp.readPosFrac &= kPosMask;
            }
            if (lp.readPos >= bufferSize) lp.readPos -= bufferSize;
            int readPos = lp.readPos;
            float frac = (float)lp.readPosFrac * (1.f / (float)kPosScale);

            // cubic interpolation coefficients
            float a2 = frac * frac;
            a2 -= 1.f;
            a2 *= (1.f / 6.f);
            float a1 = frac + 1.f;
            a1 *= 0.5f;
            float am1 = a1 - 1.f;
            float a0 = 3.f * a2;
            a1 -= a0;
            am1 -= a2;
            a0 -= frac;

            float vm1, v0, v1, v2;
            if (readPos > 0 && readPos < bufferSize - 2) {
                vm1 = lp.buf[readPos - 1];
                v0 = lp.buf[readPos];
                v1 = lp.buf[readPos + 1];
                v2 = lp.buf[readPos + 2];
            }
            else {
                if (--readPos < 0) readPos += bufferSize;
                vm1 = lp.buf[readPos];
                if (++readPos >= bufferSize) readPos -= bufferSize;
                v0 = lp.buf[readPos];
                if (++readPos >= bufferSize) readPos -= bufferSize;
                v1 = lp.buf[readPos];
                if (++readPos >= bufferSize) readPos -= bufferSize;
                v2 = lp.buf[readPos];
            }
            v0 = (am1 * vm1 + a0 * v0 + a1 * v1 + a2 * v2) * frac + v0;

            lp.readPosFrac += lp.readPosFracInc;

            v0 *= feedback;
            v0 = (lp.filterState - v0) * dampFact + v0;
            if (!std::isfinite(v0)) v0 = 0.f;
            lp.filterState = v0;

            if (n & 1)
                aOutR += v0;
            else
                aOutL += v0;

            if (--lp.randLineCnt <= 0) nextRandomLineseg(lp, n);
        }
        out1 = aOutL * kOutputGain;
        out2 = aOutR * kOutputGain;
    }
};

// ----------------------------------------------------------------- Engine
// Direct port of infrasonic::FeedbackSynth::Engine

struct Engine {
    float sr = 48000.f;
    float fbGain = 0.f;
    float echoSend = 0.f;
    float verbMix = 0.f;
    float outputLevel = 0.5f;
    float fbDelaySmoothCoef = 0.f;
    float fbDelaySamp = 1000.f, fbDelaySampTarget = 64.f;
    int maxFbDelaySamp = 12000;

    KarplusString strings[2];
    DelayLine fbDelay[2];
    Overdrive overdrive[2];
    Biquad fbLpf, fbHpf;
    ReverbSc verb;
    EchoDelay echo[2];
    // white noise seed at -90 dBFS
    float noiseAmp = 0.0000316f;

    void init(float sampleRate) {
        sr = sampleRate;
        maxFbDelaySamp = (int)(0.25f * sr);
        fbDelaySmoothCoef = onepoleCoef(0.2f, sr);
        noiseAmp = dbToLin(-90.f);
        for (int i = 0; i < 2; i++) {
            strings[i].init(sr);
            strings[i].setFreq(noteToFreq(40.f));
            fbDelay[i].init(maxFbDelaySamp);
            echo[i].init(sr);
            overdrive[i].setDrive(0.4f);
        }
        verb.init(sr);
        verb.feedback = 0.85f;
        verb.lpfreq = 12000.f;
        fbLpf.init(Biquad::LOWPASS, sr, 0.9f);
        fbLpf.setCutoff(18000.f);
        fbHpf.init(Biquad::HIGHPASS, sr, 0.9f);
        fbHpf.setCutoff(60.f);
        fbDelaySamp = 1000.f;
        fbDelaySampTarget = 64.f;
    }

    void setStringPitch(float nn) {
        float freq = noteToFreq(nn);
        strings[0].setFreq(freq);
        strings[1].setFreq(freq);
    }
    void setFeedbackGain(float gainDb) { fbGain = dbToLin(gainDb); }
    void setFeedbackDelay(float delayS) {
        fbDelaySampTarget = clamp(delayS * sr, 1.f, (float)maxFbDelaySamp - 1.f);
    }

    void process(float in, float& outL, float& outR) {
        fbDelaySamp += (fbDelaySampTarget - fbDelaySamp) * fbDelaySmoothCoef;

        float noise = noiseAmp * (2.f * random::uniform() - 1.f);

        // noise + feedback return (R offset 4 samples for stereo width)
        float inL = fbDelay[0].read(fbDelaySamp) + noise + in;
        float inR = fbDelay[1].read(std::max(1.f, fbDelaySamp - 4.f)) + noise + in;

        // KS resonator
        float sampL = strings[0].process(inL);
        float sampR = strings[1].process(inR);

        // distort + clip
        sampL = overdrive[0].process(sampL);
        sampR = overdrive[1].process(sampR);

        // loop filters
        fbLpf.processStereo(sampL, sampR);
        fbHpf.processStereo(sampL, sampR);

        // reverb (inside the loop)
        float verbL, verbR;
        verb.process(sampL, sampR, verbL, verbR);
        sampL -= (sampL - verbL) * verbMix;
        sampR -= (sampR - verbR) * verbMix;

        // write back into the body delay with gain
        fbDelay[0].write(sampL * fbGain);
        fbDelay[1].write(sampR * fbGain);

        // echo delay (outside the loop)
        float echoL = echo[0].process(sampL * echoSend);
        float echoR = echo[1].process(sampR * echoSend);

        sampL = 0.5f * (sampL + echoL);
        sampR = 0.5f * (sampR + echoR);

        outL = sampL * outputLevel;
        outR = sampR * outputLevel;
    }
};

// firmware-style smoothed parameter (t60 one-pole toward target)
struct Smoothed {
    float value = 0.f, target = 0.f, coef = 1.f;
    bool primed = false;

    void setup(float initial, float smoothTimeS, float rate) {
        value = target = initial;
        coef = onepoleCoefT60(smoothTimeS, rate);
        primed = true;
    }
    float tick() {
        value += (target - value) * coef;
        return value;
    }
};

} // namespace vorax_dsp

struct Vorax : Module {
    enum ParamId {
        PITCH_PARAM,
        FEEDBACK_PARAM,
        BODY_PARAM,
        LPF_PARAM,
        HPF_PARAM,
        VERB_MIX_PARAM,
        VERB_DECAY_PARAM,
        ECHO_SEND_PARAM,
        ECHO_TIME_PARAM,
        ECHO_FB_PARAM,
        VOLUME_PARAM,
        HALF_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        VOCT_INPUT,
        FEEDBACK_CV_INPUT,
        LPF_CV_INPUT,
        TIME_CV_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        LEFT_OUTPUT,
        RIGHT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LEVEL_LIGHT,
        LIGHTS_LEN
    };

    static constexpr int kControlDiv = 16;

    vorax_dsp::Engine engine;
    vorax_dsp::Smoothed smPitch, smFeedback, smBody, smLpf, smHpf,
        smVerbMix, smVerbDecay, smEchoSend, smEchoTime, smEchoFb, smVolume;
    int controlPhase = 0;
    float levelEnv = 0.f;

    Vorax() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(PITCH_PARAM, 16.f, 72.f, 40.f, "String pitch", " st");
        configParam(FEEDBACK_PARAM, -60.f, 12.f, -60.f, "Feedback gain", " dB");
        configParam(BODY_PARAM, 0.f, 1.f, 0.f, "Body (feedback delay)");
        configParam(LPF_PARAM, 0.f, 1.f, 1.f, "Loop lowpass cutoff", " Hz", 180.f, 100.f);
        configParam(HPF_PARAM, 0.f, 1.f, 0.605f, "Loop highpass cutoff", " Hz", 400.f, 10.f);
        configParam(VERB_MIX_PARAM, 0.f, 1.f, 0.f, "Reverb mix", "%", 0.f, 100.f);
        configParam(VERB_DECAY_PARAM, 0.f, 1.f, 0.f, "Reverb decay");
        configParam(ECHO_SEND_PARAM, 0.f, 1.f, 0.f, "Echo send");
        configParam(ECHO_TIME_PARAM, 0.f, 1.f, 0.3015f, "Echo time");
        configParam(ECHO_FB_PARAM, 0.f, 1.5f, 0.f, "Echo feedback");
        configParam(VOLUME_PARAM, 0.f, 1.f, 0.707f, "Volume");
        configSwitch(HALF_PARAM, 0.f, 1.f, 0.f, "Echo half-time warp", {"Off", "On"});
        configInput(AUDIO_INPUT, "Audio (into the feedback loop)");
        configInput(VOCT_INPUT, "String pitch (1V/oct)");
        configInput(FEEDBACK_CV_INPUT, "Feedback gain CV (7.2 dB/V)");
        configInput(LPF_CV_INPUT, "Loop lowpass cutoff CV (1V/oct)");
        configInput(TIME_CV_INPUT, "Echo time CV (1V/oct)");
        configOutput(LEFT_OUTPUT, "Left");
        configOutput(RIGHT_OUTPUT, "Right");
        configBypass(AUDIO_INPUT, LEFT_OUTPUT);
        configBypass(AUDIO_INPUT, RIGHT_OUTPUT);
        onSampleRateChange();
    }

    void onSampleRateChange() override {
        float sr = APP->engine->getSampleRate();
        engine.init(sr);
        float controlRate = sr / kControlDiv;
        // initial values and smoothing times match the firmware registry
        smPitch.setup(40.f, 0.2f, controlRate);
        smFeedback.setup(-60.f, 0.05f, controlRate);
        smBody.setup(0.001f, 1.0f, controlRate);
        smLpf.setup(18000.f, 0.05f, controlRate);
        smHpf.setup(250.f, 0.05f, controlRate);
        smVerbMix.setup(0.f, 0.05f, controlRate);
        smVerbDecay.setup(0.2f, 0.05f, controlRate);
        smEchoSend.setup(0.f, 0.05f, controlRate);
        smEchoTime.setup(0.5f, 0.1f, controlRate);
        smEchoFb.setup(0.f, 0.05f, controlRate);
        smVolume.setup(0.5f, 0.05f, controlRate);
        controlPhase = 0;
    }

    void onReset() override {
        onSampleRateChange();
    }

    void updateControls() {
        using namespace vorax_dsp;

        // pitch: note number + 1V/oct
        float nn = params[PITCH_PARAM].getValue() + 12.f * inputs[VOCT_INPUT].getVoltage();
        smPitch.target = clamp(nn, 0.f, 127.f);

        // feedback gain in dB
        float fb = params[FEEDBACK_PARAM].getValue()
                 + 7.2f * inputs[FEEDBACK_CV_INPUT].getVoltage();
        smFeedback.target = clamp(fb, -60.f, 12.f);

        // body: EXP map 1..100 ms
        float t = params[BODY_PARAM].getValue();
        smBody.target = 0.001f + t * t * 0.099f;

        // loop filter cutoffs: LOG maps, LPF gets 1V/oct CV
        t = params[LPF_PARAM].getValue();
        float lpf = 100.f * std::pow(180.f, t);
        lpf *= std::pow(2.f, inputs[LPF_CV_INPUT].getVoltage());
        smLpf.target = clamp(lpf, 100.f, 18000.f);
        t = params[HPF_PARAM].getValue();
        smHpf.target = 10.f * std::pow(400.f, t);

        smVerbMix.target = params[VERB_MIX_PARAM].getValue();
        // anti-exponential tension curve, mapped to 0.2..1.0
        smVerbDecay.target = 0.2f + 0.8f * ftension(params[VERB_DECAY_PARAM].getValue(), -3.f);

        // echo send: EXP map 0..1
        t = params[ECHO_SEND_PARAM].getValue();
        smEchoSend.target = t * t;

        // echo time: EXP map 0.05..5 s, halved by the warp switch
        // (on the normalized value, like the hardware), then 1V/oct CV
        t = params[ECHO_TIME_PARAM].getValue();
        if (params[HALF_PARAM].getValue() > 0.5f) t *= 0.5f;
        float time = 0.05f + t * t * 4.95f;
        time *= std::pow(2.f, -inputs[TIME_CV_INPUT].getVoltage());
        smEchoTime.target = clamp(time, 0.05f, 5.f);

        smEchoFb.target = params[ECHO_FB_PARAM].getValue();

        // volume: EXP map 0..1
        t = params[VOLUME_PARAM].getValue();
        smVolume.target = t * t;

        // tick smoothers and apply to the engine
        engine.setStringPitch(smPitch.tick());
        engine.setFeedbackGain(smFeedback.tick());
        engine.setFeedbackDelay(smBody.tick());
        engine.fbLpf.setCutoff(smLpf.tick());
        engine.fbHpf.setCutoff(smHpf.tick());
        engine.verbMix = clamp(smVerbMix.tick(), 0.f, 1.f);
        engine.verb.feedback = smVerbDecay.tick();
        engine.echoSend = smEchoSend.tick();
        float et = smEchoTime.tick();
        engine.echo[0].setDelayTime(et);
        engine.echo[1].setDelayTime(et);
        float efb = smEchoFb.tick();
        engine.echo[0].feedback = efb;
        engine.echo[1].feedback = efb;
        engine.outputLevel = smVolume.tick();
    }

    void process(const ProcessArgs& args) override {
        if (controlPhase == 0)
            updateControls();
        if (++controlPhase >= kControlDiv)
            controlPhase = 0;

        float in = inputs[AUDIO_INPUT].getVoltage() * 0.2f;
        float outL, outR;
        engine.process(in, outL, outR);

        outputs[LEFT_OUTPUT].setVoltage(clamp(5.f * outL, -10.f, 10.f));
        outputs[RIGHT_OUTPUT].setVoltage(clamp(5.f * outR, -10.f, 10.f));

        levelEnv += (std::fabs(outL) - levelEnv) * 0.002f;
        lights[LEVEL_LIGHT].setBrightness(clamp(levelEnv, 0.f, 1.f));
    }
};

struct VoraxWidget : ModuleWidget {
    VoraxWidget(Vorax* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/vorax.svg")));

// @layout:begin vorax 50.8 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem PITCH_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem FEEDBACK_PARAM RoundBigBlackKnob 7.62 param "" 0.0
// @elem BODY_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem LPF_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem HPF_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem VERB_MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem VERB_DECAY_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem ECHO_SEND_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem ECHO_TIME_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem ECHO_FB_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem VOLUME_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem HALF_PARAM CKSS 2.0 param "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem VOCT_INPUT PJ301MPort 4.18 input "" 0.0
// @elem FEEDBACK_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LPF_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem TIME_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem LEFT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem RIGHT_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem LEVEL_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_PITCH label 0.0 label "pitch" 0.0 10.40 28.50
// @elem LABEL_FEEDBACK label 0.0 label "feedback" 0.0 25.40 33.50
// @elem LABEL_BODY label 0.0 label "body" 0.0 40.40 28.50
// @elem LABEL_LPF label 0.0 label "lpf" 0.0 10.40 46.50
// @elem LABEL_HPF label 0.0 label "hpf" 0.0 40.40 46.50
// @elem LABEL_MIX label 0.0 label "verb" 0.0 10.40 64.50
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 25.40 64.50
// @elem LABEL_SEND label 0.0 label "send" 0.0 40.40 64.50
// @elem LABEL_TIME label 0.0 label "time" 0.0 10.40 82.50
// @elem LABEL_EFB label 0.0 label "echo fb" 0.0 25.40 82.50
// @elem LABEL_VOL label 0.0 label "vol" 0.0 40.40 82.50
// @elem LABEL_HALF label 0.0 label "half" 0.0 25.40 51.50
// @elem LABEL_IN label 0.0 label "in" 0.0 6.90 94.50
// @elem LABEL_VOCT label 0.0 label "v/oct" 0.0 16.15 94.50
// @elem LABEL_FBCV label 0.0 label "fb" 0.0 25.40 94.50
// @elem LABEL_LPFCV label 0.0 label "lpf" 0.0 34.65 94.50
// @elem LABEL_TIMECV label 0.0 label "time" 0.0 43.90 94.50
// @elem LABEL_L label 0.0 label "l" 0.0 25.10 114.00
// @elem LABEL_R label 0.0 label "r" 0.0 40.90 114.00
// @elem BOX_L panel_box 7.0 box "" 0.0 25.10 108.50
// @elem BOX_R panel_box 7.0 box "" 0.0 40.90 108.50
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 10.40 116.50

        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(5.08f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(40.64f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 20.00f)), module, Vorax::PITCH_PARAM));
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(25.40f, 22.00f)), module, Vorax::FEEDBACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 20.00f)), module, Vorax::BODY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 38.00f)), module, Vorax::LPF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 38.00f)), module, Vorax::HPF_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 56.00f)), module, Vorax::VERB_MIX_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 56.00f)), module, Vorax::VERB_DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 56.00f)), module, Vorax::ECHO_SEND_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.40f, 74.00f)), module, Vorax::ECHO_TIME_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(25.40f, 74.00f)), module, Vorax::ECHO_FB_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.40f, 74.00f)), module, Vorax::VOLUME_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(25.40f, 44.00f)), module, Vorax::HALF_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.90f, 87.00f)), module, Vorax::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(16.15f, 87.00f)), module, Vorax::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(25.40f, 87.00f)), module, Vorax::FEEDBACK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(34.65f, 87.00f)), module, Vorax::LPF_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(43.90f, 87.00f)), module, Vorax::TIME_CV_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.10f, 106.50f)), module, Vorax::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.90f, 106.50f)), module, Vorax::RIGHT_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(45.90f, 103.50f)), module, Vorax::LEVEL_LIGHT));
        // @layout:end
    }
};

Model* modelVorax = createModel<Vorax, VoraxWidget>("vorax");
