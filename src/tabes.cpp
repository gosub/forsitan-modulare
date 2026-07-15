// tabes.cpp — VCV Rack 2 module
// tabes (Latin: "wasting away, decay") is a disintegration looper. Record a
// phrase; on every pass the tape ages: high frequencies dull, mild
// saturation compresses, hiss creeps in, the level sags, and dropouts appear
// more and more often as the loop wears out. Wow/flutter warbles the
// playback. Basinski's Disintegration Loops, as a module.
//
// The degradation is done tape-style: the play head reads the loop, and the
// write head re-records a slightly worse copy in place, so loss accumulates
// pass over pass. A pristine copy of the original recording is kept; SPLICE
// swaps in fresh tape (back to pass zero).
//
// OVERLAP crossfades the loop point: at zero it is just a short anti-click
// fade, opened up it overlaps the loop's tail with its head (equal power) for
// an ambient blur. SEND/RETURN insert an external effect into the write path,
// so whatever the pedal does is re-recorded and compounds pass over pass.
//
// Controls:
//   Knobs : DECAY (loss per pass), WOW (wow/flutter depth), OVERLAP (loop
//           crossfade), SEND (fx return mix)
//   Btns  : REC (toggle recording), SPLICE (restore pristine recording)
//   In    : IN (audio), REC gate, SPLICE trigger, DECAY/WOW/OVERLAP/SEND CV,
//           RETURN (fx return)
//   Out   : OUT (audio), AGE (0.1V per pass, clamps at 10V), EOC (trigger),
//           RAMP (play head 0..10V), SEND (fx send)
//   Light : REC (on while recording)

#include "forsitan.hpp"

static constexpr float kMaxSeconds = 30.f;

struct Tabes : Module {
    enum ParamId {
        DECAY_PARAM,
        WOW_PARAM,
        REC_PARAM,
        SPLICE_PARAM,
        OVERLAP_PARAM,     // loop-point crossfade length (0 = anti-click only)
        SEND_MIX_PARAM,    // how much of the fx return is re-recorded
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_INPUT,
        REC_GATE_INPUT,
        SPLICE_TRIG_INPUT,
        DECAY_CV_INPUT,
        WOW_CV_INPUT,
        OVERLAP_CV_INPUT,
        SEND_MIX_CV_INPUT,
        RETURN_INPUT,      // fx return, folded back onto the tape
        INPUTS_LEN
    };
    enum OutputId {
        AUDIO_OUTPUT,
        AGE_OUTPUT,
        EOC_OUTPUT,
        RAMP_OUTPUT,       // play head position, 0..10V, resets at the loop point
        SEND_OUTPUT,       // the loop read, out to an external effect
        OUTPUTS_LEN
    };
    enum LightId {
        REC_LIGHT,
        EOC_LIGHT,
        OUT_LIGHT,
        LIGHTS_LEN
    };

    // Polyphonic: the tape has a "width" of `channels` tracks, fixed when a
    // recording starts (from the AUDIO input's channel count). One shared
    // transport drives them all — same wow, dropouts, seam and rec crossfade —
    // so a 2-channel poly cable records and plays back as a coherent stereo
    // tape. Per-channel buffers grow lazily to the widest recording so far and
    // never shrink (a mono patch stays at ~11 MB; 16 tracks would reach ~184).
    static constexpr int kMaxChannels = 16;
    std::vector<std::vector<float>> tape;      // [channel][sample], aging loop
    std::vector<std::vector<float>> pristine;  // [channel][sample], as recorded
    int channels = 1;             // tape width (tracks), set at record start
    size_t tapeCap = 0;           // per-channel capacity (samples)
    int loopLen = 0;              // samples in the loop (0 = empty)
    int playPos = 0;              // write/transport head (ages the tape, EOC, ramp)
    float headAge = 0.f;          // read head: samples since the current head
                                  // started (0..hop); overlap plays a chain of
                                  // heads, a new one every hop = loopLen-overlap
    int recPos = 0;
    bool recording = false;
    bool gateWasHigh = false;
    int age = 0;                  // completed passes since last splice

    float lpState[kMaxChannels] = {};   // per-channel write-head lowpass
    int xfadeTotal = 0;           // seam declick: head/input blend length
    int xfadeRemain = 0;          // samples of blend left (first pass only)
    // rec-transition crossfade: loopGain ramps 0<->1 over ~10 ms so the
    // monitor/playback handoff has no envelope step. (The old additive bridge
    // cancelled the instantaneous step but turned each rec press into a short
    // low-frequency thump.) The rec-start direction reads the loop's
    // continuation from a snapshot, because recording is overwriting the tape
    // underneath the fade.
    std::vector<std::vector<float>> xfBuf;   // [channel][k] rec-start snapshot
    int xfBufLen = 0;             // capacity of xfBuf (samples)
    int xfReadPos = 0;            // read cursor into xfBuf
    int recStartFade = 0;         // snapshot samples left to read
    float loopGain = 0.f;         // 0 = recording/empty, 1 = playing
    float loopGainStep = 0.f;     // per-sample ramp (1 / window)
    float lastOffset = 0.f;       // last wow read offset, to continue the head
    float lastOvl = 0.f;          // last overlap length (samples), for splice()
    // splice declick: a ~10 ms linear crossfade from the aged tape to the
    // pristine one, fed by a snapshot of the aged output continuation taken
    // just before the swap (mirrors the rec-start crossfade above). The old
    // additive one-pole bridge only cancelled the amplitude step (C0), leaving
    // a slope discontinuity plus the abrupt aged->fresh timbre change audible
    // as a soft click, and could be truncated by the output clamp.
    std::vector<std::vector<float>> spliceBuf;   // [channel][k] aged snapshot
    int spliceFade = 0;           // samples of splice crossfade left
    int spliceFadeTotal = 0;      // full crossfade length
    float wowPhase = 0.f, flutterPhase = 0.f;
    float dropEnv = 1.f;          // smoothed dropout gain
    int dropTimer = 0;
    enum MonitorMode { MONITOR_WHILE_REC, MONITOR_ALWAYS, MONITOR_NEVER };
    int monitorMode = MONITOR_WHILE_REC;   // context menu

    float curSampleRate = 0.f;
    uint32_t noiseState = 0x6c078965u;
    dsp::BooleanTrigger recButton, spliceButton;
    dsp::SchmittTrigger spliceTrigger;
    dsp::PulseGenerator eocPulse;
    float eocFlash = 0.f;
    float outEnv = 0.f;

    Tabes() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.4f, "Decay per pass");
        configParam(WOW_PARAM, 0.f, 1.f, 0.3f, "Wow/flutter");
        configButton(REC_PARAM, "Record");
        configButton(SPLICE_PARAM, "Splice (restore pristine tape)");
        configParam(OVERLAP_PARAM, 0.f, 1.f, 0.f, "Loop overlap");
        configParam(SEND_MIX_PARAM, 0.f, 1.f, 0.5f, "FX return mix");
        configInput(AUDIO_INPUT, "Audio (polyphonic: records all channels)");
        configInput(REC_GATE_INPUT, "Record gate");
        configInput(SPLICE_TRIG_INPUT, "Splice trigger");
        configInput(DECAY_CV_INPUT, "Decay CV");
        configInput(WOW_CV_INPUT, "Wow/flutter CV");
        configInput(OVERLAP_CV_INPUT, "Loop overlap CV");
        configInput(SEND_MIX_CV_INPUT, "FX return mix CV");
        configInput(RETURN_INPUT, "FX return (re-recorded onto the tape)");
        configOutput(AUDIO_OUTPUT, "Audio (matches the recorded channel count)");
        configOutput(AGE_OUTPUT, "Age (0.1V per pass)");
        configOutput(EOC_OUTPUT, "End of loop trigger");
        configOutput(RAMP_OUTPUT, "Play head position (0..10V ramp)");
        configOutput(SEND_OUTPUT, "FX send (the loop read)");
        configLight(REC_LIGHT, "Recording");
        configLight(EOC_LIGHT, "End of loop");
        configLight(OUT_LIGHT, "Output level");
    }

    void onReset() override {
        loopLen = 0;
        playPos = recPos = 0;
        headAge = 0.f;
        recording = false;
        age = 0;
        std::fill(std::begin(lpState), std::end(lpState), 0.f);
        spliceFade = 0;
        xfadeRemain = 0;
        recStartFade = 0;
        xfReadPos = 0;
        loopGain = 0.f;
        lastOffset = 0.f;
        wowPhase = flutterPhase = 0.f;
        dropEnv = 1.f;
        dropTimer = 0;
        eocFlash = 0.f;
        outEnv = 0.f;
        for (auto& ch : tape) std::fill(ch.begin(), ch.end(), 0.f);
    }

    // grow the per-channel buffers to at least n tracks (never shrinks)
    void ensureChannels(int n) {
        n = clamp(n, 1, kMaxChannels);
        while ((int)tape.size() < n) {
            tape.emplace_back(tapeCap, 0.f);
            pristine.emplace_back();
            xfBuf.emplace_back(xfBufLen, 0.f);
            spliceBuf.emplace_back(xfBufLen, 0.f);
        }
    }

    float noise() {
        uint32_t& s = noiseState;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }

    // uniform [0,1)
    float urand() { return noise() * 0.5f + 0.5f; }

    void startRecording(int inCh) {
        recording = true;
        recPos = 0;
        age = 0;
        xfadeRemain = 0;
        spliceFade = 0;   // drop any in-flight splice fade
        channels = clamp(inCh, 1, kMaxChannels);   // tape width for this take
        ensureChannels(channels);
        // snapshot the loop's continuation so the output can crossfade from
        // playback to the input monitor while the tape gets overwritten. Read
        // along the play head's current wow trajectory (offset frozen; wow is
        // sub-Hz, so it barely moves across the ~10 ms fade) so the snapshot
        // continues from the last played sample with no step.
        if (loopLen > 0) {
            int n = std::min(xfBufLen, loopLen);
            float base = playPos + lastOffset;
            for (int k = 0; k < n; k++) {
                float rp = base + k;
                rp -= loopLen * std::floor(rp / loopLen);
                int i0 = (int)rp;
                float f = rp - i0;
                int i1 = i0 + 1; if (i1 >= loopLen) i1 = 0;
                for (int c = 0; c < channels; c++)
                    xfBuf[c][k] = tape[c][i0] + f * (tape[c][i1] - tape[c][i0]);
            }
            xfReadPos = 0;
            recStartFade = n;
        } else {
            recStartFade = 0;
            loopGain = 0.f;
        }
    }

    void stopRecording(float sr) {
        recording = false;
        spliceFade = 0;   // no stale splice fade into the new loop
        if (recPos < (int)(0.05f * sr)) {   // too short: keep nothing
            loopLen = 0;
            return;
        }
        loopLen = recPos;
        for (int c = 0; c < channels; c++) {
            pristine[c].assign(tape[c].begin(), tape[c].begin() + loopLen);
            // start the write-head filter where the seam ends, not from zero,
            // so pass 2 doesn't get a level dip baked into the loop head
            lpState[c] = tape[c][loopLen - 1];
        }
        playPos = 0;
        headAge = 0.f;
        lastOvl = 0.f;   // recomputed on the first played sample
        dropEnv = 1.f;
        dropTimer = 0;
        // seam declick: over the next ~10 ms, crossfade the loop head with
        // the live input (see process), so tape[0] follows tape[loopLen-1]
        // as smoothly as the input itself did
        xfadeTotal = std::min((int)(0.01f * sr), loopLen);
        xfadeRemain = xfadeTotal;
    }

    bool splice() {
        if (loopLen > 0 && (int)pristine.size() >= channels
            && !pristine[0].empty()) {
            // snapshot the aged output continuation *before* overwriting the
            // tape, so the swap can be a crossfade rather than a step (see the
            // splice blend in process). Walk the same trajectory the read side
            // will: headAge advancing under the hop wrap, both heads blended
            // while they cross. Overlap geometry and wow offset are frozen at
            // their last values — fine over a 10 ms window.
            int n = std::min(xfBufLen, loopLen);
            float ovl = std::min(lastOvl, 0.5f * loopLen);
            float hopLen = loopLen - ovl;
            float ha = headAge;
            for (int k = 0; k < n; k++) {
                ha += 1.f;
                if (ha >= hopLen) ha -= hopLen;
                bool crossing = ovl > 0.5f && ha < ovl;
                float wCur = 1.f, wPrev = 0.f;
                if (crossing) {
                    float th = 0.5f * (float)M_PI * ha / ovl;
                    wCur = std::sin(th);
                    wPrev = std::cos(th);
                }
                float rp = ha + lastOffset;
                rp -= loopLen * std::floor(rp / loopLen);
                int i0 = (int)rp;
                float f = rp - i0;
                int i1 = i0 + 1; if (i1 >= loopLen) i1 = 0;
                int k0 = 0, k1 = 0; float fk = 0.f;
                if (crossing) {
                    float rq = ha + hopLen + lastOffset;
                    rq -= loopLen * std::floor(rq / loopLen);
                    k0 = (int)rq;
                    fk = rq - k0;
                    k1 = k0 + 1; if (k1 >= loopLen) k1 = 0;
                }
                for (int c = 0; c < channels; c++) {
                    float readA = tape[c][i0] + f * (tape[c][i1] - tape[c][i0]);
                    if (crossing) {
                        float readB = tape[c][k0] + fk * (tape[c][k1] - tape[c][k0]);
                        spliceBuf[c][k] = wCur * readA + wPrev * readB;
                    } else {
                        spliceBuf[c][k] = readA;
                    }
                }
            }
            for (int c = 0; c < channels; c++)
                std::copy(pristine[c].begin(), pristine[c].end(), tape[c].begin());
            spliceFadeTotal = n;
            spliceFade = n;
            age = 0;
            dropTimer = 0;
            return true;
        }
        return false;
    }

    void process(const ProcessArgs& args) override {
        const float sr = args.sampleRate;
        if (sr != curSampleRate) {
            curSampleRate = sr;
            tapeCap = (size_t)(kMaxSeconds * sr);
            xfBufLen = (int)(0.01f * sr);                    // ~10 ms crossfade
            loopGainStep = 1.f / std::max(1, xfBufLen);
            tape.clear();
            pristine.clear();
            xfBuf.clear();
            spliceBuf.clear();
            channels = 1;
            ensureChannels(1);
            onReset();
        }

        // ── record control: button toggles, gate follows its edges ──────────
        int inCh = std::max(1, inputs[AUDIO_INPUT].getChannels());
        if (recButton.process(params[REC_PARAM].getValue() > 0.5f)) {
            if (recording) stopRecording(sr); else startRecording(inCh);
        }
        bool gateHigh = inputs[REC_GATE_INPUT].getVoltage() >= 1.f;
        if (gateHigh && !gateWasHigh && !recording) startRecording(inCh);
        if (!gateHigh && gateWasHigh && recording) stopRecording(sr);
        gateWasHigh = gateHigh;

        if (spliceButton.process(params[SPLICE_PARAM].getValue() > 0.5f)
            || spliceTrigger.process(inputs[SPLICE_TRIG_INPUT].getVoltage(), 0.1f, 1.f))
            splice();

        float decay = params[DECAY_PARAM].getValue();
        if (inputs[DECAY_CV_INPUT].isConnected())
            decay += inputs[DECAY_CV_INPUT].getVoltage() * 0.2f;
        decay = clamp(decay, 0.f, 1.f);
        float wowK = params[WOW_PARAM].getValue();
        if (inputs[WOW_CV_INPUT].isConnected())
            wowK += inputs[WOW_CV_INPUT].getVoltage() * 0.2f;
        wowK = clamp(wowK, 0.f, 1.f);
        float overlap = params[OVERLAP_PARAM].getValue();
        if (inputs[OVERLAP_CV_INPUT].isConnected())
            overlap += inputs[OVERLAP_CV_INPUT].getVoltage() * 0.2f;
        overlap = clamp(overlap, 0.f, 1.f);
        float sendMix = params[SEND_MIX_PARAM].getValue();
        if (inputs[SEND_MIX_CV_INPUT].isConnected())
            sendMix += inputs[SEND_MIX_CV_INPUT].getVoltage() * 0.2f;
        sendMix = clamp(sendMix, 0.f, 1.f);
        // the fx loop only folds in the return when it is a complete loop —
        // both send patched out and return patched back — so a stray return
        // signal is never baked into the tape on its own
        bool fxActive = inputs[RETURN_INPUT].isConnected()
                     && outputs[SEND_OUTPUT].isConnected();

        // output width: the recorded tape width while a loop or recording
        // exists, otherwise follow the input so monitoring is poly too
        int nc = (loopLen > 0 || recording) ? channels : inCh;
        float in[kMaxChannels];
        for (int c = 0; c < nc; c++)
            in[c] = inputs[AUDIO_INPUT].getPolyVoltage(c) * 0.2f;   // ±5V -> ±1

        float loopSig[kMaxChannels] = {};

        if (recording) {
            // write straight to tape
            for (int c = 0; c < channels; c++)
                tape[c][recPos] = in[c];
            if (++recPos >= (int)tapeCap) stopRecording(sr);
            // feed the rec-start crossfade from the pre-record snapshot,
            // since the tape under the play head is being overwritten
            if (recStartFade > 0) {
                for (int c = 0; c < channels; c++)
                    loopSig[c] = xfBuf[c][xfReadPos];
                xfReadPos++;
                recStartFade--;
            }
        } else if (loopLen > 0) {
            // ── seam declick: first pass after stop blends the loop head
            //    with the live input, in tape and pristine alike ───────────────
            if (xfadeRemain > 0) {
                float t = 1.f - (float)xfadeRemain / xfadeTotal;
                for (int c = 0; c < channels; c++)
                    tape[c][playPos] = pristine[c][playPos]
                                     = t * tape[c][playPos] + (1.f - t) * in[c];
                xfadeRemain--;
            }

            // ── wow/flutter on the play head (shared across tracks) ──────────
            wowPhase += 0.6f / sr;
            if (wowPhase >= 1.f) wowPhase -= 1.f;
            flutterPhase += 5.3f / sr;
            if (flutterPhase >= 1.f) flutterPhase -= 1.f;
            float wowAmp = wowK * 0.002f * sr * (1.f + 0.01f * std::min(age, 100));
            float offset = wowAmp * (std::sin(2.f * M_PI * wowPhase)
                          + 0.25f * std::sin(2.f * M_PI * flutterPhase));

            lastOffset = offset;   // so a rec press can continue the head

            // ── loop overlap: two heads take turns playing the buffer straight
            //    through. A new head starts every HOP = loopLen - overlap
            //    samples; where it starts, the old head is still finishing, so
            //    they cross over an OVERLAP-long window (new fades in, old fades
            //    out, equal power). overlap=0 => HOP = loopLen, heads play
            //    strictly back to back (the old single-head loop). At the max
            //    the next head starts when the current is halfway (HOP =
            //    loopLen/2). Read-side only: the write head still ages the whole
            //    buffer once per loopLen, so the tape keeps rotting. ──────────
            float ovl = overlap * 0.5f * loopLen;      // overlap samples, 0..L/2
            float hopLen = loopLen - ovl;              // head spacing, L..L/2
            lastOvl = ovl;   // so a splice can retrace the head chain
            headAge += 1.f;
            if (headAge >= hopLen) headAge -= hopLen;  // a new head takes over

            bool crossing = ovl > 0.5f && headAge < ovl;
            float wCur = 1.f, wPrev = 0.f;
            if (crossing) {
                float th = 0.5f * (float)M_PI * headAge / ovl;
                wCur = std::sin(th);       // new head fading in
                wPrev = std::cos(th);      // old head fading out
            }
            // current head reads the buffer at its age (+ wow), wrapped
            float rp = headAge + offset;
            while (rp < 0.f) rp += loopLen;
            while (rp >= loopLen) rp -= loopLen;
            int i0 = (int)rp;
            float f = rp - i0;
            int i1 = i0 + 1; if (i1 >= loopLen) i1 = 0;
            // previous head, one hop behind, read only while the two cross
            int k0 = 0, k1 = 0; float fk = 0.f;
            if (crossing) {
                float rq = headAge + hopLen + offset;
                while (rq < 0.f) rq += loopLen;
                while (rq >= loopLen) rq -= loopLen;
                k0 = (int)rq;
                fk = rq - k0;
                k1 = k0 + 1; if (k1 >= loopLen) k1 = 0;
            }

            // write-head coefficients, shared across tracks
            float fc = 22000.f * std::pow(10.f, -decay);   // 22 kHz .. 2.2 kHz
            float a = 1.f - std::exp(-2.f * (float)M_PI * fc / sr);
            float satMix = 0.5f * decay;
            // dropouts hit the whole tape width, so decide once per sample
            if (dropTimer > 0) {
                dropTimer--;
            } else if (urand() < decay * age * 8e-7f) {
                dropTimer = (int)(sr * (0.005f + 0.02f * urand()));
            }
            dropEnv += (((dropTimer > 0) ? 0.15f : 1.f) - dropEnv) * 0.005f;

            // ── the write head re-records a slightly worse copy, per track ──
            for (int c = 0; c < channels; c++) {
                float readA = tape[c][i0] + f * (tape[c][i1] - tape[c][i0]);
                if (crossing) {
                    float readB = tape[c][k0] + fk * (tape[c][k1] - tape[c][k0]);
                    loopSig[c] = wCur * readA + wPrev * readB;
                } else {
                    loopSig[c] = readA;   // wCur == 1
                }
                float w = tape[c][playPos];
                lpState[c] += a * (w - lpState[c]);
                w = lpState[c];
                // mild saturation and level sag
                w = (1.f - satMix) * w + satMix * std::tanh(w);
                w *= 1.f - 0.002f * decay;
                // tape hiss (independent per track)
                w += decay * 2e-4f * noise();
                w *= dropEnv;
                // fold the fx return onto the tape; it compounds pass over pass
                if (fxActive)
                    w = (1.f - sendMix) * w
                      + sendMix * inputs[RETURN_INPUT].getPolyVoltage(c) * 0.2f;
                if (!std::isfinite(w)) w = 0.f;
                // bound the tape so a hot fx-return loop saturates instead of
                // exploding: transparent below ±1 (±5V nominal), tanh-fold the
                // excess above (C1 at the knee), asymptote ±2 (±10V). Applied
                // once per pass, a runaway loop compresses a little more each
                // time and settles into a drone instead of squaring off at the
                // rails like the old hard clamp
                float mag = std::fabs(w);
                if (mag > 1.f) w = std::copysign(1.f + std::tanh(mag - 1.f), w);
                tape[c][playPos] = w;
            }

            // ── splice declick: crossfade the freshly restored pristine read
            //    against the snapshot of the aged output (linear, since the two
            //    are highly correlated). t: 0 (all aged) -> 1 (all pristine) ──
            if (spliceFade > 0) {
                int k = spliceFadeTotal - spliceFade;
                float t = (float)k / spliceFadeTotal;
                for (int c = 0; c < channels; c++)
                    loopSig[c] = t * loopSig[c] + (1.f - t) * spliceBuf[c][k];
                spliceFade--;
            }

            if (++playPos >= loopLen) {
                playPos = 0;
                age++;
                eocPulse.trigger(1e-3f);
                eocFlash = 1.f;
            }
        }

        // ── ramp the loop in/out across rec presses so the monitor/playback
        //    handoff is a crossfade, not a step ──────────────────────────────
        float loopTarget = (!recording && loopLen > 0) ? 1.f : 0.f;
        if (loopGain < loopTarget)
            loopGain = std::min(loopTarget, loopGain + loopGainStep);
        else if (loopGain > loopTarget)
            loopGain = std::max(loopTarget, loopGain - loopGainStep);

        // input monitoring: ALWAYS passes through at all times; the default
        // mode monitors whatever the loop isn't covering (recording, empty,
        // and the fade between). Both ride opposite the loop's gain, so the
        // sum stays continuous through a rec press.
        float passGain = (monitorMode == MONITOR_ALWAYS)    ? 1.f            : 0.f;
        float monGain  = (monitorMode == MONITOR_WHILE_REC) ? (1.f - loopGain) : 0.f;

        outputs[AUDIO_OUTPUT].setChannels(nc);
        outputs[SEND_OUTPUT].setChannels(nc);
        float level = 0.f;
        for (int c = 0; c < nc; c++) {
            float out = (passGain + monGain) * in[c] + loopGain * loopSig[c];
            outputs[AUDIO_OUTPUT].setVoltage(5.f * clamp(out, -2.f, 2.f), c);
            // fx send: the loop read, before it is mixed with the monitor
            outputs[SEND_OUTPUT].setVoltage(5.f * clamp(loopSig[c], -2.f, 2.f), c);
            level = std::max(level, std::fabs(out));
        }
        outputs[AGE_OUTPUT].setVoltage(std::min(0.1f * age, 10.f));
        outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
        // play head as a loop-locked ramp, clean of wow so it stays a stable sync
        outputs[RAMP_OUTPUT].setVoltage(loopLen > 0 ? 10.f * playPos / loopLen : 0.f);
        lights[REC_LIGHT].setBrightness(recording ? 1.f : 0.f);
        // ~100 ms flash per loop wrap; smoothed audio level on the out badge
        eocFlash *= 1.f - 10.f * args.sampleTime;
        if (eocFlash < 0.f) eocFlash = 0.f;
        lights[EOC_LIGHT].setBrightness(eocFlash);
        outEnv += (level - outEnv) * 0.002f;
        lights[OUT_LIGHT].setBrightness(clamp(outEnv, 0.f, 1.f));
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "monitorMode", json_integer(monitorMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "monitorMode"))
            monitorMode = clamp((int)json_integer_value(j), 0, 2);
        else if (json_t* j = json_object_get(root, "monitor"))   // pre-2.7.0 patches
            monitorMode = json_boolean_value(j) ? MONITOR_ALWAYS : MONITOR_WHILE_REC;
    }
};

struct TabesWidget : ModuleWidget {
    TabesWidget(Tabes* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/tabes.svg")));

// @layout:begin tabes 60.96 128.5
// @elem SCREW_TL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_TR ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BL ScrewSilver 3.5 screw "" 0.0
// @elem SCREW_BR ScrewSilver 3.5 screw "" 0.0
// @elem DECAY_PARAM RoundBigBlackKnob 6.0 param "" 0.0
// @elem WOW_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem OVERLAP_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem SEND_MIX_PARAM RoundBlackKnob 4.5 param "" 0.0
// @elem REC_PARAM TL1105 2.0 param "" 0.0
// @elem REC_LIGHT SmallLight 1.5 light "" 0.0
// @elem SPLICE_PARAM TL1105 2.0 param "" 0.0
// @elem DECAY_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem WOW_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem OVERLAP_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SEND_MIX_CV_INPUT PJ301MPort 4.18 input "" 0.0
// @elem AUDIO_INPUT PJ301MPort 4.18 input "" 0.0
// @elem RETURN_INPUT PJ301MPort 4.18 input "" 0.0
// @elem REC_GATE_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SPLICE_TRIG_INPUT PJ301MPort 4.18 input "" 0.0
// @elem SEND_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem AGE_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem EOC_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem EOC_LIGHT SmallLight 1.5 light "" 0.0
// @elem RAMP_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem AUDIO_OUTPUT PJ301MPort 4.18 output "" 0.0
// @elem OUT_LIGHT SmallLight 1.5 light "" 0.0
// @elem LABEL_DECAY label 0.0 label "decay" 0.0 11.00 32.50
// @elem LABEL_WOW label 0.0 label "wow" 0.0 26.00 32.50
// @elem LABEL_OVERLAP label 0.0 label "overlap" 0.0 40.00 32.50
// @elem LABEL_SENDMIX label 0.0 label "send" 0.0 53.50 32.50
// @elem LABEL_REC label 0.0 label "rec" 0.0 40.00 62.50
// @elem LABEL_SPLICE label 0.0 label "splice" 0.0 53.50 62.50
// @elem LABEL_IN label 0.0 label "in" 0.0 10.16 78.50
// @elem LABEL_SEND label 0.0 label "send" 0.0 10.16 95.50
// @elem LABEL_RTN label 0.0 label "rtn" 0.0 30.48 95.50
// @elem LABEL_AGE label 0.0 label "age" 0.0 50.80 95.50
// @elem LABEL_RAMP label 0.0 label "ramp" 0.0 10.16 111.50
// @elem LABEL_EOC label 0.0 label "eoc" 0.0 30.48 111.50
// @elem LABEL_OUT label 0.0 label "out" 0.0 50.80 111.50
// @elem BOX_SEND panel_box 7.0 box "" 0.0 10.16 90.00
// @elem BOX_AGE panel_box 7.0 box "" 0.0 50.80 90.00
// @elem BOX_RAMP panel_box 7.0 box "" 0.0 10.16 106.00
// @elem BOX_EOC panel_box 7.0 box "" 0.0 30.48 106.00
// @elem BOX_OUT panel_box 7.0 box "" 0.0 50.80 106.00
// @elem LOGO forsitan_logo 0.0 logo "" 0.0 30.48 121.00

        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 0.00f)))); // SCREW_TL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 0.00f)))); // SCREW_TR
        addChild(createWidget<ScrewSilver>(mm2px(Vec(2.54f, 123.42f)))); // SCREW_BL
        addChild(createWidget<ScrewSilver>(mm2px(Vec(53.34f, 123.42f)))); // SCREW_BR
        addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(11.00f, 19.00f)), module, Tabes::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(26.00f, 21.82f)), module, Tabes::WOW_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.00f, 21.82f)), module, Tabes::OVERLAP_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(53.50f, 21.82f)), module, Tabes::SEND_MIX_PARAM));
        addParam(createParamCentered<TL1105>(mm2px(Vec(40.00f, 55.00f)), module, Tabes::REC_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(42.90f, 52.10f)), module, Tabes::REC_LIGHT));
        addParam(createParamCentered<TL1105>(mm2px(Vec(53.50f, 55.00f)), module, Tabes::SPLICE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(11.00f, 40.00f)), module, Tabes::DECAY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(26.00f, 40.00f)), module, Tabes::WOW_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.00f, 40.00f)), module, Tabes::OVERLAP_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.50f, 40.00f)), module, Tabes::SEND_MIX_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 71.00f)), module, Tabes::AUDIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.48f, 88.00f)), module, Tabes::RETURN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.00f, 71.00f)), module, Tabes::REC_GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.50f, 71.00f)), module, Tabes::SPLICE_TRIG_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.16f, 88.00f)), module, Tabes::SEND_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(50.80f, 88.00f)), module, Tabes::AGE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.48f, 104.00f)), module, Tabes::EOC_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(35.48f, 101.00f)), module, Tabes::EOC_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.16f, 104.00f)), module, Tabes::RAMP_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(50.80f, 104.00f)), module, Tabes::AUDIO_OUTPUT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(55.80f, 101.00f)), module, Tabes::OUT_LIGHT));
        // @layout:end
    }

    void appendContextMenu(Menu* menu) override {
        Tabes* m = dynamic_cast<Tabes*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Monitor input",
            {"While recording or empty", "Always", "Never"}, &m->monitorMode));
        menu->addChild(createMenuItem("Clear loop", "", [m]() { m->onReset(); }));
    }
};

Model* modelTabes = createModel<Tabes, TabesWidget>("tabes");
