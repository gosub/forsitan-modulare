#pragma once
#include <rack.hpp>

#include "../shared/dsp.hpp"
#include "../shared/modulation.hpp"

#include <cmath>
#include <cstdint>

// The FX core of artifex: nine modes around one filtered feedback loop.
//
// Every mode reads the same three controls — time, feedback, amount — plus
// the filter that sits inside the loop and the stereo detune that pulls the
// two channels apart. What they act on is the mode's business; that they mean
// rate, loop and wetness is the machine's. See doc/artifex.md.
//
// Header-only and free of Rack widgets, so test/artifex_probe can drive it.

namespace artifex_fx {

using namespace rack;
using forsitan_dsp::Delay;
using forsitan_dsp::Svf;

enum Mode {
	MODE_DELAY = 0,
	MODE_FLANGER,
	MODE_FREEZER,
	MODE_PANNER,
	MODE_CRUSHER,
	MODE_SLICER,
	MODE_PITCHER,
	MODE_REPLAYER,
	MODE_SHIFTER,
	MODE_COUNT
};

// What the right-hand display is reading.
enum TimeUnit {
	UNIT_MS = 0,
	UNIT_HZ,
	UNIT_DIV,        // a ratio of the clock period
	UNIT_STEPS,      // a length in clock steps
	UNIT_RHYTHM,     // which of the 32 patterns
	UNIT_SEMI,       // a pitch interval
	UNIT_SPEED,      // tape speed, signed
};

struct Ctl {
	int mode = 0;
	float time = 0.5f;        // 0..1, knob and CV already summed
	float amount = 0.5f;      // 0..1
	float feedback = 0.f;     // 0..1
	float filter = 0.f;       // -1..1, lowpass to the left
	float stereo = 0.f;       // 0..1
	bool trig = false;        // an edge this sample
	bool stepped = false;     // the clock advanced this sample
	int step = 0;             // which step it is on
	float stepSeconds = 0.125f;
	float trigPeriod = 0.f;   // measured period of the trig input, 0 = none
	bool limiter = true;
	float dt = 1.f / 44100.f;
};

// The longest delay the hardware has, and what every mode's time range is
// written against.
static const float kHardwareBuffer = 1.15f;

// Where the signal starts folding. Every buffer write and the output limiter
// alike run softClip over x / kClipVolts and scale back, so the path is exactly
// linear below this and asymptotic to it above. It is therefore also the level
// the input lamps and the envelope follower in src/artifex.cpp read against:
// three different numbers for the same ceiling would light the lamps after the
// sound had already been squashed.
static const float kClipVolts = 5.f;

// How long the slicer's envelope takes to open. Short enough to be a chop,
// long enough not to click.
static const float kSliceAttack = 0.002f;

// The delay's time glides towards the knob rather than following it. A knob in
// Rack arrives in steps -- a parameter moves once per UI frame -- and a delay
// that teleports its read position clicks on every one of them, which is a
// buzz at the frame rate and not a sweep. The glide is also the mode's whole
// character: a read pointer travelling through the tape at a speed other than
// one is exactly the pitch bend a tape echo makes when its motor changes
// speed, and 50 ms is slow enough to hear that and fast enough that the knob
// still feels connected.
static const float kDelayGlide = 0.050f;

// The flanger and the pitcher multiply a knob straight into a read position
// too, and need the same treatment for the same reason -- but not the bend.
// Twenty milliseconds bridges the 16.7 ms between two UI frames and is short
// enough that the knob still feels direct.
static const float kKnobGlide = 0.020f;

// A one-pole glide that snaps the first time it is asked. The mode's state is
// reset on entering it, so arriving somewhere gives you the knob rather than a
// slide from wherever the last mode left this.
struct Glide {
	float v = 0.f;
	bool primed = false;
	void reset() { primed = false; }
	float operator()(float target, float dt, float tau) {
		if (!primed) {
			v = target;
			primed = true;
		}
		else {
			v += (target - v) * (1.f - std::exp(-dt / tau));
		}
		return v;
	}
};

struct Core {
	float sr = 44100.f;
	float bufSeconds = kHardwareBuffer;

	Delay tape[2];        // delay, freezer, replayer, pitcher
	Delay frz[2];         // the chunk the freezer is holding
	Delay flg[2];         // the flanger's short modulated line
	Delay shf[2];         // the shifter's window
	Svf fbFilt[2];

	int lastMode = -1;
	float filtSm = 0.f;
	bool filtWasLow = false;
	float fbState[2] = {0.f, 0.f};

	// per-mode state
	float modPhase[2] = {0.f, 0.f};      // flanger, panner
	Glide delayGl;            // delay: the time, which is also its pitch bend
	Glide flangeGl;           // flanger: the sweep depth, off the amount knob
	Glide pitchWinGl;         // pitcher: the window, off the time knob
	Glide pitchAmtGl;         // pitcher: the shift, off the amount knob
	int panDir = 1;
	double freezePos[2] = {0.0, 0.0};
	float freezeFrames = 0.f;    // the loop length now, which the knob moves
	float capturedFrames = 0.f;  // how much history the last freeze caught
	double recorded = 0.0;        // frames recorded since arriving in the mode
	bool frozen = false;
	bool wasSilentAmount = true;
	float crushHold[2] = {0.f, 0.f};
	float crushPhase[2] = {0.f, 0.f};
	float crushDip = 0.f;
	float sliceEnv[2] = {0.f, 0.f};
	float sliceAtk[2] = {0.f, 0.f};
	float grainPhase[2] = {0.f, 0.f};
	float grainStretch = 0.f;
	double tapePos[2] = {0.0, 0.0};
	float fillLeft = 0.f;
	float shiftPhase[2] = {0.f, 0.f};
	uint32_t rng = 0x9e3779b9u;

	// what the display reads
	float uiTime = 0.f;
	int uiUnit = UNIT_MS;

	void setRates(float sampleRate, float seconds) {
		sr = sampleRate;
		bufSeconds = seconds;
		int n = (int)(seconds * sr) + 4;
		for (int c = 0; c < 2; c++) {
			tape[c].init(n);
			frz[c].init(n);
			flg[c].init((int)(0.05f * sr) + 4);
			shf[c].init((int)(0.3f * sr) + 4);
			fbFilt[c].reset();
			tapePos[c] = 0.0;
			freezePos[c] = 0.0;
		}
		lastMode = -1;
	}

	uint32_t next() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	}

	void enterMode(int mode) {
		for (int c = 0; c < 2; c++) {
			fbFilt[c].reset();
			fbState[c] = 0.f;
			sliceAtk[c] = 0.f;
			modPhase[c] = 0.f;
			crushPhase[c] = 0.f;
			crushHold[c] = 0.f;
			sliceEnv[c] = 0.f;
			grainPhase[c] = 0.f;
			shiftPhase[c] = (float)c * 0.5f;
			tapePos[c] = 0.0;
		}
		panDir = 1;
		delayGl.reset();
		flangeGl.reset();
		pitchWinGl.reset();
		pitchAmtGl.reset();
		crushDip = 0.f;
		grainStretch = 0.f;
		// Arriving in the replayer loads the tape, which is what a trigger
		// does there too. Without it, landing on the mode with amount hard
		// right — the locked position — plays a buffer nobody has recorded
		// into yet, and the mode is silent for no reason a player can see.
		fillLeft = (mode == MODE_REPLAYER) ? bufSeconds : 0.f;
		frozen = false;
		recorded = 0.0;
		capturedFrames = 0.f;
		// Arriving in the freezer captures a chunk, as on the hardware — but
		// it has to record one first. Leaving this true would freeze the
		// empty buffer on the first sample and hold silence for good.
		wasSilentAmount = false;
		lastMode = mode;
	}

	// The filter lives in the feedback path only: it colours what comes back
	// round without touching the dry signal. Open at the centre, a lowpass to
	// the left and a highpass to the right.
	//
	// It reads the smoothed knob, never the raw one, and its wet path fades
	// in over the first twentieth of the travel: crossing the centre swaps a
	// lowpass for a highpass, and no integrator state survives that. A step
	// here would be worse than elsewhere — it goes straight back into the
	// loop and comes round again.
	float loopFilter(int c, float x) {
		float wet = clamp(std::fabs(filtSm) * 20.f, 0.f, 1.f);
		if (wet < 1e-4f)
			return x;
		bool lowpass = filtSm < 0.f;
		float mag = std::fabs(filtSm);
		float fc = lowpass ? 80.f * std::pow(250.f, 1.f - mag)
		                   : 20.f * std::pow(200.f, mag);
		fc = clamp(fc, 20.f, 0.45f * sr);
		float g = std::tan((float)M_PI * fc / sr);
		float lp, hp;
		fbFilt[c].process(x, g, 1.4f, lp, hp);
		float y = lowpass ? lp : hp;
		return x + (y - x) * wet;
	}

	// Linear below kClipVolts, saturating above it -- and continuous at the
	// join, which the obvious form is not. tanh(1) is 0.762, so branching
	// straight into tanh dropped 0.238 -- 1.19 V -- the instant a signal
	// crossed the boundary, and climbed back the instant it recrossed. Every
	// mode sat on that step, because 5 V is where ordinary material sits: at
	// the flanger's normal level it read as a click twice per cycle of a tone,
	// and it was the largest discontinuity anywhere in the module.
	//
	// Starting the curve where the line ends matches the value and the slope
	// at the join, since d/dx tanh(0) is 1, so nothing happens there at all.
	// The asymptote moves from 1 to 2: kClipVolts is still where folding
	// begins, and a fully folded signal now reaches the rail rather than
	// stopping at nominal.
	static float softClip(float x) {
		if (x > 1.f)
			return 1.f + std::tanh(x - 1.f);
		if (x < -1.f)
			return -1.f - std::tanh(-x - 1.f);
		return x;
	}

	// Two channels pulled apart by the stereo knob: left slows, right speeds.
	static float detune(int c, float stereo) {
		float d = stereo * 0.35f;
		return c == 0 ? 1.f - d : 1.f + d;
	}

	void process(const Ctl& ct, float inL, float inR, float& outL, float& outR) {
		if (ct.mode != lastMode)
			enterMode(ct.mode);

		// the filter knob, smoothed, with the states cleared as it crosses
		// the centre — where the wet path above is faded out, so the clearing
		// is inaudible
		filtSm += (clamp(ct.filter, -1.f, 1.f) - filtSm)
		          * (1.f - std::exp(-ct.dt / 0.010f));
		bool low = filtSm < 0.f;
		if (low != filtWasLow) {
			for (int c = 0; c < 2; c++)
				fbFilt[c].reset();
			filtWasLow = low;
		}

		float in[2] = {inL, inR};
		float out[2] = {0.f, 0.f};
		float amt = clamp(ct.amount, 0.f, 1.f);
		float fb = clamp(ct.feedback, 0.f, 1.f);
		float t = clamp(ct.time, 0.f, 1.f);

		switch (ct.mode) {
		case MODE_DELAY:      doDelay(ct, in, out, t, amt, fb); break;
		case MODE_FLANGER:    doFlanger(ct, in, out, t, amt, fb); break;
		case MODE_FREEZER:    doFreezer(ct, in, out, t, amt, fb); break;
		case MODE_PANNER:     doPanner(ct, in, out, t, amt, fb); break;
		case MODE_CRUSHER:    doCrusher(ct, in, out, t, amt, fb); break;
		case MODE_SLICER:     doSlicer(ct, in, out, t, amt, fb); break;
		case MODE_PITCHER:    doPitcher(ct, in, out, t, amt, fb); break;
		case MODE_REPLAYER:   doReplayer(ct, in, out, t, amt, fb); break;
		default:              doShifter(ct, in, out, t, amt, fb); break;
		}

		for (int c = 0; c < 2; c++) {
			if (!std::isfinite(out[c]))
				out[c] = 0.f;
			if (ct.limiter)
				out[c] = softClip(out[c] / kClipVolts) * kClipVolts;
			else
				out[c] = clamp(out[c], -20.f, 20.f);
			fbState[c] = out[c];
		}
		outL = out[0];
		outR = out[1];
	}

	// The global loop: the module's own output, filtered, back into its input.
	float loopIn(int c, float x, const Ctl& ct, float fb) {
		return x + loopFilter(c, fbState[c]) * fb * 0.95f;
	}

	// ── 1. delay ─────────────────────────────────────────────────────────────
	// Longest to the left, 2 ms to the right. Feedback is taken before the
	// filter, as on the hardware, so the filter colours the repeats rather
	// than swallowing them.
	void doDelay(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float maxT = bufSeconds - 0.01f;
		float base = maxT * std::pow(0.002f / maxT, t);
		uiUnit = UNIT_MS;

		// A clock at the trig input snaps the time to its nearest division,
		// exactly as the hardware does when it detects one there.
		if (ct.trigPeriod > 1e-4f) {
			static const float div[22] = {
				1.f / 256, 1.f / 128, 1.f / 64, 1.f / 32, 1.f / 16, 1.f / 12,
				1.f / 8, 1.f / 6, 1.f / 4, 1.f / 3, 1.f / 2, 2.f / 3,
				1.f, 3.f / 2, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f, 16.f, 32.f};
			float best = base;
			float bestErr = 1e9f;
			for (int i = 0; i < 22; i++) {
				float cand = ct.trigPeriod * div[i];
				if (cand < 0.002f || cand > maxT)
					continue;
				float err = std::fabs(std::log(cand / base));
				if (err < bestErr) {
					bestErr = err;
					best = cand;
				}
			}
			base = best;
			uiUnit = UNIT_DIV;
			uiTime = base / std::max(ct.trigPeriod, 1e-6f);
		}
		if (uiUnit == UNIT_MS)
			uiTime = base * 1000.f;

		// The display reads the knob; the tape follows it. Arriving in the
		// mode snaps, so the first repeat is the time you asked for.
		float glided = delayGl(base, ct.dt, kDelayGlide);

		for (int c = 0; c < 2; c++) {
			float d = clamp(glided * detune(c, ct.stereo), 0.002f, maxT) * sr;
			float wet = tape[c].read(d);
			tape[c].write(softClip((in[c] + wet * fb * 0.98f) / kClipVolts) * kClipVolts);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}

	// ── 2. flanger ───────────────────────────────────────────────────────────
	// A short delay swept by a sine: chorus around the middle, flanger with
	// feedback, pitch modulation at the extremes.
	void doFlanger(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float hz = 0.02f * std::pow(500.f, t);
		uiUnit = UNIT_HZ;
		uiTime = hz;
		// The modulator's rate is integrated into a phase, so a step in it is
		// harmless. The depth is not: it multiplies the read position, and a
		// knob that moves once per frame moves the tap in jumps.
		float dep = flangeGl(amt, ct.dt, kKnobGlide);
		for (int c = 0; c < 2; c++) {
			float f = hz * detune(c, ct.stereo);
			if (ct.trig)
				modPhase[c] = 0.f;
			modPhase[c] += f * ct.dt;
			modPhase[c] -= std::floor(modPhase[c]);
			float m = std::sin(2.f * (float)M_PI * modPhase[c]);
			float base = 5.5f * 0.001f * sr;
			float depth = (5.f * dep) * 0.001f * sr;
			float wet = flg[c].read(base + depth * m);
			flg[c].write(softClip((in[c] + wet * fb * 0.95f) / kClipVolts) * kClipVolts);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - 0.5f * amt) + heard * amt;
		}
	}

	// ── 3. freezer ───────────────────────────────────────────────────────────
	// Captures a chunk and loops it. Long times are divisions of the clock, so
	// the freeze is rhythmic; short ones shrink until the loop is a pitch.
	void doFreezer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float len;
		if (t < 0.5f) {
			static const float steps[8] = {16.f, 12.f, 8.f, 6.f, 4.f, 3.f, 2.f, 1.f};
			int i = clamp((int)(t * 2.f * 8.f), 0, 7);
			len = ct.stepSeconds * steps[i];
			uiUnit = UNIT_STEPS;
			uiTime = steps[i];
		}
		else {
			len = 0.25f * std::pow(0.008f, (t - 0.5f) * 2.f);
			uiUnit = UNIT_MS;
			uiTime = len * 1000.f;
		}
		len = clamp(len, 0.002f, bufSeconds - 0.02f);

		// the record head never stops: what freezing does is take a copy, so
		// the next freeze still catches whatever has just gone past
		for (int c = 0; c < 2; c++)
			tape[c].write(in[c]);
		recorded += 1.0;
		bool ready = recorded >= (double)(len * sr);

		// three ways to catch a new chunk: arriving in the mode, a trigger,
		// and amount leaving zero -- none of which may capture a buffer that
		// has not been filled yet
		bool refreeze = ct.trig && ready;
		if (amt > 0.02f && wasSilentAmount && ready)
			refreeze = true;
		wasSilentAmount = (amt <= 0.02f);
		if (!frozen && ready)
			refreeze = true;

		// A freeze catches everything it can, not just the length asked for,
		// so the time knob can go on choosing how much of it loops. Catching
		// only `len` meant the knob did nothing at all until something
		// re-captured, and the way to hear a frozen bar shrink into a pitch
		// is to shorten the loop, not to freeze a shorter one.
		if (refreeze) {
			float maxFrames = (bufSeconds - 0.02f) * sr;
			capturedFrames = clamp((float)recorded, len * sr, maxFrames);
			int frames = (int)capturedFrames;
			for (int c = 0; c < 2; c++) {
				// the last `frames` samples ending at the freeze moment, in
				// order, as two spans rather than a modulo per sample
				int n = tape[c].size();
				int from = ((tape[c].w - frames) % n + n) % n;
				int first = std::min(frames, n - from);
				std::copy(tape[c].buf.begin() + from,
				          tape[c].buf.begin() + from + first,
				          frz[c].buf.begin());
				std::copy(tape[c].buf.begin(),
				          tape[c].buf.begin() + (frames - first),
				          frz[c].buf.begin() + first);
				freezePos[c] = 0.0;
			}
			frozen = true;
		}

		// The loop is the last `freezeFrames` before the freeze moment: the
		// captured chunk ends there, so shortening keeps the audio nearest to
		// it -- what you had just heard -- rather than the oldest of it.
		// Lengthening is limited by how much history the freeze actually got.
		freezeFrames = clamp(len * sr, 4.f, std::max(capturedFrames, 4.f));
		float base = capturedFrames - freezeFrames;

		for (int c = 0; c < 2; c++) {
			float rate = detune(c, ct.stereo);
			// the knob can shorten the loop under the playhead, so wrap it
			// round rather than assuming one subtraction is enough
			if (freezePos[c] >= (double)freezeFrames || freezePos[c] < 0.0)
				freezePos[c] -= std::floor(freezePos[c] / freezeFrames) * freezeFrames;
			float wet = frz[c].at(base + (float)freezePos[c]);
			// feedback here bleeds new audio into the frozen buffer rather
			// than running the global loop, thickening what is held
			if (fb > 0.001f)
				frz[c].poke((int)(base + (float)freezePos[c]), wet + in[c] * fb * 0.5f);
			freezePos[c] += rate;
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}

	// ── 4. panner ────────────────────────────────────────────────────────────
	// Amplitude modulation in opposite phase, from a sway to a hard
	// alternation, and up into ring modulation as the rate leaves the LFO band.
	void doPanner(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float hz = 0.05f * std::pow(40000.f, t);
		uiUnit = UNIT_HZ;
		uiTime = hz;
		float depth = clamp(amt * 2.f, 0.f, 1.f);
		float hard = clamp(amt * 2.f - 1.f, 0.f, 1.f);
		if (ct.trig) {
			panDir = -panDir;
			for (int c = 0; c < 2; c++)
				modPhase[c] = 0.f;
		}
		for (int c = 0; c < 2; c++) {
			float f = hz * detune(c, ct.stereo);
			modPhase[c] += f * ct.dt * (float)panDir;
			modPhase[c] -= std::floor(modPhase[c]);
			float m = std::sin(2.f * (float)M_PI * modPhase[c]);
			// clipping the sine towards a square as amount goes up
			m = std::tanh(m * (1.f + hard * 30.f)) / std::tanh(1.f + hard * 30.f);
			float s = (c == 0) ? m : -m;
			float x = loopIn(c, in[c], ct, fb);
			out[c] = x * (1.f - depth * 0.5f * (1.f - s));
		}
	}

	// ── 5. crusher ───────────────────────────────────────────────────────────
	// Downsampling, then bit mangling: the top of the amount knob XORs the
	// sample with a shift of itself, which is where it stops sounding like a
	// bitcrusher and starts sounding broken.
	void doCrusher(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float rate = 200.f * std::pow(150.f, t);
		uiUnit = UNIT_HZ;
		uiTime = rate;
		if (ct.trig)
			crushDip = 1.f;
		crushDip *= std::exp(-ct.dt / 0.15f);
		rate *= std::pow(0.05f, crushDip);
		float bits = 16.f - 14.f * clamp(amt * 2.f, 0.f, 1.f);
		float xorAmt = clamp(amt * 2.f - 1.f, 0.f, 1.f);
		float levels = std::pow(2.f, bits);
		// amount owns the crushing itself, but the rule that a knob at zero
		// leaves the signal alone holds here too: the first tenth of the
		// travel fades the decimated path in
		float wetMix = clamp(amt * 10.f, 0.f, 1.f);

		for (int c = 0; c < 2; c++) {
			float f = clamp(rate * detune(c, ct.stereo), 20.f, 0.5f * sr);
			crushPhase[c] += f * ct.dt;
			if (crushPhase[c] >= 1.f) {
				crushPhase[c] -= std::floor(crushPhase[c]);
				float x = loopIn(c, in[c], ct, fb * 0.5f);
				// soft, not clamped: a hard clip with feedback into it turns
				// every mode setting into the same full-scale square
				float q = std::round(softClip(x / kClipVolts) * levels) / levels;
				if (xorAmt > 0.001f) {
					// XOR the sample with a shift of itself: the top bits
					// survive, so it still follows the signal, and the low
					// ones scramble. Folding in a shift *up* instead would
					// just be full-scale noise at every setting.
					int32_t i = (int32_t)(q * 32767.f);
					float mangled = clamp((float)(i ^ (i >> 3)) / 32767.f, -1.f, 1.f);
					q = q * (1.f - xorAmt) + mangled * xorAmt;
				}
				crushHold[c] = q * 5.f;
			}
			out[c] = in[c] * (1.f - wetMix) + crushHold[c] * wetMix;
		}
	}

	// ── 6. slicer ────────────────────────────────────────────────────────────
	// The rhythm table applied to amplitude: time picks the pattern, amount is
	// the decay, feedback is the chance of a step flipping.
	void doSlicer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		int pat = clamp((int)(t * 31.999f), 0, 31);
		uiUnit = UNIT_RHYTHM;
		uiTime = (float)(pat + 1);
		// A slice is only ever quieter than what went in, so the range stops
		// at 60 ms rather than at a click, the tail is half the stated decay
		// rather than a third of it, and short slices get some of their
		// loudness back: chopping a drone into a rhythm should not also turn
		// the volume down.
		float decay = 1.f * std::pow(0.06f, amt);
		float coef = std::exp(-ct.dt / std::max(decay * 0.5f, 1e-4f));
		float wetMix = clamp(amt * 6.f, 0.f, 1.f);
		float makeup = 1.f + 0.6f * amt;

		for (int c = 0; c < 2; c++) {
			if (ct.stepped) {
				// stereo gives the two channels different rhythms
				int p = pat + (c == 1 ? (int)(ct.stereo * 8.f + 0.5f) : 0);
				bool hit = (forsitan_mod::rhythmPattern(p & 31)
				            & (uint16_t)(0x8000u >> (ct.step & 15))) != 0;
				if (fb > 0.001f && (next() % 1000u) < (uint32_t)(fb * 700.f))
					hit = !hit;
				if (hit)
					sliceAtk[c] = kSliceAttack;
			}
			if (ct.trig)
				sliceAtk[c] = kSliceAttack;
			// a couple of milliseconds of attack: an envelope that jumps
			// straight to one clicks on every step
			if (sliceAtk[c] > 0.f) {
				sliceAtk[c] -= ct.dt;
				sliceEnv[c] = std::min(1.f, sliceEnv[c] + ct.dt / kSliceAttack);
			}
			else
				sliceEnv[c] *= coef;
			float x = loopIn(c, in[c], ct, fb * 0.5f);
			float wet = x * sliceEnv[c] * makeup;
			out[c] = x * (1.f - wetMix) + wet * wetMix;
		}
	}

	// ── 7. pitcher ───────────────────────────────────────────────────────────
	// Pitch up by sweeping a delay tap with a ramp: crude on purpose, with the
	// transient duplication that comes with a single window.
	void doPitcher(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float window = 0.3f * std::pow(0.01f, t);
		uiUnit = UNIT_MS;
		uiTime = window * 1000.f;
		if (ct.trig)
			grainStretch = 1.f;
		grainStretch *= std::exp(-ct.dt / 0.25f);
		// Both knobs scale the tap position here, so both jump it when they
		// move. The ramp restarting is the mode's own crudeness and stays;
		// the knobs clicking on top of it was never part of that.
		window = pitchWinGl(window, ct.dt, kKnobGlide);
		window *= 1.f + 3.f * grainStretch;
		float shift = pitchAmtGl(amt, ct.dt, kKnobGlide);

		for (int c = 0; c < 2; c++) {
			float w = clamp(window * detune(c, ct.stereo), 0.002f, 0.4f);
			float x = loopIn(c, in[c], ct, fb);
			tape[c].write(x);
			grainPhase[c] += ct.dt / w;
			if (grainPhase[c] >= 1.f)
				grainPhase[c] -= std::floor(grainPhase[c]);
			// the tap walks from a window back towards now: a falling delay
			// raises the pitch, and the ramp restarting is the duplication
			float d = (1.f - grainPhase[c]) * w * shift * sr + 2.f;
			float wet = tape[c].read(d);
			// fade the ends of the ramp so the wrap is a click and not a bang
			float e = std::min(grainPhase[c], 1.f - grainPhase[c]) * 20.f;
			wet *= clamp(e, 0.f, 1.f);
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt * 1.4f;
		}
	}

	// ── 8. replayer ──────────────────────────────────────────────────────────
	// A tape loop. Time is the speed and the sign of it; amount decides
	// whether the tape is locked or being written over.
	void doReplayer(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		// The tape never stops. A knob whose centre is exactly zero puts a
		// dead spot in the middle of its travel — the head holds one sample
		// and the mode outputs a DC level — so the centre is the *slowest*
		// speed instead, a quarter, and which side of it you are on is the
		// direction. Two octaves down at the centre, two up at the ends.
		float u = (t - 0.5f) * 2.f;              // -1 .. +1
		float mag = 0.25f * std::pow(8.f, std::fabs(u));
		float speed = u < 0.f ? -mag : mag;
		uiUnit = UNIT_SPEED;
		uiTime = speed;
		if (ct.trig)
			fillLeft = bufSeconds;
		bool filling = fillLeft > 0.f;
		if (filling)
			fillLeft -= ct.dt;
		float rec = filling ? 1.f : (1.f - amt);
		float keep = filling ? 0.f : amt;

		for (int c = 0; c < 2; c++) {
			float sp = speed * detune(c, ct.stereo);
			int n = tape[c].size();
			float held = tape[c].at((float)tapePos[c]);
			float x = in[c] + loopFilter(c, fbState[c]) * fb * 0.9f;
			// What the tape holds after this sample is what you hear: while
			// it is recording you are listening to the head, which is how a
			// tape works and how the amount knob crossfades the old audio
			// against the new one in a single number.
			float wet = held;
			if (rec > 0.001f) {
				wet = softClip((held * keep + x * rec) / kClipVolts) * kClipVolts;
				tape[c].poke((int)tapePos[c], wet);
			}
			// a stopped tape still reads, which is how the centre holds a
			// single frozen grain
			tapePos[c] += (filling ? 1.f : sp);
			if (tapePos[c] >= n)
				tapePos[c] -= n;
			if (tapePos[c] < 0.0)
				tapePos[c] += n;
			out[c] = in[c] * (1.f - amt) + wet * amt;
		}
	}

	// ── 9. shifter ───────────────────────────────────────────────────────────
	// Two taps crossfaded, so there is no stutter — and with feedback a small
	// shift walks the tail away in pitch, one interval per pass.
	void doShifter(const Ctl& ct, float* in, float* out, float t, float amt, float fb) {
		float semis = (t - 0.5f) * 24.f;         // an octave either way
		uiUnit = UNIT_SEMI;
		uiTime = semis;
		if (ct.trig)
			for (int c = 0; c < 2; c++)
				shiftPhase[c] = (float)c * 0.5f;
		float window = 0.08f;                    // seconds of crossfade window

		for (int c = 0; c < 2; c++) {
			float s = semis + (c == 1 ? ct.stereo * 3.f : -ct.stereo * 3.f);
			float ratio = std::pow(2.f, s / 12.f);
			float x = loopIn(c, in[c], ct, fb);
			shf[c].write(x);
			// The pitch a moving tap gives is 1 - d'(t), so the ramp has to
			// run at (1 - ratio) / window: a delay that shortens raises the
			// pitch. Two taps half a window apart, crossfaded, hide the ramp
			// resetting.
			float rate = (1.f - ratio) / window;
			shiftPhase[c] += rate * ct.dt;
			shiftPhase[c] -= std::floor(shiftPhase[c]);
			float wet = 0.f;
			for (int k = 0; k < 2; k++) {
				float ph = shiftPhase[c] + 0.5f * k;
				ph -= std::floor(ph);
				float d = ph * window * sr + 2.f;
				float g = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * ph);
				wet += shf[c].read(d) * g;
			}
			float heard = loopFilter(c, wet);
			out[c] = in[c] * (1.f - amt) + heard * amt;
		}
	}
};

}   // namespace artifex_fx
